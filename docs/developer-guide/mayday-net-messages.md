# Mayday Airways Demo 网络消息协议分析（加入者视角）

> 分析方式：frida attach 运行中进程枚举 il2cpp 类（903 类全字段/方法）+ global-metadata.dat 静态提取 + 实机抓包交叉验证。
> 生成时间：2026-11-22，游戏版本 0.14.11（Demo，appid 5480890）。
> 数据来源：`A:\mayday\il2cpp_dump.txt`（运行时枚举落盘）。
> ⚠️ 字段偏移为 x64 il2cpp 对象偏移（前 0x10 字节为对象头）。标注「推断」的字段是依据命名与上下文得出的语义，未经代码级验证。

## 0. 网络架构总览

| 层 | 实现 | 说明 |
| --- | --- | --- |
| 玩法层 | **Mirror**（NetworkBehaviour + Cmd/Rpc/SyncVar） | 房主即服务器（host 模式），房主机跑全部游戏逻辑 |
| 传输层 | FizzySteamworks → SteamNetworkingSockets P2P | 经 Valve SDR 中继（`*.valve.net`），穿透成功可直连 |
| 身份层 | Steamworks.NET（Steam 大厅承载会话/列表/邀请） | 房间列表走 Steam Lobby API，非自建 HTTP |
| 备用传输 | kcp2k（UDP 7777，LAN/加 IP）、Telepathy/SimpleWeb、Edgegap 中继 | LAN 模式与未启用的云中继 |

加入者上行包体全部封装在 Mirror 的 `CommandMessage`/`EntityStateMessage`/自定义消息里，经 SDR 加密传输。

## 1. 加入握手（第一个上行包体）

### 1.1 NetAuthRequestMessage —— 加入者 → 房主

连接建立后由 `Mayday.Net.MaydayAuthenticator.OnClientAuthenticate` 自动发送，不是玩家操作触发。

| 字段 | 类型 | 偏移 | 中文注释 | 发起 |
| --- | --- | --- | --- | --- |
| `version` | string | 0x10 | 客户端版本号，房主校验版本一致才继续 | 加入者客户端自动填入 |
| `steamId` | ulong | 0x18 | 发起者的 Steam 64 位 ID；房主用它查 Steam 大厅成员表 | 加入者客户端自动填入 |
| `name` | string | 0x20 | 显示昵称（Steam persona 名） | 加入者客户端自动填入 |

房主端校验链（`MaydayAuthenticator`）：`version` 一致 → `AcceptWhenLobbyMember(conn, steamId)`（**必须在 Steam 大厅里**）→ `IsFull()`（4 人上限）→ `IsOpen()` → `Accept` / `Reject`。握手超时 `handshakeTimeout`，大厅成员验证超时 `lobbyMembershipTimeout`。

### 1.2 NetAuthResponseMessage —— 房主 → 加入者（下行，供对照）

| 字段 | 类型 | 偏移 | 中文注释 |
| --- | --- | --- | --- |
| `code` | byte | 0x10 | 结果码：接受 / 拒绝（具体枚举值未验证） |
| `reasonKey` | string | 0x18 | 拒绝原因的本地化 key |
| `arg0` | string | 0x20 | 原因文案占位参数 1 |
| `arg1` | string | 0x28 | 原因文案占位参数 2 |

## 2. Cmd 命令全表（加入者 → 房主，54 个）

通用说明：
- **发起**均为「本地玩家（加入者）操作」；Cmd 在房主端执行，加入者只是请求。
- 带尾参 `Mirror.NetworkConnectionToClient` 的重载是 Mirror weaver 生成的**服务器转发变体**（房主把行为转发给指定连接），客户端实际发送的是不带该参数的版本。
- 同名命令可能同时出现在「带 conn / 不带 conn」两组，属于同一命令的两个重载。

### 2.1 飞行操控 —— `Mayday.Flight.PlaneFlight`（飞机主控）

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdPilotInput(sbyte, sbyte, sbyte, sbyte, byte)` | 飞行姿态输入。4 个 sbyte 依次为俯仰 / 横滚 / 偏航 / （第 4 轴，推断为方向舵或配平），byte 为附加标志位（推断：刹车/增稳开关）。驾驶杆每次输入变化时由飞行操控模块发起 |
| `CmdPilotAction(byte)` | 离散飞行动作（推断：起落架 / 襟翼 / 桨距切换等动作编号） |
| `CmdSetThrottles(byte)` | 设置油门档位（0-255 归一化），油门杆操作时发起 |
| `CmdPA(byte)` | 机内广播系统（PA）开关/频道选择，由 `PASystem` 面板操作发起 |

### 2.2 地面车辆 —— `Mayday.Airport.DrivableVehicle`

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdDrive(Vector3, float×3, byte×3, float×4, double)` | 驾驶状态全量上报：位置/朝向（Vector3）+ 速度与转角（float×3）+ 灯光/喇叭/档位标志（byte×3）+ 附加运动参数（float×4）+ 客户端时间戳（double）。每 tick 由车辆驾驶模块发起 |
| `CmdVehicleAction(byte)` | 车辆离散动作（推断：鸣笛 / 灯光 / 拖车挂钩） |
| `CmdBump(byte)` | 车辆碰撞/颠簸事件上报（byte 推断为强度档） |
| `CmdDevSit` / `CmdDevRide(int)` | 开发者调试命令：直接坐进/驾驶指定车辆 ID |

### 2.3 角色交互 —— `Mayday.PlayerInteractor`

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdInteract(InteractRef, byte)` | 与世界物件交互（InteractRef 是物件引用 struct：场景 ID + 子索引；byte 推断为交互动作变体）。按 E/点击时由交互模块发起 |
| `CmdUse(InteractRef, Vector3, float, byte)` | 「使用」物件：带使用点坐标、力度/时长（float）与动作变体（byte），长按类交互由本地计时后发起 |
| `CmdLetGo` | 松手（放下搬运物 / 离开攀爬），松开交互键时发起 |
| `CmdShove(InteractRef, Vector3)` | 推撞物件（带方向冲量） |
| `CmdShovePlayer(uint, Vector3)` | 推撞其他玩家（uint 为对方 netId，Vector3 为击退方向）——仅房主端校验距离后才生效 |

### 2.4 物品栏 —— `Mayday.PlayerInventory`

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdSelect(int)` | 切换手持槽位（hotbar 0-9 索引） |
| `CmdDrop(Vector3, Vector3)` | 丢弃手中物品（位置 + 抛出初速度） |
| `CmdThrow(Vector3, Vector3, float)` | 投掷手中物品（位置 + 方向 + 力度），按住蓄力松开时发起 |

### 2.5 身份与状态 —— `Mayday.PlayerNet`

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdSetProfile(string, ulong, byte)` | 提交玩家档案（昵称 + SteamId + 头像/配色索引，推断）——入舱时由档案模块发起 |
| `CmdSetCosmetics(Cosmetics)` | 提交装饰套装（帽子/涂装等 struct） |
| `CmdSetSpeaking(bool)` | 说话状态灯开关（话筒按键按下/松开时由语音模块发起） |
| `CmdLeaveSeat` | 离开座椅 |
| `CmdTakeOffStickyShoes` | 脱掉黏鞋道具效果 |

### 2.6 动作同步 —— `Mayday.Players.PlayerSync`

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdPing(Vector3, string)` | 地图标记 ping（存活状态下的普通 ping；string 为预设标签 key）。服务端有 `ServerPingsAccepted/Refused` 频率限制 |
| `CmdGhostPing(Vector3, string)` | 死亡幽灵态的 ping（服务端单独限流 `GhostPingsSeen`） |
| `CmdReportDeath(byte)` | 死亡上报（byte 为 `DeathCause` 枚举：坠落/烧烫/窒息等） |
| `CmdEmote(byte)` | 触发表情动作（byte 为 `EmoteId`） |
| `CmdStopEmote` | 结束表情 |
| `CmdFx(byte, Vector3)` | 通用特效触发（byte 为 `PlayerFx` 枚举，Vector3 为位置） |
| `CmdDefibPick(uint)` | 除颤器拾取目标（uint 为被救者 netId） |

### 2.7 语音 —— `Mayday.Net.PlayerVoice`

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdVoice(byte, byte, byte, ushort, ArraySegment<byte>)` | 语音数据上行：byte1 = 通道路由（`SpeakerRoute`：近讲 / PA 广播 / 电台），byte2 = 编解码器 ID（Opus），byte3 = 音量/增益档（推断），ushort = 序号防乱序（`sendSeq`），ArraySegment = Opus 编码帧。录音采集模块按帧间隔发起 |
| `CmdPaSpeech(float, float, float)` | PA 讲话统计上报（响度/时长，用于服务端 `ServerAccount` 响度配额记账），广播结束后发起 |
| `CmdSetCaps(bool)` | 大写锁定提示？（推断：聊天输入状态或提示灯，语义未验证） |

服务端治理：`ServerFilter`（通道过滤）、`ServerPaAllowed`/`ServerRadioAllowed`（开广播权限）、`ServerAccount` + `tokens`（说话时长配额）、`FlushAccount`。

### 2.8 客舱服务 —— `Mayday.Service.GalleySystem` / `PassengerDirector`

| 命令签名 | 中文注释 |
| --- | --- |
| `CmdWhackCoffee` | 咖啡机拍击（彩蛋交互：敲打咖啡机） |
| `CmdInstallWork(byte)`（`PassengerDirector`） | 客舱安装作业进度上报（byte 推断为作业步骤/部件编号） |

### 2.9 道具系统（每道具类 1-2 个命令）

| 归属类 | 命令签名 | 中文注释 |
| --- | --- | --- |
| `FlightManualItem` | `CmdRead(byte, byte)` | 翻阅飞行手册（页码 + 章节索引，推断） |
| `PASystem` | `CmdHangUp` | 挂断内话 |
| `IgnitionSystem` | `CmdAct(byte)` | 点火/断电动作（电门位置枚举） |
| `SprayCanTool` | `CmdSetAccent(bool)` | 喷漆工具换色（开始/结束喷涂） |
| `TetherRescue` | `CmdHaulBeat(byte)` | 安全绳收拉节拍（byte 推断为拉力档/节拍计数） |
| `CameraItem` | `CmdSetRec(bool)` / `CmdSetSelfie(bool)` | 相机录制/自拍模式开关 |
| `LifeVestItem` | `CmdSteady(byte)` / `CmdBump(Vector3)` | 穿救生衣后的稳定/碰撞事件 |
| `ToyBlasterItem` | `CmdFire(Vector3, Vector3)` | 玩具水枪开火（起点 + 方向） |
| `GameDirector` | `CmdChooseDestination(int)` | 选择目的地（航班看板点选，int 为目的地索引） |
| `GameDirector` | `CmdBuyUpgrade(string)` | 购买升级（string 为升级项 ID），由商店 UI 发起 |

### 2.10 框架自动命令（非玩家操作，weaver 生成）

| 命令 | 归属 | 说明 |
| --- | --- | --- |
| `CmdOnAnimationServerMessage / Parameters / ResetTrigger / Trigger` | NetworkAnimator | 动画状态同步：本地 Animator 参数变化时由 Mirror 动画同步组件自动发起 |
| `CmdClientToServerBaseline / Delta / Sync(SyncData)` | Mirror 同步组件 | 基线/增量/全量状态同步（SyncVar 大数据量时的补传通道） |
| `CmdAuthenticate` | Mirror Authenticator | 框架标准认证入口（游戏实际走自定义 NetAuthRequestMessage） |

## 3. 持续状态同步（SyncVar，加入者作为 owner 自动上行）

Mirror 把下方字段的变化自动打包为 `EntityStateMessage`（dirty 掩码 + 字段值），随 tick 发送，玩家无感知。**这些就是加入者持续发出的"隐形包体"。**

### 3.1 `Mayday.Players.PlayerSync`（运动状态）

| 字段 | 类型 | 偏移 | 中文注释 | 发起 |
| --- | --- | --- | --- | --- |
| `flags` | byte | 0x68 | 蹲下 / 冲刺 / 手电 / 着地 / 打滑 / 下落 / 跳跃中 / 在世界面 等状态位掩码 | 本地运动控制器 `OwnerSetState` 写入 |
| `lookPitch` | sbyte | 0x69 | 头部俯视角度（量化为 ±90 的字节） | 鼠标视角变化时写入 |
| `lookYaw` | sbyte | 0x6A | 头部水平朝向 | 同上 |
| `jumpSeq` | byte | 0x6B | 跳跃序号（每次起跳 +1，用于触发端动画） | `OwnerJumped` 写入 |
| `ghostMode` | byte | 0x6C | 幽灵模式状态（死亡观战） | 死亡/复活流程写入 |
| `ghostTarget` | uint | 0x70 | 幽灵态观战目标的 netId | 观战切换时写入 |
| `act` | byte | 0x74 | 当前动作枚举（攀爬/搬运/推挤等） | `OwnerSetAct` 写入 |

### 3.2 `Mayday.Players.PlayerNetTransform`（位置）

| 字段 | 类型 | 偏移 | 中文注释 | 发起 |
| --- | --- | --- | --- | --- |
| （继承 Mirror NetworkTransform） | Vector3/Quaternion | — | 实时位置与旋转，移动时按阈值上报；`OwnerTeleport(Vector3, Quaternion)` 为瞬移包 | 本地移动/传送 |

### 3.3 `Mayday.PlayerNet`（玩家档案与事件状态）

| 字段 | 类型 | 偏移 | 中文注释 | 发起 |
| --- | --- | --- | --- | --- |
| `DisplayName` | string | 0x68 | 显示昵称（经 CmdSetProfile 提交） | 玩家本人 |
| `SteamId` | ulong | 0x70 | Steam ID（只读同步） | 入房时 |
| `ColorIndex` | byte | 0x78 | 玩家配色索引 | 玩家本人 |
| `DeathCause` | string | 0x80 | 死因本地化 key | 死亡时由房主经 CmdReportDeath 处理后同步 |
| `Deaths` | byte | 0x90 | 死亡次数 | 房主统计 |
| `IsDead` | bool | 0xA8 | 死亡标志 | 房主 |
| `Seat` | InteractRef | 0xAC | 所坐座位引用 | 本人落座时 |
| `IsSpeaking` | bool | 0xB4 | 是否在说话 | 语音按键（CmdSetSpeaking） |
| `Tethered` / `TetherPoint` / `TetherLength` | bool / Vector3 / float | 0xB5-0xC8 | 安全绳状态：是否拴住 / 锚点 / 长度 | 安全绳系统 |
| `Pushing` | InteractRef | 0xC8 | 正在推的物件引用 | 推挤模块 |
| `Cosmetics` | Cosmetics | 0xD0 | 装饰套装 | 本人（CmdSetCosmetics） |
| `Haul` | TetherHaul | 0x94 | 安全绳收拉状态 | TetherRescue 模块 |
| `StickyShoes` | byte | 0x9C | 黏鞋道具状态 | 道具系统 |
| `StunnedUntil` | double | 0xA0 | 眩晕截止时间 | 房主（被撞/被喷） |

### 3.4 `Mayday.Net.PlayerVoice`（语音元状态）

| 字段 | 类型 | 偏移 | 中文注释 | 发起 |
| --- | --- | --- | --- | --- |
| `steamDecode` / `sessionBackend` | bool / byte | 0x170/0x171 | 语音后端选择（Steam 语音 vs 传输通道 Opus） | 连接协商时 |
| 其余 pa*/radio*/occluded 等 | float/bool | 0x68+ | 全部是本地音频渲染参数（距离衰减、遮挡滤波、增益）——不同步，仅本地 | 本地音频栈 |

## 4. 表现型消息（动作触发的一次性包体）

| 消息 | 字段（偏移） | 中文注释 | 发起 → 接收 |
| --- | --- | --- | --- |
| `ClimbShowMessage` | `netId:uint(0x10)` `gripL/gripR/normal/dir:Vector3(0x14-0x38)` `eyeAbove/eyeFloor:float(0x44/0x48)` `grab/pull/knee/dur:float(0x4C-0x58)` | 爬墙表现：攀爬者 netId、左右手抓点、墙面法线、攀爬方向、眼高约束、抓/拉/膝盖动画参数与时长 | 攀爬者客户端 → 全员广播（`ClimbShowMessage` 由房主转发） |
| `TumbleHurtMessage` | `hurt:float(0x10)` `from:Vector3(0x14)` `fainted:byte(0x20)` | 摔伤：伤害量、受力来源方向、是否昏迷 | 物理模块（房主计算）→ 当事人广播 |
| `TumbleShowMessage` | `netId:uint(0x10)` `seq:byte(0x14)` `cause:byte(0x15)` `start:double(0x18)` `seconds:float(0x20)` `impulse:Vector3(0x24)` | 翻滚表现：谁、序号、原因、开始时间、持续时长、冲击向量 | 翻滚触发方 → 全员 |
| `SmokeRoomsMessage` | `levels:ulong(0x10)` | 吸烟区状态：64 位房间掩码（哪些舱段允许吸烟/烟雾浓度档位） | `SmokeRoomRelay`（服务端）→ 全员；`SendGap` 限频 |
| `NetAchievementMessage` | `kind:byte(0x10)` `id:string(0x18)` `delta:int(0x20)` | 成就事件：类型（解锁/进度）、成就 ID、进度增量 | 房主 `AchievementWatcher` → 全员 |
| `NetSessionEndMessage` | `reasonKey:string(0x10)` | 回合结束原因（坠机/到达/全员下机等本地化 key） | 房主 → 全员 |
| `ReportPacket` | `leg:int` `from/to:string` `delivered:int` `paxTotal:int` `satisfaction:float` `stars:float` `grade:string` `ledgerKeys:string[]` `ledgerAmounts:int[]` `infoLines:string[]` `moneyTotal:int` | 航段结算单：航段号、起讫机场、运送乘客、满意度、星级、评级、账目明细（key+金额数组）、信息行、总收入 | 房主 → 加入者（到达结算时） |
| `SummaryPacket` | `headline/cause:string` `airports/passengers/earned:int` `bestStars/minutes:float` `highlights:string[]` | 整局总结：标题、结束原因、机场数、乘客数、收入、最佳星级、时长、亮点列表 | 房主 → 全员 |
| `ConsoleReplyMessage` | `text:string(0x10)` | 调试控制台回复文本 | 房主控制台 → 请求者 |

## 5. Mirror 内置协议包（框架自动，非游戏代码发起）

| 消息 | 方向 | 说明 |
| --- | --- | --- |
| `ReadyMessage` / `NotReadyMessage` | 双向 | 场景加载就绪握手 |
| `SceneMessage` | 房主 → 加入者 | 场景切换指令（进舱加载） |
| `SpawnMessage` / `DespawnMessage` | 房主 → 加入者 | 网络对象生成/销毁（含 prefab hash、位置、owner netId） |
| `CommandMessage` / `RpcMessage` | 分别为 Cmd/Rpc 的载体 | `{netId, componentIndex, functionHash(方法名 hash), payload}` |
| `EntityStateMessage` | owner → 全员 | SyncVar 增量（dirty 掩码 + 变更字段），见第 3 节 |
| `TimeSnapshotMessage` / `SnapshotMessage` | 双向/房主→加入者 | 时间同步与状态快照 |
| Dissonance `*Message` | 双向 | 语音框架控制通道（环绕 PlayerVoice 工作） |

## 6. 线格式约定

- **字节序**：小端。
- **string**：变长压缩整数长度（7-bit VarInt）+ UTF-8 字节。
- **消息头**：2 字节消息 ID（类型全名 hash，Mirror `MessagePacker` 生成）+ 长度前缀（传输层分帧）。
- **Cmd/Rpc 调用**：`CommandMessage{ netId:uint, componentIndex:byte, functionHash:int, payload:... }`，functionHash 为方法名 hash；参数按声明顺序紧随。
- **ArraySegment\<byte\>**：长度 + 原始字节（语音 payload 受 `MaxUnreliablePayload` 上限约束）。
- 全部流量经 Steam SDR 加密——网络层抓包只见密文，操作包体必须在 il2cpp 层（Hook `NetworkClient.Send` / 消息构造点）。

## 7. 服务器（房主端）校验面

| 校验点 | 位置 | 说明 |
| --- | --- | --- |
| 版本 + 大厅成员 | `MaydayAuthenticator` | 加入者必须版本一致且在 Steam 大厅成员表中 |
| 人数上限 | `IsFull()` | 4 人 |
| ping 频率 | `PlayerSync.ServerPingsAccepted/Refused`、`GhostPingsSeen` | 存活 ping 与幽灵 ping 分开限流 |
| 语音权限与配额 | `PlayerVoice.ServerFilter`、`ServerPaAllowed`、`ServerRadioAllowed`、`ServerAccount`(tokens) | PA/电台要权限，响度/时长有配额，超限封印 |
| 交互距离/合法性 | `PlayerInteractor` / 各物品类 UserCode_Cmd* | 所有 Cmd 在房主端重验距离与状态后才执行 |

## 8. 附录：分析工具链

| 工具/文件 | 用途 |
| --- | --- |
| `A:\mayday\dump_process.py` | ctypes 调 `dbghelp!MiniDumpWriteDump` 导出全内存（零依赖） |
| `A:\mayday\scan_dump.py` | dump 的 UTF-16 字符串扫描（拿运行时类型全名） |
| `A:\mayday\enum_il2cpp.js` + `enum_run.py` | frida attach 枚举 il2cpp 类字段/方法（本文档数据源） |
| `A:\mayday\il2cpp_dump.txt` | 903 类全字段/方法落盘（1.4MB，可直接 grep） |
| `A:\mayday\mayday_dump.dmp` | 2.66GB 全内存 dump（可删） |
| `A:\pcap\mayday_join.pcapng` | 实机抓包（A 盘，重启即清） |

> 本文档基于 2026-11-22 的游戏版本 0.14.11 逆向分析产出，属开发者参考资料；游戏更新后
> 字段与命令可能变化，以重新枚举的结果为准。
