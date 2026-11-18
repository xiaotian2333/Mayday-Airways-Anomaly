<div align="center">

# Mayday Loader

**Mayday Airways (Steam Demo) 的插件平台 —— 基于 Anomaly 框架改造**

</div>

---

> [!NOTE]
> 本仓库 fork 自 [AnomalyNTE/Anomaly](https://github.com/AnomalyNTE/Anomaly)
> （《异环》插件平台，AGPL-3.0），正在改造为 **Mayday Airways Demo**（Unity IL2CPP）
> 的 mod 加载器。改造进度与计划见 [`docs/mayday-roadmap.md`](docs/mayday-roadmap.md)。

## 定位

- 🚀 **注入式加载器**：启动器以注入模式把 Runtime 带入游戏进程，不往游戏目录写文件
- 🧩 **插件框架**：复用上游纯 C ABI v1 SDK 与插件宿主（Scope ledger / 热重载）
- 🪟 **游戏内管理界面**：ImGui 界面统一管理插件启用、停用与设置
- 🔍 **开发工具**：内存诊断、签名扫描、离线 TestHost 均随框架保留

## 状态

- ✅ 通用基础设施完整保留（见 roadmap 表格）
- ✅ 游戏适配层：il2cpp 导出探测（`mayday.il2cpp`）与金币/债务服务（`mayday.money`）
- ✅ D3D12 Present hook + 内嵌 ImGui 管理界面（Mayday 实际使用 D3D12）
- ✅ 内建插件 `plugins/money`：金币修改面板，实机读写验证通过
- 📦 更多游戏服务与插件按需实现

## 构建

```pwsh
cmake -B .build/cfg -G "Visual Studio 17 2022" -A x64
```

前置与依赖说明沿用上游 `docs/developer-guide/`。

## 许可

[AGPL-3.0-only](LICENSE)，上游版权与第三方组件见 [`NOTICE`](NOTICE)。
