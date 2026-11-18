// 自由镜头插件：宿主 mayday.freecam 服务负责接管游戏相机，
// 插件只负责热键、每帧输入与设置界面。
// 控制：WASD 移动，空格上升，Shift 下降，鼠标转向。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/services/json.h"
#include "anomaly/sdk/services/mayday.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ui_resources.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

constexpr std::uint32_t kVkShift = 0x10;
constexpr std::uint32_t kVkLShift = 0xA0;
constexpr std::uint32_t kVkRShift = 0xA1;
constexpr std::uint32_t kVkSpace = 0x20;
constexpr std::uint32_t kVkEscape = 0x1B;
constexpr std::uint32_t kDefaultHotkey = 0x75;  // F6
constexpr float kDefaultSpeed = 12.0f;
constexpr float kDefaultSensitivity = 0.12f;
constexpr std::string_view kSchemaId = "freecam";
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::string_view kSchema = R"json({"type":"object","additionalProperties":false,"required":["hotkey","speed","sensitivity"],"properties":{"hotkey":{"type":"integer","minimum":1,"maximum":254},"speed":{"type":"number","minimum":0.5,"maximum":200},"sensitivity":{"type":"number","minimum":0.01,"maximum":2}}})json";

const AnomalyHostApiV1* g_host{};
const AnomalyInputServiceV1* g_input{};
const AnomalyConfigServiceV1* g_config{};
const AnomalyJsonServiceV1* g_json{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyMaydayFreecamServiceV1* g_freecam{};
anomaly::sdk::Localizer g_localizer;
AnomalyGenerationHandleV1 g_config_schema{};
std::atomic<std::uint32_t> g_hotkey{kDefaultHotkey};
std::atomic<float> g_speed{kDefaultSpeed};
std::atomic<float> g_sensitivity{kDefaultSensitivity};
std::atomic<bool> g_dirty{false};
std::atomic<bool> g_save_scheduled{false};
bool g_active{};
bool g_capture_mode{};
std::uint8_t g_previous_keys[32]{};
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

bool InputHasMouseDelta(const AnomalyInputServiceV1* service) noexcept {
    return service != nullptr && service->mouse_delta != nullptr &&
        service->struct_size >=
            offsetof(AnomalyInputServiceV1, mouse_delta) + sizeof(service->mouse_delta);
}

void QueryServices() {
    if (g_host == nullptr) return;
    const anomaly::sdk::Host host(g_host);
    if (g_input == nullptr) {
        const auto service = host.Query<AnomalyInputServiceV1>(
            ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
        g_input = service.get();
    }
    if (g_freecam == nullptr) {
        const auto service = host.Query<AnomalyMaydayFreecamServiceV1>(
            ANOMALY_MAYDAY_FREECAM_SERVICE_V1_ID,
            ANOMALY_MAYDAY_FREECAM_SERVICE_V1_VERSION);
        g_freecam = service.get();
    }
}

bool KeyDown(const AnomalyInputSnapshotV1& snapshot, std::uint32_t virtual_key) noexcept {
    if (virtual_key == 0 || virtual_key >= 256) return false;
    return (snapshot.keys[virtual_key / 8U] &
            static_cast<std::uint8_t>(1U << (virtual_key % 8U))) != 0;
}

bool ShiftDown(const AnomalyInputSnapshotV1& snapshot) noexcept {
    return KeyDown(snapshot, kVkShift) || KeyDown(snapshot, kVkLShift) ||
        KeyDown(snapshot, kVkRShift);
}

bool IsModifierKey(std::uint32_t virtual_key) noexcept {
    return virtual_key == kVkShift || virtual_key == kVkLShift ||
        virtual_key == kVkRShift || virtual_key == 0x11 || virtual_key == 0xA2 ||
        virtual_key == 0xA3 || virtual_key == 0x12 || virtual_key == 0xA4 ||
        virtual_key == 0xA5 || virtual_key == 0x5B || virtual_key == 0x5C;
}

std::string KeyName(std::uint32_t virtual_key) {
    if (virtual_key >= 0x70 && virtual_key <= 0x7B) {
        return "F" + std::to_string(virtual_key - 0x70 + 1);
    }
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        return std::string(1, static_cast<char>(virtual_key));
    }
    if (virtual_key >= '0' && virtual_key <= '9') {
        return std::string(1, static_cast<char>(virtual_key));
    }
    switch (virtual_key) {
        case kVkSpace: return "Space";
        case 0x09: return "Tab";
        case 0x0D: return "Enter";
        case 0x08: return "Backspace";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        case 0x24: return "Home";
        case 0x23: return "End";
        case 0x21: return "PageUp";
        case 0x22: return "PageDown";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0xBA: return ";";
        case 0xBB: return "=";
        case 0xBC: return ",";
        case 0xBD: return "-";
        case 0xBE: return ".";
        case 0xBF: return "/";
        case 0xC0: return "`";
        case 0xDB: return "[";
        case 0xDC: return "\\";
        case 0xDD: return "]";
        case 0xDE: return "'";
        default: break;
    }
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "VK_%02X",
                  static_cast<unsigned>(virtual_key));
    return buffer;
}

void LocalizedStatus(const char* key, const char* fallback) {
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
    } else if (message == "status.attach_failed") {
        LocalizedStatus("status.attach_failed", "Could not attach to the game runtime.");
    } else if (message == "status.not_in_flight") {
        LocalizedStatus("status.not_in_flight",
                        "Enter a flight before enabling the free camera.");
    } else if (message == "status.freecam_symbols") {
        LocalizedStatus("status.freecam_symbols",
                        "Free camera symbols are not available in this game build.");
    } else if (message == "status.freecam_no_game_state") {
        LocalizedStatus("status.freecam_no_game_state",
                        "GameState symbols are not available.");
    } else if (message == "status.freecam_no_rig_class") {
        LocalizedStatus("status.freecam_no_rig_class", "PlayerRig class was not found.");
    } else if (message == "status.freecam_no_rig_methods") {
        LocalizedStatus("status.freecam_no_rig_methods", "PlayerRig methods were not found.");
    } else if (message == "status.freecam_no_local_rig") {
        LocalizedStatus("status.freecam_no_local_rig", "PlayerRig.get_Local was not found.");
    } else if (message == "status.freecam_no_camera_rig") {
        LocalizedStatus("status.freecam_no_camera_rig", "PlayerRig.get_cameraRig was not found.");
    } else if (message == "status.freecam_no_look_getter") {
        LocalizedStatus("status.freecam_no_look_getter", "PlayerRig.get_look was not found.");
    } else if (message == "status.freecam_no_motor_getter") {
        LocalizedStatus("status.freecam_no_motor_getter", "PlayerRig.get_motor was not found.");
    } else if (message == "status.freecam_no_core_image") {
        LocalizedStatus("status.freecam_no_core_image",
                        "UnityEngine.CoreModule was not found.");
    } else if (message == "status.freecam_no_unity_classes") {
        LocalizedStatus("status.freecam_no_unity_classes",
                        "UnityEngine Transform/Behaviour classes were not found.");
    } else if (message == "status.freecam_no_unity_methods") {
        LocalizedStatus("status.freecam_no_unity_methods",
                        "UnityEngine Transform/Behaviour methods were not found.");
    } else if (message == "status.freecam_no_camera") {
        LocalizedStatus("status.freecam_no_camera",
                        "No player camera found. Enter a flight first.");
    } else if (message == "status.freecam_no_look") {
        LocalizedStatus("status.freecam_no_look",
                        "PlayerLook was not found on the local player.");
    } else if (message == "status.freecam_transform") {
        LocalizedStatus("status.freecam_transform",
                        "Reading or writing the camera transform failed.");
    } else if (message == "status.freecam_components") {
        LocalizedStatus("status.freecam_components",
                        "Switching the player control components failed.");
    } else if (message == "status.freecam_failed") {
        LocalizedStatus("status.freecam_failed", "Free camera failed to start.");
    } else if (!message.empty()) {
        // 宿主返回的运行时诊断文本（方法名候选等），原样显示便于排查。
        g_status.assign(message.data(), message.size());
    } else {
        LocalizedStatus("status.failed", "Free camera operation failed.");
    }
}

bool SaveSettings() {
    if (g_config == nullptr) return false;
    char document[192]{};
    const int written = std::snprintf(
        document, sizeof(document),
        "{\"hotkey\":%u,\"speed\":%.3f,\"sensitivity\":%.3f}",
        static_cast<unsigned>(g_hotkey.load(std::memory_order_relaxed)),
        static_cast<double>(g_speed.load(std::memory_order_relaxed)),
        static_cast<double>(g_sensitivity.load(std::memory_order_relaxed)));
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
            g_json->user,
            {document.data(), copied}, &root))) {
        return;
    }
    double hotkey = 0.0;
    double speed = 0.0;
    double sensitivity = 0.0;
    if (ReadNumber(root, "hotkey", &hotkey) && hotkey >= 1.0 && hotkey <= 254.0) {
        g_hotkey.store(static_cast<std::uint32_t>(hotkey), std::memory_order_relaxed);
    }
    if (ReadNumber(root, "speed", &speed) && speed >= 0.5 && speed <= 200.0) {
        g_speed.store(static_cast<float>(speed), std::memory_order_relaxed);
    }
    if (ReadNumber(root, "sensitivity", &sensitivity) &&
        sensitivity >= 0.01 && sensitivity <= 2.0) {
        g_sensitivity.store(static_cast<float>(sensitivity), std::memory_order_relaxed);
    }
    static_cast<void>(g_json->release(g_json->user, root));
}

void Deactivate() {
    if (g_freecam != nullptr && g_active) {
        static_cast<void>(g_freecam->set_active(g_freecam->user, 0));
    }
    g_active = false;
}

void Toggle() {
    QueryServices();
    if (g_freecam == nullptr) {
        LocalizedStatus("status.unavailable",
                  "The game is not ready yet. Enter a flight first.");
        return;
    }
    const bool next = !g_active;
    const AnomalyStatusV1 status =
        g_freecam->set_active(g_freecam->user, next ? 1 : 0);
    if (!Succeeded(status)) {
        g_active = false;
        SetStatusFromService(status);
        return;
    }
    g_active = next;
    if (next) {
        LocalizedStatus("status.active",
                  "Free camera enabled: WASD move, Space up, Shift down, mouse look.");
    } else {
        LocalizedStatus("status.inactive", "Free camera is off.");
    }
}

void StartCapture(const AnomalyInputSnapshotV1& snapshot) {
    g_capture_mode = true;
    std::memcpy(g_previous_keys, snapshot.keys, sizeof(g_previous_keys));
    LocalizedStatus("hotkey.capture", "Press any key (ESC cancels)...");
}

void DetectBinding(const AnomalyInputSnapshotV1& snapshot) {
    for (std::uint32_t virtual_key = 1; virtual_key < 255; ++virtual_key) {
        if (IsModifierKey(virtual_key)) continue;
        const bool down = KeyDown(snapshot, virtual_key);
        const bool was_down =
            (g_previous_keys[virtual_key / 8U] &
             static_cast<std::uint8_t>(1U << (virtual_key % 8U))) != 0;
        if (!down || was_down) continue;
        if (virtual_key == kVkEscape) {
            g_capture_mode = false;
            LocalizedStatus(g_active ? "status.active" : "status.inactive",
                      g_active ? "Free camera enabled." : "Free camera is off.");
        } else {
            g_hotkey.store(virtual_key, std::memory_order_relaxed);
            g_capture_mode = false;
            MarkSettingsDirty();
            LocalizedStatus(g_active ? "status.active" : "status.inactive",
                      g_active ? "Free camera enabled." : "Free camera is off.");
        }
        break;
    }
    std::memcpy(g_previous_keys, snapshot.keys, sizeof(g_previous_keys));
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    g_host = host;
    g_localizer = anomaly::sdk::Localizer(host);
    const anomaly::sdk::Host view(host);
    const auto input = view.Query<AnomalyInputServiceV1>(
        ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    const auto config = view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    const auto json = view.Query<AnomalyJsonServiceV1>(
        ANOMALY_JSON_SERVICE_V1_ID, ANOMALY_JSON_SERVICE_V1_VERSION);
    const auto scheduler = view.Query<AnomalySchedulerServiceV1>(
        ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    if (!input || !config || !json || !scheduler) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_input = input.get();
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
    LocalizedStatus("status.inactive", "Free camera is off.");
    *context = &g_speed;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    Deactivate();
    if (g_dirty.load(std::memory_order_acquire)) {
        if (SaveSettings()) g_dirty.store(false, std::memory_order_release);
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    Deactivate();
    g_host = nullptr;
    g_input = nullptr;
    g_config = nullptr;
    g_json = nullptr;
    g_scheduler = nullptr;
    g_freecam = nullptr;
    g_config_schema = {};
    g_status.clear();
}

void ANOMALY_CALL Update(void*, double) {
    QueryServices();
    if (g_input == nullptr) return;
    AnomalyInputSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (!Succeeded(g_input->snapshot(g_input->user, &snapshot))) return;
    if (g_capture_mode) {
        DetectBinding(snapshot);
        return;
    }
    const bool keyboard_captured =
        (snapshot.capture_flags &
         (ANOMALY_INPUT_CAPTURE_V1_KEYBOARD | ANOMALY_INPUT_CAPTURE_V1_TEXT)) != 0;
    if (!keyboard_captured) {
        std::int32_t pressed = 0;
        if (Succeeded(g_input->was_pressed(
                g_input->user, g_hotkey.load(std::memory_order_relaxed), &pressed)) &&
            pressed != 0) {
            Toggle();
        }
    }
    std::memcpy(g_previous_keys, snapshot.keys, sizeof(g_previous_keys));
    if (!g_active || g_freecam == nullptr) return;

    float forward = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    if (!keyboard_captured) {
        forward = (KeyDown(snapshot, 'W') ? 1.0f : 0.0f) -
            (KeyDown(snapshot, 'S') ? 1.0f : 0.0f);
        right = (KeyDown(snapshot, 'D') ? 1.0f : 0.0f) -
            (KeyDown(snapshot, 'A') ? 1.0f : 0.0f);
        up = (KeyDown(snapshot, kVkSpace) ? 1.0f : 0.0f) -
            (ShiftDown(snapshot) ? 1.0f : 0.0f);
    }
    float yaw_delta = 0.0f;
    float pitch_delta = 0.0f;
    if ((snapshot.capture_flags & ANOMALY_INPUT_CAPTURE_V1_MOUSE) == 0 &&
        InputHasMouseDelta(g_input)) {
        float delta_x = 0.0f;
        float delta_y = 0.0f;
        if (Succeeded(g_input->mouse_delta(
                g_input->user, &delta_x, &delta_y))) {
            const float sensitivity =
                g_sensitivity.load(std::memory_order_relaxed);
            yaw_delta = delta_x * sensitivity;
            pitch_delta = delta_y * sensitivity;
        }
    }
    const AnomalyStatusV1 status = g_freecam->set_input(
        g_freecam->user, forward, right, up, yaw_delta, pitch_delta,
        g_speed.load(std::memory_order_relaxed));
    if (!Succeeded(status)) {
        g_active = false;
        SetStatusFromService(status);
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) return;
    int open = 1;
    const std::string title = g_localizer.Text("window.title", "Free Camera");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    const std::string status = g_status.empty()
        ? g_localizer.Text("status.inactive", "Free camera is off.")
        : g_status;
    ui->text(ui->user, anomaly::sdk::StringView(status));
    ui->separator(ui->user);

    const std::string toggle_label = g_active
        ? g_localizer.Text("button.disable", "Disable free camera")
        : g_localizer.Text("button.enable", "Enable free camera");
    if (ui->button(ui->user, anomaly::sdk::StringView(toggle_label), -1.0f, 0.0f)) {
        Toggle();
    }

    const std::string hotkey_label = g_localizer.Text("hotkey.label", "Toggle hotkey");
    const std::string hotkey_name = KeyName(g_hotkey.load(std::memory_order_relaxed));
    const std::string hotkey_line = hotkey_label + ": " + hotkey_name;
    ui->text(ui->user, anomaly::sdk::StringView(hotkey_line));
    ui->same_line(ui->user, 0.0f, -1.0f);
    const std::string rebind_label = g_localizer.Text("hotkey.rebind", "Rebind");
    if (ui->button(ui->user, anomaly::sdk::StringView(rebind_label), 0.0f, 0.0f)) {
        g_capture_mode = false;
        if (g_input != nullptr) {
            AnomalyInputSnapshotV1 snapshot{};
            snapshot.struct_size = sizeof(snapshot);
            if (Succeeded(g_input->snapshot(g_input->user, &snapshot))) {
                StartCapture(snapshot);
            }
        }
    }
    if (g_capture_mode) {
        ui->text(ui->user, anomaly::sdk::StringView(
            g_localizer.Text("hotkey.capture", "Press any key (ESC cancels)...")));
    }

    float speed = g_speed.load(std::memory_order_relaxed);
    if (ui->slider_float(
            ui->user, anomaly::sdk::StringView(
                g_localizer.Text("speed.label", "Move speed")),
            &speed, 0.5f, 100.0f) != 0) {
        g_speed.store(speed, std::memory_order_relaxed);
        MarkSettingsDirty();
    }
    float sensitivity = g_sensitivity.load(std::memory_order_relaxed);
    if (ui->slider_float(
            ui->user, anomaly::sdk::StringView(
                g_localizer.Text("sensitivity.label", "Mouse sensitivity")),
            &sensitivity, 0.01f, 1.0f) != 0) {
        g_sensitivity.store(sensitivity, std::memory_order_relaxed);
        MarkSettingsDirty();
    }

    ui->separator(ui->user);
    const std::string help = g_localizer.Text(
        "help.controls",
        "WASD to move, Space to rise, Shift to descend; the mouse turns the view. "
        "Movement pauses while the overlay captures input.");
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
        anomaly::sdk::StringView("mayday.freecam"),
        anomaly::sdk::StringView("Free Camera"),
        anomaly::sdk::StringView("Mayday Loader"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
