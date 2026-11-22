# 内建插件

运行包默认提供五个内建插件。它们和第三方插件一样是独立目录包（`manifest.json` + `plugin.dll`），可以在 **Plugins** 页启停、重载并查看状态。

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

## Music（音乐播放器）

- **ID**：`mayday.music`
- **依赖服务**：`anomaly.ui`、`anomaly.localization`、`anomaly.core`、`anomaly.config`、`anomaly.json`、`anomaly.scheduler`、`mayday.voice`
- **capabilities**：`ui`、`configuration`、`json`、`scheduler`、`mayday-voice`

功能：

- 输入本地音乐目录并刷新，扫描其中的 `mp3` / `flac` / `wav` 文件，点击曲目即播放。
- 音乐解码后按游戏语音的采样率重采样、分帧并做 μ-law 编码，经宿主 `mayday.voice`
  服务走游戏自身语音管线（`PlayerVoice.SendVoice`）广播给房间内所有玩家；
  采样率、帧长与发送间隔都在运行时从游戏按名解析。
- 「给自己播放」开关控制本机回放：开启时插件用自己的音频设备直放原始音质文件，
  音量独立可调，与发送链路互不影响。
- 发送路由可选近讲（仅附近玩家）、机内广播（对全机，需游戏权限）与电台
  （需权限与响度配额）；机内广播/电台被游戏权限拦截时请换近讲。
- 播放路由、音量、自听开关、目录与列表循环都会持久化。

注意：广播音质为 μ-law 电话音质，来自游戏 Legacy 语音链路的固有限制；
窗口内会显示当前会话的语音后端（μ-law / Dissonance）。

## 启用与停用

1. 打开覆盖层（默认 `Insert`，或暂停菜单中的「Anomaly」）。
2. 进入 **Plugins** 页。
3. 在插件行上启用或停用；修改会在下一次插件维护时生效。
