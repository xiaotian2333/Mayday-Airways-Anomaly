#pragma once

namespace anomaly {

// 主切换热键必须用自算上升沿判定：异步输入调和（ReconcileAsyncInput）每帧
// 轮询全部虚拟键，GetAsyncKeyState 的"自上次查询后按下"低位沿位会被同帧
// 轮询清掉，不能作为按键检测依据（录键路径也因此维护自己的按下状态表）。
// 沿位状态无论焦点如何都照常推进：无焦点期间按下的键不补触发，焦点外
// 按住再切回也不会误触发一次。
[[nodiscard]] constexpr bool ShouldTogglePlatformMenus(
    const bool recording_menu_key, const bool menu_focus,
    const bool key_down, bool& key_was_down) noexcept {
    const bool pressed = menu_focus && !recording_menu_key && key_down && !key_was_down;
    key_was_down = key_down;
    return pressed;
}

}  // namespace anomaly
