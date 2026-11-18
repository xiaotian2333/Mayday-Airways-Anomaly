#pragma once

#include "anomaly/platform_ui_theme.hpp"

#include <Windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace anomaly::launcher {

struct LauncherClientConfiguration final {
    std::filesystem::path game_directory;
};

// The launcher's own background picture, named the way the theme files name theirs (relative to the
// Anomaly root). Stickers are the layout editor's job, as they are for the in-game window, so this
// holds the background and the two values that sit beside it: the fill mode and the veil drawn over
// it. The veil starts at zero -- the launcher paints its own body, and a veil on top of that body
// changes nothing anyone can see -- and a fill mode of -1 keeps whatever the layout says.
struct LauncherThemeArtwork final {
    std::string background;
    int background_mode{-1};
    float surface_opacity{0.0f};
};

// Classifies the effective, enabled background without reporting its path.
[[nodiscard]] bool IsCustomLauncherBackground(PlatformUiPalette palette,
    std::string_view background_path, bool enabled);

// Whether the launcher looks for a newer release on its own. It is the user's answer to the one
// question an update check asks -- whether this window may talk to the network unasked -- so it is
// stored with the launcher configuration rather than assumed, and a launcher that has never stored
// it checks: finding out that a build is outdated is the point of shipping the check at all.
// The endpoint is the mirror prefix that answered the last lookup (empty for the direct address).
// Asking it first is what makes a check take a second on a network where github.com itself is
// blocked, instead of spending the timeout budget on an address that is known not to answer here.
// The skipped version is the release the user answered "skip" to: the check may keep reporting it --
// the settings page still offers it -- but the launcher stops asking about that one version.
struct LauncherUpdateSettings final {
    bool auto_check{true};
    std::string endpoint;
    std::string skipped_version;
};

struct LauncherConfiguration final {
    LauncherClientConfiguration game;
    // The palette the launcher window wears, as the palette name the theme files use.
    // It is the launcher's own choice and is deliberately independent of the in-game
    // palette: the two windows are separate surfaces. An unknown or missing name keeps
    // the shipped default, Naiwa.
    std::string theme{"naiwa"};
    // The launcher's interface language: "auto" follows the Windows display language, and the
    // other two name a catalog explicitly. The in-game interface has its own setting, so this
    // one dresses this window alone. Empty means "never stored": a launcher that upgrades from a
    // build without this field starts from the language its runtime ini already names.
    std::string language;
    // Pictures of this window's own, and the colours the "custom" palette is built from. Both are
    // the launcher's choices: the in-game shell keeps its own theme files untouched.
    LauncherThemeArtwork artwork;
    anomaly::PlatformUiCustomColors custom_colors;
    LauncherUpdateSettings update;

    [[nodiscard]] LauncherClientConfiguration& Selected() noexcept { return game; }
    [[nodiscard]] const LauncherClientConfiguration& Selected() const noexcept { return game; }
};

struct LauncherConfigurationLoadResult final {
    LauncherConfiguration configuration;
    bool loaded{};
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;
};

struct LauncherConfigurationSaveResult final {
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return win32_error == ERROR_SUCCESS; }
};

struct LauncherDiscoveryOptions final {
    std::filesystem::path payload_root;
    std::vector<std::filesystem::path> running_game_executables;
    std::vector<std::filesystem::path> search_roots;
    bool include_system_locations{true};
};

[[nodiscard]] std::filesystem::path LauncherConfigurationPath(
    const std::filesystem::path& payload_root) noexcept;
[[nodiscard]] LauncherConfigurationLoadResult LoadLauncherConfiguration(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] LauncherConfigurationSaveResult SaveLauncherConfiguration(
    const std::filesystem::path& path,
    const LauncherConfiguration& configuration) noexcept;

[[nodiscard]] bool IsMaydayGameDirectory(
    const std::filesystem::path& directory) noexcept;
[[nodiscard]] LauncherClientConfiguration DiscoverLauncherConfiguration(
    const LauncherClientConfiguration& preferred,
    const LauncherDiscoveryOptions& options);

}  // namespace anomaly::launcher
