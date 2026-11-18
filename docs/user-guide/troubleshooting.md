# 故障排查

## 覆盖层打不开

1. 确认游戏进程里有 `Anomaly.Core.dll`（启动器状态或 `anomaly-cli status`）。
2. 确认 `anomaly.ini` 的 `Platform.ToggleKey` 是想要的热键。
3. 如果键盘没有 `Insert`，打开游戏暂停菜单，点击「Anomaly」入口。
4. 覆盖层展开时游戏输入被完全拦截是预期行为；关闭后恢复。

## 启动器找不到游戏

- 选择**直接包含 `MaydayAirways.exe` 的目录**，不是 Steam 库根目录。
- 如果游戏由 Steam 安装在其他库，启动器会从 Steam 的 `libraryfolders.vdf` 和常见库路径中查找；也可以手动选择。
- 启动并附加要求游戏尚未运行；已经运行时请用「注入到运行中的进程」。

## 注入失败

- 确认启动器和游戏位数一致（x64）。
- 确认游戏进程属于当前用户，且没有被反作弊或系统策略阻止打开进程。
- 查看启动器状态行或 `Anomaly\logs\` 下的日志。
- 失败不会结束游戏；可以重试或改用代理模式。

## 代理模式无效

- 确认游戏目录里同时有 `dwmapi.dll` 和 `Anomaly\Anomaly.Core.dll`。
- 某些游戏更新会校验或替换 `dwmapi.dll`；重新安装代理即可。
- 更新运行包前必须退出游戏。

## 插件不加载

- 在 **Plugins** 页查看状态和原因。
- 确认 `manifest.json` 的 `games` 包含 `mayday`、`builds` 匹配 `mayday-*`。
- 确认 required service 都有对应 capability；未知 capability 或缺失映射会导致包被拒绝。
- 用 `anomaly-plugin validate <包目录>` 做离线校验。

## 插件功能不可用

插件可以加载但功能显示 `UNAVAILABLE`，通常是游戏符号还没就绪：

- 还没进入一局（例如 Freecam 需要游戏内场景和本地玩家）。
- `GameAssembly.dll` 尚未加载或 il2cpp 类/方法解析失败。
- 查看 **Diagnostics > Developer > Adapter** 的原始诊断 JSON。

## 游戏崩溃

1. 在 **Diagnostics** 页导出日志。
2. 记录最近启用的插件和覆盖层操作。
3. 提交 Issue 时附上日志和复现步骤；分享前先删除敏感路径和账号信息。

## 相关

- [安装与启动](installation.md)
- [诊断 CLI](diagnostics-cli.md)
