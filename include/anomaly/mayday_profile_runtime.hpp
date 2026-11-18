#pragma once

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>

namespace anomaly {

class MaydayAdapter;

// Mayday Airways (Unity IL2CPP) 的 Profile 运行时。
// 上游 NteProfileRuntime 的 Mayday 对应物：供 core_main 的服务图注册与
// 诊断快照使用；引擎侧不再做 UE5 特征码 Profile，而是等待 GameAssembly.dll
// 并解析 il2cpp 导出（见 MaydayAdapter）。
class MaydayProfileRuntime final {
public:
    MaydayProfileRuntime();
    ~MaydayProfileRuntime();
    MaydayProfileRuntime(const MaydayProfileRuntime&) = delete;
    MaydayProfileRuntime& operator=(const MaydayProfileRuntime&) = delete;

    [[nodiscard]] bool Start(std::stop_token stop_token);
    bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] std::shared_ptr<MaydayAdapter> Adapter() const;
    [[nodiscard]] std::string DiagnosticsJson() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
