# Mayday 适配层（Unity IL2CPP）

Mayday Airways (Steam Demo) 是 Unity IL2CPP + Mirror 网络游戏，
与上游 Anomaly 的引擎适配层技术路径完全不同：

- 引擎符号：不走特征码 Profile，而是使用 il2cpp 导出函数
  （il2cpp_domain_get / il2cpp_class_from_name / il2cpp_runtime_invoke ...）
  以及 Il2CppDumper 生成的 script.json / dummydll 做类与方法解析
- 游戏对象访问：GameState（Mirror NetworkBehaviour 单例）等已知类型直接解析
- Hook 目标：il2cpp 函数指针 / D3D11 Present（渲染后端为 D3D11 + uGUI，非 D3D12）

实现待补（骨架阶段）。可复用经验见 Mayday-Airways-ce 仓库的 MelonLoader 版：
GameState.Money SyncVar 直接读写、ServerAddMoney 记账入口等。
