# Manifest 与 capability

对应 schema：[`schemas/plugin-manifest.schema.json`](../../schemas/plugin-manifest.schema.json)；
capability 策略源：`src/plugin/plugin_capability_policy.cpp`。

## Manifest v2

每个插件目录包必须包含 `manifest.json`，`schemaVersion` 固定为 `2`。

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `schemaVersion` | ✅ | 固定 `2` |
| `id` | ✅ | `<作者>.<插件名>`，小写 kebab 标签 |
| `name` | ✅ | 英文显示名 |
| `description` | ✅ | 英文简介 |
| `author` | ✅ | 作者 |
| `license` | ✅ | SPDX identifier |
| `version` | ✅ | 语义化版本 |
| `entry` | ✅ | DLL 文件名 |
| `api` | ✅ | `major` / `minMinor` / `maxMinor` |
| `games` | ✅ | 兼容游戏，例如 `["mayday"]` |
| `builds` | ✅ | build 模式，例如 `["mayday-*"]` |
| `loadPhase` |  | `game-ready` 等 |
| `services` |  | required / optional 服务 |
| `capabilities` |  | 插件需要的 capability |

示例：

```json
{
  "schemaVersion": 2,
  "id": "example.money-tools",
  "name": "Money Tools",
  "description": "Read and edit money.",
  "author": "Example",
  "license": "MIT",
  "version": "1.0.0",
  "entry": "plugin.dll",
  "api": { "major": 1, "minMinor": 0, "maxMinor": 0 },
  "games": ["mayday"],
  "builds": ["mayday-*"],
  "loadPhase": "game-ready",
  "services": [
    { "id": "anomaly.ui", "minVersion": 1 },
    { "id": "anomaly.localization", "minVersion": 1 },
    { "id": "mayday.money", "minVersion": 1 }
  ],
  "capabilities": ["ui", "mayday-money"]
}
```

## Capability 列表

`mods`、`commands`、`configuration`、`diagnostics`、`entity-esp`、`ipc`、`game-events`、
`interop-hook`、`interop-patch`、`interop-signature`、`mayday-combat`、`mayday-freecam`、
`mayday-money`、`mayday-voice`、`mayday-weather`、`memory-read`、`memory-write`、
`notifications`、`runtime-info`、`scheduler`、`storage`、`input`、`ui-font`、`ui-overlay`、
`ui-texture`、`ui-window`、`ui`、`json`、`websocket`。

## 服务映射

| 服务 | capability |
| --- | --- |
| `anomaly.mods` | `mods` |
| `anomaly.plugin-state` | `configuration` |
| `anomaly.config` | `configuration` |
| `anomaly.storage` | `storage` |
| `anomaly.runtime-info` | `runtime-info` |
| `anomaly.diagnostics` | `diagnostics` |
| `anomaly.scheduler` | `scheduler` |
| `anomaly.ipc` | `ipc` |
| `anomaly.websocket` | `websocket` |
| `anomaly.commands` | `commands` |
| `anomaly.notifications` | `notifications` |
| `anomaly.interop.signature` | `interop-signature` |
| `anomaly.interop.hook` | `interop-hook` |
| `anomaly.interop.patch` | `interop-patch` |
| `mayday.money` | `mayday-money` |
| `mayday.freecam` | `mayday-freecam` |
| `mayday.weather` | `mayday-weather` |
| `mayday.combat` | `mayday-combat` |
| `mayday.voice` | `mayday-voice` |
| `anomaly.ui` | `ui` |
| `anomaly.localization` | `ui` |
| `anomaly.window` | `ui-window` |
| `anomaly.font` | `ui-font` |
| `anomaly.texture` | `ui-texture` |
| `anomaly.input` | `input` |
| `anomaly.json` | `json` |
| `anomaly.profile` | `json` |

`anomaly.core` 不需要 capability。required service 缺少映射或 capability 时，包会在 DLL 映射前被拒绝。

## 授权规则

- 原始内存读取需要 `memory-read`，写入需要 `memory-write`。
- capability 只约束服务可见性与资源归属，不是同进程 native 代码的安全沙箱。
- `anomaly.core` 不会自动授予原始内存访问；优先使用语义服务。

## 相关

- [API 参考总览](README.md)
- [插件开发](../developer-guide/plugin-development.md)
