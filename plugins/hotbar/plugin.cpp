// 物品栏扩展插件：把游戏的 4 槽物品栏扩到 8 槽。
// 全部改动点由兼容 Profile 的特征码定位并按入口字节校验，代码不持有任何地址：
// 服务器侧三处补丁（物品栏生成槽位数、CmdSelect 服务端与分发两处选择上界）
// 在宿主进程充当服务器时对全部玩家生效——未安装本插件的加入者无需安装任何
// 额外内容即可携带 8 槽（Mirror SyncList 按列表整体同步，与客户端 UI 槽数无关）；
// 客户端侧两处补丁与一个 hook（原生物品栏槽位数组、滚轮取模、HotbarDown
// 检测按键 5-8）只影响本机进程。加入者不装插件时仍保持 4 槽界面与 1-4 按键，
// 属于向下兼容的降级语义（HotbarWidget.Tick 对 active 越界有边界检查，已核实）。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/profile.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/json.h"
#include "anomaly/sdk/services/ui.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// GetAsyncKeyState / GetActiveWindow；user32 由插件 target 单独链接。
#include <windows.h>

namespace {

constexpr std::string_view kProfileFile = "compatibility.profile.json";
constexpr std::string_view kProfileContract = "mayday.hotbar-slots";
constexpr std::string_view kProfileLayoutSlots = "slotCount";
constexpr std::uint32_t kVanillaSlots = 4;

// 与 profile/compatibility.profile.json 的 symbols[].name 一一对应。
constexpr std::string_view kSymSpawnSlots = "inventory_spawn_slots";
constexpr std::string_view kSymSelectUserCode = "inventory_cmdselect_usercode";
constexpr std::string_view kSymSelectInvoke = "inventory_cmdselect_invoke";
constexpr std::string_view kSymWheelModulo = "interactor_wheel_modulo";
constexpr std::string_view kSymHotbarUi = "hotbar_widget_ctor";
constexpr std::string_view kSymHotbarDown = "game_input_hotbar_down";
constexpr std::string_view kSymInputBlocked = "game_input_blocked";

// 各补丁点相对特征码命中地址的字节偏移与替换内容（按 Profile 槽数生成）：
// - inventory_spawn_slots:        循环上界 cmp ebx,4 → cmp ebx,N
// - inventory_cmdselect_usercode: 服务端选择上界 cmp edx,3 → cmp edx,N-1
// - inventory_cmdselect_invoke:   分发层选择上界 cmp edi,3 → cmp edi,N-1
// - interactor_wheel_modulo:      滚轮取模 (x+4)&3 → (x+N)&(N-1)。替换 9 字节：
//   add edx,N 三字节 + and edx,N-1 三字节 + 三条 NOP（原 6 字节 and 截短）。
//   新序列恒为非负，其后的符号修正分支不再进入，保留为死代码。
// - hotbar_widget_ctor:           HUD 槽位数组 new Slot[4] → new Slot[N]，
//   Build/Tick 按数组长度遍历，无需其它改动。
constexpr std::size_t kSpawnImmediateOffset = 18;
constexpr std::size_t kSelectUserCodeOffset = 2;
constexpr std::size_t kSelectInvokeOffset = 9;
constexpr std::size_t kWheelOffset = 2;
constexpr std::size_t kWheelSize = 9;
constexpr std::size_t kCtorImmediateOffset = 1;

const AnomalyHostApiV1* g_host{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyHookServiceV1* g_hook{};
const AnomalyPatchServiceV1* g_patch{};
anomaly::sdk::Localizer g_localizer;
anomaly::sdk::ProfileSnapshot g_profile;

// 诊断日志：定位特征码/补丁/hook 各阶段失败原因（失败原因也进界面）。
void PluginLog(const char* message) {
    if (g_core == nullptr || g_core->log == nullptr) return;
    g_core->log(g_core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                anomaly::sdk::StringView(message));
}

std::uint32_t g_slot_count{};

using HotbarDownOriginal = std::int32_t (ANOMALY_CALL*)(void*);
using InputBlockedFn = std::uint8_t (ANOMALY_CALL*)(void*);

enum PatchId : std::size_t { PatchSpawn, PatchUserCode, PatchInvoke, PatchWheel, PatchUi, PatchCount };
struct PatchState {
    AnomalyGenerationHandleV1 handle{};
    bool applied{};
};
PatchState g_patches[PatchCount]{};
AnomalyGenerationHandleV1 g_hook_handle{};
std::uintptr_t g_original{};
InputBlockedFn g_blocked_fn{};
bool g_symbols_ok{};
bool g_profile_ok{};   // ProfileSnapshot 无 operator bool，加载结果自记。
std::string g_error;  // 稳定诊断 key（reason.*），由界面翻译。
// Draw 在渲染线程每帧调用；detour 在游戏主线程调用。用宿主 UI 的帧状态
// 判定「宿主界面正在捕获键盘」（管理面板/文本输入），此时抑制 5-8 注入，
// 与游戏 1-4 键被宿主输入阻断保持一致。
std::atomic<std::uint32_t> g_ui_frame_state{0};

// 按键 5-8：与游戏 1-4 键同为「按住即选中」语义，按槽位顺序扫描。
// HandleHotbar 已核实只对本地玩家执行（Update 开头 isLocalPlayer 门控），
// Mirror 的 Command 所有权检查同时保证远程对象不会误发命令。
std::int32_t ExtraKeySlot() {
    const bool focused = GetActiveWindow() != nullptr;
    const auto frame_state = g_ui_frame_state.load(std::memory_order_relaxed);
    // 宿主浮窗常驻使 WANT_CAPTURE_KEYBOARD 恒为真，不能作为抑制条件；
    // 只在文本输入进行中（管理面板搜索框/输入框正在打字）时抑制注入。
    const bool typing = (frame_state & ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT) != 0;
    if (!focused || typing) {
        return -1;
    }
    for (std::uint32_t slot = kVanillaSlots; slot < g_slot_count; ++slot) {
        const short state =
            GetAsyncKeyState(static_cast<int>('1') + static_cast<int>(slot));
        if ((state & 0x8000) != 0) {
            static std::int32_t last_logged{-1};
            if (last_logged != static_cast<std::int32_t>(slot)) {
                last_logged = static_cast<std::int32_t>(slot);
                const std::string line =
                    "hotbar: extra key slot=" + std::to_string(slot);
                PluginLog(line.c_str());
            }
            return static_cast<std::int32_t>(slot);
        }
    }
    return -1;
}

std::int32_t ANOMALY_CALL HotbarDownThunk(void* method) {
    AnomalyGenerationHandleV1 lease{};
    // generation 失效（插件停止中）时不得触碰 trampoline。
    if (g_hook == nullptr || g_hook_handle.id == 0 ||
        !anomaly::sdk::Succeeded(
            g_hook->begin_callback(g_hook->user, g_hook_handle, &lease))) {
        static std::atomic<int> miss_count{0};
        if (miss_count.fetch_add(1, std::memory_order_relaxed) < 3) {
            PluginLog("hotbar: detour lease unavailable");
        }
        return -1;
    }
    std::int32_t result = -1;
    const auto original = reinterpret_cast<HotbarDownOriginal>(g_original);
    if (original != nullptr) {
        result = original(method);
        // 原函数返回 -1 表示「未按 1-4 或被游戏屏蔽」：先尊重游戏自身的
        // 输入屏蔽（聊天/暂停等），再补充检测 5-8。
        if (result == -1 && g_blocked_fn != nullptr &&
            g_blocked_fn(nullptr) == 0) {
            result = ExtraKeySlot();
        }
    }
    static_cast<void>(g_hook->end_callback(g_hook->user, lease));
    return result;
}

bool ApplyMemoryPatch(
    const PatchId id, const std::uintptr_t address, const std::uint8_t* bytes,
    const std::size_t size, const char* label) {
    AnomalyGenerationHandleV1 handle{};
    const auto status = g_patch->apply(
        g_patch->user, address, {bytes, size}, anomaly::sdk::StringView(label),
        &handle);
    if (!anomaly::sdk::Succeeded(status)) return false;
    g_patches[id] = {handle, true};
    return true;
}

void ReleaseAppliedPatches() {
    if (g_patch == nullptr) return;
    for (PatchState& patch : g_patches) {
        if (patch.handle.id != 0) {
            static_cast<void>(g_patch->release(g_patch->user, patch.handle));
        }
        patch = {};
    }
}

void ReleaseHook() {
    if (g_hook != nullptr && g_hook_handle.id != 0) {
        static_cast<void>(g_hook->release(g_hook->user, g_hook_handle));
    }
    g_hook_handle = {};
    g_original = 0;
    g_blocked_fn = nullptr;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    g_host = host;
    g_localizer = anomaly::sdk::Localizer(host);
    const anomaly::sdk::Host view(host);
    const auto core = view.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);
    const auto hook =
        view.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID);
    const auto patch =
        view.Query<AnomalyPatchServiceV1>(ANOMALY_PATCH_SERVICE_V1_ID);
    if (!hook || !patch) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_core = core.get();
    g_hook = hook.get();
    g_patch = patch.get();
    // Profile 解析失败不阻断加载：界面给出原因，Start 直接跳过。
    constexpr anomaly::sdk::ProfileContract kContract{
        "mayday", "GameAssembly.dll", "il2cpp", kProfileContract};
    g_profile_ok = g_profile.Load(host, kProfileFile, kContract, {}, true);
    if (!g_profile_ok) {
        g_error = "reason.profile";
        PluginLog("hotbar: profile load failed");
    } else {
        PluginLog("hotbar: profile loaded");
    }
    *context = &g_slot_count;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    if (g_host == nullptr || !g_profile_ok) return anomaly::sdk::Ok();
    g_error.clear();

    // 槽数布局：按键映射（1-8）与补丁字节都按 8 设计，其它取值一律停用功能，
    // 不猜测布局。要求 2 的幂（滚轮补丁用 and 掩码实现取模）。
    std::uint32_t slot_count = 0;
    try {
        slot_count = g_profile.Value(kProfileLayoutSlots, 16);
    } catch (...) {
        g_error = "reason.slots";
        return anomaly::sdk::Ok();
    }
    if (slot_count != kVanillaSlots * 2 ||
        (slot_count & (slot_count - 1)) != 0) {
        g_error = "reason.slots";
        return anomaly::sdk::Ok();
    }
    g_slot_count = slot_count;

    // 解析全部符号；ResolveProfileSymbol 已做唯一候选与入口字节校验。
    std::uintptr_t spawn{};
    std::uintptr_t user_code{};
    std::uintptr_t invoke{};
    std::uintptr_t wheel{};
    std::uintptr_t ui_ctor{};
    std::uintptr_t hotbar_down{};
    std::uintptr_t blocked{};
    struct NamedTarget {
        std::string_view name;
        std::uintptr_t* address;
    };
    const NamedTarget targets[] = {
        {kSymSpawnSlots, &spawn},         {kSymSelectUserCode, &user_code},
        {kSymSelectInvoke, &invoke},      {kSymWheelModulo, &wheel},
        {kSymHotbarUi, &ui_ctor},         {kSymHotbarDown, &hotbar_down},
        {kSymInputBlocked, &blocked},
    };
    g_symbols_ok = true;
    for (const NamedTarget& target : targets) {
        // Load 阶段已完成批量解析与入口字节校验，这里只取缓存地址。
        *target.address = g_profile.Address(target.name);
        if (*target.address == 0) {
            g_symbols_ok = false;
            if (g_error.empty()) g_error = "reason.symbols";
            PluginLog(("hotbar: unresolved " + std::string(target.name)).c_str());
        }
    }
    if (!g_symbols_ok) {
        PluginLog("hotbar: symbol resolution failed");
        return anomaly::sdk::Ok();
    }

    // 应用补丁：任何一处失败即停止，已应用的句柄由 Stop 释放还原。
    const auto n = static_cast<std::uint8_t>(slot_count);
    const auto last = static_cast<std::uint8_t>(slot_count - 1);
    const std::uint8_t immediate[1] = {n};
    const std::uint8_t select_bound[1] = {last};
    const std::uint8_t wheel_bytes[kWheelSize] = {
        0x83, 0xC2, n, 0x83, 0xE2, last, 0x90, 0x90, 0x90};
    struct PatchRequest {
        PatchId id;
        std::uintptr_t address;
        const std::uint8_t* bytes;
        std::size_t size;
        const char* label;
    };
    const PatchRequest requests[] = {
        {PatchSpawn, spawn + kSpawnImmediateOffset, immediate, 1,
         "hotbar.spawn_slots"},
        {PatchUserCode, user_code + kSelectUserCodeOffset, select_bound, 1,
         "hotbar.select_usercode"},
        {PatchInvoke, invoke + kSelectInvokeOffset, select_bound, 1,
         "hotbar.select_invoke"},
        {PatchWheel, wheel + kWheelOffset, wheel_bytes, kWheelSize,
         "hotbar.wheel_modulo"},
        {PatchUi, ui_ctor + kCtorImmediateOffset, immediate, 1,
         "hotbar.ui_slots"},
    };
    bool all_patched = true;
    for (const PatchRequest& request : requests) {
        if (!ApplyMemoryPatch(request.id, request.address, request.bytes,
                              request.size, request.label)) {
            all_patched = false;
            if (g_error.empty()) g_error = "reason.patch";
            PluginLog("hotbar: patch failed");
        }
    }
    if (all_patched) {
        PluginLog("hotbar: patches applied");
    }

    // 按键 hook：失败只影响 5-8 键，其余补丁保持生效。
    AnomalyHookRequestV1 hook_request{};
    hook_request.struct_size = sizeof(hook_request);
    hook_request.kind = ANOMALY_HOOK_V1_FUNCTION;
    hook_request.target = hotbar_down;
    hook_request.detour = reinterpret_cast<void*>(&HotbarDownThunk);
    hook_request.label = anomaly::sdk::StringView("hotbar.get_hotbar_down");
    std::uintptr_t original{};
    AnomalyGenerationHandleV1 hook_handle{};
    const auto hook_status =
        g_hook->create(g_hook->user, &hook_request, &original, &hook_handle);
    if (anomaly::sdk::Succeeded(hook_status) && original != 0) {
        g_original = original;
        g_hook_handle = hook_handle;
        g_blocked_fn = reinterpret_cast<InputBlockedFn>(blocked);
        PluginLog("hotbar: hook installed");
    } else {
        if (g_error.empty()) g_error = "reason.hook";
        PluginLog("hotbar: hook failed");
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    // 先摘 hook 再还原补丁：hook 目标与补丁点不重叠，顺序仅为可读性。
    ReleaseHook();
    ReleaseAppliedPatches();
    g_error.clear();
    g_symbols_ok = false;
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    g_host = nullptr;
    g_hook = nullptr;
    g_patch = nullptr;
    g_profile = {};
    g_slot_count = 0;
    g_error.clear();
    g_symbols_ok = false;
    g_profile_ok = false;
    g_ui_frame_state.store(0, std::memory_order_relaxed);
}

void ANOMALY_CALL Update(void*, double) {
    // 状态在 Start/Stop 维护，补丁与 hook 自主生效；无每帧工作。
}

void DrawTargetRow(
    const AnomalyUiServiceV1* ui, const std::string_view key,
    const std::string_view fallback, const bool applied) {
    ui->table_next_row(ui->user);
    ui->table_next_column(ui->user);
    ui->text(ui->user, anomaly::sdk::StringView(g_localizer.Text(key, fallback)));
    ui->table_next_column(ui->user);
    const std::string_view status_key = applied ? "status.applied" : "status.missing";
    const std::string_view status_fallback = applied ? "applied" : "not found";
    ui->text(ui->user, anomaly::sdk::StringView(g_localizer.Text(status_key, status_fallback)));
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) return;
    // 帧状态缓存供 detour（游戏线程）读取：宿主捕获键盘时抑制 5-8。
    if (ui->frame_state != nullptr) {
        g_ui_frame_state.store(ui->frame_state(ui->user), std::memory_order_relaxed);
    }
    // 仅首次使用时给出默认尺寸，之后尊重用户拖拽（ImGui FirstUseEver）。
    if (ui->set_next_window_size != nullptr) {
        ui->set_next_window_size(ui->user, 440.0f, 300.0f, 4);
    }
    int open = 1;
    const std::string title = g_localizer.Text("window.title", "Hotbar Slots");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    // 状态行：失败（带原因）/ 部分生效 / 全部生效。
    const bool patches_ready =
        g_patches[PatchSpawn].applied && g_patches[PatchUserCode].applied &&
        g_patches[PatchInvoke].applied && g_patches[PatchWheel].applied &&
        g_patches[PatchUi].applied;
    const bool hook_ready = g_hook_handle.id != 0;
    if (!g_error.empty()) {
        // 原因是稳定诊断 key（reason.*），在这里翻译；未知 key 回退通用文案。
        const std::string reason =
            g_localizer.Text(g_error, "the operation could not complete");
        const std::string_view args[]{reason};
        ui->text(ui->user, anomaly::sdk::StringView(g_localizer.Format(
                               "state.failed", "Not applied: {0}",
                               std::span<const std::string_view>{args})));
    } else if (!g_profile_ok) {
        ui->text(ui->user, anomaly::sdk::StringView(
                               g_localizer.Text("reason.profile",
                                                "compatibility profile did not load")));
    } else if (patches_ready && hook_ready) {
        const std::string slots = std::to_string(g_slot_count);
        const std::string_view args[]{slots};
        ui->text(ui->user, anomaly::sdk::StringView(g_localizer.Format(
                               "state.active", "Extended to {0} slots.",
                               std::span<const std::string_view>{args})));
    } else {
        ui->text(ui->user, anomaly::sdk::StringView(
                               g_localizer.Text("state.partial",
                                                "Partially applied; some targets failed.")));
    }

    ui->separator(ui->user);
    if (ui->begin_table(ui->user, anomaly::sdk::StringView("##targets"), 2, 0, 0.0f, 0.0f)) {
        DrawTargetRow(ui, "target.spawn", "Inventory spawn slots (server)",
                      g_patches[PatchSpawn].applied);
        DrawTargetRow(ui, "target.select", "Slot select bounds (server)",
                      g_patches[PatchUserCode].applied && g_patches[PatchInvoke].applied);
        DrawTargetRow(ui, "target.ui", "Native hotbar UI", g_patches[PatchUi].applied);
        DrawTargetRow(ui, "target.scroll", "Wheel cycling", g_patches[PatchWheel].applied);
        DrawTargetRow(ui, "target.keys", "Keys 5-8 (hook)", hook_ready);
        ui->end_table(ui->user);
    }

    ui->separator(ui->user);
    const auto text_wrapped = ui->text_wrapped;
    const auto draw_help = [&](const std::string_view key,
                               const std::string_view fallback) {
        const std::string line = g_localizer.Text(key, fallback);
        if (text_wrapped != nullptr) {
            text_wrapped(ui->user, anomaly::sdk::StringView(line));
        } else {
            ui->text(ui->user, anomaly::sdk::StringView(line));
        }
    };
    draw_help("help.controls",
              "Select slots with keys 1-8 or the mouse wheel. Keys 5-8 are "
              "added by this plugin and respect the game's input blocking.");
    draw_help("help.multiplayer",
              "Multiplayer: while the host runs this plugin, joiners need no "
              "extra install - their inventory already holds 8 slots "
              "server-side. A vanilla joiner still sees 4 slots; pickups "
              "beyond the first four go to hidden slots that drop on death.");
    draw_help("help.reentry",
              "Enable before entering a flight. Inventories spawned before "
              "this plugin started keep 4 slots until the next flight.");
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("mayday.hotbar"),
        anomaly::sdk::StringView("Hotbar Slots"),
        anomaly::sdk::StringView("Mayday Loader"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
