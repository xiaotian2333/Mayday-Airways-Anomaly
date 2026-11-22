#include "anomaly/plugin_capability_policy.hpp"

#include <algorithm>
#include <array>

namespace anomaly {
namespace {

struct ServiceCapabilityMapping {
    std::string_view service;
    std::string_view capability;
};

constexpr std::array<std::string_view, 28> kKnownCapabilities{
    "commands",
    "configuration",
    "diagnostics",
    "entity-esp",
    "ipc",
    "game-events",
    "interop-hook",
    "interop-patch",
    "interop-signature",
    "mayday-combat",
    "mayday-freecam",
    "mayday-money",
    "mayday-voice",
    "mayday-weather",
    "memory-read",
    "memory-write",
    "notifications",
    "runtime-info",
    "scheduler",
    "storage",
    "input",
    "ui-font",
    "ui-overlay",
    "ui-texture",
    "ui-window",
    "ui",
    "json",
    "websocket",
};

constexpr std::array<ServiceCapabilityMapping, 26> kServiceCapabilities{{
    {"anomaly.plugin-state", "configuration"},
    {"anomaly.config", "configuration"},
    {"anomaly.storage", "storage"},
    {"anomaly.runtime-info", "runtime-info"},
    {"anomaly.diagnostics", "diagnostics"},
    {"anomaly.scheduler", "scheduler"},
    {"anomaly.ipc", "ipc"},
    {"anomaly.websocket", "websocket"},
    {"anomaly.commands", "commands"},
    {"anomaly.notifications", "notifications"},
    {"anomaly.interop.signature", "interop-signature"},
    {"anomaly.interop.hook", "interop-hook"},
    {"anomaly.interop.patch", "interop-patch"},
    {"mayday.money", "mayday-money"},
    {"mayday.freecam", "mayday-freecam"},
    {"mayday.weather", "mayday-weather"},
    {"mayday.combat", "mayday-combat"},
    {"mayday.voice", "mayday-voice"},
    {"anomaly.ui", "ui"},
    {"anomaly.localization", "ui"},
    {"anomaly.window", "ui-window"},
    {"anomaly.font", "ui-font"},
    {"anomaly.texture", "ui-texture"},
    {"anomaly.input", "input"},
    {"anomaly.json", "json"},
    {"anomaly.profile", "json"},
}};

[[nodiscard]] bool IsKnownCapability(const std::string_view capability) noexcept {
    return std::ranges::find(kKnownCapabilities, capability) != kKnownCapabilities.end();
}

[[nodiscard]] const ServiceCapabilityMapping* FindServiceCapability(
    const std::string_view service) noexcept {
    const auto found = std::ranges::find(
        kServiceCapabilities, service, &ServiceCapabilityMapping::service);
    return found == kServiceCapabilities.end() ? nullptr : &*found;
}

}  // namespace

bool PluginCapabilityGrant::HasCapability(const std::string_view capability) const noexcept {
    return std::ranges::find(capabilities_, capability) != capabilities_.end();
}

PluginServiceAuthorization PluginCapabilityGrant::AuthorizeService(
    const std::string_view service_id) const noexcept {
    if (service_id == "anomaly.core") return {true, {}};
    const ServiceCapabilityMapping* mapping = FindServiceCapability(service_id);
    if (mapping == nullptr) return {false, {}};
    return {HasCapability(mapping->capability), mapping->capability};
}

PluginServiceAuthorization PluginCapabilityGrant::AuthorizeRawMemory(
    const std::string_view capability) const noexcept {
    constexpr std::string_view kMemoryReadCapability = "memory-read";
    constexpr std::string_view kMemoryWriteCapability = "memory-write";
    const std::string_view required_capability = capability == kMemoryReadCapability
        ? kMemoryReadCapability
        : capability == kMemoryWriteCapability ? kMemoryWriteCapability : std::string_view{};
    if (required_capability.empty()) return {false, {}};
    return {HasCapability(required_capability), required_capability};
}

PluginCapabilityGrant ResolvePluginCapabilityGrant(const PluginManifest* manifest) {
    PluginCapabilityGrant grant;
    if (manifest == nullptr ||
        manifest->schema_version != kLatestPluginManifestSchemaVersion) {
        grant.enforceable_ = false;
        return grant;
    }

    const auto add_capability = [&grant](const std::string_view capability) {
        if (!grant.HasCapability(capability)) grant.capabilities_.emplace_back(capability);
    };

    for (const std::string& capability : manifest->capabilities) {
        if (IsKnownCapability(capability)) {
            add_capability(capability);
        } else {
            grant.audits_.push_back({
                PluginCapabilityAuditCode::UnknownCapability,
                capability,
                {},
            });
        }
    }

    for (const PluginServiceRequirement& service : manifest->services) {
        if (service.optional || service.id == "anomaly.core") continue;
        const ServiceCapabilityMapping* mapping = FindServiceCapability(service.id);
        if (mapping == nullptr) {
            grant.enforceable_ = false;
            grant.audits_.push_back({
                PluginCapabilityAuditCode::RequiredServiceMissingMapping,
                {}, service.id,
            });
        } else if (!grant.HasCapability(mapping->capability)) {
            grant.enforceable_ = false;
            grant.audits_.push_back({
                PluginCapabilityAuditCode::RequiredServiceMissingCapability,
                std::string(mapping->capability), service.id,
            });
        }
    }
    if (std::ranges::any_of(grant.audits_, [](const PluginCapabilityAudit& audit) {
            return audit.code == PluginCapabilityAuditCode::UnknownCapability;
        })) {
        grant.enforceable_ = false;
    }
    return grant;
}

}  // namespace anomaly
