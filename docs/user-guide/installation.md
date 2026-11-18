# 安装与启动

## 运行包内容

一个完整的运行包至少包含：

```text
AnomalyLauncher.exe
Anomaly\
  Anomaly.Core.dll
  anomaly.ini
  plugins\
  locales\
  assets\
  config\
```

`Anomaly\` 是运行时根目录，必须与启动器一起部署。插件、配置和日志都放在这个目录下。

## 部署

1. 退出游戏。
2. 把运行包解压到一个固定位置。
3. 启动 `AnomalyLauncher.exe`。
4. 在 **启动** 页选择包含 `MaydayAirways.exe` 的游戏目录。

启动器会读取该目录下的 `Anomaly\` 来决定代理安装状态和可附加进程。

## 三种启动方式

### 启动并附加（推荐）

1. 在启动器选择 **启动并附加**。
2. 启动器通过 Steam 打开游戏（`steam://rungameid/5480890`）。
3. 等待 `MaydayAirways.exe` 出现后，把手动映射的 Core 注入游戏。

这种方式不修改游戏目录，也不依赖官方启动器。

### 注入运行中进程

先正常启动游戏，回到启动器点击 **注入到运行中的进程**。启动器会枚举 `MaydayAirways.exe` 并执行手动映射；失败不会重启或结束游戏。

也可以使用命令行工具：

```powershell
anomaly-inject --list
anomaly-inject --pid <PID>
```

### 代理模式

代理模式把 `dwmapi.dll` 安装到游戏目录，游戏启动时由代理加载 `Anomaly\Anomaly.Core.dll`。

1. 在启动器选择 **代理模式**。
2. 点击安装，确认目标目录包含 `MaydayAirways.exe`。
3. 从 Steam 正常启动游戏。

更新运行包时先退出游戏；启动器会检查游戏是否仍在运行并拒绝覆盖。卸载时使用启动器的卸载按钮，或删除游戏目录中的 `dwmapi.dll` 和 `Anomaly\`。

## 更新

- 启动器可以自行检查新版本并在退出游戏后替换运行包。
- 代理模式下更新前必须先关闭 `MaydayAirways.exe`。
- 运行包的 `Anomaly\` 目录会保留用户配置、插件和状态数据。

## 卸载

1. 退出游戏。
2. 如果安装过代理，先在启动器卸载代理。
3. 删除运行包目录和游戏目录中的 `Anomaly\`（如果代理模式曾把运行时装进游戏目录）。

## 下一步

- [快速上手](quickstart.md)
- [配置参考](configuration.md)
- [故障排查](troubleshooting.md)
