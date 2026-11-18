#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace anomaly {

struct PluginConfigWatcherOptions {
    std::chrono::milliseconds debounce{750};
};

// Watches one file inside a flat directory using a Windows directory change
// notification. The target file is re-signed after the configured debounce, so
// temp-file then rename publish sequences settle before callers observe the
// new content.
class PluginConfigFileWatcher final {
public:
    using Callback = std::function<void()>;
    using Clock = std::chrono::steady_clock;

    PluginConfigFileWatcher(
        std::filesystem::path directory,
        std::filesystem::path file_name,
        PluginConfigWatcherOptions options = {});
    ~PluginConfigFileWatcher();

    PluginConfigFileWatcher(const PluginConfigFileWatcher&) = delete;
    PluginConfigFileWatcher& operator=(const PluginConfigFileWatcher&) = delete;

    [[nodiscard]] bool Start(Callback callback);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    void ResetBaseline() noexcept;

private:
    [[nodiscard]] std::filesystem::path Target() const;
    [[nodiscard]] std::uint64_t Signature() const;
    void Run(std::stop_token stop_token, void* change_handle);

    std::filesystem::path directory_;
    std::filesystem::path file_name_;
    PluginConfigWatcherOptions options_;
    mutable std::mutex mutex_;
    bool pending_{};
    std::uint64_t last_signature_{};
    Callback callback_;
    std::jthread worker_;
    // 目录变更通知句柄失效（FindNextChangeNotification 失败等）时置 false，
    // 让 Running() 如实反映 watcher 是否仍在工作。
    std::atomic_bool healthy_{true};
};

}  // namespace anomaly
