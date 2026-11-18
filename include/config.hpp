#pragma once

#include "anomaly/i18n.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace anomaly {

struct ConfigDiagnostic final {
    std::string key;
    std::string message;
};

struct AnalyzerConfig {
    std::wstring pipe_prefix{L"Anomaly"};
    std::size_t max_scan_results{1024};

    bool platform_enabled{true};
    bool platform_visible{};
    bool platform_embedded{true};
    bool platform_attach_to_process_window{true};
    unsigned platform_toggle_key{0x2d};
    anomaly::LanguagePreference platform_language{anomaly::LanguagePreference::Auto};
    std::filesystem::path plugin_directory{L"plugins"};
    bool telemetry_enabled{true};
    double update_slow_milliseconds{2.0};
    double draw_slow_milliseconds{4.0};
    // 具体游戏的 Profile 标识由组合根（core_main）注入；游戏无关的配置
    // 模块不携带默认游戏名。
    std::string game_id;
    std::vector<ConfigDiagnostic> diagnostics;
    static AnalyzerConfig Load(const std::filesystem::path& path);
};

}  // namespace anomaly
