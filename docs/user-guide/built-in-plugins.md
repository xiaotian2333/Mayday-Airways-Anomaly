# 内建插件

运行包默认提供四个内建插件。它们和第三方插件一样是独立目录包（`manifest.json` + `plugin.dll`），可以在 **Plugins** 页启停、重载并查看状态。

## Money（金币/债务）

- **ID**：`mayday.money`
- **依赖服务**：`anomaly.ui`、`anomaly.localization`、`mayday.money`
- **capabilities**：`ui`、`mayday-money`

功能：

- 显示当前金币和债务，每帧刷新。
- 「设为该值」直接写 `GameState` 的 Money SyncVar，不进入游戏记账统计。
- 「增加该值」走 `ServerAddMoney` 记账入口。
- 快捷「金币 +100000」与「清空债务」。

## Freecam（自由镜头）

- **ID**：`mayday.freecam`
- **依赖服务**：`anomaly.ui`、`anomaly.localization`、`anomaly.input`、`anomaly.config`、`anomaly.json`、`anomaly.scheduler`、`mayday.freecam`
- **capabilities**：`ui`、`input`、`configuration`、`json`、`scheduler`、`mayday-freecam`

操作：

| 按键 | 效果 |
| --- | --- |
| `W` / `A` / `S` / `D` | 前后左右移动 |
| `空格` | 上升 |
| `Shift` | 下降 |
| 鼠标 | 转向 |

热键、移动速度和鼠标灵敏度都可以在插件窗口内修改并持久化。打开宿主覆盖层时，自由镜头暂停移动与转向。

## Weather（气象/时间）

- **ID**：`mayday.weather`
- **依赖服务**：`anomaly.ui`、`anomaly.localization`、`anomaly.core`、`anomaly.config`、`anomaly.json`、`anomaly.scheduler`、`mayday.weather`
- **capabilities**：`ui`、`configuration`、`json`、`scheduler`、`mayday-weather`

功能：

- 显示当前时间、天气种类、强度与时钟速率。
- 拖动时间后点击「设为该时间」，或锁定时间冻结时钟。
- 锁定天气后持续保持所选种类与强度，解除锁定后恢复自然天气。

## Auto Bat（自动棒球棍）

- **ID**：`mayday.autobat`
- **依赖服务**：`anomaly.ui`、`anomaly.localization`、`anomaly.core`、`anomaly.config`、`anomaly.json`、`anomaly.profile`、`anomaly.scheduler`、`mayday.combat`
- **capabilities**：`ui`、`configuration`、`json`、`scheduler`、`mayday-combat`

功能：

- 手持棒球棍时，自动选择攻击范围内距离最近的**队友**；不锁定自己，也不锁定 NPC 乘客。
- 以游戏自身的 Mirror `CmdUse` 命令（协议包）发送使用请求，间隔遵循游戏挥击冷却。
- 自己主持会话时，服务端命中判定期间临时把角色朝向目标，渲染帧不改变相机视角；
  加入他人房间时服务端按本地瞄准校验，因此只攻击前方 45° 锥体内的队友。
- 攻击距离、锥角与发送间隔来自包内 `profile/compatibility.profile.json` 的兼容数据。

## 启用与停用

1. 打开覆盖层（默认 `Insert`，或暂停菜单中的「Anomaly」）。
2. 进入 **Plugins** 页。
3. 在插件行上启用或停用；修改会在下一次插件维护时生效。
