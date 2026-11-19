// 自动棒球棍插件：宿主 mayday.combat 服务封装 PlayerNet/BatItem/Passenger 的
// il2cpp 细节、主线程触发与服务端静默瞄准；插件只负责开关、状态与设置持久化。
// 攻击距离与挥击间隔来自包内 compatibility.profile.json（游戏兼容数据）。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/profile.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/json.h"
#include "anomaly/sdk/services/mayday.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kSchemaId = "autobat";
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::string_view kSchema =
    R"json({"type":"object","additionalProperties":false,"required":["enabled"],"properties":{"enabled":{"type":"boolean"}}})json";
constexpr std::string_view kProfileFile = "compatibility.profile.json";
constexpr std::uint32_t kMinimumRangeMilli = 100;
constexpr std::uint32_t kMinimumIntervalMs = 100;

const AnomalyHostApiV1* g_host{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyConfigServiceV1* g_config{};
const AnomalyJsonServiceV1* g_json{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyMaydayCombatServiceV1* g_combat{};
anomaly::sdk::Localizer g_localizer;
AnomalyGenerationHandleV1 g_config_schema{};

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_service_enabled{false};
std::atomic<bool> g_dirty{false};
std::atomic<bool> g_save_scheduled{false};
std::atomic<bool> g_options_applied{false};
bool g_profile_ok{};
std::int32_t g_max_range_mm{};
std::int32_t g_attack_interval_ms{};
bool g_service_logged{};
std::string g_status;

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool Succeeded(const AnomalyStatusV1& status) noexcept {
    return anomaly::sdk::Succeeded(status);
}

bool ServiceHasField(std::size_t struct_size, std::size_t offset) noexcept {
    return struct_size >= offset + sizeof(void*);
}

void PluginLog(const AnomalyCoreLogLevelV1 level, const std::string& message) {
    if (g_core == nullptr || g_core->log == nullptr) return;
    g_core->log(g_core->user, static_cast<std::uint32_t>(level),
                anomaly::sdk::StringView(message));
}

void LocalizedStatus(const std::string_view key, const std::string_view fallback) {
    g_status = g_localizer.Text(key, fallback);
}

void SetStatusFromService(const AnomalyStatusV1& status) {
    std::string_view message;
    if (status.message.data != nullptr) {
        message = std::string_view(status.message.data, status.message.size);
    }
    if (message == "status.unavailable") {
        LocalizedStatus("status.unavailable",
                        "The game is not ready yet. Enter a flight first.");
    } else if (message == "status.not_in_flight") {
        LocalizedStatus("status.not_in_flight",
                        "Enter a flight to use automatic swinging.");
    } else if (message == "status.attach_failed") {
        LocalizedStatus("status.attach_failed",
                        "Could not attach to the game runtime.");
    } else if (message == "status.combat_no_symbols") {
        LocalizedStatus("status.combat_no_symbols",
                        "This game build is missing the symbols needed for automatic swinging.");
    } else if (message == "status.combat_no_options") {
        LocalizedStatus("status.combat_no_options",
                        "The attack range and swing interval are not configured.");
    } else if (message == "status.combat_hook_failed") {
        LocalizedStatus("status.combat_hook_failed",
                        "Could not install the game swing or main-thread hooks.");
    } else {
        LocalizedStatus("status.failed", "Operation failed.");
    }
}

void SaveSettingsTask(void*, AnomalyGenerationHandleV1) {
    g_save_scheduled.store(false, std::memory_order_release);
    if (!g_dirty.exchange(false, std::memory_order_acq_rel)) return;
    if (g_config == nullptr) {
        g_dirty.store(true, std::memory_order_release);
        return;
    }
    const char* document = g_enabled.load(std::memory_order_acquire)
        ? R"json({"enabled":true})json"
        : R"json({"enabled":false})json";
    const AnomalyStatusV1 status = g_config->write_atomic(
        g_config->user, anomaly::sdk::StringView(kSchemaId), kSchemaVersion,
        Bytes(std::string_view(document)));
    if (!Succeeded(status)) g_dirty.store(true, std::memory_order_release);
}

void MarkSettingsDirty() {
    g_dirty.store(true, std::memory_order_release);
    if (g_scheduler == nullptr) return;
    if (g_save_scheduled.exchange(true, std::memory_order_acq_rel)) return;
    AnomalyGenerationHandleV1 handle{};
    if (!Succeeded(g_scheduler->schedule(
            g_scheduler->user, 250, SaveSettingsTask, nullptr, &handle))) {
        g_save_scheduled.store(false, std::memory_order_release);
    }
}

void LoadSettings() {
    if (g_config == nullptr || g_json == nullptr) return;
    std::uint32_t version{};
    std::size_t size{};
    AnomalyStatusV1 status = g_config->read(
        g_config->user, anomaly::sdk::StringView(kSchemaId), &version,
        {nullptr, 0}, &size);
    if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) return;
    if (!Succeeded(status) || size == 0 || size > 4096) return;
    std::string document(size, '\0');
    std::size_t copied = document.size();
    status = g_config->read(
        g_config->user, anomaly::sdk::StringView(kSchemaId), &version,
        {reinterpret_cast<std::uint8_t*>(document.data()), document.size()}, &copied);
    if (!Succeeded(status) || copied == 0 || copied > document.size()) return;
    if (version != kSchemaVersion) return;
    AnomalyGenerationHandleV1 root{};
    if (!Succeeded(g_json->parse(
            g_json->user, {document.data(), copied}, &root))) {
        return;
    }
    AnomalyGenerationHandleV1 enabled{};
    std::int32_t value = 0;
    if (Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView("enabled"), &enabled)) &&
        Succeeded(g_json->boolean_value(g_json->user, enabled, &value))) {
        g_enabled.store(value != 0, std::memory_order_release);
    }
    if (enabled.id != 0) static_cast<void>(g_json->release(g_json->user, enabled));
    static_cast<void>(g_json->release(g_json->user, root));
}

bool LoadCompatibilityProfile() {
    if (g_host == nullptr) return false;
    try {
        const anomaly::sdk::Host host(g_host);
        const auto result = anomaly::sdk::LoadProfile(
            host, kProfileFile,
            anomaly::sdk::ProfileContract{
                "mayday", "bat_combat", "compat", "mayday.bat-combat"});
        if (!result) {
            LocalizedStatus("status.profile_missing",
                            "The compatibility profile that defines the game's maximum attack range is missing.");
            return false;
        }
        const auto& profile = result.value();
        const std::uint32_t range = profile.Value("swingRangeMilli", 10000u, 1u);
        const std::uint32_t interval = profile.Value("attackIntervalMs", 5000u, 1u);
        if (range < kMinimumRangeMilli || interval < kMinimumIntervalMs) {
            LocalizedStatus("status.profile_invalid",
                            "The compatibility profile is invalid.");
            return false;
        }
        g_max_range_mm = static_cast<std::int32_t>(range);
        g_attack_interval_ms = static_cast<std::int32_t>(interval);
        return true;
    } catch (const std::exception&) {
        LocalizedStatus("status.profile_invalid",
                        "The compatibility profile is invalid.");
        return false;
    }
}

void QueryServices() {
    if (g_host == nullptr) return;
    const anomaly::sdk::Host host(g_host);
    if (g_core == nullptr) {
        const auto service = host.Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
        g_core = service.get();
    }
    if (g_config == nullptr) {
        const auto service = host.Query<AnomalyConfigServiceV1>(
            ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
        g_config = service.get();
    }
    if (g_json == nullptr) {
        const auto service = host.Query<AnomalyJsonServiceV1>(
            ANOMALY_JSON_SERVICE_V1_ID, ANOMALY_JSON_SERVICE_V1_VERSION);
        g_json = service.get();
    }
    if (g_scheduler == nullptr) {
        const auto service = host.Query<AnomalySchedulerServiceV1>(
            ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
        g_scheduler = service.get();
    }
    if (g_combat == nullptr) {
        const auto service = host.Query<AnomalyMaydayCombatServiceV1>(
            ANOMALY_MAYDAY_COMBAT_SERVICE_V1_ID,
            ANOMALY_MAYDAY_COMBAT_SERVICE_V1_VERSION);
        g_combat = service.get();
        if (g_combat != nullptr) {
            g_options_applied.store(false, std::memory_order_release);
            if (!g_service_logged) {
                g_service_logged = true;
                PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                          "autobat: mayday.combat service attached");
            }
        }
    }
}

bool ApplyCompatibilityOptions() {
    if (g_combat == nullptr || !g_profile_ok) return false;
    if (g_options_applied.load(std::memory_order_acquire)) return true;
    const AnomalyStatusV1 status = g_combat->set_options(
        g_combat->user, g_max_range_mm, g_attack_interval_ms);
    if (!Succeeded(status)) {
        SetStatusFromService(status);
        return false;
    }
    g_options_applied.store(true, std::memory_order_release);
    return true;
}

bool SetCombatEnabled(const bool enabled) {
    if (g_combat == nullptr) {
        LocalizedStatus("status.waiting", "Waiting for the game combat service.");
        return false;
    }
    if (enabled && !ApplyCompatibilityOptions()) {
        PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                  "autobat: compatibility options unavailable");
        return false;
    }
    const AnomalyStatusV1 status =
        g_combat->set_enabled(g_combat->user, enabled ? 1 : 0);
    if (!Succeeded(status)) {
        SetStatusFromService(status);
        PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                  "autobat: set_enabled failed: " + g_status);
        return false;
    }
    g_service_enabled.store(enabled, std::memory_order_release);
    g_status.clear();
    PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
              enabled ? "autobat: automatic swinging enabled"
                      : "autobat: automatic swinging disabled");
    return true;
}

std::string FormatInteger(const std::int32_t value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%d", value);
    return buffer;
}

std::string FormatCount(const std::uint32_t value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%u", value);
    return buffer;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    g_host = host;
    g_localizer = anomaly::sdk::Localizer(host);
    const anomaly::sdk::Host view(host);
    const auto config = view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    const auto json = view.Query<AnomalyJsonServiceV1>(
        ANOMALY_JSON_SERVICE_V1_ID, ANOMALY_JSON_SERVICE_V1_VERSION);
    const auto scheduler = view.Query<AnomalySchedulerServiceV1>(
        ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    if (!config || !json || !scheduler) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_config = config.get();
    g_json = json.get();
    g_scheduler = scheduler.get();
    if (!ServiceHasField(g_config->struct_size,
                         offsetof(AnomalyConfigServiceV1, write_atomic)) ||
        g_config->register_schema == nullptr || g_config->read == nullptr ||
        g_config->write_atomic == nullptr || g_json->parse == nullptr ||
        g_json->object_find == nullptr || g_json->boolean_value == nullptr ||
        g_json->release == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    const AnomalyStatusV1 schema_status = g_config->register_schema(
        g_config->user, anomaly::sdk::StringView(kSchemaId), kSchemaVersion,
        Bytes(kSchema), &g_config_schema);
    if (!Succeeded(schema_status)) return schema_status;
    g_profile_ok = LoadCompatibilityProfile();
    LoadSettings();
    if (!g_profile_ok) {
        PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                  "autobat: compatibility profile unavailable");
    }
    PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "autobat: plugin loaded (1.0.0)");
    *context = &g_enabled;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    if (g_combat != nullptr && g_service_enabled.exchange(false, std::memory_order_acq_rel)) {
        static_cast<void>(g_combat->set_enabled(g_combat->user, 0));
    }
    if (g_dirty.load(std::memory_order_acquire)) {
        SaveSettingsTask(nullptr, {});
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    g_host = nullptr;
    g_core = nullptr;
    g_config = nullptr;
    g_json = nullptr;
    g_scheduler = nullptr;
    g_combat = nullptr;
    g_config_schema = {};
    g_service_logged = false;
    g_options_applied.store(false, std::memory_order_release);
    g_status.clear();
}

void ANOMALY_CALL Update(void*, double) {
    QueryServices();
    if (!g_profile_ok || g_combat == nullptr) return;
    if (g_enabled.load(std::memory_order_acquire)) {
        if (!g_service_enabled.load(std::memory_order_acquire)) {
            if (!SetCombatEnabled(true)) {
                // 符号或钩子不可用时不要每帧重试：回到关闭状态并持久化。
                g_enabled.store(false, std::memory_order_release);
                MarkSettingsDirty();
            }
        }
    } else if (g_service_enabled.load(std::memory_order_acquire)) {
        static_cast<void>(SetCombatEnabled(false));
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) return;
    int open = 1;
    const std::string title = g_localizer.Text("window.title", "Auto Bat");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    QueryServices();
    int enabled = g_enabled.load(std::memory_order_acquire) ? 1 : 0;
    const std::string toggle_label =
        g_localizer.Text("toggle.enabled", "Auto swing");
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(toggle_label), &enabled) != 0 &&
        enabled != (g_enabled.load(std::memory_order_acquire) ? 1 : 0)) {
        if (enabled != 0) {
            if (SetCombatEnabled(true)) {
                g_enabled.store(true, std::memory_order_release);
                MarkSettingsDirty();
            }
        } else {
            static_cast<void>(SetCombatEnabled(false));
            g_enabled.store(false, std::memory_order_release);
            MarkSettingsDirty();
        }
    }

    if (g_combat == nullptr) {
        ui->text(ui->user, anomaly::sdk::StringView(
            g_localizer.Text("status.waiting", "Waiting for the game combat service.")));
    } else if (!g_profile_ok) {
        ui->text(ui->user, anomaly::sdk::StringView(
            g_status.empty()
                ? g_localizer.Text("status.profile_missing",
                                   "The compatibility profile that defines the game's maximum attack range is missing.")
                : g_status));
    } else if (!g_enabled.load(std::memory_order_acquire)) {
        ui->text(ui->user, anomaly::sdk::StringView(
            g_status.empty()
                ? g_localizer.Text("status.disabled", "Automatic swinging is off.")
                : g_status));
    } else {
        AnomalyMaydayCombatStateV1 state{};
        state.struct_size = sizeof(state);
        const AnomalyStatusV1 state_status =
            g_combat->get_state(g_combat->user, &state);
        if (!Succeeded(state_status)) {
            SetStatusFromService(state_status);
            ui->text(ui->user, anomaly::sdk::StringView(g_status));
        } else if (state.in_flight == 0) {
            ui->text(ui->user, anomaly::sdk::StringView(
                g_localizer.Text("status.not_in_flight",
                                 "Enter a flight to use automatic swinging.")));
        } else if (state.holding_bat == 0) {
            ui->text(ui->user, anomaly::sdk::StringView(
                g_localizer.Text("status.hold_bat",
                                 "Hold the baseball bat first.")));
        } else {
            const std::string range_cm = FormatInteger(
                static_cast<std::int32_t>(state.max_range_mm / 10));
            if (state.has_target != 0 && state.target_distance_mm >= 0) {
                const std::string distance_cm = FormatInteger(
                    static_cast<std::int32_t>(
                        (state.target_distance_mm + 5) / 10));
                const std::string_view arguments[]{distance_cm, range_cm};
                const std::string line = g_localizer.Format(
                    "status.target", "Target at {0} cm (range {1} cm).",
                    std::span<const std::string_view>{arguments});
                ui->text(ui->user, anomaly::sdk::StringView(line));
            } else {
                const std::string_view arguments[]{range_cm};
                const std::string line = g_localizer.Format(
                    "status.no_target", "No passenger within {0} cm.",
                    std::span<const std::string_view>{arguments});
                ui->text(ui->user, anomaly::sdk::StringView(line));
            }
            if (state.attacks_sent > 0) {
                const std::string count = FormatCount(state.attacks_sent);
                const std::string_view arguments[]{count};
                const std::string line = g_localizer.Format(
                    "stats.swings", "Swings sent: {0}",
                    std::span<const std::string_view>{arguments});
                ui->text(ui->user, anomaly::sdk::StringView(line));
            }
        }
    }

    ui->separator(ui->user);
    const std::string help = g_localizer.Text(
        "help.text",
        "While enabled, holding the bat automatically locks the nearest passenger "
        "within the game's swing range and sends the use command as a protocol "
        "packet. When you host the session, the camera turns toward the target "
        "only while the server evaluates the swing, so the view never moves.");
    if (ui->text_wrapped != nullptr) {
        ui->text_wrapped(ui->user, anomaly::sdk::StringView(help));
    } else {
        ui->text(ui->user, anomaly::sdk::StringView(help));
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("mayday.autobat"),
        anomaly::sdk::StringView("Auto Bat"),
        anomaly::sdk::StringView("Mayday Loader"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
