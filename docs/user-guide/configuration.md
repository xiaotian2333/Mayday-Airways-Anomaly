# 配置参考

## 运行时配置

运行时配置位于 `Anomaly\anomaly.ini`。修改后重启游戏或重新注入生效。

```ini
[Analyzer]
PipePrefix=Anomaly
MaxScanResults=1024

[Platform]
Enabled=1
Visible=0
Embedded=1
AttachToProcessWindow=1
ToggleKey=45
Language=auto
PluginDirectory=plugins

[Telemetry]
Enabled=1

[Performance]
UpdateSlowMilliseconds=2
DrawSlowMilliseconds=4

[Profiles]
Game=mayday
```

| 键 | 说明 |
| --- | --- |
| `Platform.ToggleKey` | 覆盖层热键，`45` 是 `Insert`。也可以在游戏内重新绑定。 |
| `Platform.Visible` | `1` 时启动即展开覆盖层，仅用于调试。 |
| `Platform.Language` | `auto`、`zh-CN` 或 `en-US`。 |
| `Platform.PluginDirectory` | 相对 `Anomaly\` 的插件目录，默认 `plugins`。 |
| `Telemetry.Enabled` | 匿名使用统计开关。 |
| `Performance.UpdateSlowMilliseconds` | Game Update 慢回调阈值。 |
| `Performance.DrawSlowMilliseconds` | Draw 慢回调阈值。 |
| `Profiles.Game` | 插件兼容性使用的游戏标识，Mayday 固定为 `mayday`。 |

## 启动器配置

启动器配置位于启动器同级的 `AnomalyLauncher.json`（schemaVersion 3），包括：

- `gameDirectory`：游戏目录。
- `theme`、`language`：启动器外观与语言。
- `artwork`、`customColors`：背景和自定义配色。
- `view`：模组页布局与分页。
- `update`：自动检查更新、镜像端点和跳过版本。

旧版本的 `clients` / `selectedClient` 字段只在读取时用于迁移，保存后会写成单游戏格式。

## 插件配置

插件的开关、数值和热键通过插件窗口修改，由宿主 `anomaly.config` 服务持久化到 `Anomaly\config\plugins\<id>\`。不要在 Game/Render 回调中直接读写文件。

## 相关

- [安装与启动](installation.md)
- [故障排查](troubleshooting.md)
