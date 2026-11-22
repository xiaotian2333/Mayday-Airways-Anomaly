<div align="center">

# Mayday Loader

**Mayday Airways (Steam Demo) 的插件平台 —— 基于 Anomaly 框架改造**

</div>

---

> [!NOTE]
> 本仓库 fork 自 [AnomalyNTE/Anomaly](https://github.com/AnomalyNTE/Anomaly)
> （AGPL-3.0）。改造进度与计划见 [`docs/mayday-roadmap.md`](docs/mayday-roadmap.md)。

## 定位

- **注入式加载器**：支持 Steam 启动并附加、注入已运行进程，以及 `dwmapi.dll` 代理持久化三种方式
- **插件框架**：复用上游纯 C ABI v1 SDK 与插件宿主（Scope ledger / 热重载）
- **游戏内管理界面**：ImGui 界面统一管理插件启用、停用与设置
- **开发工具**：内存诊断、签名扫描、离线 TestHost 均随框架保留

## 状态

- 通用基础设施完整保留（见 roadmap 表格）
- Mayday 适配层：il2cpp 导出探测（`mayday.il2cpp`）、金币/债务服务（`mayday.money`）、
  气象/时间服务（`mayday.weather`）、自动棒球棍服务（`mayday.combat`）与语音注入服务
  （`mayday.voice`）
- D3D12 Present hook + 内嵌 ImGui 管理界面
- 内建插件：`Money`（金币/债务）、`Freecam`（自由镜头）、`Weather`（气象/时间）、
  `Auto Bat`（自动棒球棍）与 `Music`（语音广播音乐播放器）
- 覆盖层展开时阻断 Unity InputSystem 与 UGUI 输入；暂停菜单提供「Anomaly」入口
- 已清理上游游戏专用插件、服务、资源、工具与文档

## 启动方式

- **启动并附加**：通过 Steam 打开游戏，等待 `MaydayAirways.exe` 后手动映射 Core
- **注入运行中进程**：游戏已运行时直接附加，不重启、不结束游戏
- **代理模式**：把 `dwmapi.dll` 安装到游戏目录，随游戏启动加载 Core

## 构建

```pwsh
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --parallel
```

前置与依赖说明见 `docs/developer-guide/`。

## 许可

[AGPL-3.0-only](LICENSE)，上游版权与第三方组件见 [`NOTICE`](NOTICE)。
