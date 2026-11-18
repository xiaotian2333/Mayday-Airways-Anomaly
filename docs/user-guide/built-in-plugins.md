# 内建插件

运行包默认提供两个内建插件。它们和第三方插件一样是独立目录包（`manifest.json` + `plugin.dll`），可以在 **Plugins** 页启停、重载并查看状态。

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

## 启用与停用

1. 打开覆盖层（默认 `Insert`，或暂停菜单中的「Anomaly」）。
2. 进入 **Plugins** 页。
3. 在插件行上启用或停用；修改会在下一次插件维护时生效。
