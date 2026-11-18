# Mayday Airways 适配层改造路线图

本仓库 fork 自 [AnomalyNTE/Anomaly](https://github.com/AnomalyNTE/Anomaly)（AGPL-3.0），
目标：改造为 **Mayday Airways (Steam Demo, Unity IL2CPP)** 的 mod 加载器。

## 已完成

- [x] 移除全部上游游戏专用内容：插件、引擎适配层、Profile 与工具脚本
- [x] CMakeLists 同步清理，修复 fixtures 构建路径
- [x] 建立 `src/game/mayday/`、`profiles/mayday/` 骨架
- [x] 引擎符号层：`MaydayAdapter` 等待 `GameAssembly.dll`，解析 il2cpp 导出并发布 `mayday.il2cpp`
- [x] 渲染后端：实测 Mayday 使用 **D3D12**，上游 D3D12 Present hook + 内嵌 ImGui 覆盖层复用
- [x] 内建服务 `mayday.money`：金币/债务读写（il2cpp 线程附加、原始值参数封送、差额债务语义）
- [x] 内建插件 `plugins/money`：金币修改面板（本地化、`check_plugin_i18n.ps1` 通过），实机读写验证通过
- [x] 内建服务与插件 `mayday.freecam`：自由镜头（主线程接管相机、原始鼠标增量）
- [x] 覆盖层输入隔离：InputSystem 按钮/滚轮、UGUI `EventSystem` 与指针缓存
- [x] 暂停菜单「Anomaly」入口，等价 Insert 热键
- [x] 启动器：Mayday 单游戏配置、`MaydayAirways.exe` 进程识别、Steam 启动并附加、代理模式

## 保留的通用基础设施

| 模块 | 说明 |
|---|---|
| `apps/launcher` | 启动器 UI（游戏路径、注入/代理模式、Steam 启动） |
| `apps/injector` | 进程注入器（dwmapi 代理 / 手动映射） |
| `src/bootstrap` | dwmapi 代理 + Core 异步启动 |
| `src/runtime` | RuntimeSession / ServiceGraph / Dispatcher |
| `src/plugin` | C ABI 插件宿主、Scope ledger、热重载 |
| `src/render` + `src/ui` | 渲染桥 + 游戏内 ImGui 管理界面 |
| `src/services` `src/platform` `src/diagnostics` | 存储/日志/内存/Pattern/Pipe/Crash |
| `include/anomaly/sdk` | 稳定 C ABI v1 插件 SDK |

## 待改造

1. **更多游戏服务/插件**：参考 Mayday-Airways-ce 的 MelonLoader 版功能继续复刻
2. **启动器模组页**：UE pak 语义的模组栈已随 UE 支持移除；待按 Mayday 模型重建
3. **文档**：面向 Mayday 的用户/开发者文档继续补齐
