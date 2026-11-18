#include "anomaly/mayday_adapter.hpp"
#include "anomaly/mayday_profile_runtime.hpp"
#include "anomaly/adapter_service_registry.hpp"
#include "anomaly/sdk/services/mayday.h"
#include "anomaly/thread_local_value.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace anomaly {

// ---------------------------------------------------------------------------
// MaydayAdapter：il2cpp 探测 + GameState 解析 + 金币/债务服务
// ---------------------------------------------------------------------------
namespace {
constexpr const wchar_t* kGameAssemblyModule = L"GameAssembly.dll";
constexpr const char* kIl2CppServiceId = "mayday.il2cpp";
constexpr const char* kMoneyServiceId = ANOMALY_MAYDAY_MONEY_SERVICE_V1_ID;
constexpr std::uint32_t kIl2CppServiceVersion = 1;
constexpr std::uint32_t kMoneyServiceVersion = ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION;

std::string JsonEscape(const std::string& value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) out += c;
        }
    }
    return out;
}

// ---- il2cpp C API 函数指针签名（仅覆盖本文件用到的子集） ----
using FnDomainGet = void* (*)();
using FnDomainGetAssemblies = void** (*)(void* domain, std::size_t* size);
using FnAssemblyGetImage = void* (*)(void* assembly);
using FnImageGetName = const char* (*)(void* image);
using FnImageGetClass = void* (*)(void* image, int index);
using FnClassFromName = void* (*)(void* image, const char* ns, const char* name);
using FnClassGetMethod = void* (*)(void* klass, const char* name, int args);
using FnRuntimeInvoke = void* (*)(void* method, void* obj, void** params, void** exc);
using FnThreadAttach = void* (*)(void* domain);
using FnStringNew = void* (*)(const char* text);
using FnObjectUnbox = void* (*)(void* obj);
}  // namespace

struct MaydayAdapter::Impl {
    std::stop_token stop_token;
    std::thread probe_worker;
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};

    Il2CppTableV1 table{};
    std::atomic<bool> table_ready{false};
    std::atomic<void*> table_published{nullptr};
    std::atomic<void*> money_published{nullptr};
    std::atomic<int> resolved_exports{0};
    std::atomic<bool> game_assembly_found{false};

    // tick 回调
    std::mutex tick_mutex;
    TickCallback tick_callback;
    std::atomic<bool> tick_in_flight{false};

    // ---- il2cpp API 与 GameState 解析缓存（resolve_mutex 保护） ----
    std::mutex resolve_mutex;
    bool game_resolved{false};
    FnDomainGet domain_get{};
    FnDomainGetAssemblies domain_get_assemblies{};
    FnAssemblyGetImage assembly_get_image{};
    FnImageGetName image_get_name{};
    FnClassFromName class_from_name{};
    FnClassGetMethod class_get_method{};
    FnRuntimeInvoke runtime_invoke{};
    FnThreadAttach thread_attach{};
    FnStringNew string_new{};
    FnObjectUnbox object_unbox{};
    void* domain{};
    void* image{};            // Assembly-CSharp.dll
    void* game_state_class{};
    void* get_instance{};
    void* get_money{};
    void* set_money{};
    void* add_money{};
    void* get_debt{};
    void* add_debt{};
    AnomalyMaydayMoneyServiceV1 money_service{};

    // 服务表回调：Impl 是 MaydayAdapter 的私有嵌套类型，只有成员才能访问。
    static AnomalyStatusV1 ANOMALY_CALL MoneyGetMoney(void* user, std::int32_t* out_money);
    static AnomalyStatusV1 ANOMALY_CALL MoneySetMoney(void* user, std::int32_t value);
    static AnomalyStatusV1 ANOMALY_CALL MoneyAddMoney(void* user, std::int32_t delta);
    static AnomalyStatusV1 ANOMALY_CALL MoneyGetDebt(void* user, std::int32_t* out_debt);
    static AnomalyStatusV1 ANOMALY_CALL MoneySetDebt(void* user, std::int32_t value);

    // 当前线程的 il2cpp attach 结果；FLS 缓存（Core 禁止 thread_local），
    // 首次调用 attach，之后复用，避免每帧 Draw 重复 attach/detach。
    ThreadLocalScalar<void*> il2cpp_thread;
    void* AttachedThread();

    ~Impl() {
        if (probe_worker.joinable()) probe_worker.join();
    }

    void ProbeWorker();
    bool ResolveExports(HMODULE game_assembly);
    void PublishIl2CppTable();
    void PublishMoneyService();
    void RevokePublished();

    // 解析 GameState 类与方法；每线程需先 attach。返回 false = 游戏符号不可用。
    bool ResolveGame();
    // 拿 GameState.Instance（未开局返回 nullptr）
    void* GameInstance();
    bool InvokeInt(void* method, void* object, std::int32_t* out_value);
    bool InvokeVoid(void* method, void* object, void** params);
};

void MaydayAdapter::Impl::ProbeWorker() {
    // 等待 GameAssembly.dll：Unity IL2CPP 脚本运行时在游戏初始化早期加载，
    // 注入时可能尚未就绪。轮询至发现或停止。
    while (!stopping.load(std::memory_order_acquire)) {
        const HMODULE game_assembly = GetModuleHandleW(kGameAssemblyModule);
        if (game_assembly != nullptr) {
            game_assembly_found.store(true, std::memory_order_release);
            if (ResolveExports(game_assembly)) {
                PublishIl2CppTable();
                PublishMoneyService();
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool MaydayAdapter::Impl::ResolveExports(const HMODULE game_assembly) {
    struct Entry { void** slot; const char* name; };
    const Entry entries[] = {
        {reinterpret_cast<void**>(&table.domain_get), "il2cpp_domain_get"},
        {reinterpret_cast<void**>(&table.class_from_name), "il2cpp_class_from_name"},
        {reinterpret_cast<void**>(&table.class_get_method_from_name), "il2cpp_class_get_method_from_name"},
        {reinterpret_cast<void**>(&table.runtime_invoke), "il2cpp_runtime_invoke"},
        {reinterpret_cast<void**>(&table.thread_attach), "il2cpp_thread_attach"},
        {reinterpret_cast<void**>(&table.thread_detach), "il2cpp_thread_detach"},
        {reinterpret_cast<void**>(&table.string_new), "il2cpp_string_new"},
        {reinterpret_cast<void**>(&table.image_get_class), "il2cpp_image_get_class"},
        {reinterpret_cast<void**>(&table.assembly_get_image), "il2cpp_assembly_get_image"},
        {reinterpret_cast<void**>(&domain_get), "il2cpp_domain_get"},
        {reinterpret_cast<void**>(&domain_get_assemblies), "il2cpp_domain_get_assemblies"},
        {reinterpret_cast<void**>(&assembly_get_image), "il2cpp_assembly_get_image"},
        {reinterpret_cast<void**>(&image_get_name), "il2cpp_image_get_name"},
        {reinterpret_cast<void**>(&class_from_name), "il2cpp_class_from_name"},
        {reinterpret_cast<void**>(&class_get_method), "il2cpp_class_get_method_from_name"},
        {reinterpret_cast<void**>(&runtime_invoke), "il2cpp_runtime_invoke"},
        {reinterpret_cast<void**>(&thread_attach), "il2cpp_thread_attach"},
        {reinterpret_cast<void**>(&string_new), "il2cpp_string_new"},
        {reinterpret_cast<void**>(&object_unbox), "il2cpp_object_unbox"},
    };
    int resolved = 0;
    for (const auto& entry : entries) {
        *entry.slot = reinterpret_cast<void*>(GetProcAddress(game_assembly, entry.name));
        if (*entry.slot != nullptr) ++resolved;
    }
    resolved_exports.store(resolved, std::memory_order_release);
    const bool complete = resolved == static_cast<int>(std::size(entries));
    if (complete) {
        table.struct_size = static_cast<std::uint32_t>(sizeof(Il2CppTableV1));
        table_ready.store(true, std::memory_order_release);
    }
    return complete;
}

void MaydayAdapter::Impl::PublishIl2CppTable() {
    auto published = std::make_shared<Il2CppTableV1>(table);
    if (ProcessAdapterServices().Publish(kIl2CppServiceId, kIl2CppServiceVersion,
                                         published.get(), {}, published)) {
        table_published.store(published.get(), std::memory_order_release);
    }
}

// ---- 金币服务实现（SDK 表回调） ----
namespace {

AnomalyStatusV1 MoneyStatus(const char* message) {
    return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {message, std::strlen(message)}};
}

}  // namespace

void* MaydayAdapter::Impl::AttachedThread() {
    void* thread = il2cpp_thread.Get();
    if (thread == nullptr && thread_attach != nullptr) {
        thread = thread_attach(domain);
        if (thread != nullptr) il2cpp_thread.Set(thread);
    }
    return thread;
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::MoneyGetMoney(
    void* user, std::int32_t* out_money) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr || out_money == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveGame()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const instance = impl->GameInstance();
    if (instance == nullptr) return MoneyStatus("status.not_in_flight");
    std::int32_t money{};
    if (!impl->InvokeInt(impl->get_money, instance, &money)) {
        return MoneyStatus("status.failed");
    }
    *out_money = money;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::MoneySetMoney(
    void* user, std::int32_t value) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveGame()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const instance = impl->GameInstance();
    if (instance == nullptr) return MoneyStatus("status.not_in_flight");
    // il2cpp_runtime_invoke 的值类型参数要求传原始值地址，不能传装箱对象。
    void* params[] = {&value};
    if (!impl->InvokeVoid(impl->set_money, instance, params)) {
        return MoneyStatus("status.failed");
    }
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::MoneyAddMoney(
    void* user, std::int32_t delta) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveGame()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const instance = impl->GameInstance();
    if (instance == nullptr) return MoneyStatus("status.not_in_flight");
    void* text = impl->string_new("plugin.money.add");
    if (text == nullptr) return MoneyStatus("status.failed");
    // 值类型传原始值地址；string 是引用类型，直接传对象指针。
    void* params[] = {&delta, text};
    if (!impl->InvokeVoid(impl->add_money, instance, params)) {
        return MoneyStatus("status.failed");
    }
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::MoneyGetDebt(
    void* user, std::int32_t* out_debt) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr || out_debt == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveGame()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const instance = impl->GameInstance();
    if (instance == nullptr) return MoneyStatus("status.not_in_flight");
    std::int32_t debt{};
    if (!impl->InvokeInt(impl->get_debt, instance, &debt)) {
        return MoneyStatus("status.failed");
    }
    *out_debt = debt;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::MoneySetDebt(
    void* user, std::int32_t value) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveGame()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const instance = impl->GameInstance();
    if (instance == nullptr) return MoneyStatus("status.not_in_flight");
    // ServerAddDebt 是差额入口：当前债务 + delta = 目标值
    std::int32_t current{};
    if (!impl->InvokeInt(impl->get_debt, instance, &current)) {
        return MoneyStatus("status.failed");
    }
    std::int32_t delta = value - current;
    void* params[] = {&delta};
    if (!impl->InvokeVoid(impl->add_debt, instance, params)) {
        return MoneyStatus("status.failed");
    }
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

void MaydayAdapter::Impl::PublishMoneyService() {
    money_service = {
        sizeof(AnomalyMaydayMoneyServiceV1), kMoneyServiceVersion, this,
        MoneyGetMoney, MoneySetMoney, MoneyAddMoney, MoneyGetDebt, MoneySetDebt};
    if (ProcessAdapterServices().Publish(
            kMoneyServiceId, kMoneyServiceVersion, &money_service)) {
        money_published.store(&money_service, std::memory_order_release);
    }
}

bool MaydayAdapter::Impl::ResolveGame() {
    if (game_resolved) return true;
    if (domain_get == nullptr || class_from_name == nullptr) return false;
    domain = domain_get();
    if (domain == nullptr) return false;
    // 找 Assembly-CSharp image
    std::size_t count{};
    void** assemblies = domain_get_assemblies(domain, &count);
    if (assemblies == nullptr) return false;
    for (std::size_t i = 0; i < count; ++i) {
        void* const candidate_image = assembly_get_image(assemblies[i]);
        if (candidate_image == nullptr) continue;
        const char* const name = image_get_name(candidate_image);
        if (name != nullptr && std::strcmp(name, "Assembly-CSharp.dll") == 0) {
            image = candidate_image;
            break;
        }
    }
    if (image == nullptr) return false;
    game_state_class = class_from_name(image, "Mayday", "GameState");
    if (game_state_class == nullptr) return false;
    get_instance = class_get_method(game_state_class, "get_Instance", 0);
    get_money = class_get_method(game_state_class, "get_NetworkMoney", 0);
    set_money = class_get_method(game_state_class, "set_NetworkMoney", 1);
    add_money = class_get_method(game_state_class, "ServerAddMoney", 2);
    get_debt = class_get_method(game_state_class, "get_NetworkDebt", 0);
    add_debt = class_get_method(game_state_class, "ServerAddDebt", 1);
    if (get_instance == nullptr || get_money == nullptr || set_money == nullptr ||
        add_money == nullptr || get_debt == nullptr || add_debt == nullptr ||
        runtime_invoke == nullptr || thread_attach == nullptr || string_new == nullptr ||
        object_unbox == nullptr) {
        return false;
    }
    game_resolved = true;
    return true;
}

void* MaydayAdapter::Impl::GameInstance() {
    void* exception{};
    return runtime_invoke(get_instance, nullptr, nullptr, &exception);
}

bool MaydayAdapter::Impl::InvokeInt(
    void* method, void* object, std::int32_t* out_value) {
    if (method == nullptr || out_value == nullptr) return false;
    void* exception{};
    void* const boxed = runtime_invoke(method, object, nullptr, &exception);
    if (exception != nullptr || boxed == nullptr || object_unbox == nullptr) return false;
    *out_value = *static_cast<std::int32_t*>(object_unbox(boxed));
    return true;
}

bool MaydayAdapter::Impl::InvokeVoid(void* method, void* object, void** params) {
    if (method == nullptr) return false;
    void* exception{};
    runtime_invoke(method, object, params, &exception);
    return exception == nullptr;
}

void MaydayAdapter::Impl::RevokePublished() {
    void* const previous = table_published.exchange(nullptr, std::memory_order_acq_rel);
    if (previous != nullptr) {
        static_cast<void>(ProcessAdapterServices().Revoke(kIl2CppServiceId, previous));
    }
    void* const money = money_published.exchange(nullptr, std::memory_order_acq_rel);
    if (money != nullptr) {
        static_cast<void>(ProcessAdapterServices().Revoke(kMoneyServiceId, money));
    }
}

MaydayAdapter::MaydayAdapter() : impl_(std::make_unique<Impl>()) {
    impl_->table.struct_size = static_cast<std::uint32_t>(sizeof(Il2CppTableV1));
}

MaydayAdapter::~MaydayAdapter() = default;

bool MaydayAdapter::Start(std::stop_token stop_token) {
    if (impl_->started.exchange(true, std::memory_order_acq_rel)) return true;
    impl_->stop_token = std::move(stop_token);
    impl_->probe_worker = std::thread([impl = impl_.get()] { impl->ProbeWorker(); });
    return true;
}

bool MaydayAdapter::Stop(std::chrono::milliseconds timeout) noexcept {
    if (!impl_->started.exchange(false, std::memory_order_acq_rel)) return true;
    impl_->stopping.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (impl_->probe_worker.joinable() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        break;  // 轮询线程 500ms 一醒；析构兜底 join
    }
    impl_->RevokePublished();
    static_cast<void>(ClearTickCallback(std::chrono::seconds(2)));
    return true;
}

void MaydayAdapter::SetTickCallback(TickCallback callback) {
    std::scoped_lock lock(impl_->tick_mutex);
    impl_->tick_callback = std::move(callback);
}

bool MaydayAdapter::ClearTickCallback(std::chrono::seconds timeout) noexcept {
    {
        std::scoped_lock lock(impl_->tick_mutex);
        impl_->tick_callback = {};
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (impl_->tick_in_flight.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void MaydayAdapter::OnFrame(double delta_seconds) {
    TickCallback callback;
    {
        std::scoped_lock lock(impl_->tick_mutex);
        callback = impl_->tick_callback;
    }
    if (!callback) return;
    impl_->tick_in_flight.store(true, std::memory_order_release);
    try {
        callback(delta_seconds);
    } catch (...) {
        // 插件异常不得穿透渲染回调
    }
    impl_->tick_in_flight.store(false, std::memory_order_release);
}

std::optional<std::string> MaydayAdapter::AccountKey() const {
    return std::nullopt;
}

std::string MaydayAdapter::DiagnosticsJson() const {
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"game\":\"mayday\",\"engine\":\"unity-il2cpp\","
         << "\"gameAssemblyFound\":"
         << (impl_->game_assembly_found.load(std::memory_order_acquire) ? "true" : "false")
         << ",\"exportsResolved\":" << impl_->resolved_exports.load(std::memory_order_acquire)
         << ",\"tablePublished\":"
         << (impl_->table_published.load(std::memory_order_acquire) != nullptr ? "true" : "false")
         << ",\"moneyServicePublished\":"
         << (impl_->money_published.load(std::memory_order_acquire) != nullptr ? "true" : "false")
         << "}";
    return json.str();
}

// ---------------------------------------------------------------------------
// MaydayProfileRuntime
// ---------------------------------------------------------------------------
struct MaydayProfileRuntime::Impl {
    std::shared_ptr<MaydayAdapter> adapter;
};

MaydayProfileRuntime::MaydayProfileRuntime() : impl_(std::make_unique<Impl>()) {
    impl_->adapter = std::make_shared<MaydayAdapter>();
}

MaydayProfileRuntime::~MaydayProfileRuntime() = default;

bool MaydayProfileRuntime::Start(std::stop_token stop_token) {
    return impl_->adapter->Start(std::move(stop_token));
}

bool MaydayProfileRuntime::Stop(std::chrono::milliseconds timeout) noexcept {
    if (impl_->adapter != nullptr) {
        static_cast<void>(impl_->adapter->Stop(timeout));
    }
    return true;
}

std::shared_ptr<MaydayAdapter> MaydayProfileRuntime::Adapter() const {
    return impl_->adapter;
}

std::string MaydayProfileRuntime::DiagnosticsJson() const {
    if (impl_->adapter != nullptr) return impl_->adapter->DiagnosticsJson();
    return "{\"schemaVersion\":1,\"state\":\"not-started\"}";
}

}  // namespace anomaly
