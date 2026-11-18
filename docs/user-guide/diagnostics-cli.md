# 诊断 CLI

`anomaly-cli` 通过诊断管道连接已经注入 Core 的游戏进程，读取 Runtime、服务和内存状态。

## 连接

```powershell
anomaly-cli --pid <PID> status
```

PID 是 `MaydayAirways.exe` 的进程号。也可以使用 `--pipe <name>` 连接自定义管道。

## 常用命令

| 命令 | 用途 |
| --- | --- |
| `status` | Runtime、插件宿主、服务图和管道状态 |
| `modules` | 目标进程模块列表 |
| `sections <module>` | 模块节区 |
| `regions <module>` | 内存区域 |
| `read <address> <size>` | 读取内存 |
| `chain <base> [offset ...]` | 跟随指针链 |
| `scan <module> <pattern>` | 按字节模式扫描 |
| `snapshot <name>` | 写入诊断快照文件 |

示例：

```powershell
anomaly-cli --pid 12345 status
anomaly-cli --pid 12345 modules
anomaly-cli --pid 12345 scan GameAssembly.dll "48 8B ?? ?? ?? ??"
anomaly-cli --pid 12345 snapshot mayday-state
```

## 注意

- 只对自有进程或玩家自己启动的本机游戏使用。
- `write`、`patch`、`protect`、`alloc`、`free` 会修改目标进程，必须得到明确授权。
- 快照和日志可能包含路径、账号或游戏状态；分享前先审查并脱敏。

## 相关

- [故障排查](troubleshooting.md)
- [API 参考 · 平台服务](../api-reference/platform-services.md)
