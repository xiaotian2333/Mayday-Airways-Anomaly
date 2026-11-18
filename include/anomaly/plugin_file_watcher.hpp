#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace anomaly {

struct PluginFileWatcherOptions {
    std::chrono::milliseconds poll_interval{100};
    std::chrono::milliseconds debounce{750};
};

struct PluginFileWatcherDiagnostics {
    std::uint64_t notifications{};
    std::uint64_t scans{};
    std::uint64_t changed_packages{};
    std::uint64_t total_scan_nanoseconds{};
    std::uint64_t maximum_scan_nanoseconds{};
};

class PluginFileWatcher final {
public:
    using Callback = std::function<void(std::vector<std::string>)>;
    using Clock = std::chrono::steady_clock;

    PluginFileWatcher(
        std::filesystem::path plugin_root,
        PluginFileWatcherOptions options = {});
    ~PluginFileWatcher();

    PluginFileWatcher(const PluginFileWatcher&) = delete;
    PluginFileWatcher& operator=(const PluginFileWatcher&) = delete;

    bool Start(Callback callback);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    void SetDiagnosticsEnabled(bool enabled) noexcept;
    [[nodiscard]] PluginFileWatcherDiagnostics Diagnostics() const noexcept;
    void ResetBaseline() noexcept;

    // Deterministic seam used by tests and diagnostic hosts.
    [[nodiscard]] std::vector<std::string> PollForTests(Clock::time_point now);

private:
    struct Observation {
        std::string signature;
        Clock::time_point changed_at{};
        bool pending{};
    };

    // failed 出参：根目录短暂不可访问导致整轮扫描失败时置 true，
    // 调用方据此跳过该轮 diff，避免把所有包误判为移除。
    [[nodiscard]] std::unordered_map<std::string, std::string> Scan(bool* failed = nullptr) const;
    [[nodiscard]] std::vector<std::string> PollLocked(Clock::time_point now);
    void Run(std::stop_token stop_token, void* change_handle);

    std::filesystem::path plugin_root_;
    PluginFileWatcherOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Observation> observations_;
    bool initialized_{};
    Callback callback_;
    std::jthread worker_;
    // 目录变更通知句柄失效（FindNextChangeNotification 失败等）时置 false，
    // 让 Running() 如实反映 watcher 是否仍在工作。
    std::atomic_bool healthy_{true};
    std::atomic_bool diagnostics_enabled_{};
    mutable std::atomic_uint64_t notifications_{};
    mutable std::atomic_uint64_t scans_{};
    mutable std::atomic_uint64_t changed_packages_{};
    mutable std::atomic_uint64_t total_scan_nanoseconds_{};
    mutable std::atomic_uint64_t maximum_scan_nanoseconds_{};
};

}  // namespace anomaly
