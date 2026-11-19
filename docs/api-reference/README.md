# API 参考

这是 Anomaly 插件 **纯 C ABI v1** 的总览。权威定义以 `include/anomaly/sdk/` 下的头文件为准。

## 页面

| 页面 | 内容 |
| --- | --- |
| [生命周期与 Core 服务](lifecycle-and-core.md) | 插件入口、Host API、`anomaly.core`、`plugin-state`、`localization` |
| [平台作用域服务](platform-services.md) | `config`、`storage`、`runtime-info`、`diagnostics`、`scheduler`、`commands`、`notifications` |
| [Interop 与内存](interop-and-memory.md) | `interop.signature`、`interop.hook`、`interop.patch`、内存 grant |
| [插件间 IPC](ipc.md) | `anomaly.ipc` |
| [WebSocket 广播](websocket.md) | `anomaly.websocket` |
| [UI 服务](ui-services.md) | `anomaly.ui`、`window`、`font`、`texture`、`input` |
| [Manifest 与 capability](manifest-and-capabilities.md) | Manifest v2、capability 映射、状态码 |

## 服务列表

| 服务 ID | 版本 | capability |
| --- | --- | --- |
| `anomaly.core` | 1 | 无需 capability |
| `anomaly.plugin-state` | 1 | `configuration` |
| `anomaly.config` | 1 | `configuration` |
| `anomaly.storage` | 1 | `storage` |
| `anomaly.runtime-info` | 1 | `runtime-info` |
| `anomaly.diagnostics` | 1 | `diagnostics` |
| `anomaly.scheduler` | 1 | `scheduler` |
| `anomaly.ipc` | 1 | `ipc` |
| `anomaly.websocket` | 1 | `websocket` |
| `anomaly.commands` | 1 | `commands` |
| `anomaly.notifications` | 1 | `notifications` |
| `anomaly.interop.signature` | 1 | `interop-signature` |
| `anomaly.interop.hook` | 1 | `interop-hook` |
| `anomaly.interop.patch` | 1 | `interop-patch` |
| `anomaly.ui` | 1 | `ui` |
| `anomaly.localization` | 1 | `ui` |
| `anomaly.window` | 1 | `ui-window` |
| `anomaly.font` | 1 | `ui-font` |
| `anomaly.texture` | 1 | `ui-texture` |
| `anomaly.input` | 1 | `input` |
| `anomaly.json` | 1 | `json` |
| `anomaly.profile` | 1 | `json` |
| `anomaly.mods` | 1 | `mods` |
| `mayday.money` | 1 | `mayday-money` |
| `mayday.freecam` | 1 | `mayday-freecam` |
| `mayday.weather` | 1 | `mayday-weather` |
| `mayday.combat` | 1 | `mayday-combat` |

`mayday.money`、`mayday.freecam`、`mayday.weather` 与 `mayday.combat` 的 C 定义在
`include/anomaly/sdk/services/mayday.h`。

## ABI 约定

- 纯 C（`extern "C"`），可被 C 与 C++ 编译。
- `ANOMALY_CALL` 在 Win32 上是 `__cdecl`。
- 不跨 DLL 边界传递 STL、异常、RTTI、游戏对象指针或 `ImGuiContext*`。
- 服务表属于一个 Host lifecycle generation；停止或替换后只报告 `UNAVAILABLE`。

### 基础类型

```c
typedef struct AnomalyStringViewV1 { const char* data; size_t size; } AnomalyStringViewV1;
typedef struct AnomalyByteSpanV1 { const uint8_t* data; size_t size; } AnomalyByteSpanV1;
typedef struct AnomalyGenerationHandleV1 { uint64_t id; uint64_t generation; } AnomalyGenerationHandleV1;
typedef struct AnomalyStatusV1 { uint32_t code; uint32_t reserved; AnomalyStringViewV1 message; } AnomalyStatusV1;
```

### 状态码

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `ANOMALY_STATUS_V1_OK` | 成功 |
| 1 | `ANOMALY_STATUS_V1_INVALID_ARGUMENT` | 参数非法 |
| 2 | `ANOMALY_STATUS_V1_UNAVAILABLE` | 服务或能力尚未就绪 |
| 3 | `ANOMALY_STATUS_V1_NOT_FOUND` | 目标不存在或 cursor 过期 |
| 4 | `ANOMALY_STATUS_V1_BUFFER_TOO_SMALL` | 缓冲区不足 |
| 5 | `ANOMALY_STATUS_V1_FAILED` | 操作失败 |
| 6 | `ANOMALY_STATUS_V1_TIMEOUT` | 超时 |
| 7 | `ANOMALY_STATUS_V1_PERMISSION_DENIED` | capability 或授权不足 |
| 8 | `ANOMALY_STATUS_V1_CONFLICT` | 状态冲突 |
| 9 | `ANOMALY_STATUS_V1_CANCELLED` | 已取消 |

## 相关

- [插件开发](../developer-guide/plugin-development.md)
- [Manifest 与 capability](manifest-and-capabilities.md)
