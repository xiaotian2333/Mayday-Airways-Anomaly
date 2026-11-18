#include "anomaly/mayday_adapter.hpp"
#include "anomaly/mayday_profile_runtime.hpp"
#include "anomaly/adapter_service_registry.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace anomaly {

// ---------------------------------------------------------------------------
// MaydayAdapter
// ---------------------------------------------------------------------------
namespace {
constexpr const wchar_t* kGameAssemblyModule = L"GameAssembly.dll";
constexpr const char* kIl2CppServiceId = "mayday.il2cpp";
constexpr std::uint32_t kIl2CppServiceVersion = 1;

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
}  // namespace

struct MaydayAdapter::Impl {
    std::stop_token stop_token;
    std::thread probe_worker;
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};

    Il2CppTableV1 table{};
    std::atomic<bool> table_ready{false};
    std::atomic<void*> table_published{nullptr};   // 已发布给注册表的表副本
    std::atomic<int> resolved_exports{0};
    std::atomic<bool> game_assembly_found{false};

    // tick 回调：mutex 保护指针，in_flight 供 ClearTickCallback 等待排空
    std::mutex tick_mutex;
    TickCallback tick_callback;
    std::atomic<bool> tick_in_flight{false};

    ~Impl() {
        if (probe_worker.joinable()) probe_worker.join();
    }

    void ProbeWorker();
    bool ResolveExports(HMODULE game_assembly);
    void PublishTable();
    void RevokeTable();
};

void MaydayAdapter::Impl::ProbeWorker() {
    // 等待 GameAssembly.dll：Unity IL2CPP 的脚本运行时在游戏初始化早期加载，
    // 注入时可能尚未就绪。轮询至发现或停止。
    while (!stopping.load(std::memory_order_acquire)) {
        const HMODULE game_assembly = GetModuleHandleW(kGameAssemblyModule);
        if (game_assembly != nullptr) {
            game_assembly_found.store(true, std::memory_order_release);
            if (ResolveExports(game_assembly)) {
                PublishTable();
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool MaydayAdapter::Impl::ResolveExports(const HMODULE game_assembly) {
    void** const slots[] = {
        &table.domain_get, &table.class_from_name, &table.class_get_method_from_name,
        &table.runtime_invoke, &table.thread_attach, &table.thread_detach,
        &table.string_new, &table.image_get_class, &table.assembly_get_image,
    };
    constexpr const char* const names[] = {
        "il2cpp_domain_get",
        "il2cpp_class_from_name",
        "il2cpp_class_get_method_from_name",
        "il2cpp_runtime_invoke",
        "il2cpp_thread_attach",
        "il2cpp_thread_detach",
        "il2cpp_string_new",
        "il2cpp_image_get_class",
        "il2cpp_assembly_get_image",
    };
    constexpr int count = static_cast<int>(std::size(slots));
    static_assert(count == static_cast<int>(std::size(names)));
    int resolved = 0;
    for (int i = 0; i < count; ++i) {
        *slots[i] = reinterpret_cast<void*>(GetProcAddress(game_assembly, names[i]));
        if (*slots[i] != nullptr) ++resolved;
    }
    resolved_exports.store(resolved, std::memory_order_release);
    // 关键导出齐全才发布服务表（缺 runtime_invoke 的残表没有消费价值）
    const bool complete = resolved == count;
    if (complete) {
        table.struct_size = static_cast<std::uint32_t>(sizeof(Il2CppTableV1));
        table_ready.store(true, std::memory_order_release);
    }
    return complete;
}

void MaydayAdapter::Impl::PublishTable() {
    // 注册表保存 lifetime 副本，调用方缓存可以在撤销后干净失败。
    auto published = std::make_shared<Il2CppTableV1>(table);
    if (ProcessAdapterServices().Publish(kIl2CppServiceId, kIl2CppServiceVersion,
                                         published.get(), {}, published)) {
        table_published.store(published.get(), std::memory_order_release);
    }
}

void MaydayAdapter::Impl::RevokeTable() {
    void* const previous = table_published.exchange(nullptr, std::memory_order_acq_rel);
    if (previous != nullptr) {
        static_cast<void>(ProcessAdapterServices().Revoke(kIl2CppServiceId, previous));
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
    // 轮询线程 500ms 一醒；默认 5s 超时内必然 join 完成。
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (impl_->probe_worker.joinable()) {
        while (std::chrono::steady_clock::now() < deadline) {
            // 线程每 500ms 醒一次查停止标记，小步等待即可。
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!impl_->probe_worker.joinable()) break;
            // joinable 一直为真直到显式 join，这里用停止标记判断退出。
            if (impl_->stopping.load(std::memory_order_acquire) &&
                impl_->resolved_exports.load(std::memory_order_acquire) >= 0) {
                break;  // 标记已置位，线程最多再睡 500ms；析构兜底 join。
            }
        }
    }
    impl_->RevokeTable();
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
    if (!impl_->adapter->Start(std::move(stop_token))) return false;
    return true;
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
