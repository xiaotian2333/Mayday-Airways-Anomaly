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
constexpr const char* kWeatherServiceId = ANOMALY_MAYDAY_WEATHER_SERVICE_V1_ID;
constexpr const char* kCombatServiceId = ANOMALY_MAYDAY_COMBAT_SERVICE_V1_ID;
constexpr std::uint32_t kIl2CppServiceVersion = 1;
constexpr std::uint32_t kMoneyServiceVersion = ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION;
constexpr std::uint32_t kFreecamServiceVersion = ANOMALY_MAYDAY_FREECAM_SERVICE_V1_VERSION;
constexpr std::uint32_t kWeatherServiceVersion = ANOMALY_MAYDAY_WEATHER_SERVICE_V1_VERSION;
constexpr std::uint32_t kCombatServiceVersion = ANOMALY_MAYDAY_COMBAT_SERVICE_V1_VERSION;

// 游戏数据：WeatherKind 枚举字段的声明顺序（与 global-metadata 一致）。
// 枚举数值不在此硬编码，运行时按字段名解析。
constexpr std::array<const char*, 6> kWeatherKindNames{
    "Calm", "Cloudy", "Rain", "Storm", "Fog", "Snow"};

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

// 自动棒球棍的目标快照：object 是队友 PlayerNet 对象，chest 是
// PlayerNet.ChestPosition 返回的命中点，distance_mm 用于 UI 与就近选择。
struct CombatTarget {
    void* object{};
    Vec3 chest{};
    std::int32_t distance_mm{-1};
    bool valid{};
};

// Mayday.InteractRef 的托管布局：uint netId + ushort index（4 字节对齐）。
struct CombatInteractRef {
    std::uint32_t net_id{};
    std::uint16_t index{};
    std::uint16_t padding{};
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
using FnFieldStaticGetValue = void (*)(void* field, void* value);
using FnRuntimeInvoke = void* (*)(void* method, void* obj, void** params, void** exc);
using FnThreadAttach = void* (*)(void* domain);
using FnStringNew = void* (*)(const char* text);
using FnObjectUnbox = void* (*)(void* obj);
using FnObjectGetClass = void* (*)(void* obj);
}  // namespace

struct MaydayAdapter::Impl {
    std::stop_token stop_token;
    std::thread probe_worker;
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};
    // 探测线程退出标志：Stop 在 RevokePublished 前等待它，确保“发现
    // GameAssembly → 发布服务”的整段流程结束后才回收服务表。
    std::atomic<bool> probe_done{false};

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
    FnFieldStaticGetValue field_static_get_value{};
    FnRuntimeInvoke runtime_invoke{};
    FnThreadAttach thread_attach{};
    FnStringNew string_new{};
    FnObjectUnbox object_unbox{};
    FnObjectGetClass object_get_class{};
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

    // ---- 气象/时间：WorldEnvironment / WorldManager / WeatherSystem 解析缓存 ----
    // 时间 = WorldEnvironment.Hours；锚定走官方 Anchor(hours, now, networked,
    // rate) 方法（rate=0 即冻结），速率用静态 WantedRate()。dev 系列属性
    // getter 被 IL2CPP 裁剪，覆盖状态不可读，设置走 DevOverrideWeather /
    // DevClearOverride（存在）。
    LogSink log_sink;
    int weather_logged_step{0};        // 同一失败步骤的日志只打一次（resolve_mutex 保护）
    std::atomic<void*> weather_published{nullptr};
    // 诊断 JSON 在 resolve_mutex 外无锁读取，改用 atomic 消除良性数据竞争。
    std::atomic<bool> weather_resolved{false};
    bool weather_time_locked{false};   // 宿主记录的锁定意图（换局后可能失效）
    // dev 系列属性 getter 被 IL2CPP 裁剪，覆盖状态不可读：由宿主记录设置
    // 意图供插件 UI 回显（换局后可能失效）。
    bool weather_override_active{false};
    std::int32_t weather_override_kind_value{-1};
    float weather_override_severity{0.0f};
    // 解析失败时的步骤号（诊断 JSON weatherResolveStep）：1 前置 2 环境 3 时钟
    // 4 世界管理 5 天气系统 6 枚举 7 句柄清点；成功后置 8。
    std::atomic<int> weather_resolve_step{0};
    void* world_environment_class{};
    void* world_manager_class{};
    void* weather_system_class{};
    void* weather_kind_class{};
    void* env_get_instance{};
    void* env_get_hours{};
    void* env_set_hours{};
    void* env_get_abs_now{};
    void* env_anchor_method{};
    void* env_wanted_rate{};
    void* wm_get_instance{};
    void* wm_dev_override{};
    void* wm_dev_clear{};
    void* ws_get_instance{};
    void* ws_get_kind{};
    void* ws_get_severity{};
    std::array<std::int32_t, 6> kind_values{};   // 声明序 → 运行时枚举值
    AnomalyMaydayWeatherServiceV1 weather_service{};

    // ---- 自动棒球棍：PlayerInteractor.Update 主线程触发 + BatItem.ServerUseOnce 静默瞄准 ----
    // 渲染线程只读写 atomic 开关与快照；所有 il2cpp/Unity 调用都发生在
    // 游戏主线程的钩子里，避免在 Present 回调里触碰 Transform 或 Mirror。
    std::atomic<void*> combat_published{nullptr};
    AnomalyMaydayCombatServiceV1 combat_service{};
    std::atomic<bool> combat_enabled{false};
    std::atomic<std::int32_t> combat_max_range_mm{0};
    std::atomic<std::int32_t> combat_interval_ms{0};
    std::atomic<std::int32_t> combat_crew_cone_deg{0};
    std::atomic<bool> combat_in_flight{false};
    std::atomic<bool> combat_holding_bat{false};
    std::atomic<bool> combat_has_target{false};
    std::atomic<std::int32_t> combat_target_distance_mm{-1};
    std::atomic<std::int32_t> combat_host_mode{0};
    std::atomic<std::int32_t> combat_crew_count{0};
    std::atomic<bool> combat_gyro_enabled{false};
    std::atomic<bool> gyro_initialized{false};
    std::chrono::steady_clock::time_point gyro_last{};
    float gyro_base_yaw{};
    float gyro_spin_degrees{};
    std::atomic<std::int32_t> combat_last_swing_result{0};
    std::atomic<std::int32_t> combat_last_result{0};
    std::atomic<std::uint32_t> combat_attacks_sent{0};
    bool combat_mode_logged{false};
    std::int32_t combat_logged_host_mode{-1};
    std::chrono::steady_clock::time_point combat_last_result_log{};
    std::chrono::steady_clock::time_point combat_last_send_log{};
    std::chrono::steady_clock::time_point gyro_net_last{};
    float gyro_net_spin_degrees{};
    bool combat_resolved{false};
    bool combat_hooks_installed{false};
    std::atomic<bool> combat_shutdown{false};
    bool combat_resolve_logged{false};
    std::atomic<int> combat_in_callback{0};
    std::unique_ptr<HookBackend> combat_hook_backend;
    std::chrono::steady_clock::time_point combat_last_attack{};
    CombatTarget combat_target{};   // 主线程专属：最近一次扫描结果
    void* combat_pending_aim_target{};  // 主线程专属：本次 CmdUse 要静默瞄准的队友
    static std::atomic<Impl*> combat_owner;
    void* player_net_class{};
    void* player_inventory_class{};
    void* bat_item_class{};
    void* player_avatar_class{};
    void* interaction_registry_class{};
    void* player_interactor_class{};
    void* network_server_class{};
    void* get_local{};
    void* get_avatar{};
    void* get_avatar_rig{};
    void* get_net_transform{};
    void* get_inventory{};
    void* get_held{};
    void* get_interactor{};
    void* get_eye_position{};
    void* get_aim_direction{};
    void* get_chest_position{};
    void* get_is_seated{};
    void* get_is_stunned{};
    void* get_network_is_dead{};
    void* get_network_server_active{};
    void* player_all_field{};
    void* bat_server_use_once{};
    void* bat_rpc_swing_user_code{};
    void* bat_get_holder{};
    void* interact_ref_of{};
    void* interactor_update{};
    void* interactor_cmd_use{};
    void* player_view_field{};
    std::size_t player_view_offset{};
    void* combat_update_target{};
    void* combat_update_original{};
    void* combat_bat_target{};
    void* combat_bat_original{};
    void* combat_swing_target{};
    void* combat_swing_original{};
    bool combat_swing_hook_created{false};
    void* network_transform_class{};
    void* network_transform_base_class{};
    void* net_broadcast_method{};
    void* net_broadcast_target{};
    void* net_broadcast_original{};
    void* net_transform_target_field{};
    std::size_t net_transform_target_offset{};
    bool net_broadcast_hook_created{false};
    // BatItem 的挥击结果字段（Prefab 序列化值，运行时按字段名读取）。
    void* swing_whiff_field{};
    std::size_t swing_whiff_offset{};
    void* swing_pax_field{};
    std::size_t swing_pax_offset{};
    void* swing_crew_field{};
    std::size_t swing_crew_offset{};
    void* swing_whoosh_field{};
    std::size_t swing_whoosh_offset{};
    // 房主模式下用于重置服务端挥击冷却（远端房间无法修改房主对象）。
    void* next_swing_field{};
    std::size_t next_swing_offset{};
    // 队友列表方法在首次枚举时按实例类型解析（主线程专属，无锁）。
    void* combat_list_class{};
    void* combat_list_count{};
    void* combat_list_item{};
    // 静默瞄准复用自由镜头已解析的 Transform 世界欧拉角方法。
    // transform_get_euler / transform_set_euler 字段见上方自由镜头缓存。

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
    // UGUI 的点击由 EventSystem 驱动，绕过 InputControl 的设备状态；
    // 覆盖层展开时跳过 EventSystem.Update 才能挡住游戏弹窗按钮。
    void* event_system_update_target{};
    void* event_system_update_original{};
    void* event_system_module_field{};
    std::size_t event_system_module_offset{};
    void* input_system_ui_module_class{};
    void* input_module_reset_pointers{};
    // 覆盖层关闭后的残余点击抑制：关闭动作本身可能在同一/下一帧才被
    // UGUI 读到，必须等鼠标全部松开并再多压一小段时间。
    std::atomic<bool> event_system_was_blocked{false};
    std::atomic<unsigned long long> event_system_resume_after{0};
    std::atomic<int> event_system_module_kind{0};
    std::atomic<int> input_hook_in_callback{0};
    static std::atomic<Impl*> input_hook_owner;

    // ---- 暂停菜单入口：按钮点击等价于按下 Insert ----
    bool pause_menu_resolved{};
    bool pause_menu_hooks_installed{};
    bool pause_menu_hook_shutdown{};
    std::chrono::steady_clock::time_point pause_menu_retry_after{};
    std::unique_ptr<HookBackend> pause_menu_hook_backend;
    void* pause_screen_class{};
    void* pause_build_target{};
    void* pause_build_original{};
    void* pause_open_target{};
    void* pause_open_original{};
    void* button_click_target{};
    void* button_click_original{};
    void* button_submit_target{};
    void* button_submit_original{};
    void* pause_get_invite_btn{};
    void* pause_get_resume_btn{};
    void* invite_btn_field{};
    std::size_t invite_btn_offset{};
    void* resume_btn_field{};
    std::size_t resume_btn_offset{};
    void* uif_button_method{};
    void* ui_theme_button_height{};
    void* transform_get_parent{};
    void* transform_get_sibling_index{};
    void* transform_set_sibling_index{};
    void* transform_get_child_count{};
    void* component_get_gameobject{};
    void* gameobject_get_active{};
    void* transform_get_local_scale{};
    void* recttransform_get_size_delta{};
    std::atomic<int> pause_button_active{-1};
    std::atomic<int> pause_button_width_milli{0};
    std::atomic<int> pause_button_height_milli{0};
    std::atomic<int> pause_parent_children{-1};
    void* pause_screen{};
    std::atomic<void*> pause_button{nullptr};
    std::atomic<int> pause_menu_in_callback{0};
    std::atomic<int> pause_stage{0};
    std::atomic<int> pause_failure{0};
    std::atomic<int> pause_resolve_step{0};
    std::atomic<int> pause_anchor{0};
    static std::atomic<Impl*> pause_menu_owner;

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
    static AnomalyStatusV1 ANOMALY_CALL WeatherGetState(
        void* user, AnomalyMaydayWeatherStateV1* out_state);
    static AnomalyStatusV1 ANOMALY_CALL WeatherSetTime(void* user, float hours);
    static AnomalyStatusV1 ANOMALY_CALL WeatherSetTimeLocked(void* user, std::int32_t locked);
    static AnomalyStatusV1 ANOMALY_CALL WeatherSetWeatherOverride(
        void* user, std::int32_t active, std::int32_t kind_index, float severity);
    static AnomalyStatusV1 ANOMALY_CALL WeatherGetKindValue(
        void* user, std::int32_t kind_index, std::int32_t* out_value);
    static AnomalyStatusV1 ANOMALY_CALL CombatSetOptions(
        void* user, std::int32_t max_range_mm, std::int32_t attack_interval_ms,
        std::int32_t crew_cone_deg);
    static AnomalyStatusV1 ANOMALY_CALL CombatSetEnabled(void* user, std::int32_t enabled);
    static AnomalyStatusV1 ANOMALY_CALL CombatSetGyroEnabled(
        void* user, std::int32_t enabled);
    static AnomalyStatusV1 ANOMALY_CALL CombatGetState(
        void* user, AnomalyMaydayCombatStateV1* out_state);

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
    void PublishWeatherService();
    void PublishCombatService();
    void RevokePublished();

    // 解析 GameState 类与方法；每线程需先 attach。返回 false = 游戏符号不可用。
    bool ResolveGame();
    bool ResolveFreecam();
    // 解析 WorldEnvironment / WorldManager / WeatherSystem 与 WeatherKind 枚举值。
    bool ResolveWeather();
    // 解析自动棒球棍所需的 PlayerNet / BatItem / Passenger / 交互与网络入口。
    bool ResolveCombat();
    // 拿 GameState.Instance（未开局返回 nullptr）
    void* GameInstance();
    // 拿静态单例实例（WorldEnvironment / WorldManager / WeatherSystem）。
    void* StaticInstance(void* get_instance_method);
    // 官方锚定入口 Anchor(hours, now, networked, rate)：重锚定到目标时刻，
    // 锁定期 rate=0 冻结时钟，否则用 WantedRate() 的游戏期望速率。
    bool AnchorEnvironment(void* env, float hours);
    bool InvokeInt(void* method, void* object, std::int32_t* out_value);
    bool InvokeFloat(void* method, void* object, float* out_value);
    bool InvokeDouble(void* method, void* object, double* out_value);
    bool InvokeVoid(void* method, void* object, void** params);
    bool InvokeVec3(void* method, void* object, Vec3* out_value);
    bool InvokeQuat(void* method, void* object, Quat* out_value);
    bool InvokeBool(void* method, void* object, bool* out_value);
    bool InvokeVoidArg(void* method, void* object, void* argument);
    void* InvokeObject(void* method, void* object);
    [[nodiscard]] bool ClassDerivesFrom(void* klass, void* base) const;

    // 自动棒球棍的主线程钩子与目标选择。
    static void ANOMALY_CALL InteractorUpdateThunk(void* self);
    static void ANOMALY_CALL BatUseOnceThunk(
        void* self, void* player, void* target, const Vec3* hit_point);
    static void ANOMALY_CALL BatRpcSwingThunk(void* self, std::uint8_t result);
    static void ANOMALY_CALL NetTransformBroadcastThunk(void* self);
    void HandleInteractorUpdate(void* self);
    void HandleBatUseOnce(void* self, void* player, void* target, const Vec3* hit_point);
    void HandleBatRpcSwing(void* self, std::uint8_t result);
    void HandleNetTransformBroadcast(void* self);
    void UpdateCombat(void* self);
    bool FindNearestCrewmate(void* local, CombatTarget* out_target);
    bool BuildInteractRef(void* target, CombatInteractRef* out_ref);
    bool EnsureCombatHooks();
    void TeardownCombatHooks() noexcept;

    static void ANOMALY_CALL PlayerLookUpdateThunk(void* self);
    void HandleLookUpdate(void* self);
    bool EnsureFreecamHook();
    void* FindCameraRig(void* local);
    void ApplyGyro();
    void RestoreGyro();
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
    static void ANOMALY_CALL EventSystemUpdateThunk(void* self, void* method);
    [[nodiscard]] bool IsDeltaControlValue(void* control) const noexcept;
    [[nodiscard]] void* FindMethodInfo(void* klass, const char* name, int args) const;
    [[nodiscard]] void* FindMethodTarget(void* klass, const char* name, int args) const;
    bool ResolveInputBlock();
    bool EnsureInputBlockHook();
    void TeardownInputBlockHook() noexcept;

    // 暂停菜单入口
    bool EnsureGameImage();
    bool ResolvePauseMenu();
    bool EnsurePauseMenuHook();
    void TeardownPauseMenuHook() noexcept;
    void InstallPauseMenuButton(void* screen);
    static void ANOMALY_CALL PauseBuildThunk(void* self, void* method);
    static void ANOMALY_CALL PauseOpenThunk(void* self, void* method);
    static void ANOMALY_CALL ButtonClickThunk(void* self, void* event_data, void* method);
    static void ANOMALY_CALL ButtonSubmitThunk(void* self, void* event_data, void* method);
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
                PublishWeatherService();
                PublishCombatService();
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    // 退出前置位停止等待标志（含发布完成与停止中止两条退出路径）。
    probe_done.store(true, std::memory_order_release);
    // 首次发布之后转为低频维护线程：输入阻断与暂停菜单 Hook 的 il2cpp 解析、
    // MinHook 安装和失败重试都留在本线程。Present 回调只驱动插件 tick，不再
    // 遍历程序集；解析失败时 Ensure* 自己按 1 秒节奏重试。
    while (!stopping.load(std::memory_order_acquire)) {
        static_cast<void>(EnsureInputBlockHook());
        static_cast<void>(EnsurePauseMenuHook());
        for (int slice = 0;
             slice < 10 && !stopping.load(std::memory_order_acquire); ++slice) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
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
        {reinterpret_cast<void**>(&field_static_get_value), "il2cpp_field_static_get_value"},
        {reinterpret_cast<void**>(&runtime_invoke), "il2cpp_runtime_invoke"},
        {reinterpret_cast<void**>(&thread_attach), "il2cpp_thread_attach"},
        {reinterpret_cast<void**>(&string_new), "il2cpp_string_new"},
        {reinterpret_cast<void**>(&object_unbox), "il2cpp_object_unbox"},
        {reinterpret_cast<void**>(&object_get_class), "il2cpp_object_get_class"},
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

AnomalyStatusV1 CombatStatus(const char* message) {
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

// ---- 气象/时间服务实现（SDK 表回调） ----

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::WeatherGetState(
    void* user, AnomalyMaydayWeatherStateV1* out_state) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr || out_state == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveWeather()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    AnomalyMaydayWeatherStateV1 state{};
    state.struct_size = sizeof(state);
    state.kind_value = -1;
    state.severity = -1.0f;
    state.override_kind_value = -1;
    state.override_severity = -1.0f;
    void* const env = impl->StaticInstance(impl->env_get_instance);
    void* const wm = impl->StaticInstance(impl->wm_get_instance);
    void* const ws = impl->StaticInstance(impl->ws_get_instance);
    if (env == nullptr || wm == nullptr || ws == nullptr) {
        return MoneyStatus("status.not_in_flight");
    }
    if (!impl->InvokeFloat(impl->env_get_hours, env, &state.hours)) {
        return MoneyStatus("status.failed");
    }
    // dev 系列属性 getter 被 IL2CPP 裁剪，覆盖状态不可读：由宿主锁定意图
    // 记录（weather_time_locked / override 意图）驱动显示，仅作参考。
    state.time_locked = impl->weather_time_locked ? 1 : 0;
    state.rate = impl->weather_time_locked
        ? 0.0f
        : [&] {
              float wanted = 0.0f;
              return impl->InvokeFloat(impl->env_wanted_rate, nullptr, &wanted)
                  ? wanted
                  : -1.0f;
          }();
    state.override_active = impl->weather_override_active ? 1 : 0;
    if (impl->weather_override_active) {
        state.override_kind_value = impl->weather_override_kind_value;
        state.override_severity = impl->weather_override_severity;
    }
    std::int32_t kind{};
    if (impl->InvokeInt(impl->ws_get_kind, ws, &kind)) {
        state.kind_value = kind;
    }
    float severity{};
    if (impl->InvokeFloat(impl->ws_get_severity, ws, &severity)) {
        state.severity = severity;
    }
    *out_state = state;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::WeatherSetTime(
    void* user, float hours) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (!(hours >= 0.0f && hours < 24.0f)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveWeather()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const env = impl->StaticInstance(impl->env_get_instance);
    if (env == nullptr) return MoneyStatus("status.not_in_flight");
    return impl->AnchorEnvironment(env, hours)
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_OK, 0, {}}
        : MoneyStatus("status.failed");
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::WeatherSetTimeLocked(
    void* user, std::int32_t locked) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveWeather()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const env = impl->StaticInstance(impl->env_get_instance);
    if (env == nullptr) return MoneyStatus("status.not_in_flight");
    impl->weather_time_locked = locked != 0;
    float current_hours{};
    if (!impl->InvokeFloat(impl->env_get_hours, env, &current_hours)) {
        return MoneyStatus("status.failed");
    }
    // Anchor 一次即持久生效（rate=0 冻结时钟）；解锁恢复 WantedRate。
    return impl->AnchorEnvironment(env, current_hours)
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_OK, 0, {}}
        : MoneyStatus("status.failed");
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::WeatherSetWeatherOverride(
    void* user, std::int32_t active, std::int32_t kind_index, float severity) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveWeather()) return MoneyStatus("status.unavailable");
    if (impl->AttachedThread() == nullptr) return MoneyStatus("status.attach_failed");
    void* const wm = impl->StaticInstance(impl->wm_get_instance);
    if (wm == nullptr) return MoneyStatus("status.not_in_flight");
    if (active == 0) {
        if (!impl->InvokeVoid(impl->wm_dev_clear, wm, nullptr)) {
            return MoneyStatus("status.failed");
        }
        impl->weather_override_active = false;
        impl->weather_override_kind_value = -1;
        return {ANOMALY_STATUS_V1_OK, 0, {}};
    }
    if (kind_index < 0 ||
        kind_index >= static_cast<std::int32_t>(std::size(impl->kind_values)) ||
        !(severity >= 0.0f && severity <= 1.0f)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    std::int32_t kind_value = impl->kind_values[kind_index];
    std::uint8_t here = 1;
    // DevOverrideWeather(WeatherKind kind, Single sev, Boolean here)：
    // 值类型参数传原始值地址（bool 为 1 字节）。
    void* params[] = {&kind_value, &severity, &here};
    if (!impl->InvokeVoid(impl->wm_dev_override, wm, params)) {
        return MoneyStatus("status.failed");
    }
    // dev getter 被裁剪，覆盖意图由宿主记录供 get_state 回显。
    impl->weather_override_active = true;
    impl->weather_override_kind_value = kind_value;
    impl->weather_override_severity = severity;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::WeatherGetKindValue(
    void* user, std::int32_t kind_index, std::int32_t* out_value) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr || out_value == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    std::scoped_lock lock(impl->resolve_mutex);
    if (!impl->ResolveWeather()) return MoneyStatus("status.unavailable");
    if (kind_index < 0 ||
        kind_index >= static_cast<std::int32_t>(std::size(impl->kind_values))) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *out_value = impl->kind_values[kind_index];
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

// ---- 自动棒球棍服务实现 ----
AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::CombatSetOptions(
    void* user, std::int32_t max_range_mm, std::int32_t attack_interval_ms,
    std::int32_t crew_cone_deg) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr || max_range_mm < 100 || max_range_mm > 10000 ||
        attack_interval_ms < 10 || attack_interval_ms > 1000 ||
        crew_cone_deg < 1 || crew_cone_deg > 180) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    impl->combat_max_range_mm.store(max_range_mm, std::memory_order_release);
    impl->combat_interval_ms.store(attack_interval_ms, std::memory_order_release);
    impl->combat_crew_cone_deg.store(crew_cone_deg, std::memory_order_release);
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::CombatSetEnabled(
    void* user, std::int32_t enabled) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (enabled != 0) {
        if (!impl->ResolveCombat()) return CombatStatus("status.combat_no_symbols");
        if (impl->combat_max_range_mm.load(std::memory_order_acquire) <= 0 ||
            impl->combat_interval_ms.load(std::memory_order_acquire) <= 0 ||
            impl->combat_crew_cone_deg.load(std::memory_order_acquire) <= 0) {
            return CombatStatus("status.combat_no_options");
        }
        if (!impl->EnsureCombatHooks()) {
            return CombatStatus("status.combat_hook_failed");
        }
    }
    impl->combat_enabled.store(enabled != 0, std::memory_order_release);
    if (enabled == 0) {
        impl->combat_has_target.store(false, std::memory_order_release);
        impl->combat_target_distance_mm.store(-1, std::memory_order_release);
    }
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::CombatSetGyroEnabled(
    void* user, std::int32_t enabled) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    std::scoped_lock lock(impl->resolve_mutex);
    if (enabled != 0) {
        if (impl->combat_shutdown.load(std::memory_order_acquire)) {
            return CombatStatus("status.failed");
        }
        if (!impl->ResolveCombat()) {
            return CombatStatus("status.combat_no_symbols");
        }
        if (impl->get_local_rig == nullptr || impl->get_avatar == nullptr ||
            impl->get_avatar_rig == nullptr) {
            return CombatStatus("status.combat_gyro_unavailable");
        }
        if (!impl->EnsureCombatHooks()) {
            return CombatStatus("status.combat_gyro_hook_failed");
        }
        impl->gyro_initialized.store(false, std::memory_order_release);
        impl->combat_gyro_enabled.store(true, std::memory_order_release);
    } else {
        impl->combat_gyro_enabled.store(false, std::memory_order_release);
    }
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL MaydayAdapter::Impl::CombatGetState(
    void* user, AnomalyMaydayCombatStateV1* out_state) {
    auto* const impl = static_cast<Impl*>(user);
    if (impl == nullptr || out_state == nullptr ||
        out_state->struct_size < sizeof(*out_state)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    out_state->enabled = impl->combat_enabled.load(std::memory_order_acquire) ? 1 : 0;
    out_state->in_flight =
        impl->combat_in_flight.load(std::memory_order_acquire) ? 1 : 0;
    out_state->holding_bat =
        impl->combat_holding_bat.load(std::memory_order_acquire) ? 1 : 0;
    out_state->has_target =
        impl->combat_has_target.load(std::memory_order_acquire) ? 1 : 0;
    out_state->target_distance_mm =
        impl->combat_target_distance_mm.load(std::memory_order_acquire);
    out_state->max_range_mm =
        impl->combat_max_range_mm.load(std::memory_order_acquire);
    out_state->attack_interval_ms =
        impl->combat_interval_ms.load(std::memory_order_acquire);
    out_state->host_mode =
        impl->combat_host_mode.load(std::memory_order_acquire);
    out_state->gyro_enabled =
        impl->combat_gyro_enabled.load(std::memory_order_acquire) ? 1 : 0;
    out_state->crew_count =
        impl->combat_crew_count.load(std::memory_order_acquire);
    out_state->last_swing_result =
        impl->combat_last_swing_result.load(std::memory_order_acquire);
    out_state->last_result =
        impl->combat_last_result.load(std::memory_order_acquire);
    out_state->attacks_sent =
        impl->combat_attacks_sent.load(std::memory_order_acquire);
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

void MaydayAdapter::Impl::PublishWeatherService() {
    weather_service = {
        sizeof(AnomalyMaydayWeatherServiceV1), kWeatherServiceVersion, this,
        WeatherGetState, WeatherSetTime, WeatherSetTimeLocked,
        WeatherSetWeatherOverride, WeatherGetKindValue};
    if (ProcessAdapterServices().Publish(
            kWeatherServiceId, kWeatherServiceVersion, &weather_service)) {
        weather_published.store(&weather_service, std::memory_order_release);
    }
}

void MaydayAdapter::Impl::PublishCombatService() {
    combat_service = {
        sizeof(AnomalyMaydayCombatServiceV1), kCombatServiceVersion, this,
        CombatSetOptions, CombatSetEnabled, CombatSetGyroEnabled, CombatGetState};
    if (ProcessAdapterServices().Publish(
            kCombatServiceId, kCombatServiceVersion, &combat_service)) {
        combat_published.store(&combat_service, std::memory_order_release);
    }
}

void MaydayAdapter::SetLogSink(LogSink sink) {
    impl_->log_sink = std::move(sink);
}

bool MaydayAdapter::Impl::EnsureGameImage() {
    if (image != nullptr) return true;
    if (domain_get == nullptr || domain_get_assemblies == nullptr ||
        assembly_get_image == nullptr || image_get_name == nullptr) {
        return false;
    }
    if (domain == nullptr) {
        domain = domain_get();
        if (domain == nullptr) return false;
    }
    std::size_t count{};
    void** const assemblies = domain_get_assemblies(domain, &count);
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
    return image != nullptr;
}

bool MaydayAdapter::Impl::ResolveGame() {
    if (game_resolved) return true;
    if (class_from_name == nullptr) return false;
    if (!EnsureGameImage()) return false;
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

bool MaydayAdapter::Impl::ResolveWeather() {
    // 步骤号进诊断 JSON（weatherResolveStep）；失败同时经日志汇写入运行时日志。
    const auto fail = [this](int step) {
        weather_resolve_step.store(step, std::memory_order_release);
        if (log_sink != nullptr && weather_logged_step != step) {
            weather_logged_step = step;
            log_sink("weather.resolve_failed",
                     "resolve failed at step " + std::to_string(step) +
                     " (2 env, 3 clock, 4 world manager, 5 system, 6 enum)");
        }
        return false;
    };
    if (weather_resolved.load(std::memory_order_acquire)) return true;
    if (class_from_name == nullptr || class_get_method == nullptr ||
        class_get_field == nullptr || field_get_offset == nullptr ||
        field_static_get_value == nullptr || runtime_invoke == nullptr ||
        object_unbox == nullptr) {
        return fail(1);
    }
    if (!EnsureGameImage()) return fail(1);

    // 时间：WorldEnvironment 静态单例 + Hours 属性 + 官方锚定方法。
    // get_anchor 与 WorldClock 字段访问被 IL2CPP 裁剪，改走 Anchor 方法。
    weather_resolve_step.store(2, std::memory_order_release);
    world_environment_class = class_from_name(image, "Mayday.World", "WorldEnvironment");
    if (world_environment_class == nullptr) return fail(2);
    env_get_instance = class_get_method(world_environment_class, "get_Instance", 0);
    env_get_hours = class_get_method(world_environment_class, "get_Hours", 0);
    env_set_hours = class_get_method(world_environment_class, "set_Hours", 1);
    env_get_abs_now = class_get_method(world_environment_class, "get_AbsNow", 0);
    env_anchor_method = class_get_method(world_environment_class, "Anchor", 4);
    env_wanted_rate = class_get_method(world_environment_class, "WantedRate", 0);
    // 天气：WorldManager 自带的 dev 覆盖机制 + WeatherSystem 当前状态。
    weather_resolve_step.store(4, std::memory_order_release);
    world_manager_class = class_from_name(image, "Mayday.World", "WorldManager");
    if (world_manager_class == nullptr) return fail(4);
    wm_get_instance = class_get_method(world_manager_class, "get_Instance", 0);
    wm_dev_override = class_get_method(world_manager_class, "DevOverrideWeather", 3);
    wm_dev_clear = class_get_method(world_manager_class, "DevClearOverride", 0);
    weather_resolve_step.store(5, std::memory_order_release);
    weather_system_class = class_from_name(image, "Mayday.World", "WeatherSystem");
    if (weather_system_class == nullptr) return fail(5);
    ws_get_instance = class_get_method(weather_system_class, "get_Instance", 0);
    ws_get_kind = class_get_method(weather_system_class, "get_Kind", 0);
    ws_get_severity = class_get_method(weather_system_class, "get_Severity", 0);

    // 全部必需句柄清点：日志精确列出缺失项（IL2CPP 裁剪可能单独裁掉属性
    // getter 等未被游戏引用的方法，逐项点名才能定位）。
    struct NamedHandle { const char* name; void* handle; };
    const NamedHandle required[] = {
        {"env.get_Instance", env_get_instance},
        {"env.get_Hours", env_get_hours},
        {"env.set_Hours", env_set_hours},
        {"env.get_AbsNow", env_get_abs_now},
        {"env.Anchor", env_anchor_method},
        {"env.WantedRate", env_wanted_rate},
        {"wm.get_Instance", wm_get_instance},
        {"wm.DevOverrideWeather", wm_dev_override},
        {"wm.DevClearOverride", wm_dev_clear},
        {"ws.get_Instance", ws_get_instance},
        {"ws.get_Kind", ws_get_kind},
        {"ws.get_Severity", ws_get_severity},
    };
    std::string missing;
    for (const auto& entry : required) {
        if (entry.handle == nullptr) {
            if (!missing.empty()) missing += ' ';
            missing += entry.name;
        }
    }
    if (!missing.empty()) {
        weather_resolve_step.store(7, std::memory_order_release);
        if (log_sink != nullptr && weather_logged_step != 7) {
            weather_logged_step = 7;
            log_sink("weather.resolve_failed", "missing handles: " + missing);
        }
        return false;
    }

    // WeatherKind 枚举值按字段名运行时解析；数值互不相同才算可信。
    // 枚举字面字段是 static literal，走 il2cpp_field_static_get_value 直读。
    weather_resolve_step.store(6, std::memory_order_release);
    weather_kind_class = class_from_name(image, "Mayday", "WeatherKind");
    if (weather_kind_class == nullptr) return fail(6);
    std::array<std::int32_t, std::size(kWeatherKindNames)> parsed{};
    for (std::size_t i = 0; i < std::size(kWeatherKindNames); ++i) {
        void* const field = class_get_field(weather_kind_class, kWeatherKindNames[i]);
        if (field == nullptr) return fail(6);
        std::int32_t value = 0;
        field_static_get_value(field, &value);
        parsed[i] = value;
    }
    for (std::size_t a = 0; a < std::size(parsed); ++a) {
        for (std::size_t b = a + 1; b < std::size(parsed); ++b) {
            if (parsed[a] == parsed[b]) return fail(6);
        }
    }
    kind_values = parsed;
    weather_resolved.store(true, std::memory_order_release);
    weather_resolve_step.store(8, std::memory_order_release);
    if (log_sink != nullptr && weather_logged_step != 8) {
        weather_logged_step = 8;
        log_sink("weather.resolved",
                 "kind values: Calm=" + std::to_string(kind_values[0]) +
                 " Cloudy=" + std::to_string(kind_values[1]) +
                 " Rain=" + std::to_string(kind_values[2]) +
                 " Storm=" + std::to_string(kind_values[3]) +
                 " Fog=" + std::to_string(kind_values[4]) +
                 " Snow=" + std::to_string(kind_values[5]));
    }
    return true;
}

bool MaydayAdapter::Impl::ResolveCombat() {
    if (combat_resolved) return true;
    if (class_from_name == nullptr || class_get_method == nullptr ||
        class_get_field == nullptr || field_get_offset == nullptr ||
        field_static_get_value == nullptr || class_get_parent == nullptr ||
        runtime_invoke == nullptr || object_unbox == nullptr ||
        object_get_class == nullptr || domain_get_assemblies == nullptr ||
        assembly_get_image == nullptr || image_get_name == nullptr) {
        return false;
    }
    if (!EnsureGameImage()) return false;

    player_net_class = class_from_name(image, "Mayday", "PlayerNet");
    player_inventory_class = class_from_name(image, "Mayday", "PlayerInventory");
    bat_item_class = class_from_name(image, "Mayday.Passengers", "BatItem");
    if (player_rig_class == nullptr) {
        player_rig_class = class_from_name(image, "Mayday.Players", "PlayerRig");
    }
    player_avatar_class =
        class_from_name(image, "Mayday.Players", "PlayerAvatar");
    interaction_registry_class = class_from_name(image, "Mayday", "InteractionRegistry");
    player_interactor_class = class_from_name(image, "Mayday", "PlayerInteractor");
    if (player_net_class == nullptr || player_inventory_class == nullptr ||
        bat_item_class == nullptr ||
        interaction_registry_class == nullptr || player_interactor_class == nullptr) {
        return false;
    }
    if (player_rig_class != nullptr) {
        if (get_local_rig == nullptr) {
            get_local_rig = class_get_method(player_rig_class, "get_Local", 0);
        }
        get_avatar = class_get_method(player_rig_class, "get_Avatar", 0);
    }
    if (player_avatar_class != nullptr) {
        get_avatar_rig = class_get_method(player_avatar_class, "get_Rig", 0);
    }

    get_local = class_get_method(player_net_class, "get_Local", 0);
    get_inventory = class_get_method(player_net_class, "get_Inventory", 0);
    get_interactor = class_get_method(player_net_class, "get_Interactor", 0);
    get_eye_position = class_get_method(player_net_class, "get_EyePosition", 0);
    get_aim_direction = class_get_method(player_net_class, "get_AimDirection", 0);
    get_chest_position = class_get_method(player_net_class, "get_ChestPosition", 0);
    get_is_seated = class_get_method(player_net_class, "get_IsSeated", 0);
    get_is_stunned = class_get_method(player_net_class, "get_IsStunned", 0);
    get_network_is_dead = class_get_method(player_net_class, "get_NetworkIsDead", 0);
    player_view_field = class_get_field(player_net_class, "ViewTransform");
    if (player_view_field != nullptr) {
        player_view_offset = field_get_offset(player_view_field);
    }
    get_held = class_get_method(player_inventory_class, "get_Held", 0);
    bat_server_use_once = class_get_method(bat_item_class, "ServerUseOnce", 3);
    bat_rpc_swing_user_code =
        class_get_method(bat_item_class, "UserCode_RpcSwing__Byte", 1);
    bat_get_holder = class_get_method(bat_item_class, "get_Holder", 0);
    swing_whiff_field = class_get_field(bat_item_class, "SwingWhiff");
    swing_pax_field = class_get_field(bat_item_class, "SwingPax");
    swing_crew_field = class_get_field(bat_item_class, "SwingCrew");
    swing_whoosh_field = class_get_field(bat_item_class, "SwingWhoosh");
    if (swing_whiff_field != nullptr) {
        swing_whiff_offset = field_get_offset(swing_whiff_field);
    }
    if (swing_pax_field != nullptr) {
        swing_pax_offset = field_get_offset(swing_pax_field);
    }
    if (swing_crew_field != nullptr) {
        swing_crew_offset = field_get_offset(swing_crew_field);
    }
    if (swing_whoosh_field != nullptr) {
        swing_whoosh_offset = field_get_offset(swing_whoosh_field);
    }
    next_swing_field = class_get_field(bat_item_class, "nextSwing");
    if (next_swing_field != nullptr) {
        next_swing_offset = field_get_offset(next_swing_field);
    }
    player_all_field = class_get_field(player_net_class, "All");
    interact_ref_of = class_get_method(interaction_registry_class, "RefOf", 1);
    interactor_update = class_get_method(player_interactor_class, "Update", 0);
    interactor_cmd_use = class_get_method(player_interactor_class, "CmdUse", 4);

    // UnityEngine.CoreModule 承载 Transform / Component / Quaternion。
    std::size_t assembly_count{};
    void** const assemblies = domain_get_assemblies(domain, &assembly_count);
    void* core_image{};
    void* mirror_image{};
    void* mirror_components_image{};
    for (std::size_t i = 0; assemblies != nullptr && i < assembly_count; ++i) {
        void* const candidate = assembly_get_image(assemblies[i]);
        if (candidate == nullptr) continue;
        const char* const name = image_get_name(candidate);
        if (name == nullptr) continue;
        if (std::strcmp(name, "UnityEngine.CoreModule.dll") == 0) {
            core_image = candidate;
        } else if (std::strcmp(name, "Mirror.dll") == 0) {
            mirror_image = candidate;
        } else if (std::strcmp(name, "Mirror.Components.dll") == 0) {
            mirror_components_image = candidate;
        }
    }
    if (core_image == nullptr) return false;
    void* const transform_class = class_from_name(core_image, "UnityEngine", "Transform");
    void* const component_class = class_from_name(core_image, "UnityEngine", "Component");
    if (transform_class == nullptr || component_class == nullptr) {
        return false;
    }
    if (component_get_transform == nullptr) {
        component_get_transform = class_get_method(component_class, "get_transform", 0);
    }
    if (transform_get_position == nullptr) {
        transform_get_position = class_get_method(transform_class, "get_position", 0);
    }
    if (transform_get_euler == nullptr) {
        transform_get_euler = class_get_method(transform_class, "get_eulerAngles", 0);
    }
    if (transform_set_euler == nullptr) {
        transform_set_euler = class_get_method(transform_class, "set_eulerAngles", 1);
    }
    if (mirror_image != nullptr) {
        network_server_class = class_from_name(mirror_image, "Mirror", "NetworkServer");
        if (network_server_class != nullptr) {
            get_network_server_active =
                class_get_method(network_server_class, "get_active", 0);
        }
    }
    if (mirror_components_image != nullptr) {
        network_transform_class = class_from_name(
            mirror_components_image, "Mirror", "NetworkTransformUnreliable");
        network_transform_base_class = class_from_name(
            mirror_components_image, "Mirror", "NetworkTransformBase");
        if (network_transform_class != nullptr) {
            net_broadcast_method =
                class_get_method(network_transform_class, "UpdateClientBroadcast", 0);
            if (net_broadcast_method != nullptr) {
                net_broadcast_target =
                    *reinterpret_cast<void**>(net_broadcast_method);
            }
            void* const field_class = network_transform_base_class != nullptr
                ? network_transform_base_class : network_transform_class;
            net_transform_target_field = class_get_field(field_class, "target");
            if (net_transform_target_field != nullptr) {
                net_transform_target_offset =
                    field_get_offset(net_transform_target_field);
            }
        }
    }
    if (player_rig_class != nullptr) {
        get_net_transform =
            class_get_method(player_rig_class, "get_NetTransform", 0);
    }

    combat_update_target = interactor_update != nullptr
        ? *reinterpret_cast<void**>(interactor_update) : nullptr;
    combat_bat_target = bat_server_use_once != nullptr
        ? *reinterpret_cast<void**>(bat_server_use_once) : nullptr;
    combat_swing_target = bat_rpc_swing_user_code != nullptr
        ? *reinterpret_cast<void**>(bat_rpc_swing_user_code) : nullptr;

    struct NamedHandle { const char* name; void* handle; };
    const NamedHandle required[] = {
        {"player.get_Local", get_local},
        {"player.get_Inventory", get_inventory},
        {"player.get_Interactor", get_interactor},
        {"player.get_EyePosition", get_eye_position},
        {"player.get_AimDirection", get_aim_direction},
        {"player.get_ChestPosition", get_chest_position},
        {"player.ViewTransform", player_view_field},
        {"player.get_eulerAngles", transform_get_euler},
        {"player.set_eulerAngles", transform_set_euler},
        {"player.All", player_all_field},
        {"inventory.get_Held", get_held},
        {"bat.ServerUseOnce", bat_server_use_once},
        {"registry.RefOf", interact_ref_of},
        {"interactor.Update", interactor_update},
        {"interactor.CmdUse", interactor_cmd_use},
        {"interactor.Update.target", combat_update_target},
        {"bat.ServerUseOnce.target", combat_bat_target},
    };
    std::string missing;
    for (const auto& entry : required) {
        if (entry.handle == nullptr) {
            if (!missing.empty()) missing += ' ';
            missing += entry.name;
        }
    }
    if (player_view_offset == 0 || player_view_offset >= 0x1000) {
        if (!missing.empty()) missing += ' ';
        missing += "player.ViewTransform.offset";
    }
    if (!missing.empty()) {
        if (log_sink != nullptr && !combat_resolve_logged) {
            combat_resolve_logged = true;
            log_sink("combat.resolve_failed", "missing handles: " + missing);
        }
        return false;
    }

    combat_resolved = true;
    if (log_sink != nullptr) {
        log_sink("combat.resolved",
                 std::string("PlayerNet/BatItem/crew list/InteractionRegistry resolved netspin=") +
                 (net_broadcast_target != nullptr ? "1" : "0"));
    }
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

void* MaydayAdapter::Impl::StaticInstance(void* get_instance_method) {
    if (get_instance_method == nullptr) return nullptr;
    void* exception{};
    return runtime_invoke(get_instance_method, nullptr, nullptr, &exception);
}

bool MaydayAdapter::Impl::AnchorEnvironment(void* env, float hours) {
    // AbsNow 与锚点时钟同基准；networked 传 false（本地覆盖）。
    double now = 0.0;
    if (!InvokeDouble(env_get_abs_now, env, &now)) return false;
    double anchored_hours = static_cast<double>(hours);
    float rate = 0.0f;
    if (!weather_time_locked) {
        if (!InvokeFloat(env_wanted_rate, nullptr, &rate)) return false;
        if (!(rate > 0.0f)) rate = 1.0f;
    }
    std::uint8_t networked = 0;
    void* params[] = {&anchored_hours, &now, &networked, &rate};
    return InvokeVoid(env_anchor_method, env, params);
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

bool MaydayAdapter::Impl::InvokeFloat(
    void* method, void* object, float* out_value) {
    if (method == nullptr || out_value == nullptr) return false;
    void* exception{};
    void* const boxed = runtime_invoke(method, object, nullptr, &exception);
    if (exception != nullptr || boxed == nullptr || object_unbox == nullptr) return false;
    *out_value = *static_cast<float*>(object_unbox(boxed));
    return true;
}

bool MaydayAdapter::Impl::InvokeDouble(
    void* method, void* object, double* out_value) {
    if (method == nullptr || out_value == nullptr) return false;
    void* exception{};
    void* const boxed = runtime_invoke(method, object, nullptr, &exception);
    if (exception != nullptr || boxed == nullptr || object_unbox == nullptr) return false;
    *out_value = *static_cast<double*>(object_unbox(boxed));
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

void* MaydayAdapter::Impl::InvokeObject(void* method, void* object) {
    if (method == nullptr) return nullptr;
    void* exception{};
    void* const result = runtime_invoke(method, object, nullptr, &exception);
    return exception == nullptr ? result : nullptr;
}

bool MaydayAdapter::Impl::ClassDerivesFrom(void* klass, void* base) const {
    if (klass == nullptr || base == nullptr || class_get_parent == nullptr) return false;
    for (void* current = klass; current != nullptr; current = class_get_parent(current)) {
        if (current == base) return true;
    }
    return false;
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

void* MaydayAdapter::Impl::FindCameraRig(void* local) {
    if (local == nullptr) return nullptr;
    void* exception{};
    void* camera_rig{};
    if (camera_rig_field != nullptr && camera_rig_offset > 0 &&
        camera_rig_offset < 0x1000) {
        camera_rig = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(local) + camera_rig_offset);
    }
    if (camera_rig == nullptr && view_camera_field != nullptr &&
        view_camera_offset > 0 && view_camera_offset < 0x1000 &&
        component_get_transform != nullptr) {
        void* const camera = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(local) + view_camera_offset);
        if (camera != nullptr) {
            camera_rig = runtime_invoke(
                component_get_transform, camera, nullptr, &exception);
            if (exception != nullptr) exception = nullptr;
        }
    }
    if (camera_rig == nullptr && get_camera_rig != nullptr) {
        camera_rig = runtime_invoke(get_camera_rig, local, nullptr, &exception);
        if (exception != nullptr) exception = nullptr;
    }
    if (camera_rig == nullptr && get_view_camera != nullptr &&
        component_get_transform != nullptr) {
        void* const camera =
            runtime_invoke(get_view_camera, local, nullptr, &exception);
        if (exception != nullptr) exception = nullptr;
        if (camera != nullptr) {
            camera_rig = runtime_invoke(
                component_get_transform, camera, nullptr, &exception);
            if (exception != nullptr) exception = nullptr;
        }
    }
    if (camera_rig == nullptr && camera_get_main != nullptr &&
        component_get_transform != nullptr) {
        void* const camera =
            runtime_invoke(camera_get_main, nullptr, nullptr, &exception);
        if (exception != nullptr) exception = nullptr;
        if (camera != nullptr) {
            camera_rig = runtime_invoke(
                component_get_transform, camera, nullptr, &exception);
            if (exception != nullptr) exception = nullptr;
        }
    }
    return camera_rig;
}

void MaydayAdapter::Impl::ApplyGyro() {
    if (!combat_gyro_enabled.load(std::memory_order_acquire) ||
        get_local_rig == nullptr || get_avatar == nullptr ||
        get_avatar_rig == nullptr || object_get_class == nullptr ||
        class_get_method == nullptr || transform_get_euler == nullptr ||
        transform_set_euler == nullptr) {
        return;
    }
    void* const local = InvokeObject(get_local_rig, nullptr);
    if (local == nullptr) return;
    void* const avatar = InvokeObject(get_avatar, local);
    if (avatar == nullptr) return;
    void* const avatar_rig = InvokeObject(get_avatar_rig, avatar);
    if (avatar_rig == nullptr) return;
    void* const avatar_class = object_get_class(avatar_rig);
    if (avatar_class == nullptr) return;
    void* const get_root = class_get_method(avatar_class, "get_Root", 0);
    if (get_root == nullptr) return;
    void* const root = InvokeObject(get_root, avatar_rig);
    if (root == nullptr) return;
    // 只旋转视觉 Avatar 根节点；PlayerRig/CharacterController 根节点
    // 一旦被强制旋转会破坏移动和相机，必须避开。
    if (component_get_transform != nullptr) {
        void* const body_root = InvokeObject(component_get_transform, local);
        if (root == body_root) return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!gyro_initialized.load(std::memory_order_acquire)) {
        Vec3 euler{};
        if (!InvokeVec3(transform_get_euler, root, &euler)) return;
        gyro_base_yaw = euler.y;
        gyro_spin_degrees = 0.0f;
        gyro_last = now;
        gyro_initialized.store(true, std::memory_order_release);
    }
    float delta = std::chrono::duration<float>(now - gyro_last).count();
    gyro_last = now;
    delta = std::clamp(delta, 0.0f, 0.25f);
    // 0.1 秒一圈 = 3600 度/秒；只转身体模型，相机和移动完全不受影响。
    gyro_spin_degrees = std::fmod(gyro_spin_degrees + delta * 3600.0f, 360.0f);
    Vec3 target{0.0f, gyro_base_yaw + gyro_spin_degrees, 0.0f};
    static_cast<void>(InvokeVoidArg(transform_set_euler, root, &target));
}

void MaydayAdapter::Impl::RestoreGyro() {
    if (!gyro_initialized.exchange(false, std::memory_order_acq_rel)) return;
    if (get_local_rig == nullptr || get_avatar == nullptr ||
        get_avatar_rig == nullptr || object_get_class == nullptr ||
        class_get_method == nullptr || transform_set_euler == nullptr) {
        return;
    }
    void* const local = InvokeObject(get_local_rig, nullptr);
    if (local == nullptr) return;
    void* const avatar = InvokeObject(get_avatar, local);
    if (avatar == nullptr) return;
    void* const avatar_rig = InvokeObject(get_avatar_rig, avatar);
    if (avatar_rig == nullptr) return;
    void* const avatar_class = object_get_class(avatar_rig);
    if (avatar_class == nullptr) return;
    void* const get_root = class_get_method(avatar_class, "get_Root", 0);
    if (get_root == nullptr) return;
    void* const root = InvokeObject(get_root, avatar_rig);
    if (root == nullptr) return;
    Vec3 target{0.0f, gyro_base_yaw, 0.0f};
    static_cast<void>(InvokeVoidArg(transform_set_euler, root, &target));
}

bool MaydayAdapter::Impl::ActivateFreecam(void* local, void* look) {
    fc_error_key = nullptr;
    if (local == nullptr || look == nullptr) {
        fc_error_key = "status.not_in_flight";
        return false;
    }
    void* const camera_rig = FindCameraRig(local);
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
    void* exception{};
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
            // shutdown 检查必须与 hook 安装同锁：Stop 的拆卸段持有 resolve_mutex
            // 拆除 hook，若在锁外判断 shutdown，拆卸释放后在途调用会重新装回 hook，
            // Impl 析构后游戏线程解引用悬垂的 freecam_owner。
            if (impl->freecam_shutdown) return MoneyStatus("status.unavailable");
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
// 自动棒球棍：PlayerInteractor.Update 触发使用命令，BatItem.ServerUseOnce
// 在服务端命中判定前临时把视角朝向目标（静默瞄准，渲染帧看不到旋转）。
// ---------------------------------------------------------------------------
std::atomic<MaydayAdapter::Impl*> MaydayAdapter::Impl::combat_owner{nullptr};

void MaydayAdapter::Impl::UpdateCombat(void* self) {
    if (combat_shutdown.load(std::memory_order_acquire)) return;
    void* const local = InvokeObject(get_local, nullptr);
    if (local == nullptr) {
        combat_in_flight.store(false, std::memory_order_release);
        combat_holding_bat.store(false, std::memory_order_release);
        combat_has_target.store(false, std::memory_order_release);
        combat_target_distance_mm.store(-1, std::memory_order_release);
        // 换局/重生后重新捕获新 Avatar 的基础朝向。
        gyro_initialized.store(false, std::memory_order_release);
        return;
    }
    combat_in_flight.store(true, std::memory_order_release);
    // 服务器会为每个玩家的 PlayerInteractor 调用 Update；只处理本地玩家。
    if (InvokeObject(get_interactor, local) != self) return;

    // 陀螺模式只旋转视觉 Avatar 根节点；相机、移动和控制器根节点不碰。
    // 远端房间额外把旋转写进 NetworkTransform 的发送快照，让房主和其他
    // 玩家看到旋转；房主模式下不伪造网络快照，避免本地服务端应用后卡住。
    if (get_network_server_active != nullptr) {
        bool host = false;
        if (InvokeBool(get_network_server_active, nullptr, &host)) {
            combat_host_mode.store(host ? 1 : 0, std::memory_order_release);
        }
    }
    if (combat_gyro_enabled.load(std::memory_order_acquire)) {
        ApplyGyro();
    } else {
        RestoreGyro();
    }
    if (!combat_enabled.load(std::memory_order_acquire)) return;

    bool seated = false;
    if (get_is_seated != nullptr && InvokeBool(get_is_seated, local, &seated) && seated) {
        return;
    }
    bool stunned = false;
    if (get_is_stunned != nullptr && InvokeBool(get_is_stunned, local, &stunned) && stunned) {
        return;
    }
    bool dead = false;
    if (get_network_is_dead != nullptr &&
        InvokeBool(get_network_is_dead, local, &dead) && dead) {
        return;
    }

    void* const inventory = InvokeObject(get_inventory, local);
    void* const held = inventory != nullptr ? InvokeObject(get_held, inventory) : nullptr;
    const bool holding_bat = held != nullptr && object_get_class != nullptr &&
        ClassDerivesFrom(object_get_class(held), bat_item_class);
    combat_holding_bat.store(holding_bat, std::memory_order_release);
    if (!holding_bat) {
        combat_has_target.store(false, std::memory_order_release);
        combat_target_distance_mm.store(-1, std::memory_order_release);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    // 每次主线程 Update 都重新扫描；发送频率由 attack_interval_ms 控制。
    CombatTarget target;
    if (FindNearestCrewmate(local, &target)) {
        combat_target = target;
        combat_has_target.store(true, std::memory_order_release);
        combat_target_distance_mm.store(
            target.distance_mm, std::memory_order_release);
    } else {
        combat_target = {};
        combat_has_target.store(false, std::memory_order_release);
        combat_target_distance_mm.store(-1, std::memory_order_release);
    }
    if (!combat_has_target.load(std::memory_order_acquire)) return;

    const auto interval = std::chrono::milliseconds(
        combat_interval_ms.load(std::memory_order_acquire));
    if (now - combat_last_attack < interval) return;

    CombatInteractRef ref;
    // 目标必须是非 Passenger 的 IInteractable，服务端才会走 SwingAtCrew；
    // 手里的棒球棍本身满足条件，真正要打谁由服务端按瞄准方向选择。
    if (!BuildInteractRef(held, &ref)) {
        combat_last_result.store(2, std::memory_order_release);
        combat_last_attack = now;
        return;
    }
    float dt = 0.0f;
    std::uint8_t phase = 0;
    Vec3 hit_point = combat_target.chest;
    void* params[] = {&ref, &hit_point, &dt, &phase};
    combat_pending_aim_target = combat_target.object;
    // 房主模式下服务端对象就是本地持有的 BatItem：把 nextSwing 归零，
    // 让滑块设置的高频请求真正进入 ServerUseOnce。远端房间无法修改房主
    // 对象，仍受房主 0.8s 挥击冷却限制。
    if (combat_host_mode.load(std::memory_order_acquire) == 1 &&
        held != nullptr && next_swing_field != nullptr &&
        next_swing_offset > 0 && next_swing_offset < 0x1000) {
        *reinterpret_cast<double*>(
            reinterpret_cast<std::uint8_t*>(held) + next_swing_offset) = 0.0;
    }
    void* exception{};
    runtime_invoke(interactor_cmd_use, self, params, &exception);
    combat_last_attack = now;
    if (exception != nullptr) {
        combat_last_result.store(2, std::memory_order_release);
        combat_pending_aim_target = nullptr;
    } else {
        combat_last_result.store(1, std::memory_order_release);
        combat_attacks_sent.fetch_add(1, std::memory_order_acq_rel);
        // 远端房间没有本地服务端钩子消费该指针，立即清掉避免悬垂。
        if (combat_host_mode.load(std::memory_order_acquire) == 0) {
            combat_pending_aim_target = nullptr;
        }
    }
    if (log_sink != nullptr &&
        now - combat_last_send_log >= std::chrono::seconds(1)) {
        combat_last_send_log = now;
        log_sink("combat.attack",
                 std::string("host=") +
                 (combat_host_mode.load(std::memory_order_acquire) != 0 ? "1" : "0") +
                 " crew=1 dist_mm=" + std::to_string(combat_target.distance_mm) +
                 (exception == nullptr ? " sent" : " failed"));
    }
}

bool MaydayAdapter::Impl::FindNearestCrewmate(
    void* local, CombatTarget* out_target) {
    if (local == nullptr || out_target == nullptr || player_all_field == nullptr ||
        get_chest_position == nullptr || object_get_class == nullptr ||
        class_get_method == nullptr || get_eye_position == nullptr) {
        return false;
    }

    // 远端房间的命中判定在房主进程里执行：本地静默瞄准钩子不会运行，
    // 服务端会按玩家自己的瞄准方向做扇形检查。先在本地判断当前是不是
    // 房主；不是房主时只选择前向锥体内的队友，保证发出的攻击能命中。
    bool host = false;
    if (get_network_server_active != nullptr) {
        bool active = false;
        if (InvokeBool(get_network_server_active, nullptr, &active)) host = active;
    }
    combat_host_mode.store(host ? 1 : 0, std::memory_order_release);
    const std::int32_t host_mode = host ? 1 : 0;
    if (!combat_mode_logged || combat_logged_host_mode != host_mode) {
        combat_mode_logged = true;
        combat_logged_host_mode = host_mode;
        if (log_sink != nullptr) {
            log_sink("combat.mode", host
                ? "hosting: silent aim active"
                : "joined room: server aim cone limits targets");
        }
    }

    // PlayerNet.All 在所有端登记玩家；只保留非本地、未死亡的队友。
    void* list = nullptr;
    field_static_get_value(player_all_field, &list);
    if (list == nullptr) {
        combat_crew_count.store(0, std::memory_order_release);
        return false;
    }
    void* const list_class = object_get_class(list);
    if (list_class == nullptr) {
        combat_crew_count.store(0, std::memory_order_release);
        return false;
    }
    if (combat_list_count == nullptr || combat_list_item == nullptr ||
        combat_list_class != list_class) {
        void* const count_method = class_get_method(list_class, "get_Count", 0);
        void* const item_method = class_get_method(list_class, "get_Item", 1);
        if (count_method == nullptr || item_method == nullptr) {
            combat_crew_count.store(0, std::memory_order_release);
            return false;
        }
        combat_list_class = list_class;
        combat_list_count = count_method;
        combat_list_item = item_method;
    }
    std::int32_t count = 0;
    if (!InvokeInt(combat_list_count, list, &count) || count <= 0) {
        combat_crew_count.store(0, std::memory_order_release);
        return false;
    }

    Vec3 eye{};
    if (!InvokeVec3(get_eye_position, local, &eye)) return false;

    const bool use_cone = !host;
    const float cone_deg = static_cast<float>(
        combat_crew_cone_deg.load(std::memory_order_acquire));
    Vec3 aim{};
    const bool have_aim = use_cone && get_aim_direction != nullptr &&
        InvokeVec3(get_aim_direction, local, &aim);

    const float max_range_m =
        static_cast<float>(combat_max_range_mm.load(std::memory_order_acquire)) /
        1000.0f;
    float best_squared = max_range_m * max_range_m;
    CombatTarget best;
    std::int32_t visible_crew = 0;
    constexpr float kRadiansToDegrees = 57.295779513082320876f;
    for (std::int32_t index = 0; index < count; ++index) {
        void* params[] = {&index};
        void* exception{};
        void* const crewmate =
            runtime_invoke(combat_list_item, list, params, &exception);
        if (exception != nullptr || crewmate == nullptr || crewmate == local) {
            continue;
        }
        bool dead = false;
        if (get_network_is_dead != nullptr &&
            InvokeBool(get_network_is_dead, crewmate, &dead) && dead) {
            continue;
        }
        ++visible_crew;
        Vec3 chest{};
        if (!InvokeVec3(get_chest_position, crewmate, &chest)) continue;
        const float dx = chest.x - eye.x;
        const float dy = chest.y - eye.y;
        const float dz = chest.z - eye.z;
        const float squared = dx * dx + dy * dy + dz * dz;
        if (!(squared <= best_squared)) continue;
        if (use_cone && have_aim && cone_deg > 0.0f) {
            // 与服务端 SwingAtCrew 一致：3D 角度、极近距离跳过检查。
            if (squared > 0.0025f) {
                const float aim_squared =
                    aim.x * aim.x + aim.y * aim.y + aim.z * aim.z;
                if (aim_squared > 1e-8f) {
                    float cosine =
                        (aim.x * dx + aim.y * dy + aim.z * dz) /
                        std::sqrt(aim_squared * squared);
                    cosine = std::clamp(cosine, -1.0f, 1.0f);
                    const float angle = std::acos(cosine) * kRadiansToDegrees;
                    if (angle > cone_deg) continue;
                }
            }
        }
        best_squared = squared;
        best.object = crewmate;
        best.chest = chest;
        best.distance_mm = static_cast<std::int32_t>(
            std::lround(std::sqrt(squared) * 1000.0f));
        best.valid = true;
    }
    combat_crew_count.store(visible_crew, std::memory_order_release);
    if (!best.valid) return false;
    *out_target = best;
    return true;
}

bool MaydayAdapter::Impl::BuildInteractRef(
    void* target, CombatInteractRef* out_ref) {
    if (target == nullptr || out_ref == nullptr || interact_ref_of == nullptr ||
        object_unbox == nullptr) {
        return false;
    }
    void* params[] = {target};
    void* exception{};
    void* const boxed = runtime_invoke(interact_ref_of, nullptr, params, &exception);
    if (exception != nullptr || boxed == nullptr) return false;
    const auto* ref = static_cast<const CombatInteractRef*>(object_unbox(boxed));
    if (ref == nullptr || ref->net_id == 0) return false;
    *out_ref = *ref;
    return true;
}

void MaydayAdapter::Impl::HandleInteractorUpdate(void* self) {
    auto* const original =
        reinterpret_cast<void (ANOMALY_CALL*)(void*)>(combat_update_original);
    if (original != nullptr) original(self);
    if (combat_shutdown.load(std::memory_order_acquire)) return;
    UpdateCombat(self);
}

void MaydayAdapter::Impl::HandleBatUseOnce(
    void* self, void* player, void* target, const Vec3* hit_point) {
    auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(
        void*, void*, void*, const Vec3*)>(combat_bat_original);
    if (original == nullptr) return;
    if (!combat_enabled.load(std::memory_order_acquire) ||
        combat_shutdown.load(std::memory_order_acquire) ||
        self == nullptr || player == nullptr || target == nullptr ||
        player_view_field == nullptr || object_get_class == nullptr) {
        original(self, player, target, hit_point);
        return;
    }
    // 只对本地玩家自己的挥棒做静默瞄准；服务器处理远端玩家命令时
    // PlayerNet.Local 不是同一个对象，直接放行。
    void* const local = InvokeObject(get_local, nullptr);
    if (local == nullptr || local != player) {
        original(self, player, target, hit_point);
        return;
    }
    void* const view = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(local) + player_view_offset);
    if (view == nullptr) {
        original(self, player, target, hit_point);
        return;
    }
    void* const target_class = object_get_class(target);
    void* aim_target = nullptr;
    if (target_class != nullptr &&
        ClassDerivesFrom(target_class, player_net_class)) {
        aim_target = target;
    } else if (combat_pending_aim_target != nullptr) {
        // CmdUse 的目标是手里的棒球棍（非 Passenger），真正要打的
        // 队友由发起攻击时记录的 pending 指针提供。
        aim_target = combat_pending_aim_target;
        combat_pending_aim_target = nullptr;
    }
    if (aim_target == nullptr || get_chest_position == nullptr) {
        original(self, player, target, hit_point);
        return;
    }
    Vec3 point{};
    if (!InvokeVec3(get_chest_position, aim_target, &point)) {
        original(self, player, target, hit_point);
        return;
    }
    Vec3 eye{};
    if (!InvokeVec3(get_eye_position, local, &eye)) {
        original(self, player, target, hit_point);
        return;
    }
    Vec3 forward{point.x - eye.x, point.y - eye.y, point.z - eye.z};
    const float length_squared =
        forward.x * forward.x + forward.y * forward.y + forward.z * forward.z;
    if (!(length_squared > 1e-6f)) {
        original(self, player, target, hit_point);
        return;
    }
    // 只用水平朝向 + 俯仰构造世界欧拉角：服务端读的是 ViewTransform.forward，
    // roll 不影响命中判定，原欧拉角在调用后立即恢复。
    constexpr float kRadiansToDegrees = 57.295779513082320876f;
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    const float pitch = -std::asin(std::clamp(
        forward.y * inverse_length, -1.0f, 1.0f)) * kRadiansToDegrees;
    const float yaw =
        std::atan2(forward.x, forward.z) * kRadiansToDegrees;
    Vec3 aim_euler{pitch, yaw, 0.0f};
    Vec3 saved_euler{};
    if (!InvokeVec3(transform_get_euler, view, &saved_euler) ||
        !InvokeVoidArg(transform_set_euler, view, &aim_euler)) {
        original(self, player, target, hit_point);
        return;
    }
    original(self, player, target, hit_point);
    static_cast<void>(InvokeVoidArg(transform_set_euler, view, &saved_euler));
}

void MaydayAdapter::Impl::HandleBatRpcSwing(void* self, std::uint8_t result) {
    auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(
        void*, std::uint8_t)>(combat_swing_original);
    if (original != nullptr) original(self, result);

    // RpcSwing 会广播所有玩家的挥击；只统计本地玩家手持的那把球棒。
    void* const local = InvokeObject(get_local, nullptr);
    if (local != nullptr && bat_get_holder != nullptr) {
        void* const holder = InvokeObject(bat_get_holder, self);
        if (holder != local) return;
    }

    // SwingWhiff/SwingPax/SwingCrew/SwingWhoosh 是 BatItem 的 prefab
    // 序列化字段；按字段偏移读取实际值，避免把结果码写死在宿主里。
    std::uint8_t whiff = 0;
    std::uint8_t pax = 0;
    std::uint8_t crew = 0;
    std::uint8_t whoosh = 0;
    const auto read_byte = [self](
        void* field, std::size_t offset, std::uint8_t* out) {
        if (self == nullptr || field == nullptr || out == nullptr ||
            offset == 0 || offset >= 0x1000) {
            return false;
        }
        *out = *reinterpret_cast<const std::uint8_t*>(
            reinterpret_cast<const std::uint8_t*>(self) + offset);
        return true;
    };
    static_cast<void>(read_byte(swing_whiff_field, swing_whiff_offset, &whiff));
    static_cast<void>(read_byte(swing_pax_field, swing_pax_offset, &pax));
    static_cast<void>(read_byte(swing_crew_field, swing_crew_offset, &crew));
    static_cast<void>(read_byte(
        swing_whoosh_field, swing_whoosh_offset, &whoosh));

    // SwingAtCrew/SwingAtPassenger 的返回值就是协议里的结果码：
    // 0=whiff，1=乘客命中，2=队友命中，3=特殊命中/挥空特效。
    // prefab 上的 Swing* 字段在本作里读出来是 0，不能用来映射。
    std::int32_t semantic = 0;
    switch (result) {
        case 0: semantic = 1; break;
        case 1: semantic = 2; break;
        case 2: semantic = 3; break;
        case 3: semantic = 4; break;
        default: break;
    }
    combat_last_swing_result.store(semantic, std::memory_order_release);

    const auto now = std::chrono::steady_clock::now();
    if (log_sink != nullptr &&
        now - combat_last_result_log >= std::chrono::milliseconds(500)) {
        combat_last_result_log = now;
        log_sink("combat.swing",
                 "result=" + std::to_string(result) +
                 " whiff=" + std::to_string(whiff) +
                 " pax=" + std::to_string(pax) +
                 " crew=" + std::to_string(crew) +
                 " whoosh=" + std::to_string(whoosh));
    }
}

void MaydayAdapter::Impl::HandleNetTransformBroadcast(void* self) {
    auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(void*)>(
        net_broadcast_original);
    if (original == nullptr) return;
    if (!combat_gyro_enabled.load(std::memory_order_acquire) ||
        combat_shutdown.load(std::memory_order_acquire) || self == nullptr) {
        original(self);
        return;
    }
    void* const local_rig =
        get_local_rig != nullptr ? InvokeObject(get_local_rig, nullptr) : nullptr;
    void* const local_net =
        (local_rig != nullptr && get_net_transform != nullptr)
        ? InvokeObject(get_net_transform, local_rig) : nullptr;
    if (local_net == nullptr || local_net != self) {
        original(self);
        return;
    }
    // 房主模式的 CmdClientToServerSync 会在本地服务端应用到同一个根节点，
    // 会把临时旋转固化下来；网络旋转先只在加入他人房间时启用。
    bool host = false;
    if (get_network_server_active != nullptr) {
        bool active = false;
        if (InvokeBool(get_network_server_active, nullptr, &active)) {
            host = active;
            combat_host_mode.store(host ? 1 : 0, std::memory_order_release);
        }
    }
    if (host) {
        original(self);
        return;
    }
    if (component_get_transform == nullptr || transform_get_euler == nullptr ||
        transform_set_euler == nullptr) {
        original(self);
        return;
    }
    void* transform{};
    if (net_transform_target_field != nullptr &&
        net_transform_target_offset > 0 && net_transform_target_offset < 0x1000) {
        transform = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(self) + net_transform_target_offset);
    }
    if (transform == nullptr) {
        transform = InvokeObject(component_get_transform, self);
    }
    if (transform == nullptr) {
        original(self);
        return;
    }
    Vec3 saved{};
    if (!InvokeVec3(transform_get_euler, transform, &saved)) {
        original(self);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (gyro_net_last == std::chrono::steady_clock::time_point{}) {
        gyro_net_last = now;
        gyro_net_spin_degrees = 0.0f;
    }
    float delta = std::chrono::duration<float>(now - gyro_net_last).count();
    gyro_net_last = now;
    delta = std::clamp(delta, 0.0f, 0.25f);
    gyro_net_spin_degrees =
        std::fmod(gyro_net_spin_degrees + delta * 3600.0f, 360.0f);
    Vec3 spun{saved.x, saved.y + gyro_net_spin_degrees, saved.z};
    if (!InvokeVoidArg(transform_set_euler, transform, &spun)) {
        original(self);
        return;
    }
    // 原始 UpdateClientBroadcast 会把带旋转的快照发送给房主；随后立即
    // 恢复本地根节点，移动/相机/渲染不受影响。
    original(self);
    static_cast<void>(InvokeVoidArg(transform_set_euler, transform, &saved));
}

bool MaydayAdapter::Impl::EnsureCombatHooks() {
    if (combat_hooks_installed) return true;
    if (combat_shutdown.load(std::memory_order_acquire) ||
        combat_update_target == nullptr || combat_bat_target == nullptr) {
        return false;
    }
    combat_hook_backend = CreateMinHookBackend();
    if (combat_hook_backend == nullptr || !combat_hook_backend->Initialize()) {
        combat_hook_backend.reset();
        return false;
    }
    combat_owner.store(this, std::memory_order_release);
    bool ok = combat_hook_backend->Create(
        combat_update_target,
        reinterpret_cast<void*>(&Impl::InteractorUpdateThunk),
        &combat_update_original);
    const bool created_update = ok;
    if (ok) {
        ok = combat_hook_backend->Create(
            combat_bat_target,
            reinterpret_cast<void*>(&Impl::BatUseOnceThunk),
            &combat_bat_original);
    }
    const bool created_bat = ok;
    if (ok) ok = combat_hook_backend->Enable(combat_update_target);
    if (ok) ok = combat_hook_backend->Enable(combat_bat_target);
    if (!ok) {
        if (created_update) {
            static_cast<void>(combat_hook_backend->Remove(combat_update_target));
        }
        if (created_bat) {
            static_cast<void>(combat_hook_backend->Remove(combat_bat_target));
        }
        combat_update_original = nullptr;
        combat_bat_original = nullptr;
        combat_owner.store(nullptr, std::memory_order_release);
        static_cast<void>(combat_hook_backend->Uninitialize());
        combat_hook_backend.reset();
        return false;
    }

    // RpcSwing 结果只在客户端接收端可用；它是诊断/UI 反馈，安装失败
    // 不影响自动攻击本身。
    combat_swing_hook_created = false;
    if (combat_swing_target != nullptr) {
        const bool created_swing = combat_hook_backend->Create(
            combat_swing_target,
            reinterpret_cast<void*>(&Impl::BatRpcSwingThunk),
            &combat_swing_original);
        if (created_swing && combat_hook_backend->Enable(combat_swing_target)) {
            combat_swing_hook_created = true;
        } else {
            if (created_swing) {
                static_cast<void>(combat_hook_backend->Remove(combat_swing_target));
            }
            combat_swing_original = nullptr;
        }
    }

    // 远端陀螺的 NetworkTransform 发送钩子；安装失败不影响自动攻击。
    net_broadcast_hook_created = false;
    if (net_broadcast_target != nullptr) {
        const bool created_net = combat_hook_backend->Create(
            net_broadcast_target,
            reinterpret_cast<void*>(&Impl::NetTransformBroadcastThunk),
            &net_broadcast_original);
        if (created_net && combat_hook_backend->Enable(net_broadcast_target)) {
            net_broadcast_hook_created = true;
        } else {
            if (created_net) {
                static_cast<void>(combat_hook_backend->Remove(net_broadcast_target));
            }
            net_broadcast_original = nullptr;
        }
    }

    combat_hooks_installed = true;
    return true;
}

void MaydayAdapter::Impl::TeardownCombatHooks() noexcept {
    combat_enabled.store(false, std::memory_order_release);
    combat_gyro_enabled.store(false, std::memory_order_release);
    if (!combat_hooks_installed || combat_hook_backend == nullptr) return;
    if (combat_update_target != nullptr) {
        static_cast<void>(combat_hook_backend->Disable(combat_update_target));
    }
    if (combat_bat_target != nullptr) {
        static_cast<void>(combat_hook_backend->Disable(combat_bat_target));
    }
    if (combat_swing_hook_created && combat_swing_target != nullptr) {
        static_cast<void>(combat_hook_backend->Disable(combat_swing_target));
    }
    if (net_broadcast_hook_created && net_broadcast_target != nullptr) {
        static_cast<void>(combat_hook_backend->Disable(net_broadcast_target));
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (combat_in_callback.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    static_cast<void>(combat_hook_backend->Remove(combat_update_target));
    static_cast<void>(combat_hook_backend->Remove(combat_bat_target));
    if (combat_swing_hook_created && combat_swing_target != nullptr) {
        static_cast<void>(combat_hook_backend->Remove(combat_swing_target));
    }
    if (net_broadcast_hook_created && net_broadcast_target != nullptr) {
        static_cast<void>(combat_hook_backend->Remove(net_broadcast_target));
    }
    combat_owner.store(nullptr, std::memory_order_release);
    static_cast<void>(combat_hook_backend->Uninitialize());
    combat_hook_backend.reset();
    combat_hooks_installed = false;
    combat_swing_hook_created = false;
    net_broadcast_hook_created = false;
    combat_update_original = nullptr;
    combat_bat_original = nullptr;
    combat_swing_original = nullptr;
    net_broadcast_original = nullptr;
}

void ANOMALY_CALL MaydayAdapter::Impl::InteractorUpdateThunk(void* self) {
    Impl* const impl = combat_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->combat_in_callback.fetch_add(1, std::memory_order_acq_rel);
    try {
        impl->HandleInteractorUpdate(self);
    } catch (...) {
        // 插件逻辑异常不得穿透游戏 Update。
    }
    impl->combat_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

void ANOMALY_CALL MaydayAdapter::Impl::BatUseOnceThunk(
    void* self, void* player, void* target, const Vec3* hit_point) {
    Impl* const impl = combat_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->combat_in_callback.fetch_add(1, std::memory_order_acq_rel);
    try {
        impl->HandleBatUseOnce(self, player, target, hit_point);
    } catch (...) {
        // 静默瞄准失败时游戏仍需执行原始挥棒；HandleBatUseOnce 内部
        // 每条失败路径都已调用 original。
    }
    impl->combat_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

void ANOMALY_CALL MaydayAdapter::Impl::BatRpcSwingThunk(
    void* self, std::uint8_t result) {
    Impl* const impl = combat_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->combat_in_callback.fetch_add(1, std::memory_order_acq_rel);
    try {
        impl->HandleBatRpcSwing(self, result);
    } catch (...) {
        // 结果反馈失败不得影响游戏的挥击特效。
    }
    impl->combat_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

void ANOMALY_CALL MaydayAdapter::Impl::NetTransformBroadcastThunk(void* self) {
    Impl* const impl = combat_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->combat_in_callback.fetch_add(1, std::memory_order_acq_rel);
    try {
        impl->HandleNetTransformBroadcast(self);
    } catch (...) {
        // 网络旋转失败不得影响玩家的网络变换同步。
    }
    impl->combat_in_callback.fetch_sub(1, std::memory_order_acq_rel);
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
    void* ui_image{};
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
        } else if (std::strcmp(name, "UnityEngine.UI.dll") == 0 ||
                   std::strcmp(name, "UnityEngine.UI") == 0) {
            ui_image = candidate;
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
    if (ui_image != nullptr) {
        void* const event_system_class =
            class_from_name(ui_image, "UnityEngine.EventSystems", "EventSystem");
        event_system_update_target =
            FindMethodTarget(event_system_class, "Update", 0);
        if (class_get_field != nullptr && field_get_offset != nullptr &&
            event_system_class != nullptr) {
            event_system_module_field =
                class_get_field(event_system_class, "m_CurrentInputModule");
            if (event_system_module_field != nullptr) {
                event_system_module_offset = field_get_offset(event_system_module_field);
            }
        }
    }
    input_system_ui_module_class = class_from_name(
        input_image, "UnityEngine.InputSystem.UI", "InputSystemUIInputModule");
    if (input_system_ui_module_class != nullptr) {
        input_module_reset_pointers =
            FindMethodInfo(input_system_ui_module_class, "ResetPointers", 0);
    }

    input_block_resolved = true;
    return true;
}

void* MaydayAdapter::Impl::FindMethodInfo(
    void* klass, const char* name, int args) const {
    for (int depth = 0; klass != nullptr && depth < 8; ++depth) {
        if (class_get_method != nullptr) {
            void* const method = class_get_method(klass, name, args);
            if (method != nullptr) return method;
        }
        klass = class_get_parent != nullptr ? class_get_parent(klass) : nullptr;
    }
    return nullptr;
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

void ANOMALY_CALL MaydayAdapter::Impl::EventSystemUpdateThunk(void* self, void* method) {
    Impl* const impl = input_hook_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->input_hook_in_callback.fetch_add(1, std::memory_order_acq_rel);
    if (impl->event_system_module_offset != 0) {
        void* const module = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(self) + impl->event_system_module_offset);
        if (module != nullptr) {
            impl->event_system_module_kind.store(
                *reinterpret_cast<void**>(module) == impl->input_system_ui_module_class
                    ? 1 : 2,
                std::memory_order_release);
        }
    }
    // InputSystemUIInputModule 会在自己的指针状态里缓存按下/抬起；仅跳过
    // EventSystem.Update 不够，恢复时仍会补发一次点击。切换挡板时清空它。
    const auto reset_pointer_state = [impl, self]() {
        if (impl->event_system_module_offset == 0 ||
            impl->input_module_reset_pointers == nullptr ||
            impl->runtime_invoke == nullptr) {
            return;
        }
        void* const module = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(self) + impl->event_system_module_offset);
        if (module == nullptr) return;
        const bool is_input_system_module =
            *reinterpret_cast<void**>(module) == impl->input_system_ui_module_class;
        impl->event_system_module_kind.store(
            is_input_system_module ? 1 : 2, std::memory_order_release);
        if (!is_input_system_module) return;
        void* exception{};
        static_cast<void>(impl->runtime_invoke(
            impl->input_module_reset_pointers, module, nullptr, &exception));
    };
    bool dispatch = false;
    if (HostUiMenusCaptureMouse()) {
        if (!impl->event_system_was_blocked.exchange(true, std::memory_order_acq_rel)) {
            reset_pointer_state();
        }
    } else {
        const unsigned long long now = GetTickCount64();
        if (impl->event_system_was_blocked.exchange(false, std::memory_order_acq_rel)) {
            // 覆盖层刚关闭：清掉缓存，再吞掉关闭动作产生的按下/抬起脉冲。
            reset_pointer_state();
            impl->event_system_resume_after.store(
                now + 250, std::memory_order_release);
        }
        constexpr int kMouseButtons[]{
            VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        bool any_mouse_down = false;
        for (const int button : kMouseButtons) {
            if ((GetAsyncKeyState(button) & 0x8000) != 0) {
                any_mouse_down = true;
                break;
            }
        }
        if (any_mouse_down) {
            impl->event_system_resume_after.store(
                now + 100, std::memory_order_release);
        }
        dispatch = !any_mouse_down &&
            now >= impl->event_system_resume_after.load(std::memory_order_acquire);
    }
    if (dispatch) {
        auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(void*, void*)>(
            impl->event_system_update_original);
        if (original != nullptr) original(self, method);
    }
    impl->input_hook_in_callback.fetch_sub(1, std::memory_order_acq_rel);
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
        {event_system_update_target, reinterpret_cast<void*>(&Impl::EventSystemUpdateThunk),
         &event_system_update_original, false},
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
        mouse_scroll_delta_target, event_system_update_target};
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

// ---------------------------------------------------------------------------
// 暂停菜单入口：用游戏自己的 UIF 插入按钮，点击等价于按下 Insert
// ---------------------------------------------------------------------------
std::atomic<MaydayAdapter::Impl*> MaydayAdapter::Impl::pause_menu_owner{nullptr};

bool MaydayAdapter::Impl::ResolvePauseMenu() {
    if (pause_menu_resolved) return true;
    const auto fail = [this](int step) noexcept {
        pause_resolve_step.store(step, std::memory_order_release);
        return false;
    };
    if (runtime_invoke == nullptr || string_new == nullptr ||
        object_unbox == nullptr || class_from_name == nullptr ||
        class_get_method == nullptr || domain_get_assemblies == nullptr) {
        return fail(1);
    }
    if (!EnsureGameImage()) return fail(2);
    pause_screen_class = class_from_name(image, "Mayday.UI", "PauseScreen");
    void* const uif_class = class_from_name(image, "Mayday.UI", "UIF");
    void* const ui_button_class = class_from_name(image, "Mayday.UI", "UIButton");
    if (pause_screen_class == nullptr) return fail(3);
    if (uif_class == nullptr || ui_button_class == nullptr) return fail(4);
    void* const build_info = FindMethodInfo(pause_screen_class, "Build", 0);
    pause_get_invite_btn = FindMethodInfo(pause_screen_class, "get_inviteBtn", 0);
    pause_get_resume_btn = FindMethodInfo(pause_screen_class, "get_resumeBtn", 0);
    if (class_get_field != nullptr && field_get_offset != nullptr) {
        invite_btn_field = class_get_field(pause_screen_class, "<inviteBtn>k__BackingField");
        if (invite_btn_field == nullptr) {
            invite_btn_field = class_get_field(pause_screen_class, "inviteBtn");
        }
        if (invite_btn_field != nullptr) {
            invite_btn_offset = field_get_offset(invite_btn_field);
        }
        resume_btn_field = class_get_field(pause_screen_class, "<resumeBtn>k__BackingField");
        if (resume_btn_field == nullptr) {
            resume_btn_field = class_get_field(pause_screen_class, "resumeBtn");
        }
        if (resume_btn_field != nullptr) {
            resume_btn_offset = field_get_offset(resume_btn_field);
        }
    }
    uif_button_method = FindMethodInfo(uif_class, "Button", 10);
    void* const theme_class = class_from_name(image, "Mayday.UI", "UITheme");
    if (theme_class != nullptr) {
        ui_theme_button_height = FindMethodInfo(theme_class, "get_ButtonHeight", 0);
    }
    button_click_target = FindMethodTarget(ui_button_class, "OnPointerClick", 1);
    button_submit_target = FindMethodTarget(ui_button_class, "OnSubmit", 1);
    if (build_info == nullptr) return fail(5);
    if (uif_button_method == nullptr) return fail(6);
    if (button_click_target == nullptr) return fail(7);
    if (pause_get_invite_btn == nullptr && pause_get_resume_btn == nullptr &&
        invite_btn_field == nullptr && resume_btn_field == nullptr) {
        return fail(8);
    }
    pause_build_target = *reinterpret_cast<void**>(build_info);
    if (pause_build_target == nullptr) return fail(5);
    void* const open_info = FindMethodInfo(pause_screen_class, "OnOpen", 0);
    if (open_info != nullptr) pause_open_target = *reinterpret_cast<void**>(open_info);

    // UnityEngine.CoreModule：按钮与锚点按钮的 Transform 层级操作。
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
    if (core_image == nullptr) return fail(9);
    void* const transform_class = class_from_name(core_image, "UnityEngine", "Transform");
    void* const component_class = class_from_name(core_image, "UnityEngine", "Component");
    if (transform_class == nullptr || component_class == nullptr) return fail(10);
    if (component_get_transform == nullptr) {
        component_get_transform = class_get_method(component_class, "get_transform", 0);
    }
    transform_get_parent = class_get_method(transform_class, "get_parent", 0);
    transform_get_sibling_index = class_get_method(transform_class, "GetSiblingIndex", 0);
    transform_set_sibling_index = class_get_method(transform_class, "SetSiblingIndex", 1);
    transform_get_child_count = class_get_method(transform_class, "get_childCount", 0);
    component_get_gameobject = class_get_method(component_class, "get_gameObject", 0);
    void* const game_object_class = class_from_name(core_image, "UnityEngine", "GameObject");
    if (game_object_class != nullptr) {
        gameobject_get_active = class_get_method(game_object_class, "get_activeSelf", 0);
    }
    void* const rect_transform_class = class_from_name(core_image, "UnityEngine", "RectTransform");
    if (rect_transform_class != nullptr) {
        recttransform_get_size_delta = class_get_method(rect_transform_class, "get_sizeDelta", 0);
    }
    if (component_get_transform == nullptr) return fail(11);
    if (transform_get_parent == nullptr) return fail(12);
    if (transform_get_sibling_index == nullptr) return fail(13);
    // SetSiblingIndex 只影响插入位置；取不到时按钮仍会追加到父节点末尾。
    pause_menu_resolved = true;
    pause_stage.store(1, std::memory_order_release);
    return true;
}

void MaydayAdapter::Impl::InstallPauseMenuButton(void* screen) {
    if (screen == nullptr || uif_button_method == nullptr ||
        component_get_transform == nullptr || transform_get_parent == nullptr) {
        return;
    }
    if (pause_screen == screen && pause_button.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    pause_stage.store(3, std::memory_order_release);
    if (AttachedThread() == nullptr) {
        pause_failure.store(9, std::memory_order_release);
        return;
    }
    void* exception{};
    const auto read_button_field = [](void* object, std::size_t offset) noexcept -> void* {
        return object != nullptr && offset != 0
            ? *reinterpret_cast<void**>(
                  reinterpret_cast<std::uint8_t*>(object) + offset)
            : nullptr;
    };
    void* anchor{};
    // 继续游戏按钮始终位于可见的主按钮列；邀请按钮可能属于隐藏容器。
    if (pause_get_resume_btn != nullptr) {
        anchor = runtime_invoke(pause_get_resume_btn, screen, nullptr, &exception);
        if (exception != nullptr) {
            exception = nullptr;
            anchor = nullptr;
        }
        if (anchor != nullptr) pause_anchor.store(3, std::memory_order_release);
    }
    if (anchor == nullptr) {
        anchor = read_button_field(screen, resume_btn_offset);
        if (anchor != nullptr) pause_anchor.store(4, std::memory_order_release);
    }
    if (anchor == nullptr && pause_get_invite_btn != nullptr) {
        anchor = runtime_invoke(pause_get_invite_btn, screen, nullptr, &exception);
        if (exception != nullptr) {
            exception = nullptr;
            anchor = nullptr;
        }
        if (anchor != nullptr) pause_anchor.store(1, std::memory_order_release);
    }
    if (anchor == nullptr) {
        anchor = read_button_field(screen, invite_btn_offset);
        if (anchor != nullptr) pause_anchor.store(2, std::memory_order_release);
    }
    if (anchor == nullptr) {
        pause_failure.store(1, std::memory_order_release);
        return;
    }
    pause_stage.store(4, std::memory_order_release);
    void* const anchor_transform =
        runtime_invoke(component_get_transform, anchor, nullptr, &exception);
    if (exception != nullptr || anchor_transform == nullptr) {
        pause_failure.store(2, std::memory_order_release);
        return;
    }
    exception = nullptr;
    void* const parent =
        runtime_invoke(transform_get_parent, anchor_transform, nullptr, &exception);
    if (exception != nullptr || parent == nullptr) {
        pause_failure.store(3, std::memory_order_release);
        return;
    }
    exception = nullptr;
    pause_stage.store(5, std::memory_order_release);

    float height = 54.0f;
    if (ui_theme_button_height != nullptr) {
        void* const boxed =
            runtime_invoke(ui_theme_button_height, nullptr, nullptr, &exception);
        if (exception == nullptr && boxed != nullptr) {
            height = *reinterpret_cast<const float*>(object_unbox(boxed));
        }
        exception = nullptr;
    }

    void* const button_name = string_new("AnomalyMenuButton");
    void* const button_label = string_new("Anomaly");
    if (button_name == nullptr || button_label == nullptr) return;
    std::int32_t style = 1;  // ButtonStyle.Secondary
    float width = 0.0f;
    float font_size = -1.0f;
    float radius = -1.0f;
    // 空 Nullable<GlyphId>：全零即可，hasValue=false 时不读取 value。
    std::array<std::uint8_t, 16> glyph{};
    void* params[10] = {
        parent, button_name, button_label, nullptr, &style,
        &width, &height, static_cast<void*>(glyph.data()),
        &font_size, &radius};
    void* const button = runtime_invoke(uif_button_method, nullptr, params, &exception);
    if (exception != nullptr || button == nullptr) {
        pause_failure.store(4, std::memory_order_release);
        return;
    }
    exception = nullptr;
    pause_stage.store(6, std::memory_order_release);

    void* const button_transform =
        runtime_invoke(component_get_transform, button, nullptr, &exception);
    if (exception == nullptr && button_transform != nullptr &&
        transform_get_sibling_index != nullptr &&
        transform_set_sibling_index != nullptr) {
        std::int32_t index = 0;
        if (InvokeInt(transform_get_sibling_index, anchor_transform, &index)) {
            std::int32_t next = index + 1;
            static_cast<void>(InvokeVoidArg(
                transform_set_sibling_index, button_transform, &next));
        }
    }
    // 诊断：确认按钮对象确实激活且有非零尺寸。
    if (component_get_gameobject != nullptr && gameobject_get_active != nullptr) {
        void* const game_object =
            runtime_invoke(component_get_gameobject, button, nullptr, &exception);
        if (exception == nullptr && game_object != nullptr) {
            void* const boxed =
                runtime_invoke(gameobject_get_active, game_object, nullptr, &exception);
            if (exception == nullptr && boxed != nullptr) {
                pause_button_active.store(
                    *reinterpret_cast<const bool*>(object_unbox(boxed)) ? 1 : 0,
                    std::memory_order_release);
            }
        }
        exception = nullptr;
    }
    if (recttransform_get_size_delta != nullptr && button_transform != nullptr) {
        void* const boxed = runtime_invoke(
            recttransform_get_size_delta, button_transform, nullptr, &exception);
        if (exception == nullptr && boxed != nullptr) {
            const Vec2 size = *reinterpret_cast<const Vec2*>(object_unbox(boxed));
            pause_button_width_milli.store(
                static_cast<int>(size.x * 1000.0F), std::memory_order_release);
            pause_button_height_milli.store(
                static_cast<int>(size.y * 1000.0F), std::memory_order_release);
        }
        exception = nullptr;
    }
    if (transform_get_child_count != nullptr && parent != nullptr) {
        std::int32_t child_count = 0;
        if (InvokeInt(transform_get_child_count, parent, &child_count)) {
            pause_parent_children.store(child_count, std::memory_order_release);
        }
    }
    pause_screen = screen;
    pause_button.store(button, std::memory_order_release);
    pause_stage.store(7, std::memory_order_release);
}

void ANOMALY_CALL MaydayAdapter::Impl::PauseBuildThunk(void* self, void* method) {
    Impl* const impl = pause_menu_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->pause_menu_in_callback.fetch_add(1, std::memory_order_acq_rel);
    auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(void*, void*)>(
        impl->pause_build_original);
    if (original != nullptr) original(self, method);
    impl->InstallPauseMenuButton(self);
    impl->pause_menu_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

void ANOMALY_CALL MaydayAdapter::Impl::PauseOpenThunk(void* self, void* method) {
    Impl* const impl = pause_menu_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->pause_menu_in_callback.fetch_add(1, std::memory_order_acq_rel);
    auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(void*, void*)>(
        impl->pause_open_original);
    if (original != nullptr) original(self, method);
    impl->InstallPauseMenuButton(self);
    impl->pause_menu_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

void ANOMALY_CALL MaydayAdapter::Impl::ButtonClickThunk(
    void* self, void* event_data, void* method) {
    Impl* const impl = pause_menu_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->pause_menu_in_callback.fetch_add(1, std::memory_order_acq_rel);
    if (self == impl->pause_button.load(std::memory_order_acquire)) {
        // 与 Insert 展开路径一致：请求宿主展示并展开管理界面。
        RequestHostUiManagementExpansion();
    } else {
        auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(void*, void*, void*)>(
            impl->button_click_original);
        if (original != nullptr) original(self, event_data, method);
    }
    impl->pause_menu_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

void ANOMALY_CALL MaydayAdapter::Impl::ButtonSubmitThunk(
    void* self, void* event_data, void* method) {
    Impl* const impl = pause_menu_owner.load(std::memory_order_acquire);
    if (impl == nullptr) return;
    impl->pause_menu_in_callback.fetch_add(1, std::memory_order_acq_rel);
    if (self == impl->pause_button.load(std::memory_order_acquire)) {
        RequestHostUiManagementExpansion();
    } else {
        auto* const original = reinterpret_cast<void (ANOMALY_CALL*)(void*, void*, void*)>(
            impl->button_submit_original);
        if (original != nullptr) original(self, event_data, method);
    }
    impl->pause_menu_in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

bool MaydayAdapter::Impl::EnsurePauseMenuHook() {
    std::scoped_lock lock(resolve_mutex);
    if (pause_menu_hooks_installed) return true;
    if (pause_menu_hook_shutdown) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now < pause_menu_retry_after) return false;
    if (!ResolvePauseMenu()) {
        pause_menu_retry_after = now + std::chrono::seconds(1);
        return false;
    }
    if (pause_menu_hook_backend == nullptr) {
        pause_menu_hook_backend = CreateMinHookBackend();
        if (pause_menu_hook_backend == nullptr || !pause_menu_hook_backend->Initialize()) {
            pause_menu_hook_backend.reset();
            pause_menu_retry_after = now + std::chrono::seconds(1);
            return false;
        }
    }
    struct Entry final {
        void* target;
        void* detour;
        void** original;
        bool required;
    };
    const Entry entries[] = {
        {pause_build_target, reinterpret_cast<void*>(&Impl::PauseBuildThunk),
         &pause_build_original, true},
        {pause_open_target, reinterpret_cast<void*>(&Impl::PauseOpenThunk),
         &pause_open_original, false},
        {button_click_target, reinterpret_cast<void*>(&Impl::ButtonClickThunk),
         &button_click_original, true},
        {button_submit_target, reinterpret_cast<void*>(&Impl::ButtonSubmitThunk),
         &button_submit_original, false},
    };
    pause_menu_owner.store(this, std::memory_order_release);
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
        if (!pause_menu_hook_backend->Create(
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
            if (!pause_menu_hook_backend->Enable(entries[i].target) &&
                entries[i].required) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        for (std::size_t i = 0; i < std::size(entries); ++i) {
            if (created[i]) {
                static_cast<void>(pause_menu_hook_backend->Remove(entries[i].target));
            }
        }
        pause_menu_owner.store(nullptr, std::memory_order_release);
        static_cast<void>(pause_menu_hook_backend->Uninitialize());
        pause_menu_hook_backend.reset();
        pause_menu_retry_after = now + std::chrono::seconds(1);
        return false;
    }
    pause_menu_hooks_installed = true;
    pause_stage.store(2, std::memory_order_release);
    return true;
}

void MaydayAdapter::Impl::TeardownPauseMenuHook() noexcept {
    void* const targets[] = {
        pause_build_target, pause_open_target,
        button_click_target, button_submit_target};
    if (pause_menu_hook_backend != nullptr && pause_menu_hooks_installed) {
        for (void* const target : targets) {
            if (target != nullptr) static_cast<void>(pause_menu_hook_backend->Disable(target));
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (pause_menu_in_callback.load(std::memory_order_acquire) != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (void* const target : targets) {
            if (target != nullptr) static_cast<void>(pause_menu_hook_backend->Remove(target));
        }
    }
    pause_menu_owner.store(nullptr, std::memory_order_release);
    if (pause_menu_hook_backend != nullptr) {
        static_cast<void>(pause_menu_hook_backend->Uninitialize());
        pause_menu_hook_backend.reset();
    }
    pause_menu_hooks_installed = false;
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
    void* const weather = weather_published.exchange(nullptr, std::memory_order_acq_rel);
    if (weather != nullptr) {
        static_cast<void>(ProcessAdapterServices().Revoke(kWeatherServiceId, weather));
    }
    void* const combat = combat_published.exchange(nullptr, std::memory_order_acq_rel);
    if (combat != nullptr) {
        static_cast<void>(ProcessAdapterServices().Revoke(kCombatServiceId, combat));
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
        // 输入阻断与暂停菜单 hook 在渲染线程可能仍尝试安装；先与安装路径
        // 串行化再拆除，关闭后不得再重装。
        std::scoped_lock lock(impl_->resolve_mutex);
        impl_->input_hook_shutdown = true;
        impl_->TeardownInputBlockHook();
        impl_->pause_menu_hook_shutdown = true;
        impl_->TeardownPauseMenuHook();
        impl_->combat_shutdown.store(true, std::memory_order_release);
        impl_->TeardownCombatHooks();
    }
    // 等探测线程走完当前轮次（含全部 Publish）再回收服务表：旧的“只 sleep
    // 一次”兜底允许 RevokePublished 之后探测线程仍发布新服务，服务表会残留
    // 悬垂表项。等待上限 2 秒；超时后仍继续拆卸，Impl 析构会 join 兜底。
    const auto probe_deadline = std::chrono::steady_clock::now() +
        (std::min)(timeout, std::chrono::milliseconds(2000));
    while (!impl_->probe_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < probe_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
    bool hook_removed{true};
    {
        // freecam hook 的安装路径持 resolve_mutex（FreecamSetActive），拆卸
        // 也必须在同一把锁下进行；hook_installed 由该锁保护。
        std::scoped_lock lock(impl_->resolve_mutex);
        if (impl_->hook_installed && impl_->hook_backend != nullptr) {
            hook_removed = impl_->hook_backend->Disable(impl_->look_update_target);
            const auto drain_deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (impl_->freecam_in_callback.load(std::memory_order_acquire) != 0 &&
                   std::chrono::steady_clock::now() < drain_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            hook_removed = impl_->hook_backend->Remove(impl_->look_update_target) &&
                hook_removed;
            impl_->freecam_owner.store(nullptr, std::memory_order_release);
            static_cast<void>(impl_->hook_backend->Uninitialize());
            impl_->hook_backend.reset();
            impl_->hook_installed = false;
        }
    }
    impl_->RevokePublished();
    // tick 回调未在预算内排空说明游戏线程仍在执行回调：失败透传给调用方
    // （core_main 走 stop_deferred 分支），不按成功落账。
    const bool tick_drained = ClearTickCallback(std::chrono::seconds(2));
    return tick_drained && hook_removed;
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
    // Hook 安装与 il2cpp 扫描由探测/维护线程推进；渲染线程只读取已发布的
    // 状态并驱动插件 tick，不在 Present 中遍历程序集或安装 Hook。
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
         << ",\"weatherServicePublished\":"
         << (impl_->weather_published.load(std::memory_order_acquire) != nullptr ? "true" : "false")
         << ",\"weatherResolved\":"
         << (impl_->weather_resolved.load(std::memory_order_acquire) ? "true" : "false")
         << ",\"weatherResolveStep\":" << impl_->weather_resolve_step.load(std::memory_order_acquire)
         << ",\"inputBlockHooks\":"
         << (impl_->input_hook_installed.load(std::memory_order_acquire) ? "true" : "false")
         << ",\"eventSystemHook\":"
         << (impl_->event_system_update_target != nullptr ? "true" : "false")
         << ",\"uiInputModule\":" << impl_->event_system_module_kind.load(std::memory_order_acquire)
         << ",\"pauseStage\":" << impl_->pause_stage.load(std::memory_order_acquire)
         << ",\"pauseFailure\":" << impl_->pause_failure.load(std::memory_order_acquire)
         << ",\"pauseResolve\":" << impl_->pause_resolve_step.load(std::memory_order_acquire)
         << ",\"pauseAnchor\":" << impl_->pause_anchor.load(std::memory_order_acquire)
         << ",\"pauseButtonActive\":"
         << impl_->pause_button_active.load(std::memory_order_acquire)
         << ",\"pauseButtonWidth\":"
         << impl_->pause_button_width_milli.load(std::memory_order_acquire) / 1000.0
         << ",\"pauseButtonHeight\":"
         << impl_->pause_button_height_milli.load(std::memory_order_acquire) / 1000.0
         << ",\"pauseParentChildren\":"
         << impl_->pause_parent_children.load(std::memory_order_acquire)
         << ",\"pauseButton\":"
         << (impl_->pause_button.load(std::memory_order_acquire) != nullptr ? "true" : "false")
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

void MaydayProfileRuntime::SetLogSink(LogSink sink) {
    if (impl_->adapter != nullptr) impl_->adapter->SetLogSink(std::move(sink));
}

MaydayProfileRuntime::~MaydayProfileRuntime() = default;

bool MaydayProfileRuntime::Start(std::stop_token stop_token) {
    return impl_->adapter->Start(std::move(stop_token));
}

bool MaydayProfileRuntime::Stop(std::chrono::milliseconds timeout) noexcept {
    if (impl_->adapter != nullptr) {
        // 适配层停止失败（tick 回调/hook 拆卸超时）透传给调用方，让
        // core_main 的 stop_deferred 分支可以按真实结果处理。
        return impl_->adapter->Stop(timeout);
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
