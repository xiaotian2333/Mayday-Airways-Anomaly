// 气象调节插件：宿主 mayday.weather 服务封装 il2cpp 细节，
// 插件负责时间/天气调节界面、锁定开关与设置持久化。
// 时间调节走 WorldEnvironment 锚点时钟，天气覆盖走 WorldManager 的
// dev 覆盖机制（游戏自带持续锁定语义）。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
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
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kSchemaId = "weather";
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::string_view kSchema =
    R"json({"type":"object","additionalProperties":false,"required":["kind_index","severity_milli"],"properties":{"kind_index":{"type":"integer","minimum":0,"maximum":5},"severity_milli":{"type":"integer","minimum":0,"maximum":1000}}})json";

// 与宿主 kWeatherKindNames 的声明序一一对应（游戏 WeatherKind 顺序）。
constexpr int kWeatherKindCount = 6;
constexpr int kMaxSeverityMilli = 1000;
constexpr float kHoursPerDay = 24.0f;

const AnomalyHostApiV1* g_host{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyConfigServiceV1* g_config{};
const AnomalyJsonServiceV1* g_json{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyMaydayWeatherServiceV1* g_weather{};
anomaly::sdk::Localizer g_localizer;
AnomalyGenerationHandleV1 g_config_schema{};

std::atomic<int> g_kind_index{2};            // 默认降雨
std::atomic<int> g_severity_milli{600};      // 强度千分比
std::atomic<bool> g_dirty{false};
std::atomic<bool> g_save_scheduled{false};
std::atomic<int> g_kind_values[kWeatherKindCount]{{-1}, {-1}, {-1}, {-1}, {-1}, {-1}};
bool g_kind_values_fetched{};                // 仅 UI 线程访问
std::string g_status;
bool g_service_logged{};                     // 服务附加日志只打一次
std::chrono::steady_clock::time_point g_last_state_fail_log{};

void PluginLog(const AnomalyCoreLogLevelV1 level, const std::string& message) {
    if (g_core == nullptr || g_core->log == nullptr) return;
    g_core->log(g_core->user, static_cast<std::uint32_t>(level),
                anomaly::sdk::StringView(message));
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool Succeeded(const AnomalyStatusV1& status) noexcept {
    return anomaly::sdk::Succeeded(status);
}

void LocalizedStatus(const std::string_view key, const std::string_view fallback) {
    g_status = g_localizer.Text(key, fallback);
}

void SetStatusFromService(const AnomalyStatusV1& status) {
    const std::string_view message{status.message.data, status.message.size};
    if (message == "status.unavailable") {
        LocalizedStatus("status.unavailable",
                        "The game is not ready yet. Enter a flight first.");
    } else if (message == "status.not_in_flight") {
        LocalizedStatus("status.not_in_flight",
                        "Enter a flight before changing time or weather.");
    } else if (message == "status.attach_failed") {
        LocalizedStatus("status.attach_failed", "Failed to attach to the game runtime.");
    } else if (!message.empty()) {
        g_status.assign(message.data(), message.size());
    } else {
        LocalizedStatus("status.failed", "Weather operation failed.");
    }
}

// kind_index → 运行时枚举值缓存：首次进入界面时拉取一次。
bool FetchKindValues() {
    if (g_kind_values_fetched || g_weather == nullptr) return g_kind_values_fetched;
    for (int index = 0; index < kWeatherKindCount; ++index) {
        std::int32_t value = -1;
        if (!Succeeded(g_weather->get_kind_value(g_weather->user, index, &value))) {
            return false;
        }
        g_kind_values[index].store(value, std::memory_order_relaxed);
    }
    g_kind_values_fetched = true;
    return true;
}

int KindIndexFromValue(const std::int32_t kind_value) {
    if (kind_value < 0) return -1;
    for (int index = 0; index < kWeatherKindCount; ++index) {
        if (g_kind_values[index].load(std::memory_order_relaxed) == kind_value) {
            return index;
        }
    }
    return -1;
}

std::string KindName(const int index) {
    // key 与英文回退按宿主 kWeatherKindNames 的声明序对齐（游戏数据）。
    static constexpr const char* kKeys[kWeatherKindCount]{
        "weather.calm", "weather.cloudy", "weather.rain",
        "weather.storm", "weather.fog", "weather.snow"};
    static constexpr const char* kFallbacks[kWeatherKindCount]{
        "Calm", "Cloudy", "Rain", "Storm", "Fog", "Snow"};
    if (index < 0 || index >= kWeatherKindCount) {
        return g_localizer.Text("state.unknown", "unknown");
    }
    return g_localizer.Text(kKeys[index], kFallbacks[index]);
}

std::string FormatClock(const float hours) {
    if (!(hours >= 0.0f && hours < kHoursPerDay)) return "--:--";
    const int total_minutes =
        static_cast<int>(hours * 60.0f + 0.5f) % (24 * 60);
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d",
                  total_minutes / 60, total_minutes % 60);
    return buffer;
}

std::string FormatPercent(const float severity) {
    if (!(severity >= 0.0f && severity <= 1.0f)) return "--";
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "%d",
                  static_cast<int>(severity * 100.0f + 0.5f));
    return buffer;
}

std::string FormatRate(const float rate) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(rate));
    return buffer;
}

bool SaveSettings() {
    if (g_config == nullptr) return false;
    char document[128]{};
    const int written = std::snprintf(
        document, sizeof(document), "{\"kind_index\":%d,\"severity_milli\":%d}",
        g_kind_index.load(std::memory_order_relaxed),
        g_severity_milli.load(std::memory_order_relaxed));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(document)) return false;
    return Succeeded(g_config->write_atomic(
        g_config->user, anomaly::sdk::StringView(kSchemaId), kSchemaVersion,
        Bytes(std::string_view(document, static_cast<std::size_t>(written)))));
}

void ANOMALY_CALL SaveSettingsTask(void*, AnomalyGenerationHandleV1) {
    g_save_scheduled.store(false, std::memory_order_release);
    if (!g_dirty.exchange(false, std::memory_order_acq_rel)) return;
    if (!SaveSettings()) g_dirty.store(true, std::memory_order_release);
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

bool ReadNumber(
    AnomalyGenerationHandleV1 root, const char* key, double* out_value) {
    AnomalyGenerationHandleV1 child{};
    if (!Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView(key), &child))) {
        return false;
    }
    const bool ok = Succeeded(g_json->number_value(g_json->user, child, out_value));
    static_cast<void>(g_json->release(g_json->user, child));
    return ok;
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
    double kind_index = 0.0;
    double severity_milli = 0.0;
    if (ReadNumber(root, "kind_index", &kind_index) &&
        kind_index >= 0.0 && kind_index < kWeatherKindCount) {
        g_kind_index.store(static_cast<int>(kind_index), std::memory_order_relaxed);
    }
    if (ReadNumber(root, "severity_milli", &severity_milli) &&
        severity_milli >= 0.0 && severity_milli <= kMaxSeverityMilli) {
        g_severity_milli.store(
            static_cast<int>(severity_milli), std::memory_order_relaxed);
    }
    static_cast<void>(g_json->release(g_json->user, root));
}

void ApplyTimeDraft(const float draft_hours) {
    const AnomalyStatusV1 status = g_weather->set_time(g_weather->user, draft_hours);
    if (!Succeeded(status)) SetStatusFromService(status);
}

void ApplyWeatherOverride(const bool active) {
    const AnomalyStatusV1 status = g_weather->set_weather_override(
        g_weather->user, active ? 1 : 0,
        g_kind_index.load(std::memory_order_relaxed),
        static_cast<float>(g_severity_milli.load(std::memory_order_relaxed)) / 1000.0f);
    if (!Succeeded(status)) SetStatusFromService(status);
}

bool ServiceHasField(std::size_t struct_size, std::size_t offset) noexcept {
    return struct_size >= offset + sizeof(void*);
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
    if (g_weather == nullptr) {
        const auto service = host.Query<AnomalyMaydayWeatherServiceV1>(
            ANOMALY_MAYDAY_WEATHER_SERVICE_V1_ID,
            ANOMALY_MAYDAY_WEATHER_SERVICE_V1_VERSION);
        g_weather = service.get();
        if (g_weather != nullptr && !g_service_logged) {
            g_service_logged = true;
            PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                      "weather: mayday.weather service attached");
        }
    }
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
        g_json->object_find == nullptr || g_json->number_value == nullptr ||
        g_json->release == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    const AnomalyStatusV1 schema_status = g_config->register_schema(
        g_config->user, anomaly::sdk::StringView(kSchemaId), kSchemaVersion,
        Bytes(kSchema), &g_config_schema);
    if (!Succeeded(schema_status)) return schema_status;
    LoadSettings();
    LocalizedStatus("status.unavailable",
                    "The game is not ready yet. Enter a flight first.");
    PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "weather: plugin loaded (1.0.0)");
    *context = &g_kind_index;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    if (g_dirty.load(std::memory_order_acquire)) {
        if (SaveSettings()) g_dirty.store(false, std::memory_order_release);
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    g_host = nullptr;
    g_core = nullptr;
    g_config = nullptr;
    g_json = nullptr;
    g_scheduler = nullptr;
    g_weather = nullptr;
    g_config_schema = {};
    g_status.clear();
    g_service_logged = false;
    g_kind_values_fetched = false;
}

void ANOMALY_CALL Update(void*, double) {
    // 宿主的 dev 覆盖与时钟锁定由游戏侧机制维持，无需每帧重申。
    QueryServices();
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) return;
    int open = 1;
    const std::string title = g_localizer.Text("window.title", "Weather & Time");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    QueryServices();
    if (g_weather == nullptr) {
        ui->text(ui->user, anomaly::sdk::StringView(
            g_localizer.Text("status.unavailable",
                             "The game is not ready yet. Enter a flight first.")));
        return;
    }
    FetchKindValues();

    AnomalyMaydayWeatherStateV1 state{};
    state.struct_size = sizeof(state);
    const AnomalyStatusV1 state_status = g_weather->get_state(g_weather->user, &state);
    const bool state_ok = Succeeded(state_status);
    if (!state_ok) {
        SetStatusFromService(state_status);
        ui->text(ui->user, anomaly::sdk::StringView(g_status));
        // 失败原因限流写日志（5 秒一条），便于在宿主日志流里诊断。
        const auto now = std::chrono::steady_clock::now();
        if (now - g_last_state_fail_log > std::chrono::seconds(5)) {
            g_last_state_fail_log = now;
            PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                      "weather: get_state failed: " + g_status);
        }
    }

    // ---- 状态区 ----
    if (state_ok) {
        const std::string clock = FormatClock(state.hours);
        const std::string_view clock_args[]{clock};
        const std::string time_line = g_localizer.Format(
            "state.time", "Time: {0}", std::span<const std::string_view>{clock_args});
        ui->text(ui->user, anomaly::sdk::StringView(time_line));
        const int current_index = KindIndexFromValue(state.kind_value);
        const std::string kind = KindName(current_index);
        const std::string_view kind_args[]{kind};
        const std::string kind_line = g_localizer.Format(
            "state.kind", "Weather: {0}", std::span<const std::string_view>{kind_args});
        ui->text(ui->user, anomaly::sdk::StringView(kind_line));
        const std::string percent = FormatPercent(state.severity);
        const std::string_view percent_args[]{percent};
        const std::string severity_line = g_localizer.Format(
            "state.severity", "Severity: {0}%",
            std::span<const std::string_view>{percent_args});
        ui->text(ui->user, anomaly::sdk::StringView(severity_line));
        const std::string rate = FormatRate(state.rate);
        const std::string_view rate_args[]{rate};
        const std::string rate_line = g_localizer.Format(
            "state.rate", "Rate: {0}x", std::span<const std::string_view>{rate_args});
        ui->text(ui->user, anomaly::sdk::StringView(rate_line));
    }

    // ---- 时间区 ----
    ui->separator(ui->user);
    static float draft_hours = -1.0f;
    if (draft_hours < 0.0f && state_ok) draft_hours = state.hours;
    if (draft_hours < 0.0f) draft_hours = 12.0f;
    if (ui->slider_float(
            ui->user, anomaly::sdk::StringView(
                g_localizer.Text("time.slider", "Time of day (hours)")),
            &draft_hours, 0.0f, kHoursPerDay) != 0) {
        // 拖动即为草稿，点按钮才应用。
    }
    const std::string set_time_label =
        g_localizer.Text("time.set", "Set time of day");
    if (ui->button(ui->user, anomaly::sdk::StringView(set_time_label), 0.0f, 0.0f)) {
        ApplyTimeDraft(draft_hours);
    }
    if (state_ok) {
        int locked = state.time_locked;
        const std::string lock_label =
            g_localizer.Text("time.lock", "Lock time (freeze the clock)");
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(lock_label), &locked) != 0 &&
            locked != state.time_locked) {
            const AnomalyStatusV1 status =
                g_weather->set_time_locked(g_weather->user, locked);
            if (!Succeeded(status)) SetStatusFromService(status);
        }
    }

    // ---- 天气区 ----
    ui->separator(ui->user);
    float draft_severity =
        static_cast<float>(g_severity_milli.load(std::memory_order_relaxed)) / 1000.0f;
    if (ui->slider_float(
            ui->user, anomaly::sdk::StringView(
                g_localizer.Text("weather.severity", "Weather severity")),
            &draft_severity, 0.0f, 1.0f) != 0) {
        g_severity_milli.store(
            static_cast<int>(draft_severity * 1000.0f + 0.5f),
            std::memory_order_relaxed);
        MarkSettingsDirty();
    }
    // 种类按钮 3 + 3 两行；点击即选中，若锁定中立即重新应用。
    for (int index = 0; index < kWeatherKindCount; ++index) {
        if (index % 3 != 0) {
            ui->same_line(ui->user, 0.0f, -1.0f);
        }
        std::string label = KindName(index);
        if (index == g_kind_index.load(std::memory_order_relaxed)) {
            label.insert(0, "* ");
        }
        if (ui->button(ui->user, anomaly::sdk::StringView(label), 0.0f, 0.0f)) {
            g_kind_index.store(index, std::memory_order_relaxed);
            MarkSettingsDirty();
            if (state_ok && state.override_active != 0) {
                ApplyWeatherOverride(true);
            }
        }
    }
    if (state_ok) {
        int override_active = state.override_active;
        const std::string lock_label =
            g_localizer.Text("weather.lock", "Lock weather (override natural weather)");
        if (ui->checkbox(
                ui->user, anomaly::sdk::StringView(lock_label),
                &override_active) != 0 &&
            override_active != state.override_active) {
            ApplyWeatherOverride(override_active != 0);
        }
    }

    ui->separator(ui->user);
    const std::string help = g_localizer.Text(
        "help.controls",
        "Locking time freezes the game clock; locking weather uses the game's "
        "override mechanism and holds until cleared. Locks reset when you "
        "enter a new flight.");
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
        anomaly::sdk::StringView("mayday.weather"),
        anomaly::sdk::StringView("Weather & Time"),
        anomaly::sdk::StringView("Mayday Loader"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
