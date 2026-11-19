# Mayday 适配层（Unity IL2CPP）

Mayday Airways (Steam Demo) 是 Unity IL2CPP + Mirror 网络游戏，
与上游 Anomaly 的引擎适配层技术路径完全不同：

- 引擎符号：使用 il2cpp 导出函数
  （il2cpp_domain_get / il2cpp_class_from_name / il2cpp_runtime_invoke ...），
  类、方法与字段在运行时按名称解析并校验
- 游戏对象访问：GameState（Mirror NetworkBehaviour 单例）等已知类型直接解析
- Hook 目标：il2cpp 函数指针 / D3D12 Present（渲染后端为 D3D12 + uGUI）

实现状态：il2cpp 解析、`mayday.money` 与 `mayday.freecam` 已落地。金币服务遵循以下已验证语义：

- `Money` 是 Mirror SyncVar，持 `GameState.Instance` 读写，不受堆地址随机化影响；
- 直接写 `Money` 不进入游戏累计收入统计；「增加」调用 `ServerAddMoney`，进入记账并触发飘字；
- 债务走 `ServerAddDebt` 的差额入口（当前债务 + delta = 目标值）。
