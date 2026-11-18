#include "anomaly/mayday_adapter.hpp"
#include "anomaly/mayday_profile_runtime.hpp"
#include "anomaly/adapter_service_registry.hpp"
#include "anomaly/hook_manager.hpp"
#include "anomaly/host_ui_service.hpp"
#include "anomaly/sdk/services/mayday.h"
#include "anomaly/thread_local_value.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
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
constexpr const char* kFreecamServiceId = ANOMALY_MAYDAY_FREECAM_SERVICE_V1_ID;
constexpr std::uint32_t kIl2CppServiceVersion = 1;
constexpr std::uint32_t kMoneyServiceVersion = ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION;
constexpr std::uint32_t kFreecamServiceVersion = ANOMALY_MAYDAY_FREECAM_SERVICE_V1_VERSION;

// il2cpp 值类型的内存布局（UnityEngine.Vector2 / Vector3 / Quaternion）。
struct Vec2 {
    float x;
    float y;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Quat {
    float x;
    float y;
    float z;
    float w;
};

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
using FnClassGetMethods = void* (*)(void* klass, void** iterator);
using FnMethodGetName = const char* (*)(void* method);
using FnMethodGetParamCount = int (*)(void* method);
using FnClassGetField = void* (*)(void* klass, const char* name);
using FnClassGetParent = void* (*)(void* klass);
using FnFieldGetOffset = std::size_t (*)(void* field);
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
    FnClassGetParent class_get_parent{};
    FnClassGetMethods class_get_methods{};
    FnMethodGetName method_get_name{};
    FnMethodGetParamCount method_get_param_count{};
    FnClassGetField class_get_field{};
    FnFieldGetOffset field_get_offset{};
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

    // ---- 自由镜头：PlayerLook.Update 主线程 hook，渲染线程只提供输入 ----
    std::atomic<void*> freecam_published{nullptr};
    std::mutex freecam_state_mutex;
    std::condition_variable freecam_cv;
    std::mutex freecam_input_mutex;
    bool freecam_resolved{false};
    bool freecam_requested{false};
    bool freecam_applied{false};
    bool freecam_shutdown{false};
    bool hook_installed{false};
    std::atomic<int> freecam_in_callback{0};
    std::unique_ptr<HookBackend> hook_backend;
    void* player_rig_class{};
    void* player_look_class{};
    void* get_local_rig{};
    void* get_camera_rig{};
    void* get_view_camera{};
    void* camera_rig_field{};
    std::size_t camera_rig_offset{};
    void* view_camera_field{};
    std::size_t view_camera_offset{};
    void* camera_get_main{};
    void* component_get_transform{};
    void* get_look{};
    void* get_motor{};
    void* look_update_method{};
    void* look_update_target{};
    void* look_update_original{};
    void* transform_get_position{};
    void* transform_set_position{};
    void* transform_get_euler{};
    void* transform_set_euler{};
    void* transform_get_local_position{};
    void* transform_set_local_position{};
    void* transform_get_local_rotation{};
    void* transform_set_local_rotation{};
    void* behaviour_get_enabled{};
    void* behaviour_set_enabled{};
    void* fc_local_rig{};
    void* fc_camera_rig{};
    void* fc_look{};
    void* fc_motor{};
    bool fc_motor_enabled{};
    Vec3 fc_saved_local_position{};
    Quat fc_saved_local_rotation{};
    Vec3 fc_position{};
    float fc_yaw{};
    float fc_pitch{};
    bool fc_apply_started{false};
    const char* fc_error_key{};
    std::string fc_diag_message;
    std::chrono::steady_clock::time_point fc_last_apply{};
    float in_forward{};
    float in_right{};
    float in_up{};
    float in_speed{};
    float in_yaw{};
    float in_pitch{};
    AnomalyMaydayFreecamServiceV1 freecam_service{};
    static std::atomic<Impl*> freecam_owner;

    // ---- 覆盖层输入阻断 ----
    // 游戏走 Unity InputSystem：它从原始设备状态更新控件，不依赖 WndProc
    // 是否把消息转发给游戏窗口。因此只吞窗口消息挡不住点击与按键，必须把
    // ButtonControl 的状态读取在覆盖层展开期间固定为 false。
    bool input_block_resolved{};
    std::atomic<bool> input_hook_installed{false};
    bool input_hook_shutdown{};
    std::chrono::steady_clock::time_point input_hook_retry_after{};
    std::unique_ptr<HookBackend> input_hook_backend;
    void* is_pressed_target{};
    void* is_pressed_original{};
    void* was_pressed_target{};
    void* was_pressed_original{};
    void* was_released_target{};
    void* was_released_original{};
    // 滚轮与鼠标增量都是 DeltaControl，游戏可能读整个 Vector2，也可能读
    // 它的 AxisControl 分量；这两个泛型 ReadValue 也要在覆盖层期间归零。
    void* delta_control_class{};
    std::size_t input_control_parent_offset{};
    void* vector2_read_value_target{};
    void* vector2_read_value_original{};
    void* float_read_value_target{};
    void* float_read_value_original{};
    // UGUI 的 StandaloneInputModule 仍走旧版 Input.mouseScrollDelta。
    void* mouse_scroll_delta_target{};
    void* mouse_scroll_delta_original{};
    std::atomic<int> input_hook_in_callback{0};
    static std::atomic<Impl*> input_hook_owner;

    // 服务表回调：Impl 是 MaydayAdapter 的私有嵌套类型，只有成员才能访问。
    static AnomalyStatusV1 ANOMALY_CALL MoneyGetMoney(void* user, std::int32_t* out_money);
    static AnomalyStatusV1 ANOMALY_CALL MoneySetMoney(void* user, std::int32_t value);
    static AnomalyStatusV1 ANOMALY_CALL MoneyAddMoney(void* user, std::int32_t delta);
    static AnomalyStatusV1 ANOMALY_CALL MoneyGetDebt(void* user, std::int32_t* out_debt);
    static AnomalyStatusV1 ANOMALY_CALL MoneySetDebt(void* user, std::int32_t value);
    static AnomalyStatusV1 ANOMALY_CALL FreecamSetActive(void* user, std::int32_t active);
    static AnomalyStatusV1 ANOMALY_CALL FreecamGetState(void* user, std::int32_t* out_state);
    static AnomalyStatusV1 ANOMALY_CALL FreecamSetInput(
        void* user, float forward, float right, float up,
        float yaw_delta, float pitch_delta, float speed);

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
    void PublishFreecamService();
    void RevokePublished();

    // 解析 GameState 类与方法；每线程需先 attach。返回 false = 游戏符号不可用。
    bool ResolveGame();
    bool ResolveFreecam();
    // 拿 GameState.Instance（未开局返回 nullptr）
    void* GameInstance();
    bool InvokeInt(void* method, void* object, std::int32_t* out_value);
    bool InvokeVoid(void* method, void* object, void** params);
    bool InvokeVec3(void* method, void* object, Vec3* out_value);
    bool InvokeQuat(void* method, void* object, Quat* out_value);
    bool InvokeBool(void* method, void* object, bool* out_value);
    bool InvokeVoidArg(void* method, void* object, void* argument);

    static void ANOMALY_CALL PlayerLookUpdateThunk(void* self);
    void HandleLookUpdate(void* self);
    bool EnsureFreecamHook();
    bool ActivateFreecam(void* local, void* look);
    bool DeactivateFreecam();
    // 玩家对象已失效（换局/重生）时丢弃现场，禁止再触碰旧指针。
    void AbandonFreecam() noexcept;
    // 方法名失配时列出 PlayerRig 上关心的候选方法（诊断用）。
    std::string DescribeRigMethods();
    void ApplyFreecamInput();

    // InputSystem 的按钮状态 getter：覆盖层展开时强制返回 false。
    static bool ANOMALY_CALL IsPressedThunk(void* self, void* method);
    static bool ANOMALY_CALL WasPressedThunk(void* self, void* method);
    static bool ANOMALY_CALL WasReleasedThunk(void* self, void* method);
    // DeltaControl（滚轮/鼠标增量）及其 AxisControl 分量的 ReadValue。
    static Vec2 ANOMALY_CALL Vector2ReadValueThunk(void* self, void* method);
    static float ANOMALY_CALL FloatReadValueThunk(void* self, void* method);
    static Vec2 ANOMALY_CALL InputMouseScrollDeltaThunk(void* method);
    [[nodiscard]] bool IsDeltaControlValue(void* control) const noexcept;
    [[nodiscard]] void* FindMethodTarget(void* klass, const char* name, int args) const;
    bool ResolveInputBlock();
    bool EnsureInputBlockHook();
    void TeardownInputBlockHook() noexcept;
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
                PublishFreecamService();
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
        {reinterpret_cast<void**>(&class_get_parent), "il2cpp_class_get_parent"},
        {reinterpret_cast<void**>(&class_get_methods), "il2cpp_class_get_methods"},
        {reinterpret_cast<void**>(&method_get_name), "il2cpp_method_get_name"},
        {reinterpret_cast<void**>(&method_get_param_count), "il2cpp_method_get_param_count"},
        {reinterpret_cast<void**>(&class_get_field), "il2cpp_class_get_field_from_name"},
        {reinterpret_cast<void**>(&field_get_offset), "il2cpp_field_get_offset"},
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

void MaydayAdapter::Impl::PublishFreecamService() {
    freecam_service = {
        sizeof(AnomalyMaydayFreecamServiceV1), kFreecamServiceVersion, this,
        FreecamSetActive, FreecamGetState, FreecamSetInput};
    if (ProcessAdapterServices().Publish(
            kFreecamServiceId, kFreecamServiceVersion, &freecam_service)) {
        freecam_published.store(&freecam_service, std::memory_order_release);
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

bool MaydayAdapter::Impl::ResolveFreecam() {
    if (freecam_resolved) return true;
    const auto fail = [this](const char* key) {
        fc_error_key = key;
        return false;
    };
    if (!ResolveGame()) return fail("status.freecam_no_game_state");
    player_rig_class = class_from_name(image, "Mayday.Players", "PlayerRig");
    if (player_rig_class == nullptr) return fail("status.freecam_no_rig_class");
    player_look_class = class_from_name(image, "Mayday.Players", "PlayerLook");
    if (player_look_class == nullptr) return fail("status.freecam_no_look_class");
    get_local_rig = class_get_method(player_rig_class, "get_Local", 0);
    if (get_local_rig == nullptr) return fail("status.freecam_no_local_rig");
    get_camera_rig = class_get_method(player_rig_class, "get_CameraRig", 0);
    if (get_camera_rig == nullptr) {
        get_camera_rig = class_get_method(player_rig_class, "get_cameraRig", 0);
    }
    get_view_camera = class_get_method(player_rig_class, "get_ViewCamera", 0);
    if (get_view_camera == nullptr) {
        get_view_camera = class_get_method(player_rig_class, "get_viewCamera", 0);
    }
    if (class_get_field != nullptr && field_get_offset != nullptr) {
        camera_rig_field = class_get_field(player_rig_class, "cameraRig");
        if (camera_rig_field != nullptr) {
            camera_rig_offset = field_get_offset(camera_rig_field);
        }
        view_camera_field = class_get_field(player_rig_class, "viewCamera");
        if (view_camera_field != nullptr) {
            view_camera_offset = field_get_offset(view_camera_field);
        }
    }
    get_look = class_get_method(player_rig_class, "get_Look", 0);
    if (get_look == nullptr) get_look = class_get_method(player_rig_class, "get_look", 0);
    if (get_look == nullptr) {
        const std::string candidates = DescribeRigMethods();
        if (!candidates.empty()) {
            fc_diag_message = "rig: " + candidates;
            return fail(fc_diag_message.c_str());
        }
        return fail("status.freecam_no_look_getter");
    }
    get_motor = class_get_method(player_rig_class, "get_Motor", 0);
    if (get_motor == nullptr) get_motor = class_get_method(player_rig_class, "get_motor", 0);
    look_update_method = class_get_method(player_look_class, "Update", 0);
    if (look_update_method != nullptr) {
        look_update_target = *reinterpret_cast<void**>(look_update_method);
    }
    if (look_update_target == nullptr) return fail("status.freecam_no_look_update");
    if (get_motor == nullptr) {
        const std::string candidates = DescribeRigMethods();
        if (!candidates.empty()) {
            fc_diag_message = "rig: " + candidates;
            return fail(fc_diag_message.c_str());
        }
        return fail("status.freecam_no_motor_getter");
    }

    // UnityEngine.CoreModule 承载 Transform / Behaviour。
    std::size_t count{};
    void** const assemblies = domain_get_assemblies(domain, &count);
    void* core_image{};
    for (std::size_t i = 0; assemblies != nullptr && i < count; ++i) {
        void* const candidate = assembly_get_image(assemblies[i]);
        if (candidate == nullptr) continue;
        const char* const name = image_get_name(candidate);
        if (name != nullptr && std::strcmp(name, "UnityEngine.CoreModule.dll") == 0) {
            core_image = candidate;
            break;
        }
    }
    if (core_image == nullptr) return fail("status.freecam_no_core_image");
    void* const transform_class = class_from_name(core_image, "UnityEngine", "Transform");
    void* const behaviour_class = class_from_name(core_image, "UnityEngine", "Behaviour");
    void* const camera_class = class_from_name(core_image, "UnityEngine", "Camera");
    void* const component_class = class_from_name(core_image, "UnityEngine", "Component");
    if (transform_class == nullptr || behaviour_class == nullptr) {
        return fail("status.freecam_no_unity_classes");
    }
    if (camera_class != nullptr) {
        camera_get_main = class_get_method(camera_class, "get_main", 0);
    }
    if (component_class != nullptr) {
        component_get_transform = class_get_method(component_class, "get_transform", 0);
    }
    transform_get_position = class_get_method(transform_class, "get_position", 0);
    transform_set_position = class_get_method(transform_class, "set_position", 1);
    transform_get_euler = class_get_method(transform_class, "get_eulerAngles", 0);
    transform_set_euler = class_get_method(transform_class, "set_eulerAngles", 1);
    transform_get_local_position = class_get_method(transform_class, "get_localPosition", 0);
    transform_set_local_position = class_get_method(transform_class, "set_localPosition", 1);
    transform_get_local_rotation = class_get_method(transform_class, "get_localRotation", 0);
    transform_set_local_rotation = class_get_method(transform_class, "set_localRotation", 1);
    behaviour_get_enabled = class_get_method(behaviour_class, "get_enabled", 0);
    behaviour_set_enabled = class_get_method(behaviour_class, "set_enabled", 1);
    const bool camera_available = get_camera_rig != nullptr ||
        camera_rig_field != nullptr || view_camera_field != nullptr ||
        (get_view_camera != nullptr && component_get_transform != nullptr) ||
        (camera_get_main != nullptr && component_get_transform != nullptr);
    if (!camera_available) return fail("status.freecam_no_camera_rig");
    if (get_local_rig == nullptr || get_look == nullptr ||
        get_motor == nullptr || transform_get_position == nullptr ||
        transform_set_position == nullptr || transform_get_euler == nullptr ||
        transform_set_euler == nullptr || transform_get_local_position == nullptr ||
        transform_set_local_position == nullptr ||
        transform_get_local_rotation == nullptr ||
        transform_set_local_rotation == nullptr || behaviour_get_enabled == nullptr ||
        behaviour_set_enabled == nullptr) {
        return fail("status.freecam_no_unity_methods");
    }
    freecam_resolved = true;
    return true;
}

std::string MaydayAdapter::Impl::DescribeRigMethods() {
    std::string out;
    if (player_rig_class == nullptr || class_get_methods == nullptr ||
        method_get_name == nullptr) {
        return out;
    }
    void* iterator{};
    int total = 0;
    while (void* const method = class_get_methods(player_rig_class, &iterator)) {
        ++total;
        const char* const name = method_get_name(method);
        if (name == nullptr) continue;
        std::string lower(name);
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower.find("look") == std::string::npos &&
            lower.find("motor") == std::string::npos &&
            lower.find("cam") == std::string::npos &&
            lower.find("rig") == std::string::npos &&
            lower.find("local") == std::string::npos) {
            continue;
        }
        if (out.size() > 480) break;
        if (!out.empty()) out += ',';
        out += name;
        out += '/';
        out += std::to_string(method_get_param_count != nullptr
            ? method_get_param_count(method) : -1);
    }
    out += " n=";
    out += std::to_string(total);
    return out;
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

bool MaydayAdapter::Impl::InvokeVec3(void* method, void* object, Vec3* out_value) {
    if (method == nullptr || out_value == nullptr) return false;
    void* exception{};
    void* const boxed = runtime_invoke(method, object, nullptr, &exception);
    if (exception != nullptr || boxed == nullptr || object_unbox == nullptr) return false;
    *out_value = *static_cast<Vec3*>(object_unbox(boxed));
    return true;
}

bool MaydayAdapter::Impl::InvokeQuat(void* method, void* object, Quat* out_value) {
    if (method == nullptr || out_value == nullptr) return false;
    void* exception{};
    void* const boxed = runtime_invoke(method, object, nullptr, &exception);
    if (exception != nullptr || boxed == nullptr || object_unbox == nullptr) return false;
    *out_value = *static_cast<Quat*>(object_unbox(boxed));
    return true;
}

bool MaydayAdapter::Impl::InvokeBool(void* method, void* object, bool* out_value) {
    if (method == nullptr || out_value == nullptr) return false;
    void* exception{};
    void* const boxed = runtime_invoke(method, object, nullptr, &exception);
    if (exception != nullptr || boxed == nullptr || object_unbox == nullptr) return false;
    *out_value = *static_cast<std::uint8_t*>(object_unbox(boxed)) != 0;
    return true;
}

bool MaydayAdapter::Impl::InvokeVoidArg(void* method, void* object, void* argument) {
    if (method == nullptr) return false;
    void* exception{};
    void* params[] = {argument};
    runtime_invoke(method, object, params, &exception);
    return exception == nullptr;
}

std::atomic<MaydayAdapter::Impl*> MaydayAdapter::Impl::freecam_owner{nullptr};

void ANOMALY_CALL MaydayAdapter::Impl::PlayerLookUpdateThunk(void* self) {
    Impl* const impl = freecam_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->freecam_in_callback.fetch_add(1, std::memory_order_acq_rel);
    impl->HandleLookUpdate(self);
    impl->freecam_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

bool MaydayAdapter::Impl::EnsureFreecamHook() {
    if (hook_installed) return true;
    if (look_update_target == nullptr) {
        fc_error_key = "status.freecam_no_look_update";
        return false;
    }
    hook_backend = CreateMinHookBackend();
    if (hook_backend == nullptr || !hook_backend->Initialize()) {
        hook_backend.reset();
        fc_error_key = "status.freecam_hook_failed";
        return false;
    }
    if (!hook_backend->Create(
            look_update_target, reinterpret_cast<void*>(&Impl::PlayerLookUpdateThunk),
            &look_update_original)) {
        static_cast<void>(hook_backend->Uninitialize());
        hook_backend.reset();
        fc_error_key = "status.freecam_hook_failed";
        return false;
    }
    freecam_owner.store(this, std::memory_order_release);
    if (!hook_backend->Enable(look_update_target)) {
        static_cast<void>(hook_backend->Remove(look_update_target));
        freecam_owner.store(nullptr, std::memory_order_release);
        static_cast<void>(hook_backend->Uninitialize());
        hook_backend.reset();
        fc_error_key = "status.freecam_hook_failed";
        return false;
    }
    hook_installed = true;
    return true;
}

void MaydayAdapter::Impl::HandleLookUpdate(void* self) {
    auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(void*)>(look_update_original);
    std::unique_lock lock(freecam_state_mutex);
    if (freecam_shutdown) {
        lock.unlock();
        if (original != nullptr) original(self);
        return;
    }
    // hook 命中所有玩家的 PlayerLook；只接管本地玩家。
    void* exception{};
    void* const local = runtime_invoke(get_local_rig, nullptr, nullptr, &exception);
    void* const look = (exception == nullptr && local != nullptr)
        ? runtime_invoke(get_look, local, nullptr, &exception)
        : nullptr;
    if (exception != nullptr || local == nullptr || look == nullptr || self != look) {
        lock.unlock();
        if (original != nullptr) original(self);
        return;
    }
    if (freecam_applied && (local != fc_local_rig || look != fc_look)) {
        // 换局/重生：旧相机与组件已不可触碰。
        AbandonFreecam();
    }
    if (!freecam_requested) {
        if (freecam_applied) {
            lock.unlock();
            static_cast<void>(DeactivateFreecam());
            lock.lock();
            freecam_applied = false;
            freecam_cv.notify_all();
        }
        lock.unlock();
        if (original != nullptr) original(self);
        return;
    }
    if (!freecam_applied) {
        if (!ActivateFreecam(local, look)) {
            freecam_requested = false;
            freecam_cv.notify_all();
            lock.unlock();
            if (original != nullptr) original(self);
            return;
        }
        freecam_applied = true;
        freecam_cv.notify_all();
    }
    lock.unlock();
    ApplyFreecamInput();
    // 激活期间跳过原 Update：否则它会把相机拉回玩家头部。
}

bool MaydayAdapter::Impl::ActivateFreecam(void* local, void* look) {
    fc_error_key = nullptr;
    if (local == nullptr || look == nullptr) {
        fc_error_key = "status.not_in_flight";
        return false;
    }
    void* exception{};
    void* camera_rig{};
    if (camera_rig_field != nullptr && camera_rig_offset > 0 && camera_rig_offset < 0x1000) {
        camera_rig = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(local) + camera_rig_offset);
    }
    if (camera_rig == nullptr && view_camera_field != nullptr &&
        view_camera_offset > 0 && view_camera_offset < 0x1000 &&
        component_get_transform != nullptr) {
        void* const camera = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(local) + view_camera_offset);
        if (camera != nullptr) {
            camera_rig = runtime_invoke(component_get_transform, camera, nullptr, &exception);
            if (exception != nullptr) exception = nullptr;
        }
    }
    if (camera_rig == nullptr && get_camera_rig != nullptr) {
        camera_rig = runtime_invoke(get_camera_rig, local, nullptr, &exception);
        if (exception != nullptr) exception = nullptr;
    }
    if (camera_rig == nullptr && get_view_camera != nullptr &&
        component_get_transform != nullptr) {
        void* const camera = runtime_invoke(get_view_camera, local, nullptr, &exception);
        if (exception != nullptr) exception = nullptr;
        if (camera != nullptr) {
            camera_rig = runtime_invoke(component_get_transform, camera, nullptr, &exception);
            if (exception != nullptr) exception = nullptr;
        }
    }
    if (camera_rig == nullptr && camera_get_main != nullptr &&
        component_get_transform != nullptr) {
        void* const camera = runtime_invoke(camera_get_main, nullptr, nullptr, &exception);
        if (exception != nullptr) exception = nullptr;
        if (camera != nullptr) {
            camera_rig = runtime_invoke(component_get_transform, camera, nullptr, &exception);
            if (exception != nullptr) exception = nullptr;
        }
    }
    if (camera_rig == nullptr) {
        fc_error_key = "status.freecam_no_camera";
        return false;
    }

    Vec3 local_position{};
    Quat local_rotation{};
    Vec3 world_position{};
    Vec3 euler{};
    if (!InvokeVec3(transform_get_local_position, camera_rig, &local_position) ||
        !InvokeQuat(transform_get_local_rotation, camera_rig, &local_rotation) ||
        !InvokeVec3(transform_get_position, camera_rig, &world_position) ||
        !InvokeVec3(transform_get_euler, camera_rig, &euler)) {
        fc_error_key = "status.freecam_transform";
        return false;
    }

    // 只停玩家移动；PlayerLook 由 hook 跳过 Update，不需要改 enabled。
    void* motor = runtime_invoke(get_motor, local, nullptr, &exception);
    if (exception != nullptr) motor = nullptr;
    bool motor_enabled = false;
    if (motor != nullptr) {
        static_cast<void>(InvokeBool(behaviour_get_enabled, motor, &motor_enabled));
        bool disabled = false;
        if (!InvokeVoidArg(behaviour_set_enabled, motor, &disabled)) {
            fc_error_key = "status.freecam_components";
            return false;
        }
    }

    fc_local_rig = local;
    fc_camera_rig = camera_rig;
    fc_look = look;
    fc_motor = motor;
    fc_motor_enabled = motor_enabled;
    fc_saved_local_position = local_position;
    fc_saved_local_rotation = local_rotation;
    fc_position = world_position;
    fc_yaw = euler.y;
    fc_pitch = euler.x;
    fc_apply_started = false;
    return true;
}

bool MaydayAdapter::Impl::DeactivateFreecam() {
    if (!freecam_applied) return true;
    bool ok = true;
    if (fc_camera_rig != nullptr) {
        ok = InvokeVoidArg(transform_set_local_position, fc_camera_rig,
                           &fc_saved_local_position) && ok;
        ok = InvokeVoidArg(transform_set_local_rotation, fc_camera_rig,
                           &fc_saved_local_rotation) && ok;
    }
    if (fc_motor != nullptr) {
        ok = InvokeVoidArg(behaviour_set_enabled, fc_motor, &fc_motor_enabled) && ok;
    }
    AbandonFreecam();
    return ok;
}

void MaydayAdapter::Impl::AbandonFreecam() noexcept {
    fc_local_rig = nullptr;
    fc_camera_rig = nullptr;
    fc_look = nullptr;
    fc_motor = nullptr;
    fc_apply_started = false;
    freecam_applied = false;
}

void MaydayAdapter::Impl::ApplyFreecamInput() {
    if (!freecam_applied || fc_camera_rig == nullptr) return;
    float forward{};
    float right{};
    float up{};
    float yaw_delta{};
    float pitch_delta{};
    float speed{};
    {
        std::scoped_lock lock(freecam_input_mutex);
        forward = std::clamp(in_forward, -1.0f, 1.0f);
        right = std::clamp(in_right, -1.0f, 1.0f);
        up = std::clamp(in_up, -1.0f, 1.0f);
        yaw_delta = in_yaw;
        pitch_delta = in_pitch;
        in_yaw = 0.0f;
        in_pitch = 0.0f;
        speed = in_speed;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const auto now = std::chrono::steady_clock::now();
    double dt = 0.0;
    if (fc_apply_started) {
        dt = std::chrono::duration<double>(now - fc_last_apply).count();
        dt = std::clamp(dt, 0.0, 0.1);
    }
    fc_apply_started = true;
    fc_last_apply = now;

    fc_yaw += yaw_delta;
    if (fc_yaw > 180.0f) fc_yaw -= 360.0f;
    if (fc_yaw < -180.0f) fc_yaw += 360.0f;
    fc_pitch = std::clamp(fc_pitch + pitch_delta, -89.0f, 89.0f);

    const float yaw_rad = fc_yaw * kPi / 180.0f;
    const float pitch_rad = fc_pitch * kPi / 180.0f;
    const float cos_yaw = std::cos(yaw_rad);
    const float sin_yaw = std::sin(yaw_rad);
    const float cos_pitch = std::cos(pitch_rad);
    const float sin_pitch = std::sin(pitch_rad);
    Vec3 direction{
        forward * sin_yaw * cos_pitch + right * cos_yaw,
        -forward * sin_pitch + up,
        forward * cos_yaw * cos_pitch - right * sin_yaw};
    const float scale = (std::max)(speed, 0.0f) * static_cast<float>(dt);
    fc_position.x += direction.x * scale;
    fc_position.y += direction.y * scale;
    fc_position.z += direction.z * scale;

    Vec3 euler{fc_pitch, fc_yaw, 0.0f};
    if (dt > 0.0) {
        static_cast<void>(InvokeVoidArg(transform_set_position, fc_camera_rig, &fc_position));
    }
    static_cast<void>(InvokeVoidArg(transform_set_euler, fc_camera_rig, &euler));
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::FreecamSetActive(
    void* user, std::int32_t active) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (active != 0) {
        {
            std::scoped_lock lock(impl->resolve_mutex);
            if (!impl->ResolveFreecam()) {
                return MoneyStatus(impl->fc_error_key != nullptr
                    ? impl->fc_error_key : "status.freecam_symbols");
            }
            if (!impl->EnsureFreecamHook()) {
                return MoneyStatus(impl->fc_error_key != nullptr
                    ? impl->fc_error_key : "status.freecam_hook_failed");
            }
        }
        std::unique_lock lock(impl->freecam_state_mutex);
        if (impl->freecam_shutdown) return MoneyStatus("status.unavailable");
        if (impl->freecam_applied) return {ANOMALY_STATUS_V1_OK, 0, {}};
        impl->fc_error_key = nullptr;
        impl->freecam_requested = true;
        impl->freecam_cv.notify_all();
        impl->freecam_cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return impl->freecam_applied ||
                (!impl->freecam_requested && impl->fc_error_key != nullptr);
        });
        if (impl->freecam_applied) return {ANOMALY_STATUS_V1_OK, 0, {}};
        impl->freecam_requested = false;
        return MoneyStatus(impl->fc_error_key != nullptr
            ? impl->fc_error_key : "status.freecam_failed");
    }
    std::unique_lock lock(impl->freecam_state_mutex);
    if (!impl->freecam_requested && !impl->freecam_applied) {
        return {ANOMALY_STATUS_V1_OK, 0, {}};
    }
    impl->freecam_requested = false;
    impl->freecam_cv.notify_all();
    impl->freecam_cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
        return !impl->freecam_applied;
    });
    if (impl->freecam_applied) return MoneyStatus("status.freecam_failed");
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::FreecamGetState(
    void* user, std::int32_t* out_state) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr || out_state == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    std::scoped_lock lock(impl->freecam_state_mutex);
    *out_state = impl->freecam_applied ? 1 : 0;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::FreecamSetInput(
    void* user, float forward, float right, float up,
    float yaw_delta, float pitch_delta, float speed) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->freecam_input_mutex);
    impl->in_forward = forward;
    impl->in_right = right;
    impl->in_up = up;
    impl->in_yaw += yaw_delta;
    impl->in_pitch += pitch_delta;
    impl->in_speed = speed;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

// ---------------------------------------------------------------------------
// 覆盖层输入阻断：Unity InputSystem 的 ButtonControl 状态读取
// ---------------------------------------------------------------------------
std::atomic<MaydayAdapter::Impl*> MaydayAdapter::Impl::input_hook_owner{nullptr};

bool MaydayAdapter::Impl::ResolveInputBlock() {
    if (input_block_resolved) return true;
    if (domain_get == nullptr || domain_get_assemblies == nullptr ||
        assembly_get_image == nullptr || image_get_name == nullptr ||
        class_from_name == nullptr || class_get_method == nullptr) {
        return false;
    }
    if (domain == nullptr) {
        domain = domain_get();
        if (domain == nullptr) return false;
    }
    std::size_t count{};
    void** const assemblies = domain_get_assemblies(domain, &count);
    if (assemblies == nullptr) return false;
    void* input_image{};
    void* legacy_input_image{};
    for (std::size_t i = 0; i < count; ++i) {
        void* const candidate = assembly_get_image(assemblies[i]);
        if (candidate == nullptr) continue;
        const char* const name = image_get_name(candidate);
        if (name == nullptr) continue;
        if (std::strcmp(name, "Unity.InputSystem.dll") == 0 ||
            std::strcmp(name, "Unity.InputSystem") == 0) {
            input_image = candidate;
        } else if (std::strcmp(name, "UnityEngine.InputLegacyModule.dll") == 0 ||
                   std::strcmp(name, "UnityEngine.InputLegacyModule") == 0) {
            legacy_input_image = candidate;
        }
    }
    if (input_image == nullptr) return false;
    void* const button_class =
        class_from_name(input_image, "UnityEngine.InputSystem.Controls", "ButtonControl");
    if (button_class == nullptr) return false;
    const auto resolve_target = [this, button_class](const char* name) noexcept {
        void* const method = class_get_method(button_class, name, 0);
        return method != nullptr ? *reinterpret_cast<void**>(method) : nullptr;
    };
    is_pressed_target = resolve_target("get_isPressed");
    was_pressed_target = resolve_target("get_wasPressedThisFrame");
    was_released_target = resolve_target("get_wasReleasedThisFrame");
    if (is_pressed_target == nullptr || was_pressed_target == nullptr) return false;

    // DeltaControl 承载鼠标增量与滚轮。ReadValue 定义在泛型基类
    // InputControl<T> 上，因此按实际控件类型从继承链上取实例化后的入口。
    delta_control_class = class_from_name(
        input_image, "UnityEngine.InputSystem.Controls", "DeltaControl");
    void* const axis_control_class = class_from_name(
        input_image, "UnityEngine.InputSystem.Controls", "AxisControl");
    void* const input_control_class = class_from_name(
        input_image, "UnityEngine.InputSystem", "InputControl");
    if (class_get_field != nullptr && field_get_offset != nullptr &&
        input_control_class != nullptr) {
        void* const parent_field = class_get_field(input_control_class, "m_Parent");
        if (parent_field != nullptr) {
            input_control_parent_offset = field_get_offset(parent_field);
        }
    }
    if (delta_control_class != nullptr && input_control_parent_offset != 0) {
        vector2_read_value_target = FindMethodTarget(delta_control_class, "ReadValue", 0);
        float_read_value_target = FindMethodTarget(axis_control_class, "ReadValue", 0);
    }
    if (legacy_input_image != nullptr) {
        void* const legacy_input_class =
            class_from_name(legacy_input_image, "UnityEngine", "Input");
        mouse_scroll_delta_target =
            FindMethodTarget(legacy_input_class, "get_mouseScrollDelta", 0);
    }

    input_block_resolved = true;
    return true;
}

void* MaydayAdapter::Impl::FindMethodTarget(
    void* klass, const char* name, int args) const {
    for (int depth = 0; klass != nullptr && depth < 8; ++depth) {
        if (class_get_method != nullptr) {
            void* const method = class_get_method(klass, name, args);
            if (method != nullptr) {
                void* const target = *reinterpret_cast<void**>(method);
                if (target != nullptr) return target;
            }
        }
        klass = class_get_parent != nullptr ? class_get_parent(klass) : nullptr;
    }
    return nullptr;
}

bool MaydayAdapter::Impl::IsDeltaControlValue(void* control) const noexcept {
    if (control == nullptr || delta_control_class == nullptr ||
        input_control_parent_offset == 0) {
        return false;
    }
    // 控件对象头第一个指针就是 Il2CppClass；分量 AxisControl 需要沿
    // m_Parent 向上找到 DeltaControl。
    void* current = control;
    for (int depth = 0; depth < 4 && current != nullptr; ++depth) {
        if (*reinterpret_cast<void**>(current) == delta_control_class) return true;
        current = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(current) + input_control_parent_offset);
    }
    return false;
}

bool ANOMALY_CALL MaydayAdapter::Impl::IsPressedThunk(void* self, void* method) {
    Impl* const impl = input_hook_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return false;
    impl->input_hook_in_callback.fetch_add(1, std::memory_order_acq_rel);
    bool result = false;
    // 覆盖层展开时游戏读不到任何按下的按钮；收起后原样放行。
    if (!HostUiMenusCaptureMouse()) {
        auto* const original = reinterpret_cast<bool (ANOMALY_CALL*)(void*, void*)>(
            impl->is_pressed_original);
        if (original != nullptr) result = original(self, method);
    }
    impl->input_hook_in_callback.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

bool ANOMALY_CALL MaydayAdapter::Impl::WasPressedThunk(void* self, void* method) {
    Impl* const impl = input_hook_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return false;
    impl->input_hook_in_callback.fetch_add(1, std::memory_order_acq_rel);
    bool result = false;
    if (!HostUiMenusCaptureMouse()) {
        auto* const original = reinterpret_cast<bool (ANOMALY_CALL*)(void*, void*)>(
            impl->was_pressed_original);
        if (original != nullptr) result = original(self, method);
    }
    impl->input_hook_in_callback.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

bool ANOMALY_CALL MaydayAdapter::Impl::WasReleasedThunk(void* self, void* method) {
    Impl* const impl = input_hook_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return false;
    impl->input_hook_in_callback.fetch_add(1, std::memory_order_acq_rel);
    bool result = false;
    if (!HostUiMenusCaptureMouse()) {
        auto* const original = reinterpret_cast<bool (ANOMALY_CALL*)(void*, void*)>(
            impl->was_released_original);
        if (original != nullptr) result = original(self, method);
    }
    impl->input_hook_in_callback.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

Vec2 ANOMALY_CALL MaydayAdapter::Impl::Vector2ReadValueThunk(void* self, void* method) {
    Impl* const impl = input_hook_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return {};
    impl->input_hook_in_callback.fetch_add(1, std::memory_order_acq_rel);
    Vec2 result{};
    // 覆盖层展开时滚轮与鼠标增量都按零处理；其余 Vector2 控件不受影响。
    if (!HostUiMenusCaptureMouse() || !impl->IsDeltaControlValue(self)) {
        auto* const original = reinterpret_cast<Vec2 (ANOMALY_CALL*)(void*, void*)>(
            impl->vector2_read_value_original);
        if (original != nullptr) result = original(self, method);
    }
    impl->input_hook_in_callback.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

float ANOMALY_CALL MaydayAdapter::Impl::FloatReadValueThunk(void* self, void* method) {
    Impl* const impl = input_hook_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return 0.0F;
    impl->input_hook_in_callback.fetch_add(1, std::memory_order_acq_rel);
    float result = 0.0F;
    if (!HostUiMenusCaptureMouse() || !impl->IsDeltaControlValue(self)) {
        auto* const original = reinterpret_cast<float (ANOMALY_CALL*)(void*, void*)>(
            impl->float_read_value_original);
        if (original != nullptr) result = original(self, method);
    }
    impl->input_hook_in_callback.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

Vec2 ANOMALY_CALL MaydayAdapter::Impl::InputMouseScrollDeltaThunk(void* method) {
    Impl* const impl = input_hook_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return {};
    impl->input_hook_in_callback.fetch_add(1, std::memory_order_acq_rel);
    Vec2 result{};
    if (!HostUiMenusCaptureMouse()) {
        auto* const original = reinterpret_cast<Vec2 (ANOMALY_CALL*)(void*)>(
            impl->mouse_scroll_delta_original);
        if (original != nullptr) result = original(method);
    }
    impl->input_hook_in_callback.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

bool MaydayAdapter::Impl::EnsureInputBlockHook() {
    std::scoped_lock lock(resolve_mutex);
    if (input_hook_installed) return true;
    if (input_hook_shutdown) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now < input_hook_retry_after) return false;
    if (!ResolveInputBlock()) {
        // InputSystem 可能尚未加载，按固定节奏重试，避免逐帧遍历程序集。
        input_hook_retry_after = now + std::chrono::seconds(1);
        return false;
    }
    if (input_hook_backend == nullptr) {
        input_hook_backend = CreateMinHookBackend();
        if (input_hook_backend == nullptr || !input_hook_backend->Initialize()) {
            input_hook_backend.reset();
            input_hook_retry_after = now + std::chrono::seconds(1);
            return false;
        }
    }
    struct Entry final {
        void* target;
        void* detour;
        void** original;
        bool required;
    };
    // 按钮状态必须拦住；滚轮相关 hook 是补充能力，安装失败时不能把
    // 已经生效的按钮阻断一起拆掉。
    const Entry entries[] = {
        {is_pressed_target, reinterpret_cast<void*>(&Impl::IsPressedThunk),
         &is_pressed_original, true},
        {was_pressed_target, reinterpret_cast<void*>(&Impl::WasPressedThunk),
         &was_pressed_original, true},
        {was_released_target, reinterpret_cast<void*>(&Impl::WasReleasedThunk),
         &was_released_original, false},
        {vector2_read_value_target, reinterpret_cast<void*>(&Impl::Vector2ReadValueThunk),
         &vector2_read_value_original, false},
        {float_read_value_target, reinterpret_cast<void*>(&Impl::FloatReadValueThunk),
         &float_read_value_original, false},
        {mouse_scroll_delta_target, reinterpret_cast<void*>(&Impl::InputMouseScrollDeltaThunk),
         &mouse_scroll_delta_original, false},
    };
    input_hook_owner.store(this, std::memory_order_release);
    std::array<bool, std::size(entries)> created{};
    bool ok = true;
    for (std::size_t i = 0; i < std::size(entries); ++i) {
        if (entries[i].target == nullptr) {
            if (entries[i].required) {
                ok = false;
                break;
            }
            continue;
        }
        if (!input_hook_backend->Create(
                entries[i].target, entries[i].detour, entries[i].original)) {
            if (entries[i].required) {
                ok = false;
                break;
            }
            continue;
        }
        created[i] = true;
    }
    if (ok) {
        for (std::size_t i = 0; i < std::size(entries); ++i) {
            if (!created[i]) continue;
            if (!input_hook_backend->Enable(entries[i].target) && entries[i].required) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        for (std::size_t i = 0; i < std::size(entries); ++i) {
            if (created[i]) {
                static_cast<void>(input_hook_backend->Remove(entries[i].target));
            }
        }
        input_hook_owner.store(nullptr, std::memory_order_release);
        static_cast<void>(input_hook_backend->Uninitialize());
        input_hook_backend.reset();
        input_hook_retry_after = now + std::chrono::seconds(1);
        return false;
    }
    input_hook_installed.store(true, std::memory_order_release);
    return true;
}

void MaydayAdapter::Impl::TeardownInputBlockHook() noexcept {
    void* const targets[] = {
        is_pressed_target, was_pressed_target, was_released_target,
        vector2_read_value_target, float_read_value_target,
        mouse_scroll_delta_target};
    if (input_hook_backend != nullptr && input_hook_installed) {
        for (void* const target : targets) {
            if (target != nullptr) static_cast<void>(input_hook_backend->Disable(target));
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (input_hook_in_callback.load(std::memory_order_acquire) != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (void* const target : targets) {
            if (target != nullptr) static_cast<void>(input_hook_backend->Remove(target));
        }
    }
    input_hook_owner.store(nullptr, std::memory_order_release);
    if (input_hook_backend != nullptr) {
        static_cast<void>(input_hook_backend->Uninitialize());
        input_hook_backend.reset();
    }
    input_hook_installed.store(false, std::memory_order_release);
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
    void* const freecam = freecam_published.exchange(nullptr, std::memory_order_acq_rel);
    if (freecam != nullptr) {
        static_cast<void>(ProcessAdapterServices().Revoke(kFreecamServiceId, freecam));
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
    {
        // 输入阻断在渲染线程可能仍尝试安装；先与安装路径串行化再拆除，
        // 关闭后不得再重装。
        std::scoped_lock lock(impl_->resolve_mutex);
        impl_->input_hook_shutdown = true;
        impl_->TeardownInputBlockHook();
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (impl_->probe_worker.joinable() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        break;  // 轮询线程 500ms 一醒；析构兜底 join
    }
    {
        std::unique_lock lock(impl_->freecam_state_mutex);
        impl_->freecam_requested = false;
        impl_->freecam_cv.notify_all();
        if (impl_->freecam_applied) {
            impl_->freecam_cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
                return !impl_->freecam_applied;
            });
        }
        impl_->freecam_shutdown = true;
    }
    if (impl_->hook_installed && impl_->hook_backend != nullptr) {
        static_cast<void>(impl_->hook_backend->Disable(impl_->look_update_target));
        const auto drain_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (impl_->freecam_in_callback.load(std::memory_order_acquire) != 0 &&
               std::chrono::steady_clock::now() < drain_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        static_cast<void>(impl_->hook_backend->Remove(impl_->look_update_target));
        impl_->freecam_owner.store(nullptr, std::memory_order_release);
        static_cast<void>(impl_->hook_backend->Uninitialize());
        impl_->hook_backend.reset();
        impl_->hook_installed = false;
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
    // 覆盖层输入阻断只安装一次；hook 内部实时读取菜单状态，无需逐帧同步。
    static_cast<void>(impl_->EnsureInputBlockHook());
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
         << ",\"freecamServicePublished\":"
         << (impl_->freecam_published.load(std::memory_order_acquire) != nullptr ? "true" : "false")
         << ",\"inputBlockHooks\":"
         << (impl_->input_hook_installed.load(std::memory_order_acquire) ? "true" : "false")
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
