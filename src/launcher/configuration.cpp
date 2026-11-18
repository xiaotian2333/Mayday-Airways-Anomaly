#include "anomaly/launcher/configuration.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace anomaly::launcher {

bool IsCustomLauncherBackground(PlatformUiPalette palette,
    std::string_view background_path, bool enabled) {
    if (!enabled || background_path.empty()) return false;
    auto normalized = std::filesystem::path(std::u8string(background_path.begin(), background_path.end()))
        .lexically_normal().generic_u8string();
    for (auto& c : normalized) if (c >= u8'A' && c <= u8'Z') c = static_cast<char8_t>(c - u8'A' + u8'a');
    return palette != PlatformUiPalette::Naiwa || normalized != u8"assets/images/naiwa-background.png";
}
namespace {

constexpr std::uint64_t kMaximumConfigurationBytes = 64U * 1024U;
constexpr std::wstring_view kGameExecutableName = L"MaydayAirways.exe";

// "#rrggbb" is how the theme files spell a colour, so the launcher stores its own colours the same
// way: a value copied from the settings file drops straight into a theme layout.
std::string HexColorText(const anomaly::PlatformUiColor color) {
    const auto channel = [](const float value) {
        return static_cast<unsigned>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    char text[8]{};
    std::snprintf(text, sizeof(text), "#%02x%02x%02x",
        channel(color.red), channel(color.green), channel(color.blue));
    return text;
}

anomaly::PlatformUiColor ReadHexColor(const nlohmann::json& object, const char* key,
    const anomaly::PlatformUiColor fallback) {
    if (!object.contains(key) || !object.at(key).is_string()) return fallback;
    std::string value = object.at(key).get<std::string>();
    if (value.size() == 7 && value.front() == '#') value.erase(value.begin());
    if (value.size() != 6) return fallback;
    unsigned red{}, green{}, blue{};
    // sscanf_s 为 MSVC 全局命名空间扩展（C11 Annex K），无 std 限定。
    if (::sscanf_s(value.c_str(), "%2x%2x%2x", &red, &green, &blue) != 3) return fallback;
    return {red / 255.0f, green / 255.0f, blue / 255.0f, 1.0f};
}

class RegistryKey final {
public:
    RegistryKey() = default;
    ~RegistryKey() {
        if (value_ != nullptr) RegCloseKey(value_);
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    [[nodiscard]] HKEY* Put() noexcept { return &value_; }
    [[nodiscard]] HKEY Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    HKEY value_{};
};

struct SystemLocations final {
    std::vector<std::filesystem::path> roots;
};

std::wstring Fold(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool NameEquals(const std::filesystem::path& path, const std::wstring_view expected) {
    return Fold(path.filename().wstring()) == Fold(std::wstring(expected));
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
    if (path.empty()) return false;
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(path, error);
    return regular && !error;
}

std::string WideUtf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::system_error(GetLastError(), std::system_category());
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        throw std::system_error(GetLastError(), std::system_category());
    }
    return result;
}

std::wstring Utf8Wide(const std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) throw std::system_error(GetLastError(), std::system_category());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        throw std::system_error(GetLastError(), std::system_category());
    }
    return result;
}

std::string ReadText(const std::filesystem::path& path, DWORD& win32_error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        win32_error = GetLastError();
        if (win32_error == ERROR_SUCCESS) win32_error = ERROR_OPEN_FAILED;
        return {};
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > kMaximumConfigurationBytes) {
        win32_error = ERROR_FILE_TOO_LARGE;
        return {};
    }
    std::string result(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input && !input.eof()) {
        win32_error = ERROR_READ_FAULT;
        return {};
    }
    win32_error = ERROR_SUCCESS;
    return result;
}

std::optional<std::wstring> ReadRegistryString(
    const HKEY key, const wchar_t* value_name) {
    DWORD size{};
    if (RegGetValueW(
            key, nullptr, value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    DWORD type{};
    if (RegGetValueW(
            key, nullptr, value_name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            &type, value.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (type == REG_EXPAND_SZ && !value.empty()) {
        const DWORD expanded_size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (expanded_size != 0) {
            std::wstring expanded(expanded_size, L'\0');
            if (ExpandEnvironmentStringsW(
                    value.c_str(), expanded.data(), expanded_size) == expanded_size) {
                while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
                value = std::move(expanded);
            }
        }
    }
    return value.empty() ? std::nullopt : std::optional<std::wstring>(std::move(value));
}


std::optional<std::filesystem::path> EnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return std::nullopt;
    std::wstring value(size, L'\0');
    if (GetEnvironmentVariableW(name, value.data(), size) != size - 1) {
        return std::nullopt;
    }
    value.resize(size - 1);
    return std::filesystem::path(std::move(value));
}

void ReadSteamLibraryPaths(
    const std::filesystem::path& library_file,
    std::vector<std::filesystem::path>& roots) {
    std::ifstream input(library_file);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        try {
            const auto key = line.find("\"path\"");
            if (key == std::string::npos) continue;
            const auto first = line.find('"', key + 6);
            if (first == std::string::npos) continue;
            const auto second = line.find('"', first + 1);
            if (second == std::string::npos) continue;
            const std::string path = line.substr(first + 1, second - first - 1);
            if (!path.empty()) {
                roots.push_back(
                    std::filesystem::path(Utf8Wide(path)) / L"steamapps" / L"common");
            }
        } catch (...) {
            // 遇到坏行（如非 UTF-8 字节）只跳过该行，不中止整个库目录发现。
        }
    }
}

std::vector<std::filesystem::path> CollectSystemLocations() {
    std::vector<std::filesystem::path> roots;
    RegistryKey steam;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, steam.Put()) ==
        ERROR_SUCCESS) {
        if (const auto path = ReadRegistryString(steam.Get(), L"SteamPath")) {
            const std::filesystem::path root(*path);
            roots.push_back(root / L"steamapps" / L"common");
            ReadSteamLibraryPaths(root / L"steamapps" / L"libraryfolders.vdf", roots);
        }
    }
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        if (const auto parent = EnvironmentPath(variable)) {
            roots.push_back(*parent / L"Steam" / L"steamapps" / L"common");
            roots.push_back(*parent / L"SteamLibrary" / L"steamapps" / L"common");
            roots.push_back(*parent / L"Mayday Airways Demo");
        }
    }
    const DWORD drives = GetLogicalDrives();
    for (unsigned index = 0; index < 26; ++index) {
        if ((drives & (1UL << index)) == 0) continue;
        const std::filesystem::path drive(
            std::wstring{static_cast<wchar_t>(L'A' + index), L':', L'\\'});
        if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) continue;
        roots.push_back(drive / L"SteamLibrary" / L"steamapps" / L"common");
        roots.push_back(drive / L"Steam" / L"steamapps" / L"common");
        roots.push_back(drive / L"Games" / L"SteamLibrary" / L"steamapps" / L"common");
        roots.push_back(drive / L"Games" / L"Steam" / L"steamapps" / L"common");
        roots.push_back(drive / L"Mayday Airways Demo");
    }
    return roots;
}
std::wstring PathKey(const std::filesystem::path& path) {
    return Fold(path.lexically_normal().wstring());
}

void AddUnique(
    std::vector<std::filesystem::path>& paths, std::filesystem::path path) {
    if (path.empty()) return;
    path = path.lexically_normal();
    const std::wstring key = PathKey(path);
    if (std::ranges::none_of(paths, [&](const auto& existing) {
            return PathKey(existing) == key;
        })) {
        paths.push_back(std::move(path));
    }
}

void AddAncestors(
    std::vector<std::filesystem::path>& roots, std::filesystem::path path) {
    if (path.has_filename() && IsRegularFile(path)) path = path.parent_path();
    for (unsigned depth = 0; depth < 8 && !path.empty(); ++depth) {
        AddUnique(roots, path);
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}

std::optional<std::filesystem::path> FindGameDirectory(
    const std::vector<std::filesystem::path>& roots) {
    constexpr std::array<std::wstring_view, 4> candidates{
        L"", L"Mayday Airways Demo", L"Mayday Airways", L"MaydayAirways"};
    for (const auto& root : roots) {
        for (const auto relative : candidates) {
            const auto directory = relative.empty() ? root : root / relative;
            if (IsMaydayGameDirectory(directory)) return directory.lexically_normal();
        }
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path LauncherConfigurationPath(
    const std::filesystem::path& payload_root) noexcept {
    try {
        return payload_root.empty()
            ? std::filesystem::path{}
            : payload_root.lexically_normal() / L"AnomalyLauncher.json";
    } catch (...) {
        return {};
    }
}

LauncherConfigurationLoadResult LoadLauncherConfiguration(
    const std::filesystem::path& path) noexcept {
    LauncherConfigurationLoadResult result;
    try {
        if (path.empty()) {
            result.win32_error = ERROR_PATH_NOT_FOUND;
            result.message = "launcher configuration path is unavailable";
            return result;
        }
        DWORD read_error{};
        const std::string text = ReadText(path, read_error);
        if (read_error != ERROR_SUCCESS) {
            result.win32_error = read_error;
            result.message = read_error == ERROR_FILE_NOT_FOUND || read_error == ERROR_PATH_NOT_FOUND
                ? "launcher configuration does not exist" : "launcher configuration could not be read";
            return result;
        }
        const auto document = nlohmann::json::parse(text);
        if (!document.is_object()) {
            throw std::invalid_argument("unsupported launcher configuration shape");
        }
        const int schema_version = document.value("schemaVersion", 0);
        if (schema_version == 1 && document.contains("proxy") &&
            document.at("proxy").is_object() && document.contains("attach") &&
            document.at("attach").is_object()) {
            result.configuration.game.game_directory = Utf8Wide(
                document.at("proxy").value("gameDirectory", std::string{}));
        } else if (schema_version == 2 && document.contains("clients") &&
                   document.at("clients").is_object()) {
            const auto& clients = document.at("clients");
            const auto read_directory = [&](const char* key) -> std::filesystem::path {
                if (!clients.contains(key) || !clients.at(key).is_object()) return {};
                return Utf8Wide(clients.at(key).value("gameDirectory", std::string{}));
            };
            auto directory = read_directory("mainlandChina");
            if (directory.empty()) directory = read_directory("global");
            result.configuration.game.game_directory = std::move(directory);
        } else if (schema_version == 3) {
            result.configuration.game.game_directory = Utf8Wide(
                document.value("gameDirectory", std::string{}));
        } else {
            throw std::invalid_argument("unsupported launcher configuration shape");
        }
        // A configuration written before the launcher had a theme carries no name
        // and keeps the default the struct declares.
        if (document.contains("theme") && document.at("theme").is_string()) {
            const std::string theme = document.at("theme").get<std::string>();
            if (!theme.empty()) result.configuration.theme = theme;
        }
        if (document.contains("language") && document.at("language").is_string()) {
            const std::string language = document.at("language").get<std::string>();
            if (!language.empty()) result.configuration.language = language;
        }
        if (document.contains("artwork") && document.at("artwork").is_object()) {
            const auto& artwork = document.at("artwork");
            LauncherThemeArtwork& target = result.configuration.artwork;
            target.background = artwork.value("background", std::string{});
            target.background_mode = std::clamp(
                artwork.value("backgroundMode", target.background_mode), -1, 3);
            target.surface_opacity = std::clamp(
                artwork.value("surfaceOpacity", target.surface_opacity), -1.0f, 1.0f);
        }
        if (document.contains("update") && document.at("update").is_object()) {
            const auto& update = document.at("update");
            result.configuration.update.auto_check =
                update.value("autoCheck", result.configuration.update.auto_check);
            result.configuration.update.endpoint =
                update.value("endpoint", std::string{});
            result.configuration.update.skipped_version =
                update.value("skippedVersion", std::string{});
        }
        if (document.contains("customColors") && document.at("customColors").is_object()) {
            const auto& colors = document.at("customColors");
            anomaly::PlatformUiCustomColors& target = result.configuration.custom_colors;
            target.accent = ReadHexColor(colors, "accent", target.accent);
            target.text = ReadHexColor(colors, "text", target.text);
            target.window_background =
                ReadHexColor(colors, "windowBackground", target.window_background);
            target.child_background =
                ReadHexColor(colors, "childBackground", target.child_background);
            target.border = ReadHexColor(colors, "border", target.border);
        }
        result.loaded = true;
        result.message = "launcher configuration loaded";
    } catch (const std::exception& error) {
        result.configuration = {};
        result.win32_error = ERROR_INVALID_DATA;
        result.message = error.what();
    } catch (...) {
        result.configuration = {};
        result.win32_error = ERROR_INVALID_DATA;
        result.message = "launcher configuration is invalid";
    }
    return result;
}

LauncherConfigurationSaveResult SaveLauncherConfiguration(
    const std::filesystem::path& path,
    const LauncherConfiguration& configuration) noexcept {
    LauncherConfigurationSaveResult result;
    std::filesystem::path temporary;
    try {
        if (path.empty()) {
            result.win32_error = ERROR_PATH_NOT_FOUND;
            result.message = "launcher configuration path is unavailable";
            return result;
        }
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            result.win32_error = static_cast<DWORD>(error.value());
            result.message = "launcher configuration directory could not be created";
            return result;
        }
        const nlohmann::json document{
            {"schemaVersion", 3},
            {"gameDirectory", WideUtf8(configuration.game.game_directory.wstring())},
            {"theme", configuration.theme},
            {"language", configuration.language},
            {"artwork", {
                {"background", configuration.artwork.background},
                {"backgroundMode", configuration.artwork.background_mode},
                {"surfaceOpacity", configuration.artwork.surface_opacity},
            }},
            {"customColors", {
                {"accent", HexColorText(configuration.custom_colors.accent)},
                {"text", HexColorText(configuration.custom_colors.text)},
                {"windowBackground", HexColorText(configuration.custom_colors.window_background)},
                {"childBackground", HexColorText(configuration.custom_colors.child_background)},
                {"border", HexColorText(configuration.custom_colors.border)},
            }},
            {"update", {
                {"autoCheck", configuration.update.auto_check},
                {"endpoint", configuration.update.endpoint},
                {"skippedVersion", configuration.update.skipped_version},
            }},
        };
        const std::string text = document.dump(2) + '\n';
        temporary = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetCurrentThreadId());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            result.win32_error = GetLastError();
            if (result.win32_error == ERROR_SUCCESS) result.win32_error = ERROR_OPEN_FAILED;
            result.message = "launcher configuration temporary file could not be opened";
            return result;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        output.close();
        if (!output) {
            result.win32_error = ERROR_WRITE_FAULT;
            result.message = "launcher configuration could not be written";
            std::filesystem::remove(temporary, error);
            return result;
        }
        if (MoveFileExW(
                temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            result.win32_error = GetLastError();
            result.message = "launcher configuration could not be replaced atomically";
            std::filesystem::remove(temporary, error);
            return result;
        }
        result.message = "launcher configuration saved";
    } catch (const std::exception& error) {
        result.win32_error = ERROR_WRITE_FAULT;
        result.message = error.what();
        std::error_code ignored;
        if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
    } catch (...) {
        result.win32_error = ERROR_WRITE_FAULT;
        result.message = "launcher configuration could not be saved";
        std::error_code ignored;
        if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
    }
    return result;
}

bool IsMaydayGameDirectory(const std::filesystem::path& directory) noexcept {
    try {
        return !directory.empty() && IsRegularFile(directory / kGameExecutableName);
    } catch (...) {
        return false;
    }
}

LauncherClientConfiguration DiscoverLauncherConfiguration(
    const LauncherClientConfiguration& preferred,
    const LauncherDiscoveryOptions& options) {
    LauncherClientConfiguration result;
    if (IsMaydayGameDirectory(preferred.game_directory)) {
        result.game_directory = preferred.game_directory.lexically_normal();
    }

    std::vector<std::filesystem::path> roots;
    for (const auto& root : options.search_roots) AddUnique(roots, root);
    AddAncestors(roots, options.payload_root);
    if (options.include_system_locations) {
        for (auto root : CollectSystemLocations()) AddUnique(roots, std::move(root));
    }

    for (const auto& executable : options.running_game_executables) {
        if (NameEquals(executable, kGameExecutableName) && IsRegularFile(executable)) {
            if (result.game_directory.empty()) {
                result.game_directory = executable.parent_path().lexically_normal();
            }
            AddAncestors(roots, executable.parent_path());
        }
    }
    AddAncestors(roots, result.game_directory);

    if (result.game_directory.empty()) {
        if (const auto discovered = FindGameDirectory(roots)) {
            result.game_directory = *discovered;
        }
    }
    return result;
}

}  // namespace anomaly::launcher
