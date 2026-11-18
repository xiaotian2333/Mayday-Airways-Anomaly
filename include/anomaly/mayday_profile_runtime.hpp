#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace anomaly {

class MaydayAdapter;

// Mayday Airways (Unity IL2CPP) 的 Profile 运行时。
// 供 core_main 的服务图注册与诊断快照使用；引擎侧不做 UE5 特征码 Profile，
// 而是等待 GameAssembly.dll 并解析 il2cpp 导出（见 MaydayAdapter）。
class MaydayProfileRuntime final {
public:
    // 诊断日志汇：宿主把适配层的解析/失败事件写入运行时日志流。
    using LogSink = std::function<void(const char* event_id, const std::string& message)>;

    MaydayProfileRuntime();
    ~MaydayProfileRuntime();
    MaydayProfileRuntime(const MaydayProfileRuntime&) = delete;
    MaydayProfileRuntime& operator=(const MaydayProfileRuntime&) = delete;

    // Start 前注入；空 sink 时事件只进诊断字段。
    void SetLogSink(LogSink sink);
    [[nodiscard]] bool Start(std::stop_token stop_token);
    bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] std::shared_ptr<MaydayAdapter> Adapter() const;
    [[nodiscard]] std::string DiagnosticsJson() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
