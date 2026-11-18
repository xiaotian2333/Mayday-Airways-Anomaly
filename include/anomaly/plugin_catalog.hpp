#pragma once

#include "anomaly/plugin_compatibility.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anomaly {

enum class PluginCatalogStatus : std::uint8_t {
    Valid,
    InvalidManifest,
    InvalidPackage,
    DuplicateId,
    Incompatible,
};

// 透明哈希与等价比较：允许以 string_view 直接查找键为 std::string 的索引，
// 避免 noexcept 查找路径构造临时 std::string（分配失败即 terminate）。
struct TransparentStringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

struct TransparentStringEqual {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept {
        return left == right;
    }
};

struct PluginCatalogIssue {
    std::string code;
    std::string path;
    std::string message;
};

struct PluginCatalogEntry {
    std::filesystem::path package_root;
    std::filesystem::path manifest_file;
    std::filesystem::path entry_file;
    std::optional<PluginManifest> manifest;
    PluginCatalogStatus status{PluginCatalogStatus::InvalidManifest};
    std::vector<PluginCatalogIssue> issues;
    std::optional<PluginCompatibilityResult> compatibility;

    [[nodiscard]] std::string_view Id() const noexcept {
        return manifest ? std::string_view(manifest->id) : std::string_view{};
    }
    [[nodiscard]] bool LoadCandidate() const noexcept {
        return status == PluginCatalogStatus::Valid;
    }
};

class PluginCatalogSnapshot final {
public:
    explicit PluginCatalogSnapshot(std::vector<PluginCatalogEntry> entries = {});

    [[nodiscard]] const std::vector<PluginCatalogEntry>& Entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const PluginCatalogEntry* Find(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<AvailablePluginVersion> AvailablePlugins() const;

private:
    std::vector<PluginCatalogEntry> entries_;
    std::unordered_map<std::string, std::size_t, TransparentStringHash, TransparentStringEqual>
        index_;
};

[[nodiscard]] PluginCatalogSnapshot DiscoverPluginCatalog(
    const std::filesystem::path& plugin_root,
    const std::optional<PluginCompatibilityContext>& compatibility = std::nullopt);

}  // namespace anomaly
