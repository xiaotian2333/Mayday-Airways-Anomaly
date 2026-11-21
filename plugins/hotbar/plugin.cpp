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
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

// GetAsyncKeyState / GetActiveWindow；user32 由插件 target 单独链接。
#include <windows.h>

namespace {

constexpr std::string_view kProfileFile = "compatibility.profile.json";
constexpr std::string_view kProfileContract = "mayday.hotbar-slots";
// 槽数量固定，不做用户配置：实际槽数取 profile 的兼容上限（本构建为 10）。
constexpr std::string_view kProfileLayoutMaxSlots = "maxSlotCount";
constexpr std::string_view kProfileLayoutPivotMilli = "slotPivotXMilli";
constexpr std::uint32_t kVanillaSlots = 4;
// 插件不提供槽数配置：槽数量固定取 profile 兼容上限（本构建 10）。
constexpr std::uint32_t kSlotCount = 10;

// 与 profile/compatibility.profile.json 的 symbols[].name 一一对应。
constexpr std::string_view kSymSpawnSlots = "inventory_spawn_slots";
constexpr std::string_view kSymSelectUserCode = "inventory_cmdselect_usercode";
constexpr std::string_view kSymSelectInvoke = "inventory_cmdselect_invoke";
constexpr std::string_view kSymWheelModulo = "interactor_wheel_modulo";
constexpr std::string_view kSymHotbarUi = "hotbar_widget_ctor";
constexpr std::string_view kSymSlotPivot = "hotbar_slot_pivot";
constexpr std::string_view kSymHotbarDown = "game_input_hotbar_down";
constexpr std::string_view kSymInputBlocked = "game_input_blocked";

// 各补丁点相对特征码命中地址的字节偏移与替换内容（按配置槽数 N 生成）：
// - inventory_spawn_slots:        循环上界 cmp ebx,4 → cmp ebx,N
// - inventory_cmdselect_usercode: 服务端选择上界 cmp edx,3 → cmp edx,N-1
// - inventory_cmdselect_invoke:   分发层选择上界 cmp edi,3 → cmp edi,N-1
// - interactor_wheel_modulo:      滚轮取模 (x+4)%4 → (x+N)%N。任意 N（含非 2 的
//   幂）都走除法取模，替换 18 字节（原 add+and+jge+dec+or+inc 全部覆盖）：
//   add edx,N / push rax / mov eax,edx / xor edx,edx / mov ecx,N / div ecx /
//   pop rax / nop×2。rax（inventory 指针）用 push/pop 保护，rcx 会被其后
//   的 mov rcx,[rax+0F0h] 重写，edx 余数即 CmdSelect 的槽位实参。
// - hotbar_widget_ctor:           HUD 槽位数组 new Slot[4] → new Slot[N]，
//   Build/Tick 按数组长度遍历，无需其它改动。
constexpr std::size_t kSpawnImmediateOffset = 18;
constexpr std::size_t kSelectUserCodeOffset = 2;
constexpr std::size_t kSelectInvokeOffset = 9;
constexpr std::size_t kWheelOffset = 2;
constexpr std::size_t kWheelSize = 18;
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

std::uint32_t g_slot_count{};  // 生效槽数（profile 兼容上限，本构建固定 10）
std::uint32_t g_max_slots{};   // profile 声明的槽数；0 表示无法确定（停用）
std::uint32_t g_slot_pivot_milli{500};  // 槽行 pivot.x×1000（原版 0.5，居中值调高）
// Load 阶段解析并校验后的符号地址（ApplyForCount 复用，无需重新解析）。
std::uintptr_t g_symbol_spawn{};
std::uintptr_t g_symbol_user_code{};
std::uintptr_t g_symbol_invoke{};
std::uintptr_t g_symbol_wheel{};
std::uintptr_t g_symbol_ui_ctor{};
std::uintptr_t g_symbol_slot_pivot{};
std::uintptr_t g_symbol_hotbar_down{};
std::uintptr_t g_symbol_blocked{};

using HotbarDownOriginal = std::int32_t (ANOMALY_CALL*)(void*);
using InputBlockedFn = std::uint8_t (ANOMALY_CALL*)(void*);

enum PatchId : std::size_t {
    PatchSpawn, PatchUserCode, PatchInvoke, PatchWheel, PatchUi,
    PatchCenterPivot, PatchCount };
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
        // 数字键映射：槽 4..8 = '5'..'8'，槽 9 = '0'（第 10 格）。
        const int vk = slot == 9 ? '0' : static_cast<int>('1') + static_cast<int>(slot);
        const short state = GetAsyncKeyState(vk);
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
    const std::size_t size, const char* label,
    std::uint32_t* out_code = nullptr) {
    AnomalyGenerationHandleV1 handle{};
    const auto status = g_patch->apply(
        g_patch->user, address, {bytes, size}, anomaly::sdk::StringView(label),
        &handle);
    if (!anomaly::sdk::Succeeded(status)) {
        if (out_code != nullptr) *out_code = status.code;
        return false;
    }
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

// 按槽数 n 重新落补丁与 hook（仅 Lifecycle/Worker 域调用）。符号地址在 Load
// 阶段已缓存并校验，释放后字节回到原状，可安全重用。
// n = 0 表示「无槽位」：只落生成槽数（0 项）与 UI 槽位数组（0 格）两处补丁，
// 滚轮/选择上界/按键 hook 不适用（原版代码对空物品栏无副作用，保留原样）。
void ApplyForCount(const std::uint32_t n) {
    ReleaseHook();
    ReleaseAppliedPatches();
    g_error.clear();
    g_slot_count = n;

    const auto count = static_cast<std::uint8_t>(n);
    const auto last = static_cast<std::uint8_t>(n - 1);
    const std::uint8_t immediate[1] = {count};
    struct PatchRequest {
        PatchId id;
        std::uintptr_t address;
        const std::uint8_t* bytes;
        std::size_t size;
        const char* label;
    };
    PatchRequest requests[5];
    std::size_t request_count = 0;
    requests[request_count++] = PatchRequest{
        PatchSpawn, g_symbol_spawn + kSpawnImmediateOffset, immediate, 1,
        "hotbar.spawn_slots"};
    requests[request_count++] = PatchRequest{
        PatchUi, g_symbol_ui_ctor + kCtorImmediateOffset, immediate, 1,
        "hotbar.ui_slots"};
    std::uint8_t wheel_bytes[kWheelSize]{};
    if (n >= 1) {
        const std::uint8_t select_bound[1] = {last};
        // 滚轮取模：任意 N 用除法实现（rax 是存活寄存器，push/pop 保护；
        // rcx 会被其后的 mov rcx,[rax+0F0h] 重写）。加 4N 而非 N：N<4 且
        // active-steps+N 可能为负（div 对负数给错误余数），4N 保证恒为正，
        // 且 4N ≤ 40 仍在 imm8 范围内。
        const auto wheel_addend = static_cast<std::uint8_t>(n * 4);
        wheel_bytes[0] = 0x83;
        wheel_bytes[1] = 0xC2;
        wheel_bytes[2] = wheel_addend;  // add edx, 4N
        wheel_bytes[3] = 0x50;          // push rax
        wheel_bytes[4] = 0x8B;
        wheel_bytes[5] = 0xC2;          // mov eax, edx
        wheel_bytes[6] = 0x33;
        wheel_bytes[7] = 0xD2;          // xor edx, edx
        wheel_bytes[8] = 0xB9;
        wheel_bytes[9] = count;         // mov ecx, N（imm32 低字节）
        wheel_bytes[13] = 0xF7;
        wheel_bytes[14] = 0xF1;         // div ecx
        wheel_bytes[15] = 0x58;         // pop rax
        wheel_bytes[16] = 0x90;
        wheel_bytes[17] = 0x90;         // 补齐 18 字节窗口
        requests[request_count++] = PatchRequest{
            PatchUserCode, g_symbol_user_code + kSelectUserCodeOffset,
            select_bound, 1, "hotbar.select_usercode"};
        requests[request_count++] = PatchRequest{
            PatchInvoke, g_symbol_invoke + kSelectInvokeOffset, select_bound, 1,
            "hotbar.select_invoke"};
        requests[request_count++] = PatchRequest{
            PatchWheel, g_symbol_wheel + kWheelOffset, wheel_bytes, kWheelSize,
            "hotbar.wheel_modulo"};
    }
    bool all_patched = true;
    for (std::size_t i = 0; i < request_count; ++i) {
        const PatchRequest& request = requests[i];
        if (!ApplyMemoryPatch(request.id, request.address, request.bytes,
                              request.size, request.label)) {
            all_patched = false;
            if (g_error.empty()) g_error = "reason.patch";
            PluginLog(
                (std::string("hotbar: patch failed: ") + request.label).c_str());
        }
    }
    if (all_patched) {
        PluginLog(("hotbar: patches applied for slot count " + std::to_string(n)).c_str());
    }

    // 槽行居中：锚点函数在构造行 pivot 时从共享常量池加载 0.5（movss，池
    // 常量被 78 处引用不能改），经 unpcklps 构造出 Vector2 (0.5, 0.5)。10 槽
    // 行比原版 4 槽宽，行中心偏右；把 pivot.x 调高可让行回到屏幕中线（y 不
    // 变）。实现上把 22 字节构造窗口整体改写为 movabs rdx, imm64 直接携带
    // 目标 Vector2 (pivot_x, 0.5)，xor r8d（call 第三参数 0）与窗口后的
    // movq rdx, xmm0、call 保持原样。
    if (n >= 1 && g_core != nullptr && g_slot_pivot_milli >= 500) {
        const float pivot_x = static_cast<float>(g_slot_pivot_milli) / 1000.0f;
        std::uint8_t row_patch[22]{};
        row_patch[0] = 0x48;
        row_patch[1] = 0xBA;  // movabs rdx, imm64
        const float pair[2] = {pivot_x, 0.5f};
        std::memcpy(row_patch + 2, pair, sizeof(pair));
        row_patch[10] = 0x45;
        row_patch[11] = 0x33;
        row_patch[12] = 0xC0;  // xor r8d, r8d
        for (std::size_t i = 13; i < sizeof(row_patch); ++i) row_patch[i] = 0x90;
        // 窗口尾部必须是被保留的 movq rdx, xmm0（66 48 0f 7e c2）；符号解析
        // 已由 profile 入口字节校验，这里复核窗口边界防止落在错误位置。
        std::uint8_t window[22]{};
        const auto window_status = g_core->read_memory(
            g_core->user, g_symbol_slot_pivot + 13, {window, sizeof(window)});
        const std::uint8_t movq_bytes[5] = {0x66, 0x48, 0x0F, 0x7E, 0xC2};
        const bool window_ok = anomaly::sdk::Succeeded(window_status) &&
            std::memcmp(window + 17, movq_bytes, sizeof(movq_bytes)) == 0;
        if (window_ok &&
            ApplyMemoryPatch(PatchCenterPivot, g_symbol_slot_pivot + 13,
                             row_patch, sizeof(row_patch),
                             "hotbar.center_pivot")) {
            PluginLog("hotbar: centering applied");
        } else {
            PluginLog("hotbar: centering skipped (window mismatch)");
        }
    }

    if (n >= 1) {
        // 按键 hook：失败只影响 4 以上的按键，其余补丁保持生效。
        AnomalyHookRequestV1 hook_request{};
        hook_request.struct_size = sizeof(hook_request);
        hook_request.kind = ANOMALY_HOOK_V1_FUNCTION;
        hook_request.target = g_symbol_hotbar_down;
        hook_request.detour = reinterpret_cast<void*>(&HotbarDownThunk);
        hook_request.label = anomaly::sdk::StringView("hotbar.get_hotbar_down");
        std::uintptr_t original{};
        AnomalyGenerationHandleV1 hook_handle{};
        const auto hook_status =
            g_hook->create(g_hook->user, &hook_request, &original, &hook_handle);
        if (anomaly::sdk::Succeeded(hook_status) && original != 0) {
            g_original = original;
            g_hook_handle = hook_handle;
            g_blocked_fn = reinterpret_cast<InputBlockedFn>(g_symbol_blocked);
            PluginLog("hotbar: hook installed");
        } else {
            if (g_error.empty()) g_error = "reason.hook";
            PluginLog("hotbar: hook failed");
        }
    }
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
        // 槽数量固定，取 profile 的兼容上限（本构建为 10）；缺失即视为不可用。
        try {
            g_max_slots = g_profile.Value(kProfileLayoutMaxSlots, 16);
            g_slot_pivot_milli = g_profile.Value(kProfileLayoutPivotMilli, 100000);
        } catch (...) {
            g_max_slots = 0;
        }
    }
    *context = &g_slot_count;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    if (g_host == nullptr || !g_profile_ok) return anomaly::sdk::Ok();
    g_error.clear();

    if (g_max_slots == 0) {
        g_error = "reason.slots";
        return anomaly::sdk::Ok();
    }

    // 解析全部符号；ResolveProfileSymbol 已做唯一候选与入口字节校验。
    struct NamedTarget {
        std::string_view name;
        std::uintptr_t* address;
    };
    const NamedTarget targets[] = {
        {kSymSpawnSlots, &g_symbol_spawn},
        {kSymSelectUserCode, &g_symbol_user_code},
        {kSymSelectInvoke, &g_symbol_invoke},
        {kSymWheelModulo, &g_symbol_wheel},
        {kSymHotbarUi, &g_symbol_ui_ctor},
        {kSymSlotPivot, &g_symbol_slot_pivot},
        {kSymHotbarDown, &g_symbol_hotbar_down},
        {kSymInputBlocked, &g_symbol_blocked},
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
        // 诊断：对失败符号分层探测（v3 批量 / v1 单发 / 入口字节实读）。
        const anomaly::sdk::Host view(g_host);
        const auto sig = view.Query<AnomalySignatureServiceV1>(
            ANOMALY_SIGNATURE_SERVICE_V1_ID);
        const auto core = view.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID);
        struct Probe { std::string_view name; std::string_view pattern; };
        const Probe probes[] = {
            {kSymSelectInvoke, "4C 39 44 D1 F8 75 ?? 83 FF 03 77"},
            {kSymWheelModulo, "2B D7 83 C2 04 81 E2 03 00 00 80"},
            {kSymHotbarDown,
             "48 83 EC 28 80 3D ?? ?? ?? ?? 00 75 ?? 48 8D 0D 54 EF 90 03 E8 B7 DF F2 FF"},
        };
        for (const Probe& probe : probes) {
            if (sig) {
                std::uintptr_t address{};
                const auto status = sig->resolve(
                    sig->user, anomaly::sdk::StringView(g_profile.Document().module),
                    anomaly::sdk::StringView(g_profile.Document().section),
                    anomaly::sdk::StringView(probe.pattern), &address);
                const std::string line = std::string("hotbar: diag v1 ") +
                    std::string(probe.name) + " code=" + std::to_string(status.code) +
                    " addr=0x" + [&] {
                        char buffer[24]{};
                        std::snprintf(buffer, sizeof(buffer), "%llx",
                                      static_cast<unsigned long long>(address));
                        return std::string(buffer);
                    }();
                PluginLog(line.c_str());
                if (anomaly::sdk::Succeeded(status) && address != 0 && core) {
                    std::uint8_t bytes[24]{};
                    const auto read_status = core->read_memory(
                        core->user, address, {bytes, sizeof(bytes)});
                    char hex[80]{};
                    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
                        std::snprintf(hex + i * 3, 4, "%02x ", bytes[i]);
                    }
                    PluginLog(("hotbar: diag bytes " + std::string(hex) + " rd=" +
                               std::to_string(read_status.code)).c_str());
                }
            }
        }
        return anomaly::sdk::Ok();
    }

    // 槽数量固定为 profile 兼容上限（本构建 10），启用即按该值应用全部补丁。
    const std::uint32_t configured =
        g_max_slots != 0 && g_max_slots < kSlotCount ? g_max_slots : kSlotCount;
    g_slot_count = configured;
    ApplyForCount(configured);
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
    g_core = nullptr;
    g_hook = nullptr;
    g_patch = nullptr;
    g_profile = {};
    g_slot_count = 0;
    g_max_slots = 0;
    g_slot_pivot_milli = 500;
    g_symbol_spawn = 0;
    g_symbol_user_code = 0;
    g_symbol_invoke = 0;
    g_symbol_wheel = 0;
    g_symbol_ui_ctor = 0;
    g_symbol_hotbar_down = 0;
    g_symbol_blocked = 0;
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

    // 状态行：失败（带原因）/ 部分生效 / 全部生效。N=0 只要求两处基础补丁。
    const bool base_ready =
        g_patches[PatchSpawn].applied && g_patches[PatchUi].applied;
    const bool extras_ready = g_patches[PatchUserCode].applied &&
        g_patches[PatchInvoke].applied && g_patches[PatchWheel].applied;
    const bool hook_ready = g_hook_handle.id != 0;
    const bool center_ready = g_patches[PatchCenterPivot].applied;
    const bool feature_ready = g_slot_count == 0
        ? base_ready
        : base_ready && extras_ready && hook_ready && center_ready;
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
    } else if (!feature_ready) {
        ui->text(ui->user, anomaly::sdk::StringView(
                               g_localizer.Text("state.partial",
                                                "Partially applied; some targets failed.")));
    } else {
        const std::string slots = std::to_string(g_slot_count);
        const std::string_view args[]{slots};
        ui->text(ui->user, anomaly::sdk::StringView(g_localizer.Format(
                               "state.active", "Inventory slots: {0}.",
                               std::span<const std::string_view>{args})));
    }

    ui->separator(ui->user);
    if (ui->begin_table(ui->user, anomaly::sdk::StringView("##targets"), 2, 0, 0.0f, 0.0f)) {
        DrawTargetRow(ui, "target.spawn", "Inventory spawn slots (server)",
                      g_patches[PatchSpawn].applied);
        DrawTargetRow(ui, "target.ui", "Native hotbar UI", g_patches[PatchUi].applied);
        if (g_slot_count >= 1) {
            DrawTargetRow(ui, "target.select", "Slot select bounds (server)",
                          g_patches[PatchUserCode].applied && g_patches[PatchInvoke].applied);
            DrawTargetRow(ui, "target.scroll", "Wheel cycling",
                          g_patches[PatchWheel].applied);
            DrawTargetRow(ui, "target.center", "Slot row centering", center_ready);
            DrawTargetRow(ui, "target.keys", "Keys beyond 4 (hook)", hook_ready);
        }
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
              "Select slots with keys 1-9, 0 (the 10th slot) or the mouse "
              "wheel. Keys beyond 4 are added by this plugin and respect the "
              "game's input blocking.");
    draw_help("help.settings",
              "The inventory is fixed at 10 slots. Enable the plugin before "
              "entering a flight; inventories spawned earlier update on the "
              "next flight.");
    draw_help("help.multiplayer",
              "Multiplayer: while the host runs this plugin, joiners need no "
              "extra install - their inventory already holds the extended "
              "slots server-side. A vanilla joiner still sees 4 slots; pickups "
              "beyond the first four go to hidden slots that drop on death.");
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
