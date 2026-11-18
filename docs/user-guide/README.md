# 用户文档

这里写 Mayday Loader 的日常用法：安装、启动、插件和排错。要从源码构建或编写插件，请看[开发者文档](../developer-guide/README.md)。

## 阅读顺序

1. [安装与启动](installation.md) — 部署运行包，选择 Steam 启动、注入或代理模式。
2. [快速上手](quickstart.md) — 第一次打开覆盖层并启用插件。
3. [内建插件](built-in-plugins.md) — Money 与 Freecam 的用法。
4. [第三方插件](third-party-plugins.md) — 安装、更新与卸载插件。
5. [配置参考](configuration.md) — 切换键、语言、插件目录与运行时配置。
6. [主题与背景](theme.md) — 配色、背景和贴纸。
7. [诊断 CLI](diagnostics-cli.md) — 查看 Runtime 与内存状态。
8. [故障排查](troubleshooting.md) — 覆盖层、插件、启动与代理问题。

## 术语

| 术语 | 含义 |
| --- | --- |
| **Runtime / Core** | 注入游戏进程的 `Anomaly.Core.dll`。 |
| **代理** | 游戏目录内的 `dwmapi.dll`，随游戏启动引导 Core。 |
| **平台 / Platform** | Runtime 内的覆盖层与插件管理界面。 |
| **插件** | 含 `manifest.json` 与 `plugin.dll` 的目录包。 |
| **服务** | Host 通过纯 C ABI 提供给插件的版本化能力，如 `mayday.money`。 |

> 使用前请阅读根目录 [README](../../README.md) 中的免责声明。
