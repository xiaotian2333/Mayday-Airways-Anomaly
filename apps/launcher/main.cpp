#include "anomaly/liquid_glass.hpp"
#include "anomaly/launcher/manual_map.hpp"
#include "anomaly/launcher/mod_archive.hpp"
#include "anomaly/launcher/configuration.hpp"
#include "anomaly/launcher/proxy_installation.hpp"
#include "anomaly/launcher/release_update.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/platform_notice.hpp"
#include "anomaly/sdk/version.h"
#include "anomaly/platform_ui_layout.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/telemetry.hpp"
#include "anomaly/runtime_launch.hpp"
#include "anomaly/reliable_storage.hpp"
#include "anomaly/plugin_package.hpp"
#include "anomaly/ui_resource_decoder.hpp"
#include "config.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <winhttp.h>
#include <winreg.h>
#include <TlHelp32.h>
#include <wincrypt.h>
#include <commdlg.h>
#include <d3d11.h>
// The window's content is a composition swap chain rather than a window-bound one,
// which is what gives its pixels an alpha channel. IDXGIFactory2 comes from the 1.2
// header, IDCompositionDevice from dcomp.
#include <dcomp.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kLogoResourceId = 101;
constexpr int kIconResourceId = 201;
// Where the project itself answers. The about section at the foot of the settings page offers these
// two, so the addresses live here with the other links rather than inside the drawing code.
constexpr std::string_view kRepositoryUrl{"https://github.com/xiaotian2333/Mayday-Airways-Anomaly"};
constexpr std::string_view kDiscordUrl{"https://discord.gg/0000"};
// Sized for the launcher's 17px font. These were laid out for 13px text, and a
// child that cannot fit its own contents grows a scrollbar instead of clipping,
// which is what a mode row of 48px did to a 30px button plus its padding.
// The launcher draws on a themed surface with a wallpaper and its veil behind it,
// where the palette's own muted ink is too pale to read on a light body. Every
// secondary label here uses this tone instead whenever the palette is one of the
// light ones, which is also what ImGuiCol_TextDisabled is set to, so disabled and
// secondary text stay consistent.
const ImVec4 kLauncherMutedInkLightSurface(0.24f, 0.21f, 0.15f, 1.0f);
constexpr float kHeaderHeight = 44.0f;
// The shell canvas. It is the whole window except for one transparent band above it, and nothing
// may be laid out past its edges: a sticker that overhangs is clipped at the canvas, which is why
// the band exists only where a sticker actually reaches.
constexpr float kLauncherCanvasWidth = 1280.0f;
constexpr float kLauncherCanvasHeight = 860.0f;
// The transparent band above the canvas, in canvas pixels, for the launcher's own sticker. The
// geometry comes from config/themes/<palette>.launcher.json, checked against what the runtime
// actually places (the layout drops its `height` and derives it from the artwork's aspect):
//   width  0.614291 * 1280 = 786.29
//   height 786.29 * 2722 / 1956 = 1094.15
//   centre 0.421821 * 860 = 362.77  ->  image top 362.77 - 547.08 = -184.31, i.e. 184.31 px above
//   the canvas; its first opaque row is 301 of 2722, 121.03 px below that edge, so the artwork
//   starts 63.28 px above the canvas.
// Six more pixels keep that top edge off the window border, where a cut sticker looks clipped.
constexpr float kLauncherBandTop = 69.3f;
constexpr float kModeHeight = 76.0f;
// The lower command row both launch sub-modes anchor their own buttons to: this far above the page's
// bottom edge, and this tall. The sponsor button shares that line, which is why the two numbers live
// here rather than in each of the three places that place something on it.
constexpr float kLaunchActionRowBottom = 82.0f;
constexpr float kLaunchActionRowHeight = 62.0f;
constexpr float kProxyActionLeftPadding = 4.0f;
// The band's "a newer release exists" chip is drawn on the modes row, which is a child window of the
// root, while the update dialog belongs to the root. The chip therefore leaves a request here and the
// dialog takes it: the answer to "do you want to update" is the same dialog the check opens, wherever
// the user asked for it from.
bool g_update_prompt_requested{};
constexpr float kLauncherFontScale = 20.0f / 13.0f;
constexpr float kDefaultDpi = 96.0f;

ImVec4 ThemeColor(const ue5mem::PlatformUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

ImVec4 ThemeColorWithAlpha(
    const ue5mem::PlatformUiColor& color, const float alpha) noexcept {
    return {color.red, color.green, color.blue, alpha};
}

// The launcher paints its own body from the palette's surface colour, under a wallpaper
// and its veil, and the palettes it can wear are not all light: Naiwa and Paper are cream
// and paper with dark cocoa ink, the other presets are dark with light ink. The surface
// therefore decides which side of that line every label, chip and button lands on, so a
// theme switch in the settings page cannot leave black text on a dark body.
bool LauncherSurfaceIsLight() noexcept {
    const auto surface = ue5mem::PlatformUiSurfaceColor();
    return 0.2126f * surface.red + 0.7152f * surface.green +
        0.0722f * surface.blue >= 0.5f;
}

ImVec4 LauncherInk() noexcept {
    // Plain black on a light surface: the palette's own ink is tuned for the shell's
    // panels, and those are empty while a wallpaper is showing.
    return LauncherSurfaceIsLight()
        ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
        : ThemeColor(ue5mem::PlatformUiTheme().text);
}

ImVec4 LauncherMutedInk() noexcept {
    return LauncherSurfaceIsLight()
        ? kLauncherMutedInkLightSurface
        : ThemeColor(ue5mem::PlatformUiTheme().text_muted);
}

enum class LauncherMode : std::uint8_t { Launch, Settings };
enum class MessageKind : std::uint8_t { Neutral, Success, Error };

// The state of the launcher's own update, as the settings page reports it. The phases are the
// steps the update actually takes, so the page can only ever show a step that happened.
enum class LauncherUpdatePhase : std::uint8_t {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Installing,
    Installed,
    Failed,
};

// What a failed update step was. The launcher names the ones it can explain -- an unreachable host,
// a rate-limited API -- and leaves the rest to the installer, whose diagnostic says what could not
// be written.
enum class LauncherUpdateFailure : std::uint8_t {
    None,
    Network,
    RateLimited,
    Http,
    NotRelease,
    Check,
    Install,
};

// What the last check found and, once it is asked for, how far the update got. Everything here is
// read from the snapshot; the phase is the only field the page branches on.
struct LauncherUpdateStatus final {
    LauncherUpdatePhase phase{LauncherUpdatePhase::Idle};
    bool auto_check{true};
    // The release the user answered "skip this version" to: the check keeps reporting it, but the
    // launcher does not ask about that one version again.
    std::string skipped_version;
    std::string remote_tag;
    std::string remote_version;
    std::string page_url;
    // The package the check chose: its address, its published length and its published digest, which
    // is what a download is verified against before anything is extracted.
    anomaly::launcher::ReleaseAsset asset;
    // The release notes the dialog shows before the user decides.
    std::string notes;
    LauncherUpdateFailure failure{LauncherUpdateFailure::None};
    // The HTTP status of a failed lookup, and the diagnostic a failed install reported.
    int http_status{};
    std::string detail;

    [[nodiscard]] bool Busy() const noexcept {
        return phase == LauncherUpdatePhase::Checking ||
            phase == LauncherUpdatePhase::Downloading ||
            phase == LauncherUpdatePhase::Installing;
    }

    [[nodiscard]] bool CanInstall() const noexcept {
        return !asset.download_url.empty() &&
            (phase == LauncherUpdatePhase::Available ||
             (phase == LauncherUpdatePhase::Failed && failure == LauncherUpdateFailure::Install));
    }
};

// One decoded theme image. The worker decodes and the render thread only
// uploads, so a large background never stalls on WIC during a frame.
struct LauncherThemeImage final {
    // Only the upload reads these; they are dropped as soon as the texture owns the
    // picture, which is why the field is mutable behind a shared const theme.
    mutable std::vector<std::uint8_t> pixels;
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool Ok() const noexcept {
        // Dimensions are set together with a successful decode, so this keeps saying
        // yes after the pixels are gone.
        return width != 0 && height != 0;
    }
};

// Everything the launcher needs to dress itself: the palette chosen in its own
// settings page, plus the background and stickers from config/themes.
struct LauncherTheme final {
    std::uint64_t version{};
    anomaly::PlatformUiPalette palette{anomaly::PlatformUiPalette::Naiwa};
    anomaly::PlatformUiLayoutDocument layout;
    LauncherThemeImage background;
    std::vector<LauncherThemeImage> stickers;

    [[nodiscard]] bool HasBackground() const noexcept {
        return background.Ok() &&
            layout.background.mode != anomaly::PlatformUiBackgroundMode::Disabled;
    }
};

struct LauncherMessage final {
    anomaly::MessageId id{anomaly::MessageId::LauncherStateReady};
    std::vector<std::string> arguments;
    std::string detail;
};

// Which mode a status message belongs to. The controller keeps a single message, so
// without this an attach outcome stayed on screen after switching to the proxy mode,
// where it described something that mode cannot do. Shared covers what neither mode
// owns: the initial scan, settings and hotkey writes.
enum class MessageScope { Both, Proxy, Attach };

// When a process rescan is allowed to write the status line: a background poll must not overwrite an
// outcome, but it does have to correct a status that has become untrue.
enum class ProcessAnnounce { Never, Always, OnChange };

struct LauncherSnapshot final {
    std::filesystem::path game_directory;
    anomaly::launcher::ProxyInstallationStatus proxy;
    std::vector<anomaly::launcher::AttachableProcess> processes;
    DWORD attached_process{};
    bool core_available{};
    std::string runtime_version;
    std::string runtime_message;
    std::uint32_t toggle_key{VK_INSERT};
    // The launcher's own language and page preferences, straight from its configuration.
    std::string language{"auto"};
    // The pictures and the custom colours this window was dressed with: the theme carries the
    // artwork, the colours are applied on top of it every frame so dragging a colour updates the
    // window without decoding a wallpaper again.
    anomaly::launcher::LauncherThemeArtwork artwork;
    anomaly::PlatformUiCustomColors custom_colors;
    bool busy{};
    LauncherMessage message;
    MessageKind message_kind{MessageKind::Neutral};
    MessageScope message_scope{MessageScope::Both};
    LauncherUpdateStatus update;
    // Set once the new image is in place and has been started: the window is done, because it is
    // drawn by the image that was replaced.
    bool restart_pending{};
    std::shared_ptr<const LauncherTheme> theme;
};

// The launcher wears whichever palette the settings page stored, and Naiwa -- the
// shipped default -- when nothing was chosen. It deliberately does not follow the
// palette picked in the in-game settings: the two windows are separate surfaces, and a
// launcher that changed colour with every launch would lose the identity it has as the
// first thing a user sees. The layout is resolved for the launcher rather than for the
// game: config/themes/<palette>.launcher.json when the theme ships one, which is how a
// sticker can sit correctly on a 1100x860 window and on the in-game shell at once.
// The fill mode travels through the configuration as an int, the way the in-game settings keep it, so
// it round-trips without a separate string enum. Anything unknown means "do not draw a wallpaper".
anomaly::PlatformUiBackgroundMode BackgroundModeFromInt(const int value) noexcept {
    switch (value) {
    case static_cast<int>(anomaly::PlatformUiBackgroundMode::Center):
        return anomaly::PlatformUiBackgroundMode::Center;
    case static_cast<int>(anomaly::PlatformUiBackgroundMode::Stretch):
        return anomaly::PlatformUiBackgroundMode::Stretch;
    case static_cast<int>(anomaly::PlatformUiBackgroundMode::Crop):
        return anomaly::PlatformUiBackgroundMode::Crop;
    default:
        return anomaly::PlatformUiBackgroundMode::Disabled;
    }
}

std::shared_ptr<const LauncherTheme> LoadLauncherTheme(
    const std::filesystem::path& runtime_root, const std::string_view palette_name,
    const anomaly::launcher::LauncherThemeArtwork& artwork, const std::uint64_t version) {
    auto theme = std::make_shared<LauncherTheme>();
    theme->version = version;
    theme->palette = anomaly::ParsePlatformUiPalette(palette_name);
    try {
        const auto read = anomaly::ReadUiResourceBytes(
            anomaly::ResolveLauncherUiLayoutFile(
                runtime_root, anomaly::ToString(theme->palette)),
            anomaly::kPlatformUiLayoutMaximumBytes);
        if (read) {
            const std::string text(
                reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
            auto parsed = anomaly::ParsePlatformUiLayout(text);
            if (parsed.ok) theme->layout = std::move(parsed.layout);
        }

        // The picture the launcher was pointed at replaces the one the layout names, with the fill
        // mode and the veil that sit beside it. A layout that ships its wallpaper turned off would
        // otherwise load a picture and never draw it, so an unpicked mode falls back to crop.
        if (!artwork.background.empty()) {
            theme->layout.background.path = artwork.background;
            if (theme->layout.background.mode == anomaly::PlatformUiBackgroundMode::Disabled &&
                artwork.background_mode < 0)
                theme->layout.background.mode = anomaly::PlatformUiBackgroundMode::Crop;
            if (theme->layout.background.opacity <= 0.0f)
                theme->layout.background.opacity = 1.0f;
        }
        if (artwork.background_mode >= 0)
            theme->layout.background.mode = BackgroundModeFromInt(artwork.background_mode);
        if (artwork.surface_opacity >= 0.0f)
            theme->layout.surface_opacity = artwork.surface_opacity;

        // Identical files decode once; a repeated sticker reuses the pixels.
        std::map<std::string, LauncherThemeImage> decoded;
        const auto decode = [&](const std::string& path) -> const LauncherThemeImage& {
            const auto found = decoded.find(path);
            if (found != decoded.end()) return found->second;
            LauncherThemeImage image;
            std::filesystem::path resolved;
            if (anomaly::ResolvePlatformUiLayoutPath(runtime_root, path, resolved)) {
                const auto bytes = anomaly::ReadUiResourceBytes(
                    resolved, anomaly::kDefaultUiResourceEncodedByteLimit);
                if (bytes) {
                    auto result = anomaly::DecodeUiImageRgba8(bytes.bytes);
                    if (result) {
                        image.pixels = std::move(result.image.pixels);
                        image.width = result.image.width;
                        image.height = result.image.height;
                    }
                }
            }
            return decoded.emplace(path, std::move(image)).first->second;
        };
        if (theme->layout.background.mode != anomaly::PlatformUiBackgroundMode::Disabled) {
            theme->background = decode(theme->layout.background.path);
        }
        theme->stickers.reserve(theme->layout.stickers.size());
        for (const auto& sticker : theme->layout.stickers) {
            theme->stickers.push_back(decode(sticker.path));
        }
    } catch (...) {
        // A theme is decoration: a failed load keeps the plain launcher.
    }
    return theme;
}

std::filesystem::path ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path);
}

std::filesystem::path ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

struct AdministratorLaunchResult final {
    bool run_current_process{};
    int exit_code{};
};

AdministratorLaunchResult EnsureAdministrator(PWSTR command_line) noexcept {
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    const BOOL queried = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    const DWORD query_error = queried == FALSE ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(token);
    if (queried == FALSE) return {false, static_cast<int>(query_error)};
    if (elevation.TokenIsElevated != 0) return {true, ERROR_SUCCESS};

    const auto executable = ExecutablePath();
    if (executable.empty()) return {false, ERROR_FILE_NOT_FOUND};
    const std::wstring working_directory = executable.parent_path().wstring();
    SHELLEXECUTEINFOW launch{
        .cbSize = sizeof(launch),
        .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
        .lpVerb = L"runas",
        .lpFile = executable.c_str(),
        .lpParameters = command_line != nullptr && command_line[0] != L'\0'
            ? command_line : nullptr,
        .lpDirectory = working_directory.c_str(),
        .nShow = SW_SHOWNORMAL,
    };
    if (ShellExecuteExW(&launch) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    if (launch.hProcess != nullptr) CloseHandle(launch.hProcess);
    return {false, ERROR_SUCCESS};
}

HMODULE LoadSystemDwmapi() {
    std::wstring directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(
        directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) return nullptr;
    directory.resize(length);
    return LoadLibraryExW(
        (std::filesystem::path(directory) / L"dwmapi.dll").c_str(),
        nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

std::string WideUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring Utf8Wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        return {};
    }
    return result;
}

std::string PathUtf8(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    return WideUtf8(value);
}

std::string VirtualKeyName(const std::uint32_t key) {
    const std::string fallback = "Key " + std::to_string(key);
    const UINT scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    wchar_t buffer[64]{};
    const LONG parameter = static_cast<LONG>(scan_code << 16U);
    if (GetKeyNameTextW(parameter, buffer, static_cast<int>(std::size(buffer))) <= 0) {
        return fallback;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return fallback;
    std::string result(static_cast<std::size_t>(size), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, result.data(), size, nullptr, nullptr));
    result.pop_back();
    return result;
}

bool PathsEqual(
    const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    try {
        const std::wstring left_value = left.lexically_normal().wstring();
        const std::wstring right_value = right.lexically_normal().wstring();
        return !left_value.empty() && !right_value.empty() &&
            CompareStringOrdinal(
                left_value.c_str(), -1, right_value.c_str(), -1, TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

std::string EncodeUtf8(char32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7fU) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return result;
}

std::string Ellipsize(std::string_view value, float width) {
    if (ImGui::CalcTextSize(value.data(), value.data() + value.size()).x <= width) {
        return std::string(value);
    }
    constexpr std::string_view suffix{"..."};
    if (ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x > width) {
        return {};
    }
    std::size_t end = value.size();
    while (end > 0) {
        --end;
        while (end > 0 &&
               (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        const auto candidate = std::string(value.substr(0, end)) + std::string(suffix);
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
    }
    return std::string(suffix);
}

// Every icon the launcher draws, and therefore the whole icon font it bakes. A glyph
// that is not in this list cannot be drawn, so adding one here is what makes it
// available as well as visible. 0xeb9f is the picture mark a card without a cover wears.
constexpr std::array<char32_t, 9> kLauncherIcons{
    0xe838, 0xe72c, 0xe768, 0xe73e, 0xe7ba, 0xe711, 0xe8b7, 0xe8e5, 0xeb9f};

// The launcher draws its body text at this multiple of the platform base size and its
// title with an explicit size from the same font, so one bake serves every string on
// screen. Another bake would only be a second copy of a 15.7 MB font.
constexpr std::array kLauncherFontScales{2.00f};

const char* Glyph(char32_t codepoint) {
    struct Entry final { char32_t codepoint; std::string text; };
    static const std::array<Entry, kLauncherIcons.size()> entries = [] {
        std::array<Entry, kLauncherIcons.size()> built{};
        for (std::size_t index = 0; index < built.size(); ++index) {
            built[index] = Entry{kLauncherIcons[index], EncodeUtf8(kLauncherIcons[index])};
        }
        return built;
    }();
    const auto found = std::find_if(entries.begin(), entries.end(), [codepoint](const auto& entry) {
        return entry.codepoint == codepoint;
    });
    return found == entries.end() ? "?" : found->text.c_str();
}

LauncherMessage MakeLauncherMessage(
    anomaly::MessageId id,
    std::initializer_list<std::string_view> arguments = {},
    std::string detail = {}) {
    LauncherMessage message;
    message.id = id;
    message.arguments.reserve(arguments.size());
    for (const auto argument : arguments) message.arguments.emplace_back(argument);
    message.detail = std::move(detail);
    return message;
}

anomaly::MessageId ProxyStateMessageId(
    anomaly::launcher::ProxyInstallationState state) noexcept {
    using State = anomaly::launcher::ProxyInstallationState;
    switch (state) {
    case State::NotInstalled: return anomaly::MessageId::LauncherProxyStateNotInstalled;
    case State::Enabled: return anomaly::MessageId::LauncherProxyStateEnabled;
    case State::Disabled: return anomaly::MessageId::LauncherProxyStateDisabled;
    case State::UpdateAvailable:
        return anomaly::MessageId::LauncherProxyStateUpdateAvailable;
    case State::Conflict: return anomaly::MessageId::LauncherProxyStateConflict;
    case State::Unavailable: return anomaly::MessageId::LauncherProxyStateUnavailable;
    }
    return anomaly::MessageId::LauncherProxyStateUnavailable;
}

// The launcher's network helpers live further down; the update check on
// this side of them needs their declarations only. The status code is reported as well as the body
// because the update check has to tell a rate-limited API answer from a release that is not there.
struct LauncherDownloadProgress final {
    std::atomic<long long> bytes{};
    std::atomic<long long> total{};
    std::atomic<bool> cancel{};
};

std::string LauncherHttpGetUrl(const std::string& url, bool use_system_proxy = false,
    int* status_code = nullptr, DWORD timeout_ms = 8000);
bool LauncherDownloadUrl(const std::string& url, const std::filesystem::path& destination,
    LauncherDownloadProgress* progress = nullptr);
void LauncherUpdateLog(const std::string& line);

// The addresses one github.com URL is asked at, best-known first: the mirror that answered last time,
// then the direct address, then the rest. A launcher started on a network where github.com does not
// answer would otherwise spend its whole timeout budget on the direct attempt before it ever reaches
// the mirror that works.
std::vector<std::string> LauncherLookupCandidates(
    const std::string& url, const std::string& endpoint) {
    std::vector<std::string> candidates;
    candidates.reserve(1 + anomaly::launcher::kGithubProxyPrefixes.size());
    if (!endpoint.empty()) candidates.push_back(endpoint + url);
    candidates.push_back(url);
    for (const auto prefix : anomaly::launcher::kGithubProxyPrefixes) {
        if (std::string(prefix) != endpoint) candidates.push_back(std::string(prefix) + url);
    }
    return candidates;
}

// Which mirror a URL was asked through: the prefix it starts with, or nothing for the direct address.
std::string LauncherEndpointOfUrl(const std::string& url) {
    for (const auto prefix : anomaly::launcher::kGithubProxyPrefixes) {
        if (url.starts_with(prefix)) return std::string(prefix);
    }
    return {};
}



// Launcher updates run on their own worker.
LauncherDownloadProgress g_update_download;

class LauncherController final {
public:
    explicit LauncherController(std::filesystem::path payload_root)
        : payload_root_(std::move(payload_root)),
          source_{payload_root_ / L"dwmapi.dll", payload_root_ / L"Anomaly"},
          configuration_path_(anomaly::launcher::LauncherConfigurationPath(payload_root_)),
          worker_([this](std::stop_token stop) { WorkerMain(stop); }) {
        Queue(anomaly::MessageId::LauncherStatusScanningLocal, MessageScope::Both, [this] {
            InitializePathsImpl();
        });
    }

    ~LauncherController() {
        g_update_download.cancel.store(true);
        update_check_worker_.request_stop();
        worker_.request_stop();
        queue_changed_.notify_all();
        if (update_check_worker_.joinable()) update_check_worker_.join();
        worker_.join();
        if (telemetry_worker_.joinable()) {
            telemetry_worker_.request_stop();
            telemetry_worker_.join();
        }
        // Closing the window may cancel a queued preference write. Once initialization completed,
        // persist the final in-memory configuration after the worker has stopped.
        if (state_.theme != nullptr) static_cast<void>(PersistConfigurationImpl());
    }

    LauncherController(const LauncherController&) = delete;
    LauncherController& operator=(const LauncherController&) = delete;

    [[nodiscard]] LauncherSnapshot Snapshot() const {
        std::scoped_lock lock(state_mutex_);
        return state_;
    }

    void SelectGameDirectory(std::filesystem::path directory) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.game_directory = std::move(directory);
            configuration_.Selected().game_directory = state_.game_directory;
        }
        Queue(anomaly::MessageId::LauncherStatusInspectingProxy, MessageScope::Proxy, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshHotkeyImpl();
            RefreshProxyImpl();
            RefreshProcessesImpl(ProcessAnnounce::Never);
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void RefreshProxy() {
        Queue(anomaly::MessageId::LauncherStatusInspectingProxy, MessageScope::Proxy, [this] {
            RefreshProxyImpl();
            RefreshProcessesImpl(ProcessAnnounce::Never);
        });
    }

    void InstallProxy() {
        Queue(anomaly::MessageId::LauncherStatusInstallingRuntime, MessageScope::Proxy, [this] {
            StartTelemetryImpl();
            const auto game = GameDirectory();
            const auto result = anomaly::launcher::InstallProxyRuntime(game, source_);
            PublishProxy(result);
            RefreshProcessesImpl(ProcessAnnounce::Never);
        });
    }

    void SetProxyEnabled(bool enabled) {
        Queue(enabled ? anomaly::MessageId::LauncherStatusEnablingProxy
                      : anomaly::MessageId::LauncherStatusDisablingProxy,
            MessageScope::Proxy, [this, enabled] {
            const auto game = GameDirectory();
            const auto result = anomaly::launcher::SetProxyEnabled(
                game, source_, enabled);
            PublishProxy(result);
        });
    }

    void UninstallProxy() {
        Queue(anomaly::MessageId::LauncherStatusUninstallingProxy, MessageScope::Proxy, [this] {
            StartTelemetryImpl();
            const auto result = anomaly::launcher::UninstallProxy(GameDirectory(), source_);
            PublishProxy(result);
            RefreshProcessesImpl(ProcessAnnounce::Never);
            if (result.Ok() && result.state == anomaly::launcher::ProxyInstallationState::NotInstalled)
                PublishMessage(anomaly::MessageId::LauncherStatusProxyUninstalled, MessageKind::Success);
        });
    }

    void RefreshProcesses() {
        Queue(anomaly::MessageId::LauncherStatusScanningProcesses,
            MessageScope::Attach, [this] { RefreshProcessesImpl(); });
    }

    // A game that starts after this window is up would otherwise leave the list empty until the user asks
    // for a scan. Polling keeps it current: the work runs on the worker like every other operation, but it
    // is silent -- it never takes the busy flag, so buttons do not flicker, and it never writes the status
    // line, so an outcome stays readable while the list behind it catches up.
    void PollProcesses() {
        constexpr auto kInterval = std::chrono::seconds(2);
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            const auto now = std::chrono::steady_clock::now();
            if (last_process_poll_ != std::chrono::steady_clock::time_point{} &&
                now - last_process_poll_ < kInterval) {
                return;
            }
            last_process_poll_ = now;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back([this] { RefreshProcessesImpl(ProcessAnnounce::OnChange); });
        }
        queue_changed_.notify_one();
    }

    [[nodiscard]] anomaly::launcher::ManualMapResult AttachToRunningProcessImpl(
        const DWORD process_id) {
        const auto selected = ResolveAttachRuntime();
        if (!selected.Ok()) {
            PublishRuntimeFailure(selected);
            return {};
        }
        anomaly::launcher::ManualMapOptions options;
        options.process_id = process_id;
        options.core_path = selected.core_path;
        options.runtime_root = selected.runtime_root;
        options.log_directory = options.runtime_root / L"logs";
        options.delivery = anomaly::launcher::ManualMapDelivery::Automatic;
        const auto result = anomaly::launcher::AttachToRunningProcess(options);
        if (result.Ok()) RefreshProcessesImpl(ProcessAnnounce::Never);
        return result;
    }

    void LaunchAndAttach() {
        Queue(anomaly::MessageId::LauncherStatusLaunchingAttach, MessageScope::Attach, [this] {
            StartTelemetryImpl();
            const auto selected = ResolveAttachRuntime();
            if (!selected.Ok()) {
                PublishRuntimeFailure(selected);
                return;
            }
            // The demo is a Steam title; starting it through its Steam URL keeps the Steam client,
            // DRM and cloud state in the same order the player's own shortcut produces.
            constexpr wchar_t kSteamUrl[] = L"steam://rungameid/5480890";
            const auto launch_result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL));
            if (launch_result <= 32) {
                std::scoped_lock lock(state_mutex_);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusLaunchAttachFailed,
                    {std::to_string(launch_result)}, "Steam could not start the game");
                state_.message_kind = MessageKind::Error;
                return;
            }
            DWORD process_id{};
            const ULONGLONG deadline = GetTickCount64() + 120000;
            while (!worker_.get_stop_token().stop_requested() &&
                   GetTickCount64() < deadline) {
                for (const auto& process :
                     anomaly::launcher::EnumerateAttachableProcesses(L"MaydayAirways.exe")) {
                    if (process.Compatible()) {
                        process_id = process.process_id;
                        break;
                    }
                }
                if (process_id != 0) break;
                Sleep(500);
            }
            if (process_id == 0) {
                std::scoped_lock lock(state_mutex_);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusLaunchAttachFailed,
                    {"0"}, "the game process did not appear");
                state_.message_kind = MessageKind::Error;
                return;
            }
            const auto result = AttachToRunningProcessImpl(process_id);
            std::scoped_lock lock(state_mutex_);
            if (result.Ok()) {
                state_.attached_process = process_id;
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusLaunchAttached,
                    {std::to_string(process_id)});
                state_.message_kind = MessageKind::Success;
            } else {
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusLaunchAttachFailed,
                    {std::to_string(result.win32_error)}, result.message);
                state_.message_kind = MessageKind::Error;
            }
        });
    }

    // Live attach: map the core into a game the player started from its own shortcut, at whatever point
    // they start the launcher. Nothing is relaunched and nothing is terminated, so a failed attach leaves
    // the running game exactly as it was.
    void InjectIntoRunningProcess(const DWORD process_id) {
        if (process_id == 0) return;
        Queue(anomaly::MessageId::LauncherStatusInjectingProcess, MessageScope::Attach,
            [this, process_id] {
            StartTelemetryImpl();
            const auto result = AttachToRunningProcessImpl(process_id);
            std::scoped_lock lock(state_mutex_);
            if (result.Ok()) {
                state_.attached_process = process_id;
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusInjected,
                    {std::to_string(process_id)});
                state_.message_kind = MessageKind::Success;
            } else {
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusInjectFailed,
                    {std::to_string(result.win32_error)}, result.message);
                state_.message_kind = MessageKind::Error;
            }
        });
    }

    void SetToggleKey(const std::uint32_t key) {
        Queue(anomaly::MessageId::LauncherStatusSavingSettings, MessageScope::Both, [this, key] {
            if (!SaveToggleKeyImpl(key)) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, "menu toggle preference could not be written");
                return;
            }
            std::scoped_lock lock(state_mutex_);
            state_.toggle_key = key;
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusSettingsSaved);
            state_.message_kind = MessageKind::Success;
        });
    }

    // The launcher's own theme. It is stored in the launcher configuration rather than
    // in the game's settings, because it dresses this window alone, and the reload runs
    // on the worker like every other file read: decoding a wallpaper on the render
    // thread would stutter the window it is painting.
    void SetLauncherTheme(const anomaly::PlatformUiPalette palette) {
        const std::string name(anomaly::ToString(palette));
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || configuration_.theme == name) return;
            configuration_.theme = name;
        }
        Queue(anomaly::MessageId::LauncherStatusSavingSettings, MessageScope::Both, [this] {
            RefreshThemeImpl();
            const auto saved = PersistConfigurationImpl();
            if (!saved.Ok()) {
                PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
                return;
            }
            PublishMessage(anomaly::MessageId::LauncherStatusSettingsSaved,
                MessageKind::Success);
        });
    }

    // The colours behind the "custom" palette. They are applied on the frame that stores them, so a
    // colour being dragged is visible while it moves, and written with the rest of the settings.
    void SetLauncherCustomColors(const anomaly::PlatformUiCustomColors& colors) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.custom_colors == colors) return;
            configuration_.custom_colors = colors;
            state_.custom_colors = colors;
            settings_dirty_ = true;
        }
    }

    // The launcher's own background: the picture and the fill mode beside it. Both are stored with
    // the launcher configuration, and a change reloads the theme on the worker -- decoding a wallpaper
    // on the render thread would stutter the window that is painting it.
    void SetLauncherBackground(const std::string& path, const int mode) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.artwork.background == path &&
                configuration_.artwork.background_mode == mode) return;
            configuration_.artwork.background = path;
            configuration_.artwork.background_mode = mode;
            state_.artwork = configuration_.artwork;
            settings_dirty_ = true;
            theme_dirty_ = true;
        }
    }

    // The veil over that picture: the same value the in-game settings call the background's
    // visibility, and the same one the layout calls surfaceOpacity.
    void SetLauncherSurfaceOpacity(const float opacity) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.artwork.surface_opacity == opacity) return;
            configuration_.artwork.surface_opacity = opacity;
            state_.artwork = configuration_.artwork;
            settings_dirty_ = true;
            theme_dirty_ = true;
        }
    }

    // The launcher's own language. It is stored in the launcher configuration, not in the ini
    // the runtime reads, because it dresses this window alone -- and it takes effect on the
    // next frame rather than on the next launch, see LoadLauncherCatalog.
    void SetLanguage(const anomaly::LanguagePreference preference) {
        const std::string name(anomaly::LanguagePreferenceName(preference));
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.language == name) return;
            configuration_.language = name;
            state_.language = name;
            settings_dirty_ = true;
        }
    }

    // The launcher's own update. "Check" is the read of the newest release and its comparison with
    // this build; "install" downloads that release's runtime package, publishes it over this
    // installation and starts the new image. Both run on the worker: neither may block a frame.
    // AutoCheckUpdates is what the window calls every frame, and it decides for itself whether this
    // launch still owes a check.
    void SetAutoCheckUpdates(const bool enabled) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.update.auto_check == enabled) return;
            configuration_.update.auto_check = enabled;
            state_.update.auto_check = enabled;
            settings_dirty_ = true;
        }
    }

    // "Skip this version": the release stays known and installable from the settings page, but the
    // prompt stops asking about it, this session and every later one.
    void SkipUpdateVersion(std::string version) {
        {
            std::scoped_lock lock(state_mutex_);
            if (version.empty() || configuration_.update.skipped_version == version) return;
            configuration_.update.skipped_version = std::move(version);
            state_.update.skipped_version = configuration_.update.skipped_version;
            settings_dirty_ = true;
        }
    }

    void AutoCheckUpdates() {
        {
            std::scoped_lock lock(state_mutex_);
            if (auto_check_done_ || state_.busy || !configuration_.update.auto_check) return;
            auto_check_done_ = true;
        }
        if (!StartUpdateCheck()) {
            std::scoped_lock lock(state_mutex_);
            auto_check_done_ = false;  // the worker was busy; the next frame tries again
        }
    }

    void CheckForUpdates() {
        StartUpdateCheck();
    }

    void InstallUpdate() {
        const auto snapshot = Snapshot();
        if (snapshot.busy || !snapshot.update.CanInstall()) return;
        g_update_download.cancel.store(false);
        g_update_download.bytes.store(0);
        g_update_download.total.store(0);
        QueueUpdate([this] { InstallUpdateImpl(); }, LauncherUpdateFailure::Install);
    }

    // Called once a frame by the UI. A preference change is held in memory until the worker is
    // free, so flipping a switch or dragging the page-size slider costs one configuration write
    // instead of one per frame, and nothing announces itself: the control the user just moved is
    // its own feedback.
    void FlushSettings() {
        bool reload_theme{};
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || !settings_dirty_) return;
            settings_dirty_ = false;
            reload_theme = std::exchange(theme_dirty_, false);
        }
        if (!QueueSilent([this, reload_theme] {
                if (reload_theme) RefreshThemeImpl();
                const auto saved = PersistConfigurationImpl();
                if (!saved.Ok()) PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            })) {
            std::scoped_lock lock(state_mutex_);
            settings_dirty_ = true;
            theme_dirty_ = theme_dirty_ || reload_theme;
        }
    }

private:
    using Work = std::function<void()>;

    bool StartUpdateCheck() {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || state_.update.Busy()) return false;
            state_.update.phase = LauncherUpdatePhase::Checking;
            auto_check_done_ = true;
        }
        // A release lookup must not reserve the launch/install worker for every network timeout.
        // The previous lookup has published its result before another check becomes available.
        if (update_check_worker_.joinable()) update_check_worker_.join();
        update_check_worker_ = std::jthread([this](std::stop_token stop) {
            RunUpdate([this, stop] { CheckForUpdatesImpl(stop); }, LauncherUpdateFailure::Check);
        });
        return true;
    }

    bool QueueUpdate(Work work, LauncherUpdateFailure failure) {
        return QueueSilent([this, work = std::move(work), failure] {
            RunUpdate(work, failure);
        });
    }

    void RunUpdate(const Work& work, LauncherUpdateFailure failure) {
        try {
            work();
        } catch (...) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Failed;
            state_.update.failure = failure;
            state_.update.detail = "update operation could not be completed";
        }
    }

    [[nodiscard]] anomaly::RuntimeLaunchResult ResolveAttachRuntime() const {
        auto bundled = anomaly::ResolveRuntimeLaunch({source_.runtime_directory});
        if (bundled.Ok()) return bundled;
        auto installed = anomaly::ResolveRuntimeLaunch({GameDirectory() / L"Anomaly"});
        return installed.Ok() ? installed : bundled;
    }

    void PublishRuntimeFailure(const anomaly::RuntimeLaunchResult& selected) {
        std::scoped_lock lock(state_mutex_);
        state_.runtime_version.clear();
        state_.runtime_message = selected.message;
        state_.core_available = false;
        state_.message = MakeLauncherMessage(
            anomaly::MessageId::LauncherStatusCoreUnavailable, {}, selected.message);
        state_.message_kind = MessageKind::Error;
    }

    void PublishMessage(anomaly::MessageId id, MessageKind kind, std::string detail = {}) {
        std::scoped_lock lock(state_mutex_);
        state_.message = MakeLauncherMessage(id, {}, std::move(detail));
        state_.message_kind = kind;
    }

    [[nodiscard]] std::filesystem::path GameDirectory() const {
        std::scoped_lock lock(state_mutex_);
        return state_.game_directory;
    }

    // Which runtime the game ends up loading depends on the mode: a proxy installation keeps its
    // runtime inside the game folder, while the core this launcher maps itself runs from the
    // bundled payload. Both read their own anomaly.ini, so both have to carry the startup hotkey --
    // writing only the installed one left an attached game toggling on the payload's shipped
    // default, and writing only the payload one left a proxy-installed game on its old key.
    // The installed runtime comes first because that is the file the row reads back from.
    [[nodiscard]] std::vector<std::filesystem::path> RuntimeSettingsRoots() const {
        std::vector<std::filesystem::path> roots;
        std::vector<std::filesystem::path> game_directories;
        {
            std::scoped_lock lock(state_mutex_);
            game_directories.push_back(state_.game_directory);
        }
        const auto add = [&roots](const std::filesystem::path& root) {
            std::error_code error;
            if (root.empty() || !std::filesystem::is_directory(root, error) || error) return;
            if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
        };
        for (const auto& game_directory : game_directories) {
            if (game_directory.empty()) continue;
            const auto installed = game_directory / L"Anomaly";
            std::error_code error;
            // Only an installation the game folder really holds: creating the runtime directory
            // from a settings write would make an empty one look installed.
            if (std::filesystem::is_regular_file(
                    installed / L"Anomaly.Core.dll", error) && !error) {
                add(installed);
            }
        }
        // The launcher's own runtime last: a manual map starts the core from here, and this ini is
        // the file the launcher itself reads at start-up.
        add(source_.runtime_directory);
        return roots;
    }

    void RefreshHotkeyImpl() {
        const auto roots = RuntimeSettingsRoots();
        const auto root = roots.empty() ? source_.runtime_directory : roots.front();
        const auto config = ue5mem::AnalyzerConfig::Load(root / L"anomaly.ini");
        std::scoped_lock lock(state_mutex_);
        state_.toggle_key = config.platform_toggle_key;
    }

    [[nodiscard]] bool SaveToggleKeyImpl(const std::uint32_t key) const {
        const std::wstring value = std::to_wstring(key);
        const auto roots = RuntimeSettingsRoots();
        // Nothing written is not a saved preference, so an empty list starts out failed instead of
        // reporting a success the game never sees.
        bool saved = !roots.empty();
        for (const auto& root : roots) {
            const bool written = WritePrivateProfileStringW(
                L"Platform", L"ToggleKey", value.c_str(),
                (root / L"anomaly.ini").c_str()) != FALSE;
            saved = written && saved;
        }
        return saved;
    }

    // The launcher's own ini, which is the file this process reads at start-up. The runtime's
    // copy next to the game holds the same keys for the in-game window; this one is the
    // launcher's, exactly like its configuration next to the executable.
    [[nodiscard]] std::filesystem::path LauncherSettingsPath() const {
        return payload_root_ / L"Anomaly" / L"anomaly.ini";
    }

    // A queued unit of work that does not replace the status line: preferences are written
    // silently, because a "saving" line under a switch the user just flipped is noise.
    bool QueueSilent(Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back(std::move(work));
        }
        queue_changed_.notify_one();
        return true;
    }

    bool Queue(anomaly::MessageId activity, MessageScope scope, Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
            state_.message = MakeLauncherMessage(activity);
            state_.message_kind = MessageKind::Neutral;
            // The scope is the operation's, so every message the work publishes --
            // including the ones written deeper in, such as a failed settings save --
            // stays attributed to the mode that asked for it.
            state_.message_scope = scope;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back(std::move(work));
        }
        queue_changed_.notify_one();
        return true;
    }

    void WorkerMain(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Work work;
            {
                std::unique_lock lock(queue_mutex_);
                queue_changed_.wait(lock, stop, [this] { return !queue_.empty(); });
                if (stop.stop_requested()) break;
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                work();
            } catch (...) {
                std::scoped_lock lock(state_mutex_);
                state_.message = MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusUnexpectedFailure);
                state_.message_kind = MessageKind::Error;
            }
            std::scoped_lock lock(state_mutex_);
            state_.busy = false;
        }
    }

    void RefreshProxyImpl() {
        const auto game = GameDirectory();
        if (game.empty()) {
            std::scoped_lock lock(state_mutex_);
            state_.proxy = {};
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusSelectGameDirectory);
            state_.message_kind = MessageKind::Neutral;
            return;
        }
        PublishProxy(anomaly::launcher::InspectProxyInstallation(game, source_));
    }

    void PublishProxy(anomaly::launcher::ProxyInstallationStatus result) {
        std::scoped_lock lock(state_mutex_);
        state_.proxy = std::move(result);
        state_.message_scope = MessageScope::Proxy;
        state_.message = state_.proxy.Ok()
            ? MakeLauncherMessage(
                ProxyStateMessageId(state_.proxy.state), {}, state_.proxy.message)
            : MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusProxyOperationFailed, {},
                state_.proxy.message);
        state_.message_kind = state_.proxy.Ok()
            ? (state_.proxy.state == anomaly::launcher::ProxyInstallationState::Enabled ||
                state_.proxy.state == anomaly::launcher::ProxyInstallationState::Disabled
                ? MessageKind::Success : MessageKind::Neutral)
            : MessageKind::Error;
    }

    void InitializePathsImpl() {
        const auto loaded = anomaly::launcher::LoadLauncherConfiguration(configuration_path_);
        auto configuration = loaded.configuration;
        anomaly::launcher::LauncherDiscoveryOptions options;
        options.payload_root = payload_root_;
        configuration.game = anomaly::launcher::DiscoverLauncherConfiguration(
            configuration.game, options);
        // A launcher that stores no language yet follows the one its runtime ini already names,
        // which is what an installation upgraded from an older build has. From here on the
        // configuration owns the value and the ini is the only seed.
        if (configuration.language.empty()) {
            const ue5mem::AnalyzerConfig ini =
                ue5mem::AnalyzerConfig::Load(LauncherSettingsPath());
            configuration.language =
                std::string(anomaly::LanguagePreferenceName(ini.platform_language));
        }
        {
            std::scoped_lock lock(state_mutex_);
            configuration_ = std::move(configuration);
            state_.game_directory = configuration_.game.game_directory;
            state_.language = configuration_.language;
            state_.artwork = configuration_.artwork;
            state_.custom_colors = configuration_.custom_colors;
            state_.update.auto_check = configuration_.update.auto_check;
            state_.update.skipped_version = configuration_.update.skipped_version;
        }
        const auto saved = PersistConfigurationImpl();
        // A previous update left the image it replaced behind, which this process can now delete:
        // while that image was running it could only be renamed.
        anomaly::launcher::RemoveUpdateLeftovers(payload_root_);
        RefreshHotkeyImpl();
        RefreshProxyImpl();
        RefreshProcessesImpl();
        RefreshThemeImpl();
        if (!saved.Ok()) {
            PublishMessage(anomaly::MessageId::LauncherStatusUnexpectedFailure,
                MessageKind::Error, saved.message);
        }
    }

    void ReconcileRelatedPathsImpl() {
        anomaly::launcher::LauncherClientConfiguration preferred;
        {
            std::scoped_lock lock(state_mutex_);
            preferred.game_directory = state_.game_directory;
        }
        anomaly::launcher::LauncherDiscoveryOptions options;
        options.payload_root = payload_root_;
        const auto discovered = anomaly::launcher::DiscoverLauncherConfiguration(
            preferred, options);
        std::scoped_lock lock(state_mutex_);
        configuration_.game = discovered;
        state_.game_directory = discovered.game_directory;
    }

    [[nodiscard]] anomaly::launcher::LauncherConfigurationSaveResult
    PersistConfigurationImpl() const {
        anomaly::launcher::LauncherConfiguration configuration;
        {
            std::scoped_lock lock(state_mutex_);
            configuration = configuration_;
        }
        return anomaly::launcher::SaveLauncherConfiguration(
            configuration_path_, configuration);
    }

    void StartTelemetryImpl() noexcept {
        try {
            const auto day = anomaly::TelemetryDay(std::chrono::system_clock::now());
            if (telemetry_attempt_day_ == day) return;
            anomaly::TelemetryOptions options;
            options.enabled = ue5mem::AnalyzerConfig::Load(LauncherSettingsPath()).telemetry_enabled;
            if (!options.enabled) return;
            const auto snapshot = Snapshot();
            if (!snapshot.theme) return;
            options.language = anomaly::LocaleName(anomaly::ResolveUserLocale(
                anomaly::ParseLanguagePreference(snapshot.language).preference).locale);
            anomaly::LauncherTelemetryUsage usage;
            usage.theme = anomaly::ToString(snapshot.theme->palette);
            usage.custom_background = anomaly::launcher::IsCustomLauncherBackground(
                snapshot.theme->palette, snapshot.theme->layout.background.path, snapshot.theme->HasBackground());
            telemetry_attempt_day_ = day;
            telemetry_worker_ = std::jthread([options = std::move(options), usage = std::move(usage)](
                std::stop_token stop) { anomaly::ReportLauncherUsage(options, usage, stop); });
        } catch (...) {}
    }

    void RefreshThemeImpl() {
        // The launcher reads the theme next to itself, exactly like its locales:
        // payload_root_ points at wherever the payload is installed, which is not
        // necessarily where this launcher keeps config/themes and assets.
        std::string palette;
        {
            std::scoped_lock lock(state_mutex_);
            palette = configuration_.theme;
        }
        anomaly::launcher::LauncherThemeArtwork artwork;
        {
            std::scoped_lock lock(state_mutex_);
            artwork = configuration_.artwork;
        }
        auto theme = LoadLauncherTheme(
            ExecutableDirectory() / L"Anomaly", palette, artwork, theme_version_ + 1);
        ++theme_version_;
        std::scoped_lock lock(state_mutex_);
        state_.theme = std::move(theme);
    }

    // GET /repos/<repository>/releases/latest, asked at the endpoint that answered last before any
    // other: the answer is the same JSON wherever it comes from, so the first endpoint that returns a
    // release ends the search and is remembered for the next launch. The status the API itself
    // reported is what a failure is described with, because that is the one that says "rate limited".
    void CheckForUpdatesImpl(std::stop_token stop) {
        // The mirror that answered last time, if any: asked first, so a launch on a network that
        // cannot reach api.github.com does not spend its budget on an address known not to answer.
        std::string endpoint;
        {
            std::scoped_lock lock(state_mutex_);
            endpoint = configuration_.update.endpoint;
            state_.update.phase = LauncherUpdatePhase::Checking;
            state_.update.failure = LauncherUpdateFailure::None;
            state_.update.detail.clear();
            state_.update.http_status = 0;
        }
        const std::string api_url = "https://api.github.com/repos/" +
            std::string(anomaly::launcher::kReleaseRepository) + "/releases/latest";
        std::optional<anomaly::launcher::ReleaseInfo> release;
        std::string answered_by;
        int status{};
        bool answered{};
        bool first = true;
        for (const std::string& url : LauncherLookupCandidates(api_url, endpoint)) {
            if (stop.stop_requested()) return;
            // The first endpoint gets both routes -- the system proxy the machine is configured with,
            // and a direct connection -- because a stale proxy is a common reason for a lookup to
            // return nothing. The rest are tried once: whoever answers, answers.
            int attempt_status{};
            std::string body = LauncherHttpGetUrl(url, false, &attempt_status, 3000);
            if (body.empty() && first && !stop.stop_requested())
                body = LauncherHttpGetUrl(url, true, &attempt_status, 3000);
            first = false;
            if (attempt_status != 0) status = attempt_status;
            if (body.empty()) continue;
            answered = true;
            if (auto parsed = anomaly::launcher::ParseLatestRelease(body); parsed.has_value()) {
                release = std::move(parsed);
                answered_by = url;
                break;
            }
        }

        {
            std::scoped_lock lock(state_mutex_);
            if (release.has_value() && !answered_by.empty()) {
                // Remember which endpoint answered, so the next launch starts there instead of
                // waiting out the addresses this network cannot reach.
                const std::string used = LauncherEndpointOfUrl(answered_by);
                if (configuration_.update.endpoint != used) {
                    configuration_.update.endpoint = used;
                    settings_dirty_ = true;
                }
            }
            state_.update.http_status = status;
            if (!release.has_value()) {
                state_.update.phase = LauncherUpdatePhase::Failed;
                if (status == 0 && !answered) {
                    state_.update.failure = LauncherUpdateFailure::Network;
                } else if (status == 403 || status == 429) {
                    state_.update.failure = LauncherUpdateFailure::RateLimited;
                } else if (status != 0 && status != 200) {
                    state_.update.failure = LauncherUpdateFailure::Http;
                } else {
                    state_.update.failure = LauncherUpdateFailure::NotRelease;
                }
                return;
            }
            state_.update.remote_tag = release->tag;
            state_.update.remote_version = release->version;
            state_.update.page_url = release->page_url;
            state_.update.notes = release->notes;
            const auto* const asset = anomaly::launcher::FindRuntimeAsset(*release);
            state_.update.asset = asset != nullptr
                ? *asset : anomaly::launcher::ReleaseAsset{};
            switch (anomaly::launcher::CompareRelease(ANOMALY_SDK_VERSION_STRING, *release)) {
            case anomaly::launcher::ReleaseComparison::Newer:
                if (asset != nullptr) {
                    state_.update.phase = LauncherUpdatePhase::Available;
                } else {
                    state_.update.phase = LauncherUpdatePhase::Failed;
                    state_.update.failure = LauncherUpdateFailure::Check;
                    state_.update.detail = "release publishes no verified runtime package for this version";
                }
                break;
            case anomaly::launcher::ReleaseComparison::UpToDate:
                state_.update.phase = LauncherUpdatePhase::UpToDate;
                break;
            case anomaly::launcher::ReleaseComparison::Unknown:
                state_.update.phase = LauncherUpdatePhase::Failed;
                state_.update.failure = LauncherUpdateFailure::Check;
                state_.update.detail = "release version could not be compared with this build";
                break;
            }
        }
    }

    // Downloads the runtime package the check found and publishes it over this installation. The
    // download goes to a directory of its own: two updates never share a half-written archive, and
    // the archive itself is thrown away as soon as it is extracted. The package comes from the
    // release's own address and is verified against the length and digest the release published,
    // because a truncated transfer or someone else's bytes must not reach an extraction.
    void InstallUpdateImpl() {
        anomaly::launcher::ReleaseAsset asset;
        {
            std::scoped_lock lock(state_mutex_);
            if (!state_.update.CanInstall()) return;
            asset = state_.update.asset;
            if (asset.download_url.empty()) return;
            state_.update.phase = LauncherUpdatePhase::Downloading;
            state_.update.failure = LauncherUpdateFailure::None;
            state_.update.detail.clear();
        }
        const auto fail = [this](std::string detail) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Failed;
            state_.update.failure = LauncherUpdateFailure::Install;
            state_.update.detail = std::move(detail);
        };
        std::error_code error;
        GUID download_id{};
        wchar_t download_name[40]{};
        if (FAILED(CoCreateGuid(&download_id)) ||
            StringFromGUID2(download_id, download_name, static_cast<int>(std::size(download_name))) == 0) {
            fail("update download directory could not be allocated");
            return;
        }
        const auto directory = std::filesystem::temp_directory_path(error) /
            (std::wstring(L"anomaly-launcher-update-") + download_name);
        if (error) {
            fail("temporary directory is unavailable");
            return;
        }
        if (!std::filesystem::create_directory(directory, error)) {
            fail("update download directory could not be created");
            return;
        }
        struct DownloadDirectory final {
            std::filesystem::path path;
            ~DownloadDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{directory};
        const auto archive = directory / L"runtime.zip";

        // The package is fetched from the release's own address, once: the gh-proxy mirrors exist to
        // reach the API when github.com does not answer, not to carry the payload, and a package that
        // arrived through a third party would be one more thing to trust. A user whose network cannot
        // reach github.com at all has the netdisk buttons in this dialog for exactly that case.
        anomaly::launcher::PackageVerification verification;
        std::string rejected;
        bool downloaded = LauncherDownloadUrl(asset.download_url, archive, &g_update_download);
        if (downloaded) {
            verification = anomaly::launcher::VerifyReleasePackage(archive, asset);
            if (!verification.Ok()) {
                rejected = std::string("package failed verification (") +
                    (verification.check == anomaly::launcher::PackageCheck::Size
                        ? "size " + std::to_string(verification.size) + " != " +
                            std::to_string(asset.size)
                        : verification.check == anomaly::launcher::PackageCheck::Digest
                            ? "sha256 " + verification.sha256
                            : std::string("unreadable")) + ')';
                LauncherUpdateLog(rejected);
                std::filesystem::remove(archive, error);
                downloaded = false;
            }
        } else {
            LauncherUpdateLog("package download failed: " + asset.download_url);
        }
        if (!downloaded || g_update_download.cancel.load()) {
            if (g_update_download.cancel.load()) {
                // Cancelling is not a failure: the release is still there to install, and the dialog
                // goes back to offering it.
                std::scoped_lock lock(state_mutex_);
                state_.update.phase = LauncherUpdatePhase::Available;
                state_.update.failure = LauncherUpdateFailure::None;
                state_.update.detail.clear();
                return;
            }
            fail(rejected.empty()
                ? std::string("update package could not be downloaded")
                : rejected);
            return;
        }
        LauncherUpdateLog("package verified: " + asset.name + " size=" +
            std::to_string(verification.size) + " sha256=" + verification.sha256);
        {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Installing;
        }
        const auto extracted = anomaly::launcher::ExtractModArchive(archive, &g_update_download.cancel);
        if (!extracted.Ok()) {
            if (extracted.error == anomaly::launcher::ModArchiveError::Cancelled) {
                std::scoped_lock lock(state_mutex_);
                state_.update.phase = LauncherUpdatePhase::Available;
                return;
            }
            fail("update package could not be extracted");
            return;
        }
        const auto published =
            anomaly::launcher::PublishStagedRelease(extracted.directory, payload_root_);
        if (!published.Ok()) {
            fail(published.failure);
            return;
        }
        std::string restart_failure;
        if (!RelaunchUpdatedLauncher(restart_failure)) {
            fail(std::move(restart_failure));
            return;
        }
        std::scoped_lock lock(state_mutex_);
        state_.update.phase = LauncherUpdatePhase::Installed;
        state_.restart_pending = true;
    }

    // The new image is in place, so this process now runs the image that was replaced: it starts
    // the new one and lets the window close. The child inherits this token, which is what keeps an
    // elevated launcher elevated without a second prompt.
    bool RelaunchUpdatedLauncher(std::string& failure) {
        const auto executable = payload_root_ / anomaly::launcher::kLauncherExecutableName;
        // Wait before reading configuration or removing the renamed image. The old controller
        // persists its final settings during destruction, after the window closes.
        std::wstring command = L"\"" + executable.wstring() + L"\" --wait-for-update " +
            std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{.cb = sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_UNICODE_ENVIRONMENT, nullptr, executable.parent_path().c_str(),
                &startup, &process) == FALSE) {
            failure = "new launcher could not be started (Win32 " +
                std::to_string(GetLastError()) + ')';
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    void RefreshProcessesImpl(ProcessAnnounce announce = ProcessAnnounce::Always) {
        auto processes = anomaly::launcher::EnumerateAttachableProcesses();
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            std::erase_if(processes, [&game_directory](const auto& process) {
                return process.executable_path.empty() ||
                    !PathsEqual(process.executable_path.parent_path(), game_directory);
            });
        }
        const auto runtime = ResolveAttachRuntime();
        const bool core_available = runtime.Ok();
        std::scoped_lock lock(state_mutex_);
        const bool had_processes = !state_.processes.empty();
        state_.processes = std::move(processes);
        state_.core_available = core_available;
        state_.runtime_version = runtime.version;
        state_.runtime_message = runtime.message;
        if (std::none_of(
                state_.processes.begin(), state_.processes.end(),
                [this](const auto& process) {
                    return process.process_id == state_.attached_process;
                })) {
            state_.attached_process = 0;
        }
        if (announce == ProcessAnnounce::Never) {
            return;
        }
        // The background poll only speaks when the answer changed, so a list that fills in does not keep
        // claiming that nothing is running.
        if (announce == ProcessAnnounce::OnChange && had_processes == !state_.processes.empty()) {
            return;
        }
        state_.message_scope = MessageScope::Attach;
        if (!core_available) {
            state_.message = runtime.message.empty()
                ? MakeLauncherMessage(anomaly::MessageId::LauncherStatusCoreUnavailable)
                : MakeLauncherMessage(
                    anomaly::MessageId::LauncherStatusCoreUnavailable, {},
                    runtime.message);
            state_.message_kind = MessageKind::Error;
        } else if (state_.processes.empty()) {
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusNoProcesses);
            state_.message_kind = MessageKind::Neutral;
        } else if (state_.attached_process != 0) {
            // The status describes the state, not the act of scanning, so a scan that
            // finds the attached process keeps reporting the attachment instead of
            // replacing it with a note that the list was rebuilt.
            const std::string process_id = std::to_string(state_.attached_process);
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusLaunchAttached, {process_id});
            state_.message_kind = MessageKind::Success;
        } else {
            // Opening the launcher while the game already runs lands here. The process
            // and its id are what the mode has to report; a note that the list was
            // rebuilt says nothing about the state it just read.
            const std::string process_id =
                std::to_string(state_.processes.front().process_id);
            state_.message = MakeLauncherMessage(
                anomaly::MessageId::LauncherStatusProcessFound, {process_id});
            state_.message_kind = MessageKind::Neutral;
        }
    }

    std::filesystem::path payload_root_;
    std::uint64_t theme_version_{};
    anomaly::launcher::ProxyInstallationSource source_;
    std::filesystem::path configuration_path_;
    mutable std::mutex state_mutex_;
    anomaly::launcher::LauncherConfiguration configuration_;
    LauncherSnapshot state_;
    // Set when a preference changed in memory and the configuration still has to be written.
    // Guarded by state_mutex_, flushed by FlushSettings once the worker is free.
    bool settings_dirty_{};
    bool theme_dirty_{};
    // Set when this launch has done its automatic update check, whether or not it found anything.
    bool auto_check_done_{};
    std::mutex queue_mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Work> queue_;
    // Throttles PollProcesses: a per-frame caller must not turn into a per-frame process scan.
    std::chrono::steady_clock::time_point last_process_poll_{};
    std::jthread update_check_worker_;
    std::string telemetry_attempt_day_;
    std::jthread telemetry_worker_;
    std::jthread worker_;
};

struct Graphics final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    // The window is created without a redirection bitmap, so it draws nothing of its
    // own: the swap chain is bound to a composition visual, and that visual is the
    // window's content. The indirection is what lets the back buffer's alpha reach
    // the desktop, which is what its rounded corners are cut out of.
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<IDCompositionDevice> composition_device;
    ComPtr<IDCompositionTarget> composition_target;
    ComPtr<IDCompositionVisual> composition_visual;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11ShaderResourceView> logo;
    ComPtr<ID3D11ShaderResourceView> theme_background;
    std::uint32_t theme_background_width{};
    std::uint32_t theme_background_height{};
    std::vector<ComPtr<ID3D11ShaderResourceView>> theme_stickers;
    std::vector<ImVec2> theme_sticker_sizes;
    std::uint64_t theme_version{};
};

Graphics* g_graphics{};
float g_launcher_dpi_scale{1.0f};
bool g_launcher_dpi_changed{};
bool g_launcher_hotkey_capture{};
std::array<bool, 256> g_launcher_hotkey_down{};

bool IsLauncherHotkeyModifier(const std::uint32_t key) noexcept {
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        key == VK_LCONTROL ||
        key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU;
}

void BeginLauncherHotkeyCapture() {
    g_launcher_hotkey_capture = true;
    for (std::uint32_t key = 0; key <= 0xff; ++key) {
        g_launcher_hotkey_down[key] =
            (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
}

std::optional<std::uint32_t> CaptureLauncherHotkey() {
    if (!g_launcher_hotkey_capture) return std::nullopt;
    for (std::uint32_t key = 8; key <= 0xff; ++key) {
        if (key >= VK_LBUTTON && key <= VK_XBUTTON2) continue;
        // The generic aliases report both physical Shift keys. Skip them so
        // the left/right virtual key, especially VK_RSHIFT, can be captured.
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
        const bool down =
            (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        const bool pressed = down && !g_launcher_hotkey_down[key];
        g_launcher_hotkey_down[key] = down;
        if (!pressed) continue;
        if (key == VK_ESCAPE) {
            g_launcher_hotkey_capture = false;
            return std::nullopt;
        }
        if (IsLauncherHotkeyModifier(key)) continue;
        g_launcher_hotkey_capture = false;
        return key;
    }
    return std::nullopt;
}

float DpiScale(UINT dpi) noexcept {
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / kDefaultDpi;
}

float Scale(float value) noexcept {
    return std::round(value * g_launcher_dpi_scale);
}

ImVec2 Scale(float x, float y) noexcept {
    return ImVec2(Scale(x), Scale(y));
}

float ButtonHeight(float logical_height) noexcept {
    return (std::max)(Scale(logical_height), ImGui::GetFrameHeight());
}

// The content layer of the page: the glass the launcher is built on -- a lens over the pane with the
// material's own rim, gloss and refraction, which is what a card here has always looked like. Its
// glow is switched off, because around a shape with a radius this large the shadow only reads as a
// box in the corner.
anomaly::glass::SurfaceOptions LauncherCardSurface(const float rounding) {
    anomaly::glass::SurfaceOptions surface;
    surface.variant = anomaly::glass::Variant::Regular;
    surface.rounding = rounding;
    surface.scale = g_launcher_dpi_scale;
    surface.shadow = false;
    return surface;
}

void ApplyLauncherDpiScale() noexcept {
    ImGuiIO& io = ImGui::GetIO();
    // This launcher has one baked font. Minimal atlas retention discards its source
    // name, so the host's name-based font selection cannot be used after upload.
    if (io.FontDefault != nullptr && io.FontDefault->FontSize > 0.0f) {
        io.FontDefault->Scale = ue5mem::PlatformUiBaseFontSize() / io.FontDefault->FontSize;
    }
    io.FontGlobalScale = g_launcher_dpi_scale * kLauncherFontScale;
    // The shared style already scales its metrics by FontGlobalScale.
    ue5mem::ApplyPlatformUiStyle();
    // The launcher writes with its own two ink tones rather than the palette's: it
    // paints the body itself under the wallpaper and its veil, and both are chosen for
    // the surface the palette actually has -- see LauncherInk.
    ImGui::GetStyle().Colors[ImGuiCol_Text] = LauncherInk();
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = LauncherMutedInk();
    // No popup in this launcher dims what is behind it. The launcher is a themed surface with a
    // wallpaper and stickers, and a dim veil over all of it reads as the window switching off
    // rather than as a question being asked; the popup itself is frosted glass and already
    // separates from the page. Set here rather than per popup, so a popup added later cannot
    // reintroduce the veil by accident.
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    g_launcher_dpi_changed = false;
}

bool CreateRenderTarget(Graphics& graphics) {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(graphics.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    return SUCCEEDED(graphics.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &graphics.render_target));
}

bool CreateGraphics(HWND window, Graphics& graphics) {
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    // The device is created on its own rather than through
    // D3D11CreateDeviceAndSwapChain: DXGI_SWAP_CHAIN_DESC carries no AlphaMode field,
    // and an alpha channel is what keeps the window's rounded corners transparent.
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &graphics.device, &selected, &graphics.context))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(graphics.device.As(&dxgi_device))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    // A composition swap chain has no output window to inherit a size from, so that
    // pair cannot be left at zero the way a window-bound one allows: DXGI rejects it
    // with DXGI_ERROR_INVALID_CALL and the launcher never reaches a first frame. The
    // size is taken here, and the resize handler keeps it in step from then on.
    RECT client{};
    if (GetClientRect(window, &client) == FALSE) return false;
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>(client.right - client.left);
    description.Height = static_cast<UINT>(client.bottom - client.top);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    // Composition takes sequential rather than discard, and premultiplied is the only
    // alpha mode it accepts.
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> swap_chain;
    if (FAILED(factory->CreateSwapChainForComposition(
            graphics.device.Get(), &description, nullptr, &swap_chain))) {
        return false;
    }
    if (FAILED(DCompositionCreateDevice(
            dxgi_device.Get(), IID_PPV_ARGS(&graphics.composition_device)))) {
        return false;
    }
    if (FAILED(graphics.composition_device->CreateTargetForHwnd(
            window, TRUE, &graphics.composition_target))) {
        return false;
    }
    if (FAILED(graphics.composition_device->CreateVisual(&graphics.composition_visual))) {
        return false;
    }
    if (FAILED(graphics.composition_visual->SetContent(swap_chain.Get()))) return false;
    if (FAILED(graphics.composition_target->SetRoot(graphics.composition_visual.Get()))) {
        return false;
    }
    if (FAILED(graphics.composition_device->Commit())) return false;
    if (FAILED(swap_chain.As(&graphics.swap_chain))) return false;
    return CreateRenderTarget(graphics);
}

// Uploads decoded RGBA8 pixels. The launcher owns its device directly, so there
// is no resource registry to go through.
ComPtr<ID3D11ShaderResourceView> CreateTextureFromPixels(
    Graphics& graphics, const std::span<const std::uint8_t> pixels,
    const std::uint32_t width, const std::uint32_t height) {
    if (graphics.device == nullptr || width == 0 || height == 0 ||
        pixels.size() < static_cast<std::size_t>(width) * height * 4U) {
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * 4U;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(graphics.device->CreateTexture2D(&description, &initial, &texture))) return {};
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(graphics.device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
        return {};
    }
    return view;
}

// Theme artwork arrives at whatever resolution the artist had: the launcher's own sticker is
// 1956 x 2722 and is drawn into roughly 795 x 1106, so it is always *minified*. A single-mip texture
// would be sampled with bilinear minification and sparkle as the window moves, wasting the extra
// resolution, so the full mip chain is generated once here and the renderer picks the level.
ComPtr<ID3D11ShaderResourceView> CreateTextureFromThemeImage(
    Graphics& graphics, const LauncherThemeImage& image, std::uint32_t& width,
    std::uint32_t& height) {
    width = 0;
    height = 0;
    if (graphics.device == nullptr || graphics.context == nullptr || image.width == 0 ||
        image.height == 0 ||
        image.pixels.size() < static_cast<std::size_t>(image.width) * image.height * 4U) {
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.width;
    description.Height = image.height;
    description.MipLevels = 0;  // the whole chain
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(graphics.device->CreateTexture2D(&description, nullptr, &texture))) return {};
    graphics.context->UpdateSubresource(texture.Get(), 0, nullptr, image.pixels.data(),
        image.width * 4U, 0);
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(graphics.device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
        return {};
    }
    graphics.context->GenerateMips(view.Get());
    width = image.width;
    height = image.height;
    return view;
}

// Decodes one embedded picture and uploads it. The decoded size is reported through the
// optional out parameters: a caller that draws the picture into a slot it did not author
// needs the pixel size to crop rather than stretch it.
ComPtr<ID3D11ShaderResourceView> LoadEmbeddedTexture(
    Graphics& graphics, const int resource_id, std::uint32_t* width = nullptr,
    std::uint32_t* height = nullptr) {
    if (width != nullptr) *width = 0;
    if (height != nullptr) *height = 0;
    const HRSRC resource = FindResourceW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (resource == nullptr) return {};
    const HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const void* data = loaded == nullptr ? nullptr : LockResource(loaded);
    if (data == nullptr || size == 0) return {};
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto decoded = anomaly::DecodeUiImageRgba8(std::span(bytes, size));
    if (!decoded) return {};
    auto view = CreateTextureFromPixels(graphics, decoded.image.pixels,
        decoded.image.width, decoded.image.height);
    if (view != nullptr) {
        if (width != nullptr) *width = decoded.image.width;
        if (height != nullptr) *height = decoded.image.height;
    }
    return view;
}

// Uploads a newly published theme payload. Decoding already happened on the
// worker, so the render thread only copies pixels into a device texture.
void UploadLauncherTheme(
    Graphics& graphics, const std::shared_ptr<const LauncherTheme>& theme) {
    if (theme == nullptr || theme->version == graphics.theme_version) return;
    graphics.theme_version = theme->version;
    graphics.theme_background.Reset();
    graphics.theme_background_width = 0;
    graphics.theme_background_height = 0;
    graphics.theme_stickers.clear();
    graphics.theme_sticker_sizes.clear();
    if (theme->HasBackground()) {
        graphics.theme_background = CreateTextureFromThemeImage(
            graphics, theme->background, graphics.theme_background_width,
            graphics.theme_background_height);
    }
    graphics.theme_stickers.reserve(theme->stickers.size());
    graphics.theme_sticker_sizes.reserve(theme->stickers.size());
    for (const LauncherThemeImage& image : theme->stickers) {
        std::uint32_t width{};
        std::uint32_t height{};
        graphics.theme_stickers.push_back(
            CreateTextureFromThemeImage(graphics, image, width, height));
        graphics.theme_sticker_sizes.emplace_back(
            static_cast<float>(width), static_cast<float>(height));
    }
    // The textures are the picture now. Keeping the decoded RGBA8 copies would hold
    // the wallpaper and both stickers resident -- 15 MB -- for a frame that draws
    // from the textures, so nothing reads them again.
    const auto release_pixels = [](const LauncherThemeImage& image) {
        std::vector<std::uint8_t>().swap(image.pixels);
    };
    release_pixels(theme->background);
    for (const LauncherThemeImage& image : theme->stickers) release_pixels(image);
}

// The launcher shares the palette and layout the in-game shell uses, so a theme
// only has to be authored once. The palette is the one the settings page stored, so
// switching it there repaints the window on the next frame rather than on the next
// launch. A palette of "custom" is built from the colours the launcher stores, which are applied
// here every frame: a colour being dragged updates the window without reloading its artwork.
void ApplyLauncherTheme(const std::shared_ptr<const LauncherTheme>& theme,
    const anomaly::PlatformUiCustomColors& custom_colors) {
    if (theme == nullptr) return;
    if (theme->palette == anomaly::PlatformUiPalette::Custom)
        ue5mem::SetPlatformUiCustomColors(custom_colors);
    ue5mem::SetPlatformUiPalette(theme->palette);
    ue5mem::SetPlatformUiSurfaceAlpha(
        theme->HasBackground() ? theme->layout.surface_opacity : 1.0f);
    // Rebuilds the style from the palette just stored, so every control follows the
    // theme rather than the one the previous frame drew with.
    ApplyLauncherDpiScale();
    // These two tones are the launcher's own: it paints the body itself instead of
    // using the shell's panels, so its ink follows the surface lightness.
    ImGui::GetStyle().Colors[ImGuiCol_Text] = LauncherInk();
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = LauncherMutedInk();
}

void DrawLauncherThemeImages(
    Graphics& graphics, const std::shared_ptr<const LauncherTheme>& theme,
    const ImVec2 origin, const ImVec2 size, const bool front) {
    if (theme == nullptr) return;
    const auto& layout = theme->layout;
    const auto tint = [](const anomaly::PlatformUiColor& color, const float alpha) {
        return ImGui::ColorConvertFloat4ToU32(
            ImVec4(color.red, color.green, color.blue, color.alpha * alpha));
    };
    ImDrawList* const list = front ? ImGui::GetForegroundDrawList()
                                   : ImGui::GetWindowDrawList();
    if (list == nullptr) return;
    // The wallpaper and the veil are the shell's own body and stop at its edges, so nothing paints
    // past the canvas and the window's corners stay transparent.
    list->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    const float body_rounding = ImGui::GetStyle().WindowRounding;
    if (!front) {
        // The window contributes no body of its own: it has no redirection bitmap, and
        // every panel fill is empty while a theme is active. The shell therefore paints
        // its own opaque base first -- the layer the in-game shell gets for free from
        // its opaque window. Without it the veil would composite against the desktop
        // instead of against the surface colour, and whatever sits behind the launcher
        // would show through the window.
        list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
            tint(ue5mem::PlatformUiSurfaceColor(), 1.0f), body_rounding);
    }
    if (!front && graphics.theme_background != nullptr &&
        graphics.theme_background_width != 0 && graphics.theme_background_height != 0) {
        const auto placement = anomaly::ComputePlatformUiBackgroundPlacement(
            layout.background.mode,
            static_cast<float>(graphics.theme_background_width),
            static_cast<float>(graphics.theme_background_height),
            origin.x, origin.y, size.x, size.y);
        if (placement.Visible()) {
            const bool covers_body = placement.x <= origin.x + 0.5f &&
                placement.y <= origin.y + 0.5f &&
                placement.x + placement.width >= origin.x + size.x - 0.5f &&
                placement.y + placement.height >= origin.y + size.y - 0.5f;
            list->AddImageRounded(
                static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(graphics.theme_background.Get())),
                ImVec2(placement.x, placement.y),
                ImVec2(placement.x + placement.width, placement.y + placement.height),
                ImVec2(placement.uv0_x, placement.uv0_y),
                ImVec2(placement.uv1_x, placement.uv1_y),
                tint(layout.background.tint, layout.background.opacity),
                covers_body ? body_rounding : 0.0f);
        }
    }
    // The shell paints one translucent veil between its wallpaper and its
    // stickers and leaves every panel fill empty while a theme is active, so the
    // launcher has to paint that same veil: without it nothing stands in for the
    // panels and the window loses its palette instead of showing the theme.
    if (!front) {
        const bool wallpaper = graphics.theme_background != nullptr &&
            layout.background.mode != anomaly::PlatformUiBackgroundMode::Disabled;
        const float veil_alpha =
            wallpaper ? ue5mem::GetPlatformUiSurfaceAlpha() : 1.0f;
        if (veil_alpha > 0.0f) {
            list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                tint(ue5mem::PlatformUiSurfaceColor(), veil_alpha), body_rounding);
        }
    }
    list->PopClipRect();
    // Stickers, unlike the body, are meant to spill: the root window's own clip rect is inset by its
    // padding, which would slice off whatever lies past the canvas and leave the plain surface
    // showing instead. This is what lets a sticker reach the canvas edge.
    list->PushClipRect(
        ImVec2(-8192.0f, -8192.0f), ImVec2(8192.0f, 8192.0f), false);
    if (front) {
        list->PushClipRect(
            origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    }
    for (std::size_t index = 0;
         index < layout.stickers.size() && index < graphics.theme_stickers.size();
         ++index) {
        const auto& sticker = layout.stickers[index];
        if ((sticker.layer == anomaly::PlatformUiStickerLayer::Front) != front) continue;
        if (graphics.theme_stickers[index] == nullptr) continue;
        const ImVec2 image_size = graphics.theme_sticker_sizes[index];
        // The layout stores a width and a height for every sticker, both
        // normalised to a viewport the launcher does not share. Using both scales
        // the two axes independently, which stretches the artwork. Keeping only
        // the width lets the placement derive the height from the image's own
        // aspect, so a sticker keeps the proportions it has in the shell.
        anomaly::PlatformUiSticker fitted = sticker;
        if (fitted.width > 0.0f && fitted.height > 0.0f) fitted.height = 0.0f;
        const auto placement = anomaly::ComputePlatformUiStickerPlacement(
            fitted, image_size.x, image_size.y, size.x, size.y);
        if (!placement.Visible()) continue;
        const ImTextureID texture = static_cast<ImTextureID>(
            reinterpret_cast<std::uintptr_t>(graphics.theme_stickers[index].Get()));
        const ImU32 color = tint(sticker.tint, sticker.opacity);
        const ImVec2 position(origin.x + placement.x, origin.y + placement.y);
        if (placement.rotation == 0.0f) {
            list->AddImage(texture, position,
                ImVec2(position.x + placement.width, position.y + placement.height),
                ImVec2(placement.uv0_x, placement.uv0_y),
                ImVec2(placement.uv1_x, placement.uv1_y), color);
            continue;
        }
        const auto quad = anomaly::ComputePlatformUiImageQuad(placement);
        list->AddImageQuad(texture,
            ImVec2(origin.x + quad.x[0], origin.y + quad.y[0]),
            ImVec2(origin.x + quad.x[1], origin.y + quad.y[1]),
            ImVec2(origin.x + quad.x[2], origin.y + quad.y[2]),
            ImVec2(origin.x + quad.x[3], origin.y + quad.y[3]),
            ImVec2(quad.u[0], quad.v[0]), ImVec2(quad.u[1], quad.v[1]),
            ImVec2(quad.u[2], quad.v[2]), ImVec2(quad.u[3], quad.v[3]), color);
    }
    if (front) list->PopClipRect();
    list->PopClipRect();
}

LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_NCCALCSIZE:
        // The window is frameless and the launcher draws its own title bar, so
        // the client area has to cover the whole window. Leaving the frame in
        // place would show a strip of window background above the caption.
        if (wparam != 0) return 0;
        break;
    case WM_SIZE:
        if (g_graphics != nullptr && g_graphics->swap_chain != nullptr &&
            wparam != SIZE_MINIMIZED) {
            g_graphics->render_target.Reset();
            if (SUCCEEDED(g_graphics->swap_chain->ResizeBuffers(
                    0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0))) {
                static_cast<void>(CreateRenderTarget(*g_graphics));
            }
        }
        return 0;
    case WM_DPICHANGED: {
        g_launcher_dpi_scale = DpiScale(HIWORD(wparam));
        g_launcher_dpi_changed = true;
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            static_cast<void>(SetWindowPos(
                window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER));
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {
            static_cast<LONG>(Scale(720.0f)), static_cast<LONG>(Scale(500.0f))};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::optional<std::filesystem::path> ChooseGameDirectory(
    HWND owner, const std::filesystem::path& current,
    const anomaly::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    const std::wstring title = Utf8Wide(
        translator.Text(anomaly::MessageId::LauncherDialogGameDirectory));
    dialog->SetTitle(title.c_str());
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

// File and network helpers run on worker threads and have no translator parameter, but they still
// produce user-visible text. The UI stores the active translator here each frame; lookups are
// read-only after start-up, so concurrent reads are safe.
// Background downloads also translate their results. Each reader retains its catalog while a
// language switch publishes the replacement.
std::atomic<std::shared_ptr<const anomaly::Translator>> g_translator;
// The language the catalog in hand was built for, so the render loop only reloads on a real change.
anomaly::LanguagePreference g_translator_language{anomaly::LanguagePreference::Auto};

// Loads the host catalog for one language into the launcher's globals. Called at start-up and again
// whenever the settings page changes the language: both locales are already baked into the atlas,
// so a switch costs one JSON parse and nothing else.
bool LoadLauncherCatalog(const anomaly::LanguagePreference preference,
    const std::filesystem::path& locale_directory) {
    const auto locale = anomaly::ResolveUserLocale(preference);
    const auto loaded = anomaly::LoadHostCatalog(locale.locale, locale_directory);
    if (loaded.translator == nullptr) return false;
    g_translator.store(loaded.translator);
    g_translator_language = preference;
    return true;
}

std::string Num(const double value, const int precision) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

std::string Tr(const anomaly::MessageId id) {
    const auto translator = g_translator.load();
    return translator != nullptr ? std::string(translator->Text(id)) : std::string{};
}

std::string Tr(const anomaly::MessageId id, std::initializer_list<std::string_view> arguments) {
    const auto translator = g_translator.load();
    if (translator == nullptr) return {};
    return translator->Format(id, std::span<const std::string_view>(arguments.begin(),
        arguments.size()));
}

std::string Format(
    const anomaly::Translator& translator, anomaly::MessageId id,
    std::initializer_list<std::string_view> arguments) {
    return translator.Format(id, std::span<const std::string_view>(
        arguments.begin(), arguments.size()));
}


void Tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

// Every command in the launcher is one chip of the same material: an invisible item for the hit box,
// the liquid-glass surface behind it and the label centred on top. A command that carries a message
// (destructive, primary) tints that surface instead of falling back to a flat fill, so a row of
// commands still reads as one material rather than as glass next to paint.
struct LauncherChipStyle final {
    ImVec4 tint{};             // alpha 0 keeps the palette's own tint colour
    ImVec4 ink{};              // alpha 0 keeps the palette's text colour
    float tint_strength{-1.0f};
    float rounding{-1.0f};     // negative is a capsule
};

// The part of an ImGui label that is drawn: anything after "##" only names the widget.
std::string_view LauncherChipText(const char* label) noexcept {
    const std::string_view text(label);
    const auto hidden = text.find("##");
    return hidden == std::string_view::npos ? text : text.substr(0, hidden);
}

bool LauncherChip(const char* label, const ImVec2 requested = {}, const bool enabled = true,
    const LauncherChipStyle& style = {}) {
    const std::string caption = Ellipsize(
        LauncherChipText(label), (requested.x > 0.0f ? requested.x : 4096.0f) - Scale(16.0f));
    const ImVec2 caption_size = ImGui::CalcTextSize(caption.c_str());
    ImVec2 size = requested;
    if (size.x <= 0.0f) size.x = caption_size.x + Scale(30.0f);
    if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    // The invisible item is the button: it owns the id, the hit box and the pressed state, which is
    // released on mouse up exactly like the themed button it replaces.
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    // The same glass the panes are made of, one size down: a capsule with the material's rim, gloss
    // and refraction. Its glow is off -- around a shape this small it reads as a box -- and the tint
    // rises while the pointer is on it, which is what tells a control from the pane behind it.
    anomaly::glass::SurfaceOptions surface;
    surface.variant = anomaly::glass::Variant::Regular;
    surface.rounding = style.rounding >= 0.0f ? Scale(style.rounding) : size.y * 0.5f;
    surface.scale = g_launcher_dpi_scale;
    surface.shadow = false;
    surface.minimum_alpha = enabled ? 0.82f : 0.56f;
    float strength = style.tint_strength >= 0.0f
        ? style.tint_strength : (LauncherSurfaceIsLight() ? 0.24f : 0.20f);
    if (ImGui::IsItemActive()) strength += 0.16f;
    else if (hovered && enabled) strength += 0.10f;
    if (!enabled) strength *= 0.55f;
    surface.tint_strength = strength;
    if (style.tint.w > 0.0f) surface.tint = style.tint;
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    anomaly::glass::DrawSurface(draw, min, max, surface);
    if (!caption.empty()) {
        const ImVec4 ink = style.ink.w > 0.0f ? style.ink : LauncherInk();
        const ImVec4 colour = enabled ? ink : ImVec4(ink.x, ink.y, ink.z, ink.w * 0.45f);
        draw->AddText(ImVec2(min.x + (size.x - caption_size.x) * 0.5f,
                          min.y + (size.y - caption_size.y) * 0.5f),
            ImGui::GetColorU32(colour), caption.c_str());
    }
    return pressed && enabled;
}

bool IconButton(const char* id, char32_t glyph, const char* tooltip, bool enabled = true) {
    ImGui::PushID(id);
    const float extent = ButtonHeight(38.0f);
    LauncherChipStyle style;
    style.rounding = extent * 0.5f;
    const bool pressed = LauncherChip("##icon", ImVec2(extent, extent), enabled, style);
    // The mark is centred by hand. These are private-use icon glyphs whose bearing
    // and advance are not symmetric, so leaving the centring to the text layout
    // puts them visibly off centre inside the frame.
    const ImVec2 frame_min = ImGui::GetItemRectMin();
    const ImVec2 frame_max = ImGui::GetItemRectMax();
    const char* const mark = Glyph(glyph);
    const ImVec2 mark_size = ImGui::CalcTextSize(mark);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(frame_min.x + (frame_max.x - frame_min.x - mark_size.x) * 0.5f,
            frame_min.y + (frame_max.y - frame_min.y - mark_size.y) * 0.5f),
        ImGui::GetColorU32(enabled ? LauncherInk() : ImVec4(
            LauncherInk().x, LauncherInk().y, LauncherInk().z, 0.45f)), mark);
    Tooltip(tooltip);
    ImGui::PopID();
    return pressed && enabled;
}

bool CommandButton(
    const char* id, char32_t glyph, const char* label,
    bool primary, bool enabled, ImVec2 size = {}) {
    std::string text = std::string(Glyph(glyph)) + "  " + label;
    ImVec2 scaled_size(
        size.x > 0.0f ? Scale(size.x) : 0.0f,
        ButtonHeight(size.y > 0.0f ? size.y : 30.0f));
    if (scaled_size.x > 0.0f) {
        text = Ellipsize(text, (std::max)(0.0f, scaled_size.x - Scale(16.0f)));
    }
    const auto& theme = ue5mem::PlatformUiTheme();
    LauncherChipStyle style;
    // A primary command keeps the accent colour inside the glass; a secondary one is the plain
    // material, so the two differ by tint rather than by being made of different things.
    if (primary) {
        style.tint = ThemeColor(theme.accent);
        style.tint.w = 1.0f;
        style.tint_strength = 0.86f;
        style.ink = ThemeColor(theme.inverse_text);
    } else {
        style.ink = LauncherInk();
    }
    ImGui::PushID(id);
    const bool pressed = LauncherChip((text + "##command").c_str(), scaled_size, enabled, style);
    ImGui::PopID();
    return pressed && enabled;
}

// A command that destroys something. It is the only button in a row that carries the palette's
// danger colour, so a list of grey commands never gets a destructive click by mistake -- on a
// grid card the same applies, where every other command is a plain chip.
bool DestructiveButton(const char* label, const ImVec2 size, const bool enabled) {
    const auto& theme = ue5mem::PlatformUiTheme();
    LauncherChipStyle style;
    style.tint = ThemeColor(theme.danger);
    style.tint.w = 1.0f;
    style.tint_strength = 0.34f;
    style.ink = ThemeColor(theme.danger);
    return LauncherChip(label, size, enabled, style);
}

// The width a command button needs for its own label, in the same unscaled units CommandButton takes. A
// translation is as long as it is: a fixed width ellipsizes the ones that grew.
float CommandButtonWidth(char32_t glyph, const char* label) {
    const std::string text = std::string(Glyph(glyph)) + "  " + label;
    const float pixels = ImGui::CalcTextSize(text.c_str()).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    const float scale = g_launcher_dpi_scale > 0.0f ? g_launcher_dpi_scale : 1.0f;
    return std::ceil(pixels / scale) + 6.0f;
}

const char* Text(const anomaly::Translator& translator, anomaly::MessageId id) noexcept {
    return translator.Text(id).data();
}

std::string StableLabel(
    const anomaly::Translator& translator, anomaly::MessageId id,
    std::string_view stable_id) {
    return anomaly::StableDisplayLabel(translator.Text(id), stable_id);
}

std::string RenderMessage(
    const anomaly::Translator& translator, const LauncherMessage& message) {
    std::vector<std::string_view> arguments;
    arguments.reserve(message.arguments.size());
    for (const auto& argument : message.arguments) arguments.push_back(argument);
    std::string result = message.arguments.empty()
        ? std::string(translator.Text(message.id))
        : translator.Format(message.id, arguments);
    if (!message.detail.empty()) {
        if (!result.empty()) result.append(": ");
        result.append(message.detail);
    }
    return result;
}

ImVec4 ProxyStateColor(anomaly::launcher::ProxyInstallationState state) {
    const auto& theme = ue5mem::PlatformUiTheme();
    using State = anomaly::launcher::ProxyInstallationState;
    switch (state) {
    case State::Enabled: return ThemeColor(theme.success);
    case State::UpdateAvailable: return ThemeColor(theme.warning);
    case State::Disabled:
    case State::NotInstalled: return LauncherMutedInk();
    case State::Conflict:
    case State::Unavailable: return ThemeColor(theme.danger);
    }
    return LauncherMutedInk();
}

void DrawModes(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    LauncherMode& mode, const anomaly::Translator& translator) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 9.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "LauncherModes", ImVec2(0.0f, Scale(kModeHeight)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    const std::array<const char*, 2> labels{
        Text(translator, anomaly::MessageId::LauncherModeLaunch),
        Text(translator, anomaly::MessageId::LauncherModeSettings)};
    anomaly::glass::SegmentedOptions options;
    options.height = 54.0f;
    options.padding_x = 32.0f;
    options.scale = g_launcher_dpi_scale;
    int selected = static_cast<int>(mode);
    if (anomaly::glass::SegmentedControl("launcher-modes", labels, selected, options)) {
        mode = static_cast<LauncherMode>(selected);
    }
    // A release this launcher can install is worth saying on the band, which is the one row that is
    // on screen in every mode. Clicking it asks the same question the automatic check asks -- the
    // dialog with the notes and the two answers -- rather than sending the user off to another page.
    if (snapshot.update.phase == LauncherUpdatePhase::Available &&
        !snapshot.update.remote_version.empty()) {
        ImGui::SameLine(0.0f, Scale(12.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
            (options.height * g_launcher_dpi_scale - ButtonHeight(38.0f)) * 0.5f);
        LauncherChipStyle style;
        // #ffa31a, filled rather than tinted: the badge is the one place in this row that is a colour
        // rather than a material, and the palette's inverse ink keeps the label readable on it.
        style.tint = ImVec4(1.0f, 0.639f, 0.102f, 1.0f);
        style.tint.w = 1.0f;
        style.tint_strength = 0.86f;
        style.ink = ThemeColor(ue5mem::PlatformUiTheme().inverse_text);
        if (LauncherChip(Format(translator, anomaly::MessageId::LauncherUpdateAvailable,
                {snapshot.update.remote_version}).c_str(),
                ImVec2(0.0f, ButtonHeight(38.0f)), true, style)) {
            g_update_prompt_requested = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DrawReadOnlyPath(
    const char* id, const std::filesystem::path& path, float trailing_width,
    const anomaly::Translator& translator) {
    std::string value = path.empty()
        ? std::string(translator.Text(anomaly::MessageId::LauncherNoDirectorySelected))
        : PathUtf8(path);
    ImGui::SetNextItemWidth((std::max)(
        Scale(120.0f), ImGui::GetContentRegionAvail().x - Scale(trailing_width)));
    ImGui::InputText(
        id, value.data(), value.size() + 1,
        ImGuiInputTextFlags_ReadOnly);
}

void DrawStartupSettings(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionSettings));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingMenuToggle));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(190.0f));
    const auto captured = CaptureLauncherHotkey();
    if (captured) controller.SetToggleKey(*captured);
    const std::string label = g_launcher_hotkey_capture
        ? std::string(Text(translator, anomaly::MessageId::LauncherSettingPressKey))
        : VirtualKeyName(snapshot.toggle_key);
    ImGui::BeginDisabled(snapshot.busy);
    if (LauncherChip(label.c_str(), ImVec2(Scale(180.0f), ButtonHeight(38.0f)))) {
        BeginLauncherHotkeyCapture();
    }
    ImGui::EndDisabled();
    if (g_launcher_hotkey_capture) {
        ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSettingEscapeHint));
    }
}

// The width a status line has to leave free for the refresh at its right edge, so
// the text beside it wraps before the icon instead of running underneath it.
float StatusRefreshReserve() {
    return ButtonHeight(38.0f) + Scale(16.0f) + Scale(8.0f);
}

// The refresh belongs on a mode's status line, at its right edge: it acts on exactly
// the state shown beside it. On an action row below it followed a right-aligned
// button and was pushed off the panel entirely. Both modes carry one, so the
// placement and the centring live here rather than being repeated.
bool StatusRefreshButton(
    const char* id, const char* tooltip, const bool enabled, const float status_top) {
    const float icon_extent = ButtonHeight(38.0f);
    ImGui::SameLine();
    // Centred against the status line rather than sitting on its baseline, which a
    // frame-height button always overflows.
    ImGui::SetCursorPosY(
        status_top - (icon_extent - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::SetCursorPosX(
        ImGui::GetWindowWidth() - icon_extent - Scale(16.0f));
    return IconButton(id, 0xe72c, tooltip, enabled);
}

void DrawProxyMode(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    using State = anomaly::launcher::ProxyInstallationState;

    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionInstallation));
    ImGui::TextColored(ProxyStateColor(snapshot.proxy.state), "%s",
        Text(translator, ProxyStateMessageId(snapshot.proxy.state)));
    const float status_top = ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y;
    if (StatusRefreshButton(
            "refresh-proxy", Text(translator, anomaly::MessageId::LauncherProxyRefresh),
            !snapshot.busy && !snapshot.game_directory.empty(), status_top)) {
        controller.RefreshProxy();
    }
    if (snapshot.proxy.state == State::UpdateAvailable) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(LauncherMutedInk(), "%s", Text(
            translator, anomaly::MessageId::LauncherProxyUpdateDescription));
        ImGui::PopTextWrapPos();
    }
    if (snapshot.message_kind != MessageKind::Neutral &&
        snapshot.message_scope != MessageScope::Attach) {
        // Install and enable failures used to be reported by the footer, which is
        // gone, so the status block carries them now. Attach outcomes are left out:
        // the controller keeps one message, and without this an attach result
        // described itself here, under a mode that cannot attach.
        const ImVec4 proxy_ink = snapshot.message_kind == MessageKind::Error
            ? ThemeColor(theme.danger)
            : ThemeColor(theme.success);
        const std::string proxy_status = RenderMessage(translator, snapshot.message);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(proxy_ink, "%s", proxy_status.c_str());
        ImGui::PopTextWrapPos();
    }

    const bool installed = snapshot.proxy.state == State::Enabled ||
        snapshot.proxy.state == State::Disabled || snapshot.proxy.state == State::UpdateAvailable;
    const bool can_uninstall = !snapshot.busy && installed && snapshot.processes.empty();
    const bool can_install = !snapshot.busy && !snapshot.game_directory.empty() &&
        (snapshot.proxy.state == State::NotInstalled || snapshot.proxy.state == State::UpdateAvailable);
    ImGui::SetCursorPosY((std::max)(ImGui::GetCursorPosY() + Scale(20.0f),
        ImGui::GetWindowHeight() - Scale(kLaunchActionRowBottom)));
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
        ImGui::GetWindowWidth() - Scale(180.0f * 3.0f + 24.0f) - Scale(16.0f)));
    const std::string popup_name = std::string(Text(
        translator, anomaly::MessageId::LauncherProxyCompleteUninstall)) + "###complete-uninstall-proxy";
    static std::filesystem::path pending_directory;
    if (DestructiveButton(Text(translator, anomaly::MessageId::LauncherProxyCompleteUninstall),
            ImVec2(Scale(180.0f), ButtonHeight(62.0f)),
            can_uninstall)) {
        pending_directory = snapshot.game_directory;
        ImGui::OpenPopup(popup_name.c_str());
    }
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (CommandButton("install-proxy", 0xe8b7,
            Text(translator, snapshot.proxy.state == State::NotInstalled
                ? anomaly::MessageId::CommonInstall : anomaly::MessageId::CommonUpdate),
            true, can_install, ImVec2(180.0f, 62.0f))) {
        controller.InstallProxy();
    }
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (CommandButton("toggle-proxy", snapshot.proxy.proxy_enabled ? 0xe711 : 0xe768,
            Text(translator, snapshot.proxy.proxy_enabled
                ? anomaly::MessageId::CommonDisable : anomaly::MessageId::CommonEnable),
            false, !snapshot.busy && installed, ImVec2(180.0f, 62.0f))) {
        controller.SetProxyEnabled(!snapshot.proxy.proxy_enabled);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(Scale(540.0f, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(20.0f, 20.0f));
    if (ImGui::BeginPopupModal(popup_name.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherProxyConfirmUninstall));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", PathUtf8(pending_directory).c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherProxyUninstallHint));
        ImGui::Spacing();
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
            ImGui::GetContentRegionMax().x - Scale(312.0f)));
        if (LauncherChip(Text(translator, anomaly::MessageId::CommonCancel), Scale(120.0f, 42.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine(0.0f, Scale(12.0f));
        if (DestructiveButton(Text(translator, anomaly::MessageId::LauncherProxyCompleteUninstall),
                Scale(180.0f, 42.0f), can_uninstall && pending_directory == snapshot.game_directory)) {
            controller.UninstallProxy();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Hot selections change configuration; compatibility selections change filenames before startup.
// path::string() converts through the ANSI code page, which turns Chinese mod names into "?".
std::string LauncherNarrow(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length,
        nullptr, nullptr);
    return utf8;
}

void LauncherUpdateLog(const std::string& line) {
    std::error_code error;
    const auto directory = ExecutableDirectory() / L"Anomaly" / L"logs";
    std::filesystem::create_directories(directory, error);
    std::ofstream(directory / L"launcher-update.log", std::ios::app)
        << line << "\n";
}

std::wstring LauncherSystemProxy() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_READ,
            &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD enabled{};
    DWORD size = sizeof(enabled);
    DWORD type{};
    std::wstring proxy;
    if (RegQueryValueExW(key, L"ProxyEnable", nullptr, &type,
            reinterpret_cast<LPBYTE>(&enabled), &size) == ERROR_SUCCESS && enabled != 0) {
        wchar_t buffer[512]{};
        DWORD bytes = sizeof(buffer);
        if (RegQueryValueExW(key, L"ProxyServer", nullptr, &type,
                reinterpret_cast<LPBYTE>(buffer), &bytes) == ERROR_SUCCESS) {
            proxy = buffer;
        }
    }
    RegCloseKey(key);
    return proxy;
}

// One GET to one endpoint. The callers split by how they know their address -- a host and a path, or
// a whole URL -- and both end up here. The timeout is theirs because an address known not to answer
// is not worth the wait a download is worth.
std::string LauncherHttpGetRequest(const std::wstring& host, const std::wstring& path,
    const INTERNET_PORT port, const bool secure, const bool use_system_proxy, int* status_code,
    const DWORD timeout_ms) {
    std::string result;
    if (status_code != nullptr) *status_code = 0;
    // Direct first: a stale or half-configured system proxy is a common cause of "works in curl,
    // fails in WinHTTP". The caller retries with the system proxy when this returns nothing.
    const std::wstring proxy = use_system_proxy ? std::wstring{} : LauncherSystemProxy();
    HINTERNET session = WinHttpOpen(L"AnomalyLauncher/1.0",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        LauncherUpdateLog("open session failed: " + std::to_string(GetLastError()));
        return result;
    }
    // TLS 1.2/1.3 explicitly: WinHTTP's default protocol set still excludes them on some builds,
    // which shows up as an intermittent handshake failure.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms),
        static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection != nullptr) {
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (request != nullptr) {
            // Some hosts answer with redirects and reject unknown clients, so the policy follows
            // redirects and the request presents a browser-style identity.
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                sizeof(redirect_policy));
            const wchar_t* headers =
                L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AnomalyLauncher/1.0\r\n"
                L"Accept: application/json\r\n"
                L"Accept-Encoding: identity\r\n";
            const BOOL sent = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            const DWORD send_error = sent == FALSE ? GetLastError() : 0;
            const BOOL received = sent != FALSE ? WinHttpReceiveResponse(request, nullptr) : FALSE;
            const DWORD receive_error = received == FALSE ? GetLastError() : 0;
            if (sent != FALSE && received != FALSE) {
                // The status is what separates "this repository publishes no release" from "the API
                // refused to answer at all", and the launcher reports the difference to the user.
                DWORD status{};
                DWORD status_size = sizeof(status);
                if (WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX) != FALSE && status_code != nullptr) {
                    *status_code = static_cast<int>(status);
                }
                // Read until the stream reports zero bytes: QueryDataAvailable returns 0 for
                // "nothing right now" too, which used to truncate the body mid-JSON.
                std::string chunk(16 * 1024, '\0');
                DWORD read{};
                while (result.size() <= (1u << 20) &&
                    WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read) != FALSE &&
                    read > 0) {
                    result.append(chunk.data(), read);
                    read = 0;
                }
            }
            if (result.empty()) {
                DWORD status{};
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                LauncherUpdateLog("get " + std::string(path.begin(), path.end()) +
                    " empty; status=" + std::to_string(status) +
                    " error=" + std::to_string(GetLastError()));
            }
            WinHttpCloseHandle(request);
        } else {
            LauncherUpdateLog("open request failed: " + std::to_string(GetLastError()));
        }
        WinHttpCloseHandle(connection);
    } else {
        LauncherUpdateLog("connect failed: " + std::to_string(GetLastError()));
    }
    WinHttpCloseHandle(session);
    return result;
}

// produced a host of "https:\" and the request failed with 12005.
bool LauncherDownloadUrl(const std::string& url, const std::filesystem::path& destination,
    LauncherDownloadProgress* const progress) {
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE) {
        LauncherUpdateLog("crack failed " + std::to_string(GetLastError()) + " url=" + url);
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const INTERNET_PORT port = parts.nPort == 0 ? INTERNET_DEFAULT_HTTPS_PORT : parts.nPort;
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring proxy = LauncherSystemProxy();
    HINTERNET session = WinHttpOpen(L"AnomalyLauncher/1.0",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        LauncherUpdateLog("url dl: session failed " + std::to_string(GetLastError()));
        return false;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    bool ok{};
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection == nullptr) {
        LauncherUpdateLog("url dl: connect failed " + std::to_string(GetLastError()) +
            " host=" + std::string(host.begin(), host.end()));
    }
    if (connection != nullptr) {
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (request != nullptr) {
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                sizeof(redirect_policy));
            const wchar_t* headers =
                L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AnomalyLauncher/1.0\r\n"
                L"Accept: */*\r\n";
            if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                    0, 0, 0) != FALSE &&
                WinHttpReceiveResponse(request, nullptr) != FALSE) {
                if (progress != nullptr) {
                    progress->bytes.store(0);
                    wchar_t length_text[32]{};
                    DWORD length_size = sizeof(length_text);
                    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size,
                            WINHTTP_NO_HEADER_INDEX) != FALSE) {
                        progress->total.store(_wtoi64(length_text));
                    } else {
                        progress->total.store(0);
                    }
                }
                std::filesystem::create_directories(destination.parent_path());
                std::ofstream out(destination, std::ios::binary | std::ios::trunc);
                if (out) {
                    ok = true;
                    // Read until zero bytes come back (true EOF); QueryDataAvailable returning 0
                    // only means "nothing right now" and used to truncate the payload.
                    std::string chunk(64 * 1024, '\0');
                    const auto started = std::chrono::steady_clock::now();
                    long long received{};
                    DWORD read{};
                    for (;;) {
                        if (!WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                            ok = false;
                            break;
                        }
                        if (read == 0) break;
                        out.write(chunk.data(), static_cast<std::streamsize>(read));
                        if (!out) { ok = false; break; }
                        received += static_cast<long long>(read);
                        if (progress != nullptr) progress->bytes.fetch_add(static_cast<long long>(read));
                        read = 0;
                        if (progress != nullptr && progress->cancel.load()) {
                            LauncherUpdateLog("url dl: cancelled by user");
                            ok = false;
                            break;
                        }
                        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
                            LauncherUpdateLog("url dl: total timeout after " +
                                std::to_string(received) + " bytes");
                            ok = false;
                            break;
                        }
                    }
                    out.close();
                    if (!out) ok = false;
                    if (ok && received == 0) {
                        LauncherUpdateLog("url dl: empty body " + url);
                        ok = false;
                    }
                }
            } else {
                LauncherUpdateLog("url dl: request failed " + std::to_string(GetLastError()) +
                    " url=" + url);
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    if (!ok) {
        std::error_code error;
        std::filesystem::remove(destination, error);
    }
    return ok;
}

std::string LauncherHttpGetUrl(const std::string& url, const bool use_system_proxy,
    int* status_code, const DWORD timeout_ms) {
    if (status_code != nullptr) *status_code = 0;
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE) {
        LauncherUpdateLog("url get: crack failed " + std::to_string(GetLastError()) +
            " url=" + url);
        return {};
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const INTERNET_PORT port = parts.nPort == 0 ? INTERNET_DEFAULT_HTTPS_PORT : parts.nPort;
    return LauncherHttpGetRequest(host, path, port, parts.nScheme == INTERNET_SCHEME_HTTPS,
        use_system_proxy, status_code, timeout_ms);
}

// "https://gamebanana.com/mods/735632" -> 735632
void DrawAttachMode(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionProcesses));
    // The status is the controller's own last message, which already carries every
    // distinction this needs: waiting for MaydayAirways.exe while the launch runs, then the
    // attached PID or the failure, coloured by its kind. It replaced a static "ready"
    // line that said the same thing no matter what the launcher was doing, and it
    // also restores the reporting the deleted footer used to provide.
    const bool show_message = snapshot.message_scope != MessageScope::Proxy;
    const ImVec4 status_ink = show_message && snapshot.message_kind == MessageKind::Error
        ? ThemeColor(theme.danger)
        : show_message && snapshot.message_kind == MessageKind::Success
            ? ThemeColor(theme.success)
            : LauncherMutedInk();
    const std::string process_id = std::to_string(snapshot.attached_process != 0
        ? snapshot.attached_process
        : snapshot.processes.empty() ? 0 : snapshot.processes.front().process_id);
    const std::string status = show_message ? RenderMessage(translator, snapshot.message)
        : !snapshot.core_available ? std::string(Text(translator, anomaly::MessageId::LauncherStatusCoreUnavailable))
        : snapshot.processes.empty() ? std::string(Text(translator, anomaly::MessageId::LauncherStatusNoProcesses))
        : Format(translator, snapshot.attached_process != 0
            ? anomaly::MessageId::LauncherStatusLaunchAttached
            : anomaly::MessageId::LauncherStatusProcessFound, {process_id});
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - StatusRefreshReserve());
    ImGui::TextColored(status_ink, "%s", status.c_str());
    ImGui::PopTextWrapPos();
    // The process summary needs a refresh of its own. Without one the launcher keeps
    // reporting the process it attached to after that process has exited, and the
    // stale list leaves the attach action disabled until the launcher is restarted.
    const float process_status_top =
        ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y;
    if (StatusRefreshButton(
            "refresh-processes",
            Text(translator, anomaly::MessageId::LauncherProcessRefresh),
            !snapshot.busy, process_status_top)) {
        controller.RefreshProcesses();
    }

    if (snapshot.proxy.proxy_enabled) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Text(translator, anomaly::MessageId::LauncherInjectProxyEnabled));
    }
    const bool can_launch = !snapshot.busy && !snapshot.proxy.proxy_enabled && snapshot.core_available &&
        !snapshot.game_directory.empty() && snapshot.processes.empty();
    // A game that is already running is the other half of the same choice: the launch action needs an
    // empty process list, this one needs a target in it.
    DWORD inject_target{};
    for (const auto& process : snapshot.processes) {
        if (process.Compatible()) {
            inject_target = process.process_id;
            break;
        }
    }
    const bool can_inject = !snapshot.busy && !snapshot.proxy.proxy_enabled && snapshot.core_available && inject_target != 0;
    // The same bottom-right placement and size as the proxy mode's primary action, so switching modes does not
    // move the button under the cursor -- but the widths are measured from the labels, because a fixed width
    // ellipsized the English "Inject into running process" down to "Inject into running ...".
    const char* inject_label = Text(translator, anomaly::MessageId::LauncherActionInjectRunning);
    const char* launch_label = Text(translator, anomaly::MessageId::LauncherLaunchAttach);
    const float inject_width = (std::max)(200.0f, CommandButtonWidth(0xe8e5, inject_label));
    const float launch_width = (std::max)(240.0f, CommandButtonWidth(0xe768, launch_label));
    ImGui::SetCursorPosY((std::max)(ImGui::GetCursorPosY() + Scale(20.0f),
        ImGui::GetWindowHeight() - Scale(kLaunchActionRowBottom)));
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
        ImGui::GetWindowWidth() - Scale(inject_width + 12.0f + launch_width) - Scale(16.0f)));
    if (CommandButton(
            "inject-running-process", 0xe8e5, inject_label, false,
            can_inject, ImVec2(inject_width, 62.0f))) {
        controller.InjectIntoRunningProcess(inject_target);
    }
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (CommandButton(
            "launch-attach-core", 0xe768, launch_label, true, can_launch,
            ImVec2(launch_width, 62.0f))) {
        controller.LaunchAndAttach();
    }
}

// The two controls in this window that leave the machine: they hand the address to the shell, which
// opens it in the user's browser, rather than reimplementing a payment flow in a game launcher.
void DrawLaunchMode(
    HWND window, LauncherController& controller,
    const LauncherSnapshot& snapshot, const anomaly::Translator& translator) {
    static int launch_mode = 0;

    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionGameDirectory));
    DrawReadOnlyPath("##GameDirectory", snapshot.game_directory, 38.0f, translator);
    ImGui::SameLine();
    if (IconButton("choose-game-directory", 0xe838,
            Text(translator, anomaly::MessageId::LauncherChooseGameDirectory), !snapshot.busy)) {
        if (const auto selected = ChooseGameDirectory(window, snapshot.game_directory, translator)) {
            controller.SelectGameDirectory(*selected);
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const std::array<const char*, 2> labels{
        Text(translator, anomaly::MessageId::LauncherModeLiveAttach),
        Text(translator, anomaly::MessageId::LauncherModeProxyInstall)};
    anomaly::glass::SegmentedOptions options;
    options.height = 54.0f;
    options.padding_x = 38.0f;
    options.scale = g_launcher_dpi_scale;
    anomaly::glass::SegmentedControl("launch-mode", labels, launch_mode, options);
    ImGui::Spacing();
    if (ImGui::BeginChild("LaunchModeContent", ImVec2(0.0f, 0.0f))) {
        if (launch_mode == 0) DrawAttachMode(controller, snapshot, translator);
        else DrawProxyMode(controller, snapshot, translator);
    }
    ImGui::EndChild();
}

// The settings page holds what belongs to the launcher window itself rather than to
// one of the install modes above: the theme decides how all four of them look, and the
// hotkey is the same key whichever page is showing. Both were written straight into
// the launcher configuration, so the page needs no apply step.
// One picture for the launcher's own theme. It is copied next to the runtime's own artwork and
// stored as a path relative to that root, so the theme resolver reads it exactly like the pictures
// the theme files name and the configuration survives the launcher folder being moved.
std::optional<std::string> ChooseLauncherThemeImage(
    HWND owner, const anomaly::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    }
    const COMDLG_FILTERSPEC filters[]{{L"Images", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp"}};
    dialog->SetFileTypes(1, filters);
    const std::wstring title = Utf8Wide(
        translator.Text(anomaly::MessageId::SettingsBackgroundOpen));
    dialog->SetTitle(title.c_str());
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path source(raw);
    CoTaskMemFree(raw);
    const auto directory = ExecutableDirectory() / L"Anomaly" / L"assets" / L"theme";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return std::nullopt;
    const auto target = directory / source.filename();
    bool same{};
    if (std::filesystem::exists(target, error))
        same = std::filesystem::equivalent(source, target, error) && !error;
    if (!same) {
        std::filesystem::copy_file(source, target,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) return std::nullopt;
    }
    return LauncherNarrow(std::filesystem::path(L"assets") / L"theme" / source.filename());
}

// The release page in the user's browser: the one place that explains a version in full, and where
// the dialog sends anyone the lookup itself could not help.
void LauncherOpenReleasePage(const LauncherUpdateStatus& update) {
    const std::wstring url = Utf8Wide(update.page_url.empty()
        ? std::string(anomaly::launcher::kReleasePageUrl) : update.page_url);
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The failure a check or an install reported, worded by the launcher where it can be. A rate-limited
// API, an unreachable host and an answer that is not a release are different problems with
// different answers, and the installer's own diagnostics are passed through as they were produced.
std::string UpdateFailureText(
    const anomaly::Translator& translator, const LauncherUpdateStatus& update) {
    const std::string status = std::to_string(update.http_status);
    switch (update.failure) {
    case LauncherUpdateFailure::Network:
        return Text(translator, anomaly::MessageId::LauncherUpdateNetworkFailed);
    case LauncherUpdateFailure::RateLimited:
        return Format(translator, anomaly::MessageId::LauncherUpdateRateLimited, {status});
    case LauncherUpdateFailure::Http:
        return Format(translator, anomaly::MessageId::LauncherUpdateHttpFailed, {status});
    case LauncherUpdateFailure::NotRelease:
        return Format(translator, anomaly::MessageId::LauncherUpdateNotRelease, {status});
    case LauncherUpdateFailure::Check:
        return Format(translator, anomaly::MessageId::LauncherUpdateCheckFailed, {update.detail});
    case LauncherUpdateFailure::Install:
        return Format(translator, anomaly::MessageId::LauncherUpdateFailed, {update.detail});
    case LauncherUpdateFailure::None:
        break;
    }
    return Format(translator, anomaly::MessageId::LauncherUpdateCheckFailed, {update.detail});
}

// The update's own byte counters and cancellation control.
// A transfer without a reported length shows received bytes instead of a percentage.
void LauncherDrawDownloadProgress(const anomaly::Translator& translator) {
    const double done = static_cast<double>(g_update_download.bytes.load());
    const double total = static_cast<double>(g_update_download.total.load());
    // Seconds since this download started: the bar only appears while one runs, so a static clock
    // reset when the transfer begins is all the elapsed time it needs.
    static double started{};
    static long long last_seen{};
    static bool running{};
    if (!running || done < last_seen) {
        started = ImGui::GetTime();
        running = true;
    }
    last_seen = static_cast<long long>(done);
    const double seconds = ImGui::GetTime() - started;
    const double megabytes = done / (1024.0 * 1024.0);
    if (total > 0.0) {
        const std::string overlay = Format(translator,
            anomaly::MessageId::LauncherUpdateProgressPercent,
            {Num(megabytes, 1), Num(total / (1024.0 * 1024.0), 1),
                Num(done * 100.0 / total, 0), Num(seconds, 0)});
        ImGui::ProgressBar(static_cast<float>(done / total), ImVec2(Scale(260.0f), 0.0f),
            overlay.c_str());
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Format(translator,
            anomaly::MessageId::LauncherUpdateProgressBytes,
            {Num(megabytes, 1), Num(seconds, 0)}).c_str());
    }
    ImGui::SameLine();
    // The one command that makes sense beside a running transfer.
    if (LauncherChip((Tr(anomaly::MessageId::CommonCancel) +
            "##cancel-update-download").c_str())) {
        g_update_download.cancel.store(true);
    }
}

// The state of the running update, as both the page and the dialog report it.
std::string UpdatePhaseText(
    const anomaly::Translator& translator, const LauncherUpdateStatus& update) {
    switch (update.phase) {
    case LauncherUpdatePhase::Idle:
        return {};
    case LauncherUpdatePhase::Checking:
        return Text(translator, anomaly::MessageId::LauncherUpdateChecking);
    case LauncherUpdatePhase::UpToDate:
        return Format(translator, anomaly::MessageId::LauncherUpdateUpToDate,
            {update.remote_version});
    case LauncherUpdatePhase::Available:
        return Format(translator, anomaly::MessageId::LauncherUpdateAvailable,
            {update.remote_version});
    case LauncherUpdatePhase::Downloading:
        return Text(translator, anomaly::MessageId::LauncherUpdateDownloading);
    case LauncherUpdatePhase::Installing:
        return Text(translator, anomaly::MessageId::LauncherUpdateInstalling);
    case LauncherUpdatePhase::Installed:
        return Text(translator, anomaly::MessageId::LauncherUpdateInstalled);
    case LauncherUpdatePhase::Failed:
        return UpdateFailureText(translator, update);
    }
    return {};
}

ImVec4 UpdatePhaseInk(const LauncherUpdatePhase phase) {
    const auto& theme = ue5mem::PlatformUiTheme();
    switch (phase) {
    case LauncherUpdatePhase::Available: return ThemeColor(theme.warning);
    case LauncherUpdatePhase::Installed: return ThemeColor(theme.success);
    case LauncherUpdatePhase::Failed: return ThemeColor(theme.danger);
    default: return LauncherMutedInk();
    }
}

// The launcher's own updates: the version this build is, the newest release it found, and the two
// commands that follow from them. The update replaces the files of this folder and restarts the
// window, so the button that starts it names the version it installs.
void DrawUpdateSettings(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const LauncherUpdateStatus& update = snapshot.update;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionUpdates));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingAutoUpdate));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(70.0f) - Scale(10.0f));
    bool auto_check = update.auto_check;
    if (anomaly::glass::Switch("##launcher-auto-update", &auto_check)) {
        controller.SetAutoCheckUpdates(auto_check);
    }
    ImGui::TextDisabled("%s", Format(translator, anomaly::MessageId::LauncherUpdateCurrent,
        {ANOMALY_SDK_VERSION_STRING}).c_str());
    const bool busy = snapshot.busy || update.Busy();
    if (LauncherChip(Text(translator, update.phase == LauncherUpdatePhase::Checking
            ? anomaly::MessageId::LauncherUpdateChecking
            : anomaly::MessageId::LauncherUpdateCheck), {}, !busy)) {
        controller.CheckForUpdates();
    }
    // The update command exists only while there is a release to install, so the row never offers
    // something the launcher has no version for.
    if (update.CanInstall()) {
        ImGui::SameLine();
        if (LauncherChip(Format(translator, anomaly::MessageId::LauncherUpdateInstall,
                {update.remote_version}).c_str(), {}, !busy)) {
            controller.InstallUpdate();
        }
    }
    ImGui::SameLine();
    // The release page is the only place that says what a new version changes, and it is where a
    // user goes when they would rather read about it before installing.
    if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateOpenPage))) {
        LauncherOpenReleasePage(update);
    }
    const std::string status = UpdatePhaseText(translator, update);
    if (update.phase == LauncherUpdatePhase::Downloading) {
        LauncherDrawDownloadProgress(translator);
    } else if (!status.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(UpdatePhaseInk(update.phase), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
}

// The release notes are markdown, and the shell already owns the parser for the markdown it draws
// (the first-run notice is authored in it). The launcher reuses that parser and draws the blocks
// itself, in its own ink: the same document, rendered the way the rest of this window is.
//
// Emphasis is faux bold -- the run drawn twice -- because the launcher bakes one font weight, and a
// run that wrapped is not doubled: the second draw would land on the line above it.
void LauncherDrawEmphasis(const std::string& text) {
    ImGui::TextUnformatted(text.c_str());
    if (ImGui::GetItemRectSize().y > ImGui::GetTextLineHeight() * 1.6f) return;
    const ImVec2 origin = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(origin.x + (std::max)(0.5f, Scale(0.5f)), origin.y),
        ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
}

// One line of a block: the runs stay in a single sentence, so emphasis and links keep their place.
void LauncherDrawNoticeRuns(const std::vector<anomaly::NoticeRun>& runs) {
    const auto& theme = ue5mem::PlatformUiTheme();
    ImGui::PushTextWrapPos();
    for (std::size_t index = 0; index < runs.size(); ++index) {
        if (index > 0) ImGui::SameLine(0.0f, 0.0f);
        const anomaly::NoticeRun& run = runs[index];
        switch (run.style) {
        case anomaly::NoticeRunStyle::Text:
            ImGui::TextUnformatted(run.text.c_str());
            break;
        case anomaly::NoticeRunStyle::Emphasis:
            LauncherDrawEmphasis(run.text);
            break;
        case anomaly::NoticeRunStyle::Code:
            ImGui::TextColored(LauncherMutedInk(), "%s", run.text.c_str());
            break;
        case anomaly::NoticeRunStyle::Link:
            ImGui::TextColored(ThemeColor(theme.accent), "%s", run.text.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", run.url.c_str());
            }
            // A link in a release note is the one thing here that leaves the machine: it goes to the
            // shell like every other address this window opens.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !run.url.empty()) {
                const std::wstring url = Utf8Wide(run.url);
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
    }
    ImGui::PopTextWrapPos();
}

// A heading: the same text a little larger and painted twice a hair apart, which is the whole of the
// emphasis this window has -- it bakes one font weight, so "bold" is the glyphs drawn again and
// "larger" is the atlas drawn at a bigger size. A heading that would not fit the pane keeps the body
// size and wraps like any other line, because a clipped heading is worse than a plain one.
void LauncherDrawHeading(const std::vector<anomaly::NoticeRun>& runs, const bool muted) {
    std::string text;
    for (const anomaly::NoticeRun& run : runs) text += run.text;
    if (text.empty()) return;
    ImFont* const font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 1.18f;
    const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const ImVec4 ink = muted ? LauncherMutedInk() : LauncherInk();
    if (measured.x > ImGui::GetContentRegionAvail().x) {
        ImGui::TextWrapped("%s", text.c_str());
        return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ink);
    const float bold = (std::max)(0.6f, Scale(0.6f));
    draw->AddText(font, size, origin, color, text.c_str());
    draw->AddText(font, size, ImVec2(origin.x + bold, origin.y), color, text.c_str());
    ImGui::Dummy(measured);
}

void LauncherDrawReleaseNotes(const anomaly::NoticeDocument& document) {
    for (std::size_t index = 0; index < document.blocks.size(); ++index) {
        const anomaly::NoticeBlock& block = document.blocks[index];
        switch (block.kind) {
        case anomaly::NoticeBlockKind::Heading:
        case anomaly::NoticeBlockKind::Subheading: {
            // The release opens with its own name and date, which is context rather than a section;
            // every heading is the same emphasis, only the first is quieter.
            LauncherDrawHeading(block.runs, index == 0);
            ImGui::Spacing();
            break;
        }
        case anomaly::NoticeBlockKind::Paragraph:
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case anomaly::NoticeBlockKind::Bullet:
            ImGui::Bullet();
            ImGui::SameLine();
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case anomaly::NoticeBlockKind::Numbered:
            ImGui::TextUnformatted(block.marker.c_str());
            ImGui::SameLine();
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case anomaly::NoticeBlockKind::Separator:
            ImGui::Separator();
            break;
        }
    }
}

// The parsed notes, kept across frames: the document only changes when the release does, and parsing
// a few kilobytes of markdown every frame for a dialog that is usually closed is wasted work.
// git-cliff marks a change's scope with single asterisks -- "*(autotaxi)* Add ..." -- which is not
// emphasis in the shell's markdown subset, so the markers would reach the screen. Keeping "**" (real
// bold) and the asterisk a bullet starts with, and dropping every other one, leaves the scope text
// readable and the bold intact.
std::string LauncherNotesMarkdown(const std::string& notes) {
    std::string cleaned;
    cleaned.reserve(notes.size());
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const char character = notes[index];
        if (character != '*') {
            cleaned.push_back(character);
            continue;
        }
        const bool line_start = index == 0 || notes[index - 1] == '\n' ||
            (notes[index - 1] == ' ' && index > 1 && notes[index - 2] == '\n');
        if (line_start) {
            cleaned.push_back(character);
            continue;
        }
        if (index + 1 < notes.size() && notes[index + 1] == '*') {
            cleaned.append("**");
            ++index;
        }
    }
    return cleaned;
}

const anomaly::NoticeDocument* LauncherReleaseNotes(const std::string& version,
    const std::string& notes) {
    static std::string parsed_for;
    static anomaly::NoticeDocument document;
    const std::string key = version + ':' + std::to_string(notes.size());
    if (key != parsed_for) {
        document = anomaly::ParseNoticeMarkdown(LauncherNotesMarkdown(notes), 0);
        parsed_for = key;
    }
    return &document;
}

// The prompt an update check ends in: the release's own notes in a pane, and two answers below --
// skipping that version on the left, updating on the right. Nothing is installed until the right one
// is pressed, which is the difference between an update and a silent one.
void DrawUpdatePrompt(
    LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const LauncherUpdateStatus& update = snapshot.update;
    // The dialog's name is its id, and nothing else: its title is drawn below, four pixels lower than
    // the title bar would have put it, so the line under it keeps the place it has always had. The two
    // strings still have to be the same one -- OpenPopup and BeginPopupModal pair on this id.
    const std::string title = Format(translator, anomaly::MessageId::LauncherUpdateAvailable,
        {update.remote_version});
    const std::string name = "##update-available";
    // One question per release, and none for a release the user already skipped: a version answered
    // once is not asked about again, this session or a later one. The band's chip asks for the same
    // dialog explicitly, which is why that path opens it even when this session already saw the
    // automatic one.
    static std::string prompted_version;
    const bool asked = std::exchange(g_update_prompt_requested, false);
    if (update.phase == LauncherUpdatePhase::Available && !update.remote_version.empty() &&
        (asked || (update.skipped_version != update.remote_version &&
            prompted_version != update.remote_version))) {
        prompted_version = update.remote_version;
        ImGui::OpenPopup(name.c_str());
    }
    // The popup's padding: its left edge is the inset the title bar uses (FramePadding.x), so the
    // first line of content sits exactly under the title instead of a few pixels left of it, and
    // the bottom of it gives the last row the same room the top gives the first.
    const ImGuiStyle& dialog_style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(dialog_style.FramePadding.x, Scale(2.0f)));
    // Centred on the launcher's canvas -- not on the OS window, which reaches up into the transparent
    // band above it -- and held there: a question is not a window the user should have to put back.
    const ImVec2 canvas_min = ImGui::GetWindowPos();
    const ImVec2 canvas_size = ImGui::GetWindowSize();
    ImGui::SetNextWindowPos(
        ImVec2(canvas_min.x + canvas_size.x * 0.5f, canvas_min.y + canvas_size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Sized by its contents, like the mod settings dialog, so it grows with the language it is drawn
    // in rather than clipping a localized label.
    if (!ImGui::BeginPopupModal(name.c_str(), nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize)) {
        ImGui::PopStyleVar();
        return;
    }
    // The title bar itself draws nothing: its text is put here instead, a few pixels lower than the
    // bar would have placed it, so the line below keeps the position it has always had. The clip is
    // widened for it -- a window clips its items to the content area, and the title bar is above that
    // -- or the text would be drawn and never seen.
    {
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 corner(origin.x + ImGui::GetWindowSize().x,
            origin.y + ImGui::GetFontSize() + dialog_style.FramePadding.y * 2.0f);
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, corner, false);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
            ImVec2(origin.x + dialog_style.FramePadding.x,
                origin.y + dialog_style.FramePadding.y + Scale(4.0f)),
            ImGui::GetColorU32(LauncherInk()), title.c_str());
        draw->PopClipRect();
    }
    // The version being left behind, under the title that names the one being installed.
    ImGui::TextDisabled("%s", Format(translator, anomaly::MessageId::LauncherUpdateCurrent,
        {ANOMALY_SDK_VERSION_STRING}).c_str());
    ImGui::Spacing();

    // The notes sit in a pane of their own, on the same material as the page's cards but quieter: it
    // is a recess inside the dialog, not another object floating over it.
    const float pane_width = Scale(760.0f);
    const bool has_notes = !update.notes.empty();
    if (has_notes) {
        const float pane_height = Scale(380.0f);
        const ImVec2 pane_min = ImGui::GetCursorScreenPos();
        const ImVec2 pane_max(pane_min.x + pane_width, pane_min.y + pane_height);
        // A plain recess, not a second pane of glass: the material belongs to the surface a window is
        // cut from, and another layer of it inside the first only muddies both. A tinted fill with a
        // hairline is what a panel inside a dialog reads as, and it keeps the notes legible.
        const auto& theme = ue5mem::PlatformUiTheme();
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pane_min, pane_max,
            ImGui::GetColorU32(ThemeColorWithAlpha(theme.child_background, 0.72f)),
            Scale(12.0f));
        draw->AddRect(pane_min, pane_max,
            ImGui::GetColorU32(ThemeColorWithAlpha(theme.border, 0.55f)),
            Scale(12.0f), ImDrawFlags_None, Scale(1.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        // A child without a border ignores style.WindowPadding unless it is asked to use it, which is
        // how the notes ended up written against the pane's edges. AlwaysUseWindowPadding gives the
        // text the inset it needs, and the wrap position follows that inset instead of the window edge.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(18.0f, 14.0f));
        ImGui::BeginChild("##update-notes", ImVec2(pane_width, pane_height),
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
        // The release publishes markdown; the shell's own parser turns it into blocks, and the blocks
        // are drawn here in this window's ink rather than dumped as a wall of hashes and asterisks.
        LauncherDrawReleaseNotes(*LauncherReleaseNotes(update.remote_version, update.notes));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    const bool busy = snapshot.busy || update.Busy();
    const std::string status = UpdatePhaseText(translator, update);
    if (update.phase == LauncherUpdatePhase::Downloading) {
        LauncherDrawDownloadProgress(translator);
    } else if (!status.empty() && update.phase != LauncherUpdatePhase::Available) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(UpdatePhaseInk(update.phase), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    // The two answers are centred as one pair with a gap between them: a dialog asks a question, and
    // its answers read as the pair that answers it rather than as two corners of the window. The update
    // is the launcher's primary command -- accent inside the glass, with the download mark -- so the
    // pair also carries the hierarchy.
    const float row_height = ButtonHeight(44.0f);
    const char* const skip_label = Text(translator, anomaly::MessageId::LauncherUpdateSkip);
    const std::string update_label = Format(translator, anomaly::MessageId::LauncherUpdateInstall,
        {update.remote_version});
    // Both widths are measured the way the two controls size themselves -- the chip from its caption,
    // the command from its mark and label -- so the pair stays centred in every language.
    const float skip_width = ImGui::CalcTextSize(skip_label).x + Scale(30.0f);
    const float update_width = ImGui::CalcTextSize(Glyph(0xe8b7)).x +
        ImGui::CalcTextSize(update_label.c_str()).x + Scale(46.0f);
    float pair_width = skip_width + update_width + Scale(16.0f);
    if (!has_notes) {
        // A release without notes has nothing to read here, so the page that does have something to
        // read joins the pair instead of leaving an empty pane on screen.
        pair_width += ImGui::CalcTextSize(
            Text(translator, anomaly::MessageId::LauncherUpdateOpenPage)).x + Scale(46.0f);
    }
    const float pair_left = ImGui::GetCursorPosX() +
        (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - pair_width) * 0.5f);
    ImGui::SetCursorPosX(pair_left);
    ImGui::BeginDisabled(busy);
    if (LauncherChip(skip_label, ImVec2(skip_width, row_height))) {
        controller.SkipUpdateVersion(update.remote_version);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (!has_notes) {
        ImGui::SameLine(0.0f, Scale(8.0f));
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateOpenPage),
                ImVec2(0.0f, row_height))) {
            LauncherOpenReleasePage(update);
        }
    }
    ImGui::SameLine(0.0f, Scale(16.0f));
    ImGui::BeginDisabled(busy);
    if (CommandButton("update-install", 0xe8b7, update_label.c_str(), true, true,
            ImVec2(update_width / g_launcher_dpi_scale, 44.0f))) {
        controller.InstallUpdate();
    }
    ImGui::EndDisabled();
    // The package is downloaded from github.com, which a network behind the wrong wall may not reach
    // at all. These are the same release published where it can be fetched, offered next to the
    // launcher's own download instead of in place of it. No extra spacing above them: the line after
    // the answers already carries the row spacing, and a Spacing here doubled it.
    {
        const float quark_width = ImGui::CalcTextSize(
            Text(translator, anomaly::MessageId::LauncherUpdateQuark)).x + Scale(30.0f);
        const float baidu_width = ImGui::CalcTextSize(
            Text(translator, anomaly::MessageId::LauncherUpdateBaidu)).x + Scale(30.0f);
        const float links_width = quark_width + baidu_width + Scale(8.0f);
        const float links_left = ImGui::GetCursorPosX() +
            (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - links_width) * 0.5f);
        ImGui::SetCursorPosX(links_left);
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateQuark),
                ImVec2(quark_width, row_height))) {
            const std::wstring url(anomaly::launcher::kReleaseQuarkUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0.0f, Scale(8.0f));
        if (LauncherChip(Text(translator, anomaly::MessageId::LauncherUpdateBaidu),
                ImVec2(baidu_width, row_height))) {
            const std::wstring url(anomaly::launcher::kReleaseBaiduUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    // The padding no longer gives the bottom the room the top does not need, so the last row keeps a
    // reserve of its own instead of sitting on the window's edge.
    ImGui::Dummy(ImVec2(0.0f, Scale(10.0f)));
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

// The about section closes the page: the project's own two addresses -- where its source lives, and
// where the people using it gather -- side by side under one heading. Neither is a page this window
// draws; both are handed to the shell, the way every other link here leaves the machine.
void DrawAboutSettings(const anomaly::Translator& translator) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionAbout));
    struct Address final {
        anomaly::MessageId label;
        const char* id;         // names the chip; the drawn caption is the label
        std::string_view url;   // data, not copy -- a translated address leads nowhere
    };
    constexpr std::array<Address, 2> addresses{{
        {anomaly::MessageId::LauncherAboutGithub, "##about-github", kRepositoryUrl},
        {anomaly::MessageId::LauncherAboutDiscord, "##about-discord", kDiscordUrl},
    }};
    const float row_height = ButtonHeight(40.0f);
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        const Address& address = addresses[index];
        const char* const label = Text(translator, address.label);
        // The capsule is measured from its own caption rather than from a constant: the two names are
        // the platforms' and are spelled the same way in both locales, but the padding around them
        // belongs to this window and follows its scale.
        const float width = ImGui::CalcTextSize(label).x + Scale(40.0f);
        if (index > 0) ImGui::SameLine(0.0f, Scale(12.0f));
        const std::string chip = std::string(label) + address.id;
        if (LauncherChip(chip.c_str(), ImVec2(width, row_height))) {
            const std::wstring url = Utf8Wide(address.url);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        // The address itself, for the user who would rather copy it than follow it.
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%.*s", static_cast<int>(address.url.size()), address.url.data());
        }
    }
}

void DrawSettingsMode(
    HWND window, LauncherController& controller, const LauncherSnapshot& snapshot,
    const anomaly::Translator& translator) {
    const auto& theme = ue5mem::PlatformUiTheme();
    // The page may outgrow the body -- theme, language, the mods page options and the startup
    // settings together are taller than the canvas on a narrow window -- so it scrolls on its
    // own, and its scrollbar appears only when it actually needs one.
    ImGui::BeginChild("SettingsPage", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    ImGui::TextDisabled("%s",
        Text(translator, anomaly::MessageId::LauncherSectionAppearance));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingTheme));
    // Naiwa leads the row because it is the shipped default and the only preset with a
    // wallpaper of its own, so the segment a fresh launcher opens on is the first one.
    // AnomalyHub is the shell's own dark palette and closes the row, the same order the
    // in-game settings lists them in.
    constexpr std::array<anomaly::PlatformUiPalette, 7> palettes{
        anomaly::PlatformUiPalette::Naiwa,
        anomaly::PlatformUiPalette::Moss,
        anomaly::PlatformUiPalette::Aurora,
        anomaly::PlatformUiPalette::Ember,
        anomaly::PlatformUiPalette::Paper,
        anomaly::PlatformUiPalette::AnomalyHub,
        anomaly::PlatformUiPalette::Custom,
    };
    // The palette names are the in-game settings' own labels, already localized. The hub
    // palette is a product name in both locales there, and is spelled the same way here.
    const std::array<const char*, palettes.size()> theme_labels{
        Text(translator, anomaly::MessageId::SettingsPaletteNaiwa),
        Text(translator, anomaly::MessageId::SettingsPaletteMoss),
        Text(translator, anomaly::MessageId::SettingsPaletteAurora),
        Text(translator, anomaly::MessageId::SettingsPaletteEmber),
        Text(translator, anomaly::MessageId::SettingsPalettePaper),
        "AnomalyHub",
        Text(translator, anomaly::MessageId::SettingsPaletteCustom)};
    const anomaly::PlatformUiPalette current = snapshot.theme != nullptr
        ? snapshot.theme->palette : anomaly::PlatformUiPalette::Naiwa;
    int selected{};
    for (std::size_t index = 0; index < palettes.size(); ++index) {
        if (palettes[index] == current) {
            selected = static_cast<int>(index);
            break;
        }
    }
    anomaly::glass::SegmentedOptions theme_options;
    theme_options.height = 46.0f;
    theme_options.padding_x = 26.0f;
    theme_options.scale = g_launcher_dpi_scale;
    theme_options.disabled_mask = snapshot.busy ? 0x7fU : 0U;
    if (anomaly::glass::SegmentedControl(
            "launcher-theme", theme_labels, selected, theme_options)) {
        controller.SetLauncherTheme(palettes[static_cast<std::size_t>(selected)]);
    }

    // A palette of this window's own: the same five colours the in-game settings collect, stored with
    // the launcher configuration and applied on the frame they change, so dragging one repaints the
    // window instead of waiting for a reload.
    if (current == anomaly::PlatformUiPalette::Custom) {
        anomaly::PlatformUiCustomColors colors = snapshot.custom_colors;
        bool changed{};
        const auto color_row = [&](const char* id, const anomaly::MessageId label,
                                   anomaly::PlatformUiColor& color) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Text(translator, label));
            ImGui::SameLine(Scale(240.0f));
            ImGui::SetNextItemWidth(Scale(180.0f));
            std::array channels{color.red, color.green, color.blue};
            if (ImGui::ColorEdit3(id, channels.data())) {
                color = {channels[0], channels[1], channels[2], 1.0f};
                changed = true;
            }
        };
        color_row("##custom-accent", anomaly::MessageId::SettingsCustomAccent, colors.accent);
        color_row("##custom-text", anomaly::MessageId::SettingsCustomText, colors.text);
        color_row("##custom-surface", anomaly::MessageId::SettingsCustomWindowBackground,
            colors.window_background);
        color_row("##custom-child", anomaly::MessageId::SettingsCustomChildBackground,
            colors.child_background);
        color_row("##custom-border", anomaly::MessageId::SettingsCustomBorder, colors.border);
        if (changed) controller.SetLauncherCustomColors(colors);
    }

    // The custom background, laid out the way the in-game settings lay theirs out: the label in the
    // section's label column, then the enable switch, the picture picker and the fill mode on that
    // row, and the veil over it on the next. Stickers are not part of this -- the theme layout
    // editor authors those for both windows.
    constexpr int background_disabled =
        static_cast<int>(anomaly::PlatformUiBackgroundMode::Disabled);
    constexpr int background_crop = static_cast<int>(anomaly::PlatformUiBackgroundMode::Crop);
    const int background_mode = snapshot.theme != nullptr
        ? static_cast<int>(snapshot.theme->layout.background.mode) : background_disabled;
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::SettingsBackgroundImage));
    // The switch, the picker and the fill mode are one group, right-aligned on the section's edge, and
    // every slider below spans exactly its width. The group's extent is measured rather than guessed
    // -- the picker's label is localized, so its width is not a constant -- and the measurement is
    // reused next frame, which keeps the rows stable instead of reflowing on every label change.
    static float controls_width = Scale(380.0f);
    const float right_edge = ImGui::GetContentRegionMax().x;
    ImGui::SameLine(right_edge - controls_width - Scale(10.0f));
    const float controls_left = ImGui::GetCursorScreenPos().x;
    bool background_enabled = background_mode != background_disabled;
    if (anomaly::glass::Switch("##launcher-background-enabled", &background_enabled)) {
        controller.SetLauncherBackground(snapshot.artwork.background,
            background_enabled ? (background_mode == background_disabled
                ? background_crop : background_mode) : background_disabled);
    }
    ImGui::SameLine();
    if (LauncherChip((std::string(Text(translator, anomaly::MessageId::SettingsBackgroundOpen)) +
            "##launcher-background-open").c_str())) {
        if (const auto picked = ChooseLauncherThemeImage(window, translator)) {
            controller.SetLauncherBackground(*picked,
                background_mode == background_disabled ? background_crop : background_mode);
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Scale(200.0f));
    const std::array<const char*, 3> background_modes{
        Text(translator, anomaly::MessageId::SettingsBackgroundModeCenter),
        Text(translator, anomaly::MessageId::SettingsBackgroundModeCrop),
        Text(translator, anomaly::MessageId::SettingsBackgroundModeStretch)};
    int background_mode_index = std::clamp(background_mode, 1, 3) - 1;
    if (ImGui::Combo("##launcher-background-mode", &background_mode_index,
            background_modes.data(), static_cast<int>(background_modes.size()))) {
        controller.SetLauncherBackground(
            snapshot.artwork.background, background_mode_index + 1);
    }
    controls_width = ImGui::GetItemRectMax().x - controls_left;
    // The veil over that picture. The slider is the picture's transparency, not the veil's opacity:
    // 100% is a wallpaper you can see, 0% is the plain surface with nothing behind it. The layout
    // stores it the other way round (surfaceOpacity), so the value is flipped here.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::SettingsBackgroundVisibility));
    ImGui::SameLine(right_edge - controls_width - Scale(10.0f));
    // The veil's slider works the way the in-game one does: it shows a value the page holds, and the
    // snapshot only becomes the truth again once the reload it asked for has landed. Committing per
    // frame reloaded the theme (and decoded the wallpaper) under the pointer, and dropping the held
    // value on release showed the old value for a frame -- the bounce.
    static std::optional<float> held_opacity;
    static bool held_committed{true};
    static std::uint64_t held_version{};
    const float applied_opacity = snapshot.theme != nullptr
        ? snapshot.theme->layout.surface_opacity : 1.0f;
    const std::uint64_t theme_version = snapshot.theme != nullptr ? snapshot.theme->version : 0;
    if (held_opacity.has_value() && held_committed && theme_version != held_version) {
        held_opacity.reset();  // the reload reported the value; follow the snapshot again
    }
    int transparency = static_cast<int>(std::lround((1.0f - std::clamp(
        held_opacity.value_or(applied_opacity),
        anomaly::kPlatformUiLayoutSurfaceOpacityMinimum, 1.0f)) * 100.0f));
    ImGui::SetNextItemWidth(controls_width);
    if (anomaly::glass::SliderInt("##launcher-background-visible", &transparency, 0, 100, "%d%%")) {
        held_opacity = 1.0f - static_cast<float>(transparency) / 100.0f;
        held_committed = false;
    }
    // The release test the in-game settings use: the pointer has let go and the slider is no longer
    // active, which is also true when the drag ends outside the track.
    if (!held_committed && held_opacity.has_value() && !ImGui::IsItemActive() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        held_committed = true;
        held_version = theme_version;
        controller.SetLauncherSurfaceOpacity(*held_opacity);
    }

    // Language: the same three choices the in-game settings offer, but this one dresses the
    // launcher alone and applies on the next frame.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, anomaly::MessageId::LauncherSectionLanguage));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, anomaly::MessageId::LauncherSettingLanguage));
    constexpr std::array<anomaly::LanguagePreference, 3> languages{
        anomaly::LanguagePreference::Auto,
        anomaly::LanguagePreference::ZhCn,
        anomaly::LanguagePreference::EnUs,
    };
    const std::array<const char*, languages.size()> language_labels{
        Text(translator, anomaly::MessageId::SettingsAutomatic),
        Text(translator, anomaly::MessageId::SettingsSimplifiedChinese),
        Text(translator, anomaly::MessageId::SettingsEnglish)};
    const auto current_language =
        anomaly::ParseLanguagePreference(snapshot.language).preference;
    int language_index{};
    for (std::size_t index = 0; index < languages.size(); ++index) {
        if (languages[index] == current_language) {
            language_index = static_cast<int>(index);
            break;
        }
    }
    anomaly::glass::SegmentedOptions language_options;
    language_options.height = 46.0f;
    language_options.padding_x = 26.0f;
    language_options.scale = g_launcher_dpi_scale;
    if (anomaly::glass::SegmentedControl(
            "launcher-language", language_labels, language_index, language_options)) {
        controller.SetLanguage(languages[static_cast<std::size_t>(language_index)]);
    }

    DrawStartupSettings(controller, snapshot, translator);
    DrawUpdateSettings(controller, snapshot, translator);
    DrawAboutSettings(translator);

    // The page reports the outcome of its own writes. Everything it can change is saved
    // with the shared scope, so a failure is the only thing that has to be said out
    // loud here; the success line is what tells the user the theme was stored rather
    // than previewed.
    if (snapshot.message_kind != MessageKind::Neutral &&
        snapshot.message_scope == MessageScope::Both) {
        const ImVec4 settings_ink = snapshot.message_kind == MessageKind::Error
            ? ThemeColor(theme.danger)
            : ThemeColor(theme.success);
        const std::string settings_status = RenderMessage(translator, snapshot.message);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(settings_ink, "%s", settings_status.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

// The native caption is gone, so the launcher draws its own: a drag band holding
// the window title, then the minimise and close controls the frame used to own.
// Nothing here paints a background, so the wallpaper and the stickers show
// through the caption exactly as they do behind the rest of the window.
void DrawLauncherTitleBar(Graphics& graphics) {
    const auto& theme = ue5mem::PlatformUiTheme();
    const float height = Scale(64.0f);
    // The marks are narrower than the caption is tall. At the full height the two of
    // them sat a whole caption apart, which read as two unrelated buttons rather than
    // as one pair.
    const float button_width = Scale(44.0f);
    // Everything in the caption sits a couple of pixels below true centre: with a
    // mark this large, an exactly centred band reads as if it is crowding the top
    // edge.
    const float nudge = Scale(3.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    // The app mark and its name belong to the caption now. They are drawn straight
    // onto the window list rather than laid out, so they occupy no space and the
    // drag band still runs the full width behind them.
    {
        ImDrawList* const caption = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float label_x = origin.x + Scale(12.0f);
        if (graphics.logo != nullptr) {
            const float mark = Scale(48.0f);
            caption->AddImage(
                static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(graphics.logo.Get())),
                ImVec2(label_x, origin.y + (height - mark) * 0.5f + nudge),
                ImVec2(label_x + mark, origin.y + (height + mark) * 0.5f + nudge));
            label_x += mark + Scale(12.0f);
        }
        // The caption draws its own size rather than the atlas default, which is
        // baked for body text and reads as too small next to a mark this size.
        const float title_size = Scale(28.0f);
        caption->AddText(ImGui::GetFont(), title_size,
            ImVec2(label_x, origin.y + (height - title_size) * 0.5f + nudge),
            ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.text)),
            "AnomalyLauncher");
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    // No frame at all: the caption floats over the wallpaper, and a box around a
    // button that only shows a glyph is visual noise.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(theme.text));
    // The two caption marks are invisible items, so they draw their own feedback. It is
    // the mark itself that changes -- it turns white under the pointer -- because a
    // filled rectangle behind it reads as a button in a bar that has no other buttons.
    const auto caption_ink = [&theme]() {
        return ImGui::ColorConvertFloat4ToU32(ImGui::IsItemHovered()
            ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
            : ThemeColor(theme.text));
    };
    // The pair sits a little in from the right edge instead of flush against it.
    const float caption_margin = Scale(10.0f);
    ImGui::InvisibleButton("##launcher-drag",
        ImVec2((std::max)(1.0f, width - button_width * 2.0f - caption_margin), height));
    // The window is moved by hand instead of handing the press to the system drag
    // loop. WM_NCLBUTTONDOWN runs a modal loop that swallows the matching release,
    // which leaves ImGui holding a pressed item forever and silently kills every
    // button in the window from then on.
    //
    // The maths is deliberately absolute rather than per-frame: accumulating
    // MouseDelta feeds the window's own movement back into the next delta, since
    // moving the window moves the cursor relative to it, and the result jitters.
    // The screen cursor and the window origin captured at drag start have no such
    // feedback.
    // Drag state kept between frames: the screen cursor and the window origin
    // captured when the drag started, so the position is absolute rather than
    // accumulated frame by frame.
    static bool drag_active = false;
    static POINT drag_cursor{};
    static POINT drag_window{};
    if (ImGui::IsItemActivated()) {
        drag_cursor = POINT{};
        drag_window = POINT{};
        if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
            if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                RECT rect{};
                drag_active = GetCursorPos(&drag_cursor) != FALSE
                    && GetWindowRect(handle, &rect) != FALSE;
                if (drag_active) {
                    drag_window.x = rect.left;
                    drag_window.y = rect.top;
                }
            }
        }
    }
    if (drag_active && ImGui::IsItemActive()) {
        POINT cursor{};
        if (GetCursorPos(&cursor) != FALSE) {
            if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
                if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                    static_cast<void>(SetWindowPos(handle, nullptr,
                        drag_window.x + (cursor.x - drag_cursor.x),
                        drag_window.y + (cursor.y - drag_cursor.y),
                        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
                }
            }
        }
    }
    if (!ImGui::IsItemActive()) drag_active = false;
    ImGui::SameLine();
    ImGui::InvisibleButton("##launcher-minimize", ImVec2(button_width, height));
    const ImVec2 minimize_min = ImGui::GetItemRectMin();
    const ImVec2 minimize_max = ImGui::GetItemRectMax();
    const ImVec2 minimize_center((minimize_min.x + minimize_max.x) * 0.5f,
        (minimize_min.y + minimize_max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(minimize_center.x - Scale(9.0f), minimize_center.y),
        ImVec2(minimize_center.x + Scale(9.0f), minimize_center.y),
        caption_ink(), Scale(2.4f));
    if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered()) {
        if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
            if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                static_cast<void>(ShowWindow(handle, SW_MINIMIZE));
            }
        }
    }
    ImGui::SameLine();
    ImGui::InvisibleButton("##launcher-close", ImVec2(button_width, height));
    const ImVec2 close_min = ImGui::GetItemRectMin();
    const ImVec2 close_max = ImGui::GetItemRectMax();
    const ImVec2 close_center((close_min.x + close_max.x) * 0.5f,
        (close_min.y + close_max.y) * 0.5f);
    const ImU32 close_ink = caption_ink();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(close_center.x - Scale(9.0f), close_center.y - Scale(9.0f)),
        ImVec2(close_center.x + Scale(9.0f), close_center.y + Scale(9.0f)),
        close_ink, Scale(2.4f));
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(close_center.x - Scale(9.0f), close_center.y + Scale(9.0f)),
        ImVec2(close_center.x + Scale(9.0f), close_center.y - Scale(9.0f)),
        close_ink, Scale(2.4f));
    if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered()) {
        if (ImGuiViewport* const viewport = ImGui::GetMainViewport()) {
            if (const HWND handle = static_cast<HWND>(viewport->PlatformHandleRaw)) {
                static_cast<void>(PostMessageW(handle, WM_CLOSE, 0, 0));
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

void DrawLauncher(
    HWND window, Graphics& graphics, LauncherController& controller, LauncherMode& mode) {
    // Runs every frame whatever page is showing: a game that starts after this window is up has to appear
    // in the list without the user pressing anything, and the poll throttles itself to one scan every couple
    // of seconds on the worker thread.
    controller.PollProcesses();
    // A preference the user changed is written once the worker is free, so the settings page
    // needs no save button and a slider drag costs one write.
    controller.FlushSettings();
    // The update check this launch owes, if it owes one: it runs on the worker and reports into
    // the settings page, so nothing pops up over the page the user is reading.
    controller.AutoCheckUpdates();
    const LauncherSnapshot snapshot = controller.Snapshot();
    // The update has replaced the image this window was drawn by and started the new one. This
    // window closes rather than outliving the file it was loaded from, and so that the two
    // launchers never write the same configuration at the same time.
    if (snapshot.restart_pending) {
        static bool close_posted{};
        if (!close_posted) {
            close_posted = true;
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
    // The language is applied here rather than at start-up only: switching it on the settings page
    // repaints this frame, and the window title follows. The reference below is taken after the
    // swap, so every page draws with the catalog that is in hand.
    {
        const auto wanted = anomaly::ParseLanguagePreference(snapshot.language).preference;
        if (wanted != g_translator_language &&
            LoadLauncherCatalog(wanted, ExecutableDirectory() / L"Anomaly" / L"locales" / L"host")) {
            SetWindowTextW(window,
                Utf8Wide(g_translator.load()->Text(anomaly::MessageId::LauncherWindowTitle)).c_str());
        }
    }
    const auto frame_translator = g_translator.load();
    const anomaly::Translator& translator = *frame_translator;
    ApplyLauncherTheme(snapshot.theme, snapshot.custom_colors);
    UploadLauncherTheme(graphics, snapshot.theme);
    const ImGuiIO& io = ImGui::GetIO();
    // The root window is the shell canvas, pushed down by the transparent band above it: the band is
    // window, not canvas, so a sticker drawn past the top edge lands there instead of being cut.
    const float band_top = Scale(kLauncherBandTop);
    ImGui::SetNextWindowPos(ImVec2(0.0f, band_top));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - band_top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(
        "AnomalyLauncherRoot", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    const ImVec2 root_origin = ImGui::GetWindowPos();
    const ImVec2 root_size = ImGui::GetWindowSize();
    DrawLauncherThemeImages(graphics, snapshot.theme, root_origin, root_size, false);
    anomaly::glass::CaptureBackdrop(ImGui::GetWindowDrawList());
    DrawLauncherTitleBar(graphics);
    DrawModes(controller, snapshot, mode, translator);
    // The body is one frosted card inset from the canvas edge, so the wallpaper
    // and the stickers stay visible around it and refract through it.
    {
        const ImVec2 card_min(root_origin.x + Scale(14.0f), ImGui::GetCursorScreenPos().y);
        const ImVec2 card_max(root_origin.x + root_size.x - Scale(14.0f),
            root_origin.y + root_size.y - Scale(14.0f));
        anomaly::glass::SurfaceOptions card;
        card.variant = anomaly::glass::Variant::Regular;
        card.rounding = Scale(20.0f);
        card.scale = g_launcher_dpi_scale;
        // The pane is the one surface that refracts the wallpaper, so it keeps the lens; its glow is
        // switched off, because a glow around a shape this large only reads as a box in the corner.
        card.shadow = false;
        anomaly::glass::DrawSurface(ImGui::GetWindowDrawList(), card_min, card_max, card);
        ImGui::SetCursorScreenPos(card_min);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(20.0f, 18.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "LauncherBody", ImVec2(root_size.x - Scale(28.0f), -Scale(14.0f)),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (mode == LauncherMode::Launch) {
        DrawLaunchMode(window, controller, snapshot, translator);
    } else {
        DrawSettingsMode(window, controller, snapshot, translator);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    DrawLauncherThemeImages(graphics, snapshot.theme, root_origin, root_size, true);
    // The update prompt is drawn last, on the root window: an update check runs in the background, so
    // the question it ends in has to reach the user on whatever page they are reading -- and drawing it
    // after the page keeps the dialog the last window created in the frame, which is what puts it in
    // front of the page's own child windows instead of behind them.
    DrawUpdatePrompt(controller, snapshot, translator);
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const auto administrator = EnsureAdministrator(command_line);
    if (!administrator.run_current_process) return administrator.exit_code;

    int argument_count{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    DWORD parent_process{};
    if (arguments != nullptr) {
        if (argument_count == 3 && std::wstring_view(arguments[1]) == L"--wait-for-update") {
            wchar_t* end{};
            parent_process = wcstoul(arguments[2], &end, 10);
            if (end == arguments[2] || *end != L'\0' || parent_process == GetCurrentProcessId()) {
                LocalFree(arguments);
                return ERROR_INVALID_PARAMETER;
            }
        }
        LocalFree(arguments);
    }
    if (parent_process != 0) {
        const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_process);
        if (parent != nullptr) {
            const DWORD waited = WaitForSingleObject(parent, 60000);
            CloseHandle(parent);
            if (waited != WAIT_OBJECT_0) return ERROR_TIMEOUT;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
            return static_cast<int>(GetLastError());
        }
    }

    // The release directory also contains the proxy payload named dwmapi.dll.
    // Pin the system module before ImGui's delay imports are resolved.
    const HMODULE system_dwmapi = LoadSystemDwmapi();
    if (system_dwmapi == nullptr) return static_cast<int>(GetLastError());

    const std::filesystem::path runtime_root = ExecutableDirectory() / L"Anomaly";
    // The launcher's own settings live in this ini: it is the file this process reads at start-up,
    // so a language or hotkey change written here is what the next launch sees.
    const std::filesystem::path settings_path = runtime_root / L"anomaly.ini";
    const std::filesystem::path locale_directory = runtime_root / L"locales" / L"host";
    const ue5mem::AnalyzerConfig config = ue5mem::AnalyzerConfig::Load(settings_path);
    if (!LoadLauncherCatalog(config.platform_language, locale_directory)) {
        return ERROR_RESOURCE_DATA_NOT_FOUND;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    WNDCLASSEXW window_class{
        .cbSize = sizeof(window_class),
        .style = CS_CLASSDC,
        .lpfnWndProc = WindowProc,
        .hInstance = instance,
        .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = L"AnomalyLauncherWindow",
        .hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
    };
    if (RegisterClassExW(&window_class) == 0) return 1;
    const std::wstring window_title = Utf8Wide(
        g_translator.load()->Text(anomaly::MessageId::LauncherWindowTitle));
    g_launcher_dpi_scale = DpiScale(GetDpiForSystem());
    // WS_OVERLAPPEDWINDOW cannot be used here: it carries a caption, and removing
    // the frame from the client area with WM_NCCALCSIZE does not stop the window
    // from owning one, so the native title bar comes straight back. A popup has no
    // caption to begin with; WS_EX_APPWINDOW keeps the taskbar entry that a plain
    // popup would lose, and the placement is computed because a popup ignores
    // CW_USEDEFAULT.
    // The window is the shell canvas plus the transparent band above it, where the launcher's own
    // sticker overhangs; the bands beside it are gone, so the width is the canvas width.
    const int window_width = static_cast<int>(Scale(kLauncherCanvasWidth));
    const int window_height = static_cast<int>(Scale(kLauncherCanvasHeight) +
        Scale(kLauncherBandTop));
    const int window_x = (GetSystemMetrics(SM_CXSCREEN) - window_width) / 2;
    const int window_y = (GetSystemMetrics(SM_CYSCREEN) - window_height) / 2;
    // WS_EX_NOREDIRECTIONBITMAP stops the window from painting a background of its
    // own, so the composition visual is the only thing that reaches the screen and
    // whatever the shell leaves untouched stays transparent, which is how the window's
    // rounded corners are made.
    const HWND window = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        window_class.lpszClassName, window_title.c_str(),
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        window_x, window_y, window_width, window_height,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(window_class.lpszClassName, instance);
        return 2;
    }

    Graphics graphics;
    g_graphics = &graphics;
    if (!CreateGraphics(window, graphics)) {
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 3;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // The launcher owns its whole surface, so it bakes only what it draws: one type
    // scale, its eight icons, an atlas no taller than the glyphs need, and none of the
    // symbol blocks plugin copy draws from. The host keeps its own ladder of sizes, the
    // whole icon range and those blocks for the in-game windows.
    static_cast<void>(ue5mem::ConfigurePlatformUiFontAtlas(runtime_root,
        ue5mem::PlatformUiFontBake{
            .scales = kLauncherFontScales, .icons = kLauncherIcons, .tight_atlas = true,
            .symbol_blocks = false}));
    g_launcher_dpi_scale = DpiScale(GetDpiForWindow(window));
    ApplyLauncherDpiScale();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAuto = false;
    io.ConfigNavEscapeClearFocusWindow = true;
    io.IniFilename = nullptr;
    if (!ImGui_ImplWin32_Init(window) ||
        !ImGui_ImplDX11_Init(graphics.device.Get(), graphics.context.Get())) {
        ImGui::DestroyContext();
        graphics = {};
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 4;
    }
    // Losing the glass renderer only costs the effect; every surface keeps a
    // solid fallback.
    // The launcher is a desktop window, not an overlay: its composition carries
    // real alpha, and its material stays neutral rather than palette-tinted.
    static_cast<void>(anomaly::glass::InitializeRenderer(
        graphics.device.Get(), graphics.context.Get(), false));
    // Upload the font atlas now rather than on the first frame, so what it costs
    // besides its texture can be released before the window is shown. The launcher
    // owns its device for its lifetime and never adds another font, so it keeps
    // neither the texture pixels nor the font files.
    static_cast<void>(ImGui_ImplDX11_CreateDeviceObjects());
    ue5mem::ReleasePlatformUiFontAtlas(ue5mem::PlatformUiFontRetention::Minimal);
    ApplyLauncherDpiScale();
    graphics.logo = LoadEmbeddedTexture(graphics, kLogoResourceId);
    LauncherController controller(ExecutableDirectory());
    LauncherMode mode = LauncherMode::Launch;

    bool running = true;
    bool window_shown = false;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (g_launcher_dpi_changed) ApplyLauncherDpiScale();
        if (IsIconic(window)) {
            Sleep(16);
            continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawLauncher(window, graphics, controller, mode);
        ImGui::Render();
        // What the shell leaves untouched has to be fully transparent -- and under premultiplied
        // alpha that means colour zero as well, or the rounded corners would composite as a tinted
        // margin.
        graphics.context->OMSetRenderTargets(1, graphics.render_target.GetAddressOf(), nullptr);
        const float transparent_clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
        graphics.context->ClearRenderTargetView(graphics.render_target.Get(), transparent_clear);
        anomaly::glass::PrepareDrawData(ImGui::GetDrawData());
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        graphics.swap_chain->Present(1, 0);
        if (!window_shown) {
            // The window stays hidden until a themed frame has been presented. It has no
            // redirection bitmap, so until the composition visual holds a frame the
            // desktop is looking at an empty window; and the theme arrives from a worker
            // thread, so the earliest frames are drawn against the palette's defaults.
            // Showing at that point is the flash that used to come before the launcher.
            // A theme that never arrives is not a reason to stay hidden, so a snapshot
            // without one shows the window too.
            const LauncherSnapshot frame = controller.Snapshot();
            if (frame.theme == nullptr || graphics.theme_version != 0) {
                window_shown = true;
                ShowWindow(window, SW_SHOWDEFAULT);
            }
        }
    }

    anomaly::glass::ShutdownRenderer();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_graphics = nullptr;
    graphics = {};
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
