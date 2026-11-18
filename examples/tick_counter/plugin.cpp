#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace {

const AnomalyUiServiceV1* g_ui{};
unsigned long long g_ticks{};
// 用户可见文案统一经本地化服务解析；语言在进程内固定，Load 时查一次保存。
anomaly::sdk::Localizer g_localizer;

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    const auto ui = anomaly::sdk::Host(host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();
    g_localizer = anomaly::sdk::Localizer(host);
    *context = &g_ticks;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }

void ANOMALY_CALL Unload(void*) {
    g_ui = nullptr;
    g_localizer = {};
    g_ticks = 0;
}

void ANOMALY_CALL Update(void*, double) { ++g_ticks; }

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;
    const std::string title = g_localizer.Text("window.title", "Game Tick Counter");
    int open = 1;
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;
    const std::string ticks = std::to_string(g_ticks);
    const std::string_view tick_args[]{ticks};
    const std::string text = g_localizer.Format(
        "state.ticks", "Validated game-thread ticks: {0}",
        std::span<const std::string_view>{tick_args});
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.tick-counter"),
        anomaly::sdk::StringView("Game Tick Counter"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
