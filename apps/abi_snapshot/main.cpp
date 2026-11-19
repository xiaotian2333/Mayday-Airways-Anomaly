#include "anomaly/sdk/anomaly_sdk.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <io.h>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32) || !defined(_WIN64)
#error "The SDK ABI snapshot is defined for Windows x64"
#endif

namespace {

struct Field final {
    std::string_view name;
    std::size_t offset;
};

void AppendJsonString(std::string& output, const std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"': output.append("\\\""); break;
        case '\\': output.append("\\\\"); break;
        case '\b': output.append("\\b"); break;
        case '\f': output.append("\\f"); break;
        case '\n': output.append("\\n"); break;
        case '\r': output.append("\\r"); break;
        case '\t': output.append("\\t"); break;
        default: {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20u) {
                output.append("\\u00");
                output.push_back(hex[(byte >> 4u) & 0x0fu]);
                output.push_back(hex[byte & 0x0fu]);
            } else {
                output.push_back(character);
            }
            break;
        }
        }
    }
    output.push_back('"');
}

void AppendUnsigned(std::string& output, const std::uint64_t value) {
    output.append(std::to_string(value));
}

void AppendStruct(
    std::string& output,
    const std::string_view name,
    const std::size_t size,
    const std::size_t alignment,
    const std::initializer_list<Field> fields,
    const bool last) {
    output.append("    ");
    AppendJsonString(output, name);
    output.append(": {\n      \"size\": ");
    AppendUnsigned(output, size);
    output.append(",\n      \"alignment\": ");
    AppendUnsigned(output, alignment);
    output.append(",\n      \"offsets\": {\n");
    auto iterator = fields.begin();
    while (iterator != fields.end()) {
        output.append("        ");
        AppendJsonString(output, iterator->name);
        output.append(": ");
        AppendUnsigned(output, iterator->offset);
        ++iterator;
        output.append(iterator == fields.end() ? "\n" : ",\n");
    }
    output.append("      }\n    }");
    output.append(last ? "\n" : ",\n");
}

void AppendEnum(
    std::string& output,
    const std::string_view name,
    const std::size_t size,
    const std::size_t alignment,
    const std::initializer_list<std::pair<std::string_view, std::int64_t>> values,
    const bool last) {
    output.append("    ");
    AppendJsonString(output, name);
    output.append(": {\n      \"size\": ");
    AppendUnsigned(output, size);
    output.append(",\n      \"alignment\": ");
    AppendUnsigned(output, alignment);
    output.append(",\n      \"values\": {\n");
    auto iterator = values.begin();
    while (iterator != values.end()) {
        output.append("        ");
        AppendJsonString(output, iterator->first);
        output.append(": ");
        output.append(std::to_string(iterator->second));
        ++iterator;
        output.append(iterator == values.end() ? "\n" : ",\n");
    }
    output.append("      }\n    }");
    output.append(last ? "\n" : ",\n");
}

void AppendService(
    std::string& output,
    const std::string_view id,
    const std::uint32_t version,
    const std::string_view table,
    const bool last) {
    output.append("    { \"id\": ");
    AppendJsonString(output, id);
    output.append(", \"version\": ");
    AppendUnsigned(output, version);
    output.append(", \"table\": ");
    AppendJsonString(output, table);
    output.append(last ? " }\n" : " },\n");
}

std::string BuildSnapshot() {
    static_assert(sizeof(void*) == 8u);
    static_assert(sizeof(std::size_t) == 8u);
    static_assert(sizeof(std::uintptr_t) == 8u);

    std::string output;
    output.reserve(12'000u);
    output.append("{\n");
    output.append("  \"schema_version\": 1,\n");
    output.append("  \"target\": {\n");
    output.append("    \"os\": \"windows\",\n");
    output.append("    \"architecture\": \"x86_64\",\n");
    output.append("    \"calling_convention\": \"cdecl\",\n");
    output.append("    \"pointer_size\": 8,\n");
    output.append("    \"size_t_size\": 8\n");
    output.append("  },\n");
    output.append("  \"constants\": {\n");
    output.append("    \"ANOMALY_SDK_VERSION_MAJOR\": ");
    AppendUnsigned(output, ANOMALY_SDK_VERSION_MAJOR);
    output.append(",\n    \"ANOMALY_SDK_VERSION_MINOR\": ");
    AppendUnsigned(output, ANOMALY_SDK_VERSION_MINOR);
    output.append(",\n    \"ANOMALY_SDK_VERSION_PATCH\": ");
    AppendUnsigned(output, ANOMALY_SDK_VERSION_PATCH);
    output.append(",\n    \"ANOMALY_SDK_VERSION_STRING\": ");
    AppendJsonString(output, ANOMALY_SDK_VERSION_STRING);
    output.append(",\n    \"ANOMALY_PLUGIN_API_V1_MAJOR\": ");
    AppendUnsigned(output, ANOMALY_PLUGIN_API_V1_MAJOR);
    output.append(",\n    \"ANOMALY_PLUGIN_API_V1_MINOR\": ");
    AppendUnsigned(output, ANOMALY_PLUGIN_API_V1_MINOR);
    output.append(",\n    \"ANOMALY_PLUGIN_V1_ENTRY_NAME\": ");
    AppendJsonString(output, ANOMALY_PLUGIN_V1_ENTRY_NAME);
    output.append(",\n    \"ANOMALY_CORE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_CORE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_CONFIG_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_STORAGE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_STORAGE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_JSON_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_JSON_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_SCHEDULER_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_IPC_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_IPC_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_WEBSOCKET_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_WEBSOCKET_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_IPC_SCHEMA_HASH_V1_SIZE\": ");
    AppendUnsigned(output, ANOMALY_IPC_SCHEMA_HASH_V1_SIZE);
    output.append(",\n    \"ANOMALY_COMMANDS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_COMMANDS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_SIGNATURE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_SIGNATURE_SERVICE_V2_VERSION\": ");
    AppendUnsigned(output, ANOMALY_SIGNATURE_SERVICE_V2_VERSION);
    output.append(",\n    \"ANOMALY_SIGNATURE_SERVICE_V3_VERSION\": ");
    AppendUnsigned(output, ANOMALY_SIGNATURE_SERVICE_V3_VERSION);
    output.append(",\n    \"ANOMALY_SIGNATURE_BATCH_MAX_PATTERNS_V3\": ");
    AppendUnsigned(output, ANOMALY_SIGNATURE_BATCH_MAX_PATTERNS_V3);
    output.append(",\n    \"ANOMALY_SIGNATURE_LAYOUT_MAX_FIELDS_V2\": ");
    AppendUnsigned(output, ANOMALY_SIGNATURE_LAYOUT_MAX_FIELDS_V2);
    output.append(",\n    \"ANOMALY_SIGNATURE_LAYOUT_MAX_WITNESSES_V2\": ");
    AppendUnsigned(output, ANOMALY_SIGNATURE_LAYOUT_MAX_WITNESSES_V2);
    output.append(",\n    \"ANOMALY_HOOK_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_HOOK_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_PATCH_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_PATCH_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_UI_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_UI_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_WINDOW_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_WINDOW_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_FONT_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_FONT_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_TEXTURE_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_TEXTURE_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_INPUT_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_INPUT_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_LOCALIZATION_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_LOCALIZATION_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_MAYDAY_FREECAM_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_MAYDAY_FREECAM_SERVICE_V1_VERSION);
    output.append(",\n    \"ANOMALY_MAYDAY_WEATHER_SERVICE_V1_VERSION\": ");
    AppendUnsigned(output, ANOMALY_MAYDAY_WEATHER_SERVICE_V1_VERSION);
    output.append("\n  },\n");

    output.append("  \"enums\": {\n");
    AppendEnum(
        output,
        "AnomalyStatusCodeV1",
        sizeof(AnomalyStatusCodeV1),
        alignof(AnomalyStatusCodeV1),
        {{"ANOMALY_STATUS_V1_OK", ANOMALY_STATUS_V1_OK},
         {"ANOMALY_STATUS_V1_INVALID_ARGUMENT", ANOMALY_STATUS_V1_INVALID_ARGUMENT},
         {"ANOMALY_STATUS_V1_UNAVAILABLE", ANOMALY_STATUS_V1_UNAVAILABLE},
         {"ANOMALY_STATUS_V1_NOT_FOUND", ANOMALY_STATUS_V1_NOT_FOUND},
         {"ANOMALY_STATUS_V1_BUFFER_TOO_SMALL", ANOMALY_STATUS_V1_BUFFER_TOO_SMALL},
         {"ANOMALY_STATUS_V1_FAILED", ANOMALY_STATUS_V1_FAILED},
         {"ANOMALY_STATUS_V1_TIMEOUT", ANOMALY_STATUS_V1_TIMEOUT},
         {"ANOMALY_STATUS_V1_PERMISSION_DENIED", ANOMALY_STATUS_V1_PERMISSION_DENIED},
         {"ANOMALY_STATUS_V1_CONFLICT", ANOMALY_STATUS_V1_CONFLICT},
         {"ANOMALY_STATUS_V1_CANCELLED", ANOMALY_STATUS_V1_CANCELLED}},
        false);
    AppendEnum(
        output,
        "AnomalyFeatureStateV1",
        sizeof(AnomalyFeatureStateV1),
        alignof(AnomalyFeatureStateV1),
        {{"ANOMALY_FEATURE_V1_UNAVAILABLE", ANOMALY_FEATURE_V1_UNAVAILABLE},
         {"ANOMALY_FEATURE_V1_AVAILABLE", ANOMALY_FEATURE_V1_AVAILABLE}},
        false);
    AppendEnum(
        output,
        "AnomalyJsonKindV1",
        sizeof(AnomalyJsonKindV1),
        alignof(AnomalyJsonKindV1),
        {{"ANOMALY_JSON_V1_NULL", ANOMALY_JSON_V1_NULL},
         {"ANOMALY_JSON_V1_BOOLEAN", ANOMALY_JSON_V1_BOOLEAN},
         {"ANOMALY_JSON_V1_NUMBER", ANOMALY_JSON_V1_NUMBER},
         {"ANOMALY_JSON_V1_STRING", ANOMALY_JSON_V1_STRING},
         {"ANOMALY_JSON_V1_ARRAY", ANOMALY_JSON_V1_ARRAY},
         {"ANOMALY_JSON_V1_OBJECT", ANOMALY_JSON_V1_OBJECT}},
        false);
    AppendEnum(
        output,
        "AnomalyEspBoxFlagsV1",
        sizeof(AnomalyEspBoxFlagsV1),
        alignof(AnomalyEspBoxFlagsV1),
        {{"ANOMALY_ESP_BOX_V1_NONE", ANOMALY_ESP_BOX_V1_NONE},
         {"ANOMALY_ESP_BOX_V1_OUTLINE", ANOMALY_ESP_BOX_V1_OUTLINE}},
        false);
    AppendEnum(
        output,
        "AnomalyNotificationSeverityV1",
        sizeof(AnomalyNotificationSeverityV1),
        alignof(AnomalyNotificationSeverityV1),
        {{"ANOMALY_NOTIFICATION_V1_INFO", ANOMALY_NOTIFICATION_V1_INFO},
         {"ANOMALY_NOTIFICATION_V1_WARNING", ANOMALY_NOTIFICATION_V1_WARNING},
         {"ANOMALY_NOTIFICATION_V1_ERROR", ANOMALY_NOTIFICATION_V1_ERROR}},
        false);
    AppendEnum(
        output,
        "AnomalySignatureMemberAccessV2",
        sizeof(AnomalySignatureMemberAccessV2),
        alignof(AnomalySignatureMemberAccessV2),
        {{"ANOMALY_SIGNATURE_MEMBER_READ_V2", ANOMALY_SIGNATURE_MEMBER_READ_V2},
         {"ANOMALY_SIGNATURE_MEMBER_WRITE_V2", ANOMALY_SIGNATURE_MEMBER_WRITE_V2},
         {"ANOMALY_SIGNATURE_MEMBER_ADDRESS_V2", ANOMALY_SIGNATURE_MEMBER_ADDRESS_V2}},
        false);
    AppendEnum(
        output,
        "AnomalyHookKindV1",
        sizeof(AnomalyHookKindV1),
        alignof(AnomalyHookKindV1),
        {{"ANOMALY_HOOK_V1_FUNCTION", ANOMALY_HOOK_V1_FUNCTION},
         {"ANOMALY_HOOK_V1_IAT", ANOMALY_HOOK_V1_IAT},
         {"ANOMALY_HOOK_V1_EXPORT", ANOMALY_HOOK_V1_EXPORT},
         {"ANOMALY_HOOK_V1_VTABLE", ANOMALY_HOOK_V1_VTABLE}},
        false);
    AppendEnum(
        output,
        "AnomalyUiFrameStateV1",
        sizeof(AnomalyUiFrameStateV1),
        alignof(AnomalyUiFrameStateV1),
        {{"ANOMALY_UI_FRAME_V1_NONE", ANOMALY_UI_FRAME_V1_NONE},
         {"ANOMALY_UI_FRAME_V1_ITEM_HOVERED", ANOMALY_UI_FRAME_V1_ITEM_HOVERED},
         {"ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED", ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED},
         {"ANOMALY_UI_FRAME_V1_ITEM_ACTIVE", ANOMALY_UI_FRAME_V1_ITEM_ACTIVE},
         {"ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE", ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE},
         {"ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD",
          ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD},
         {"ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT", ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT}},
        false);
    AppendEnum(
        output,
        "AnomalyUiTextInputFlagsV1",
        sizeof(AnomalyUiTextInputFlagsV1),
        alignof(AnomalyUiTextInputFlagsV1),
        {{"ANOMALY_UI_TEXT_INPUT_V1_NONE", ANOMALY_UI_TEXT_INPUT_V1_NONE},
         {"ANOMALY_UI_TEXT_INPUT_V1_DIGITS", ANOMALY_UI_TEXT_INPUT_V1_DIGITS},
         {"ANOMALY_UI_TEXT_INPUT_V1_PASSWORD", ANOMALY_UI_TEXT_INPUT_V1_PASSWORD}},
        false);
    AppendEnum(
        output,
        "AnomalyUiTableFlagsV1",
        sizeof(AnomalyUiTableFlagsV1),
        alignof(AnomalyUiTableFlagsV1),
        {{"ANOMALY_UI_TABLE_V1_NONE", ANOMALY_UI_TABLE_V1_NONE},
         {"ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT",
          ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT},
         {"ANOMALY_UI_TABLE_V1_SIZING_STRETCH_PROP",
          ANOMALY_UI_TABLE_V1_SIZING_STRETCH_PROP}},
        false);
    AppendEnum(
        output,
        "AnomalyUiChildFlagsV1",
        sizeof(AnomalyUiChildFlagsV1),
        alignof(AnomalyUiChildFlagsV1),
        {{"ANOMALY_UI_CHILD_V1_NONE", ANOMALY_UI_CHILD_V1_NONE},
         {"ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y", ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y}},
        false);
    AppendEnum(
        output,
        "AnomalyUiTabBarFlagsV1",
        sizeof(AnomalyUiTabBarFlagsV1),
        alignof(AnomalyUiTabBarFlagsV1),
        {{"ANOMALY_UI_TAB_BAR_V1_NONE", ANOMALY_UI_TAB_BAR_V1_NONE},
         {"ANOMALY_UI_TAB_BAR_V1_REORDERABLE", ANOMALY_UI_TAB_BAR_V1_REORDERABLE},
         {"ANOMALY_UI_TAB_BAR_V1_AUTO_SELECT_NEW_TABS",
          ANOMALY_UI_TAB_BAR_V1_AUTO_SELECT_NEW_TABS},
         {"ANOMALY_UI_TAB_BAR_V1_NO_TAB_LIST_SCROLLING_BUTTONS",
          ANOMALY_UI_TAB_BAR_V1_NO_TAB_LIST_SCROLLING_BUTTONS},
         {"ANOMALY_UI_TAB_BAR_V1_NO_TOOLTIP", ANOMALY_UI_TAB_BAR_V1_NO_TOOLTIP},
         {"ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN",
          ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN},
         {"ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_SCROLL",
          ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_SCROLL}},
        false);
    AppendEnum(
        output,
        "AnomalyUiTabItemFlagsV1",
        sizeof(AnomalyUiTabItemFlagsV1),
        alignof(AnomalyUiTabItemFlagsV1),
        {{"ANOMALY_UI_TAB_ITEM_V1_NONE", ANOMALY_UI_TAB_ITEM_V1_NONE},
         {"ANOMALY_UI_TAB_ITEM_V1_UNSAVED_DOCUMENT",
          ANOMALY_UI_TAB_ITEM_V1_UNSAVED_DOCUMENT},
         {"ANOMALY_UI_TAB_ITEM_V1_SET_SELECTED", ANOMALY_UI_TAB_ITEM_V1_SET_SELECTED},
         {"ANOMALY_UI_TAB_ITEM_V1_NO_CLOSE_WITH_MIDDLE_MOUSE_BUTTON",
          ANOMALY_UI_TAB_ITEM_V1_NO_CLOSE_WITH_MIDDLE_MOUSE_BUTTON},
         {"ANOMALY_UI_TAB_ITEM_V1_NO_PUSH_ID", ANOMALY_UI_TAB_ITEM_V1_NO_PUSH_ID},
         {"ANOMALY_UI_TAB_ITEM_V1_NO_TOOLTIP", ANOMALY_UI_TAB_ITEM_V1_NO_TOOLTIP}},
        false);
    AppendEnum(
        output,
        "AnomalyUiGlassVariantV1",
        sizeof(AnomalyUiGlassVariantV1),
        alignof(AnomalyUiGlassVariantV1),
        {{"ANOMALY_UI_GLASS_V1_REGULAR", ANOMALY_UI_GLASS_V1_REGULAR},
         {"ANOMALY_UI_GLASS_V1_CLEAR", ANOMALY_UI_GLASS_V1_CLEAR},
         {"ANOMALY_UI_GLASS_V1_MATERIAL", ANOMALY_UI_GLASS_V1_MATERIAL},
         {"ANOMALY_UI_GLASS_V1_THUMB", ANOMALY_UI_GLASS_V1_THUMB}},
        false);
    AppendEnum(
        output,
        "AnomalyUiGlassFlagsV1",
        sizeof(AnomalyUiGlassFlagsV1),
        alignof(AnomalyUiGlassFlagsV1),
        {{"ANOMALY_UI_GLASS_V1_NONE", ANOMALY_UI_GLASS_V1_NONE},
         {"ANOMALY_UI_GLASS_V1_NO_SHADOW", ANOMALY_UI_GLASS_V1_NO_SHADOW},
         {"ANOMALY_UI_GLASS_V1_NO_RIM", ANOMALY_UI_GLASS_V1_NO_RIM}},
        false);
    AppendEnum(
        output,
        "AnomalyWindowFlagsV1",
        sizeof(AnomalyWindowFlagsV1),
        alignof(AnomalyWindowFlagsV1),
        {{"ANOMALY_WINDOW_V1_NONE", ANOMALY_WINDOW_V1_NONE},
         {"ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS", ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS},
         {"ANOMALY_WINDOW_V1_NO_COLLAPSE", ANOMALY_WINDOW_V1_NO_COLLAPSE}},
        false);
    AppendEnum(
        output,
        "AnomalyGlyphRangeV1",
        sizeof(AnomalyGlyphRangeV1),
        alignof(AnomalyGlyphRangeV1),
        {{"ANOMALY_GLYPH_RANGE_V1_DEFAULT", ANOMALY_GLYPH_RANGE_V1_DEFAULT},
         {"ANOMALY_GLYPH_RANGE_V1_LATIN", ANOMALY_GLYPH_RANGE_V1_LATIN},
         {"ANOMALY_GLYPH_RANGE_V1_CYRILLIC", ANOMALY_GLYPH_RANGE_V1_CYRILLIC},
         {"ANOMALY_GLYPH_RANGE_V1_JAPANESE", ANOMALY_GLYPH_RANGE_V1_JAPANESE},
         {"ANOMALY_GLYPH_RANGE_V1_CHINESE_FULL", ANOMALY_GLYPH_RANGE_V1_CHINESE_FULL},
         {"ANOMALY_GLYPH_RANGE_V1_PRIVATE_USE", ANOMALY_GLYPH_RANGE_V1_PRIVATE_USE}},
        false);
    AppendEnum(
        output,
        "AnomalyFontStateFlagsV1",
        sizeof(AnomalyFontStateFlagsV1),
        alignof(AnomalyFontStateFlagsV1),
        {{"ANOMALY_FONT_STATE_V1_NONE", ANOMALY_FONT_STATE_V1_NONE},
         {"ANOMALY_FONT_STATE_V1_QUEUED", ANOMALY_FONT_STATE_V1_QUEUED},
         {"ANOMALY_FONT_STATE_V1_READY", ANOMALY_FONT_STATE_V1_READY},
         {"ANOMALY_FONT_STATE_V1_FAILED", ANOMALY_FONT_STATE_V1_FAILED},
         {"ANOMALY_FONT_STATE_V1_STALE_DEVICE", ANOMALY_FONT_STATE_V1_STALE_DEVICE}},
        false);
    AppendEnum(
        output,
        "AnomalyTextureFormatV1",
        sizeof(AnomalyTextureFormatV1),
        alignof(AnomalyTextureFormatV1),
        {{"ANOMALY_TEXTURE_FORMAT_V1_AUTO", ANOMALY_TEXTURE_FORMAT_V1_AUTO},
         {"ANOMALY_TEXTURE_FORMAT_V1_RGBA8", ANOMALY_TEXTURE_FORMAT_V1_RGBA8}},
        false);
    AppendEnum(
        output,
        "AnomalyTextureStateFlagsV1",
        sizeof(AnomalyTextureStateFlagsV1),
        alignof(AnomalyTextureStateFlagsV1),
        {{"ANOMALY_TEXTURE_STATE_V1_NONE", ANOMALY_TEXTURE_STATE_V1_NONE},
         {"ANOMALY_TEXTURE_STATE_V1_QUEUED", ANOMALY_TEXTURE_STATE_V1_QUEUED},
         {"ANOMALY_TEXTURE_STATE_V1_READY", ANOMALY_TEXTURE_STATE_V1_READY},
         {"ANOMALY_TEXTURE_STATE_V1_FAILED", ANOMALY_TEXTURE_STATE_V1_FAILED},
         {"ANOMALY_TEXTURE_STATE_V1_STALE_DEVICE", ANOMALY_TEXTURE_STATE_V1_STALE_DEVICE}},
        false);
    AppendEnum(
        output,
        "AnomalyInputModifiersV1",
        sizeof(AnomalyInputModifiersV1),
        alignof(AnomalyInputModifiersV1),
        {{"ANOMALY_INPUT_MODIFIER_V1_NONE", ANOMALY_INPUT_MODIFIER_V1_NONE},
         {"ANOMALY_INPUT_MODIFIER_V1_SHIFT", ANOMALY_INPUT_MODIFIER_V1_SHIFT},
         {"ANOMALY_INPUT_MODIFIER_V1_CONTROL", ANOMALY_INPUT_MODIFIER_V1_CONTROL},
         {"ANOMALY_INPUT_MODIFIER_V1_ALT", ANOMALY_INPUT_MODIFIER_V1_ALT},
         {"ANOMALY_INPUT_MODIFIER_V1_SUPER", ANOMALY_INPUT_MODIFIER_V1_SUPER}},
        false);
    AppendEnum(
        output,
        "AnomalyInputCaptureFlagsV1",
        sizeof(AnomalyInputCaptureFlagsV1),
        alignof(AnomalyInputCaptureFlagsV1),
        {{"ANOMALY_INPUT_CAPTURE_V1_NONE", ANOMALY_INPUT_CAPTURE_V1_NONE},
         {"ANOMALY_INPUT_CAPTURE_V1_MOUSE", ANOMALY_INPUT_CAPTURE_V1_MOUSE},
         {"ANOMALY_INPUT_CAPTURE_V1_KEYBOARD", ANOMALY_INPUT_CAPTURE_V1_KEYBOARD},
         {"ANOMALY_INPUT_CAPTURE_V1_TEXT", ANOMALY_INPUT_CAPTURE_V1_TEXT}},
        false);
    AppendEnum(
        output,
        "AnomalyHotkeyFlagsV1",
        sizeof(AnomalyHotkeyFlagsV1),
        alignof(AnomalyHotkeyFlagsV1),
        {{"ANOMALY_HOTKEY_V1_NONE", ANOMALY_HOTKEY_V1_NONE},
         {"ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS",
          ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS},
         {"ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED",
          ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED},
         {"ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED",
          ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED}},
        false);
    AppendEnum(
        output,
        "AnomalyCoreLogLevelV1",
        sizeof(AnomalyCoreLogLevelV1),
        alignof(AnomalyCoreLogLevelV1),
        {{"ANOMALY_CORE_LOG_LEVEL_V1_TRACE", ANOMALY_CORE_LOG_LEVEL_V1_TRACE},
         {"ANOMALY_CORE_LOG_LEVEL_V1_INFO", ANOMALY_CORE_LOG_LEVEL_V1_INFO},
         {"ANOMALY_CORE_LOG_LEVEL_V1_WARNING", ANOMALY_CORE_LOG_LEVEL_V1_WARNING},
         {"ANOMALY_CORE_LOG_LEVEL_V1_ERROR", ANOMALY_CORE_LOG_LEVEL_V1_ERROR}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcModeV1",
        sizeof(AnomalyIpcModeV1),
        alignof(AnomalyIpcModeV1),
        {{"ANOMALY_IPC_MODE_V1_SYNC_REQUEST", ANOMALY_IPC_MODE_V1_SYNC_REQUEST},
         {"ANOMALY_IPC_MODE_V1_ASYNC_REQUEST", ANOMALY_IPC_MODE_V1_ASYNC_REQUEST},
         {"ANOMALY_IPC_MODE_V1_EVENT", ANOMALY_IPC_MODE_V1_EVENT}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcAffinityV1",
        sizeof(AnomalyIpcAffinityV1),
        alignof(AnomalyIpcAffinityV1),
        {{"ANOMALY_IPC_AFFINITY_V1_CALLER", ANOMALY_IPC_AFFINITY_V1_CALLER},
         {"ANOMALY_IPC_AFFINITY_V1_WORKER", ANOMALY_IPC_AFFINITY_V1_WORKER},
         {"ANOMALY_IPC_AFFINITY_V1_LIFECYCLE", ANOMALY_IPC_AFFINITY_V1_LIFECYCLE},
         {"ANOMALY_IPC_AFFINITY_V1_GAME", ANOMALY_IPC_AFFINITY_V1_GAME},
         {"ANOMALY_IPC_AFFINITY_V1_RENDER", ANOMALY_IPC_AFFINITY_V1_RENDER}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcReentrancyV1",
        sizeof(AnomalyIpcReentrancyV1),
        alignof(AnomalyIpcReentrancyV1),
        {{"ANOMALY_IPC_REENTRANCY_V1_REJECT", ANOMALY_IPC_REENTRANCY_V1_REJECT},
         {"ANOMALY_IPC_REENTRANCY_V1_ALLOW", ANOMALY_IPC_REENTRANCY_V1_ALLOW}},
        false);
    AppendEnum(
        output,
        "AnomalyIpcErrorV1",
        sizeof(AnomalyIpcErrorV1),
        alignof(AnomalyIpcErrorV1),
        {{"ANOMALY_IPC_ERROR_V1_NONE", ANOMALY_IPC_ERROR_V1_NONE},
         {"ANOMALY_IPC_ERROR_V1_PROVIDER_MISSING", ANOMALY_IPC_ERROR_V1_PROVIDER_MISSING},
         {"ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH", ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH},
         {"ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH", ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH},
         {"ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE", ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE},
         {"ANOMALY_IPC_ERROR_V1_TIMEOUT", ANOMALY_IPC_ERROR_V1_TIMEOUT},
         {"ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE", ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE},
         {"ANOMALY_IPC_ERROR_V1_QUEUE_FULL", ANOMALY_IPC_ERROR_V1_QUEUE_FULL},
         {"ANOMALY_IPC_ERROR_V1_STALE_GENERATION", ANOMALY_IPC_ERROR_V1_STALE_GENERATION},
         {"ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED", ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED}},
        true);
    output.append("  },\n");

    output.append("  \"structs\": {\n");
    AppendStruct(
        output,
        "AnomalyStringViewV1",
        sizeof(AnomalyStringViewV1),
        alignof(AnomalyStringViewV1),
        {{"data", offsetof(AnomalyStringViewV1, data)}, {"size", offsetof(AnomalyStringViewV1, size)}},
        false);
    AppendStruct(
        output,
        "AnomalyByteSpanV1",
        sizeof(AnomalyByteSpanV1),
        alignof(AnomalyByteSpanV1),
        {{"data", offsetof(AnomalyByteSpanV1, data)}, {"size", offsetof(AnomalyByteSpanV1, size)}},
        false);
    AppendStruct(
        output,
        "AnomalyMutableByteSpanV1",
        sizeof(AnomalyMutableByteSpanV1),
        alignof(AnomalyMutableByteSpanV1),
        {{"data", offsetof(AnomalyMutableByteSpanV1, data)},
         {"size", offsetof(AnomalyMutableByteSpanV1, size)}},
        false);
    AppendStruct(
        output,
        "AnomalyStatusV1",
        sizeof(AnomalyStatusV1),
        alignof(AnomalyStatusV1),
        {{"code", offsetof(AnomalyStatusV1, code)},
         {"reserved", offsetof(AnomalyStatusV1, reserved)},
         {"message", offsetof(AnomalyStatusV1, message)}},
        false);
    AppendStruct(
        output,
        "AnomalyAllocatorV1",
        sizeof(AnomalyAllocatorV1),
        alignof(AnomalyAllocatorV1),
        {{"struct_size", offsetof(AnomalyAllocatorV1, struct_size)},
         {"reserved", offsetof(AnomalyAllocatorV1, reserved)},
         {"user", offsetof(AnomalyAllocatorV1, user)},
         {"allocate", offsetof(AnomalyAllocatorV1, allocate)},
         {"reallocate", offsetof(AnomalyAllocatorV1, reallocate)},
         {"release", offsetof(AnomalyAllocatorV1, release)}},
        false);
    AppendStruct(
        output,
        "AnomalyGenerationHandleV1",
        sizeof(AnomalyGenerationHandleV1),
        alignof(AnomalyGenerationHandleV1),
        {{"id", offsetof(AnomalyGenerationHandleV1, id)},
         {"generation", offsetof(AnomalyGenerationHandleV1, generation)}},
        false);
    AppendStruct(
        output,
        "AnomalyHostApiV1",
        sizeof(AnomalyHostApiV1),
        alignof(AnomalyHostApiV1),
        {{"struct_size", offsetof(AnomalyHostApiV1, struct_size)},
         {"api_major", offsetof(AnomalyHostApiV1, api_major)},
         {"api_minor", offsetof(AnomalyHostApiV1, api_minor)},
         {"host_context", offsetof(AnomalyHostApiV1, host_context)},
         {"allocator", offsetof(AnomalyHostApiV1, allocator)},
         {"query_service", offsetof(AnomalyHostApiV1, query_service)}},
        false);
    AppendStruct(
        output,
        "AnomalyPluginDescriptorV1",
        sizeof(AnomalyPluginDescriptorV1),
        alignof(AnomalyPluginDescriptorV1),
        {{"struct_size", offsetof(AnomalyPluginDescriptorV1, struct_size)},
         {"api_major", offsetof(AnomalyPluginDescriptorV1, api_major)},
         {"api_minor", offsetof(AnomalyPluginDescriptorV1, api_minor)},
         {"id", offsetof(AnomalyPluginDescriptorV1, id)},
         {"name", offsetof(AnomalyPluginDescriptorV1, name)},
         {"author", offsetof(AnomalyPluginDescriptorV1, author)},
         {"version", offsetof(AnomalyPluginDescriptorV1, version)},
         {"on_load", offsetof(AnomalyPluginDescriptorV1, on_load)},
         {"on_start", offsetof(AnomalyPluginDescriptorV1, on_start)},
         {"on_stop", offsetof(AnomalyPluginDescriptorV1, on_stop)},
         {"on_unload", offsetof(AnomalyPluginDescriptorV1, on_unload)},
         {"on_update", offsetof(AnomalyPluginDescriptorV1, on_update)},
         {"on_draw", offsetof(AnomalyPluginDescriptorV1, on_draw)},
         {"on_overlay", offsetof(AnomalyPluginDescriptorV1, on_overlay)}},
        false);
    AppendStruct(
        output,
        "AnomalyCoreServiceV1",
        sizeof(AnomalyCoreServiceV1),
        alignof(AnomalyCoreServiceV1),
        {{"struct_size", offsetof(AnomalyCoreServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyCoreServiceV1, service_version)},
         {"user", offsetof(AnomalyCoreServiceV1, user)},
         {"log", offsetof(AnomalyCoreServiceV1, log)},
         {"read_memory", offsetof(AnomalyCoreServiceV1, read_memory)},
         {"write_memory", offsetof(AnomalyCoreServiceV1, write_memory)},
         {"plugin_directory", offsetof(AnomalyCoreServiceV1, plugin_directory)}},
        false);
    AppendStruct(
        output,
        "AnomalyPluginStateServiceV1",
        sizeof(AnomalyPluginStateServiceV1),
        alignof(AnomalyPluginStateServiceV1),
        {{"struct_size", offsetof(AnomalyPluginStateServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyPluginStateServiceV1, service_version)},
         {"user", offsetof(AnomalyPluginStateServiceV1, user)},
         {"directory", offsetof(AnomalyPluginStateServiceV1, directory)}},
        false);
    AppendStruct(
        output,
        "AnomalyRuntimeInfoV1",
        sizeof(AnomalyRuntimeInfoV1),
        alignof(AnomalyRuntimeInfoV1),
        {{"struct_size", offsetof(AnomalyRuntimeInfoV1, struct_size)},
         {"runtime_version_major", offsetof(AnomalyRuntimeInfoV1, runtime_version_major)},
         {"runtime_version_minor", offsetof(AnomalyRuntimeInfoV1, runtime_version_minor)},
         {"runtime_version_patch", offsetof(AnomalyRuntimeInfoV1, runtime_version_patch)},
         {"process_id", offsetof(AnomalyRuntimeInfoV1, process_id)},
         {"thread_id", offsetof(AnomalyRuntimeInfoV1, thread_id)},
         {"uptime_milliseconds", offsetof(AnomalyRuntimeInfoV1, uptime_milliseconds)},
         {"plugin_generation", offsetof(AnomalyRuntimeInfoV1, plugin_generation)}},
        false);
    AppendStruct(
        output,
        "AnomalyConfigServiceV1",
        sizeof(AnomalyConfigServiceV1),
        alignof(AnomalyConfigServiceV1),
        {{"struct_size", offsetof(AnomalyConfigServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyConfigServiceV1, service_version)},
         {"user", offsetof(AnomalyConfigServiceV1, user)},
         {"register_schema", offsetof(AnomalyConfigServiceV1, register_schema)},
         {"unregister_schema", offsetof(AnomalyConfigServiceV1, unregister_schema)},
         {"read", offsetof(AnomalyConfigServiceV1, read)},
         {"write_atomic", offsetof(AnomalyConfigServiceV1, write_atomic)},
         {"migrate", offsetof(AnomalyConfigServiceV1, migrate)}},
        false);
    AppendStruct(
        output,
        "AnomalyStorageServiceV1",
        sizeof(AnomalyStorageServiceV1),
        alignof(AnomalyStorageServiceV1),
        {{"struct_size", offsetof(AnomalyStorageServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyStorageServiceV1, service_version)},
         {"user", offsetof(AnomalyStorageServiceV1, user)},
         {"read", offsetof(AnomalyStorageServiceV1, read)},
         {"write_atomic", offsetof(AnomalyStorageServiceV1, write_atomic)},
         {"remove", offsetof(AnomalyStorageServiceV1, remove)}},
        false);
    AppendStruct(
        output,
        "AnomalyJsonServiceV1",
        sizeof(AnomalyJsonServiceV1),
        alignof(AnomalyJsonServiceV1),
        {{"struct_size", offsetof(AnomalyJsonServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyJsonServiceV1, service_version)},
         {"user", offsetof(AnomalyJsonServiceV1, user)},
         {"parse", offsetof(AnomalyJsonServiceV1, parse)},
         {"release", offsetof(AnomalyJsonServiceV1, release)},
         {"kind", offsetof(AnomalyJsonServiceV1, kind)},
         {"boolean_value", offsetof(AnomalyJsonServiceV1, boolean_value)},
         {"number_value", offsetof(AnomalyJsonServiceV1, number_value)},
         {"string_value", offsetof(AnomalyJsonServiceV1, string_value)},
         {"array_size", offsetof(AnomalyJsonServiceV1, array_size)},
         {"array_item", offsetof(AnomalyJsonServiceV1, array_item)},
         {"object_size", offsetof(AnomalyJsonServiceV1, object_size)},
         {"object_key_at", offsetof(AnomalyJsonServiceV1, object_key_at)},
         {"object_find", offsetof(AnomalyJsonServiceV1, object_find)},
         {"serialize", offsetof(AnomalyJsonServiceV1, serialize)}},
        false);
    AppendStruct(
        output,
        "AnomalyProfileServiceV1",
        sizeof(AnomalyProfileServiceV1),
        alignof(AnomalyProfileServiceV1),
        {{"struct_size", offsetof(AnomalyProfileServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyProfileServiceV1, service_version)},
         {"user", offsetof(AnomalyProfileServiceV1, user)},
         {"load", offsetof(AnomalyProfileServiceV1, load)}},
        false);
    AppendStruct(
        output,
        "AnomalyRuntimeInfoServiceV1",
        sizeof(AnomalyRuntimeInfoServiceV1),
        alignof(AnomalyRuntimeInfoServiceV1),
        {{"struct_size", offsetof(AnomalyRuntimeInfoServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyRuntimeInfoServiceV1, service_version)},
         {"user", offsetof(AnomalyRuntimeInfoServiceV1, user)},
         {"snapshot", offsetof(AnomalyRuntimeInfoServiceV1, snapshot)},
         {"runtime_version_utf8", offsetof(AnomalyRuntimeInfoServiceV1, runtime_version_utf8)}},
        false);
    AppendStruct(
        output,
        "AnomalyDiagnosticsServiceV1",
        sizeof(AnomalyDiagnosticsServiceV1),
        alignof(AnomalyDiagnosticsServiceV1),
        {{"struct_size", offsetof(AnomalyDiagnosticsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyDiagnosticsServiceV1, service_version)},
         {"user", offsetof(AnomalyDiagnosticsServiceV1, user)},
         {"register_self_test", offsetof(AnomalyDiagnosticsServiceV1, register_self_test)},
         {"unregister_self_test", offsetof(AnomalyDiagnosticsServiceV1, unregister_self_test)},
         {"run_self_test", offsetof(AnomalyDiagnosticsServiceV1, run_self_test)},
         {"snapshot_json", offsetof(AnomalyDiagnosticsServiceV1, snapshot_json)}},
        false);
    AppendStruct(
        output,
        "AnomalySchedulerServiceV1",
        sizeof(AnomalySchedulerServiceV1),
        alignof(AnomalySchedulerServiceV1),
        {{"struct_size", offsetof(AnomalySchedulerServiceV1, struct_size)},
         {"service_version", offsetof(AnomalySchedulerServiceV1, service_version)},
         {"user", offsetof(AnomalySchedulerServiceV1, user)},
         {"schedule", offsetof(AnomalySchedulerServiceV1, schedule)},
         {"cancel", offsetof(AnomalySchedulerServiceV1, cancel)}},
        false);
    AppendStruct(
        output,
        "AnomalyWebSocketServerInfoV1",
        sizeof(AnomalyWebSocketServerInfoV1),
        alignof(AnomalyWebSocketServerInfoV1),
        {{"struct_size", offsetof(AnomalyWebSocketServerInfoV1, struct_size)},
         {"port", offsetof(AnomalyWebSocketServerInfoV1, port)},
         {"reserved", offsetof(AnomalyWebSocketServerInfoV1, reserved)},
         {"connected_clients", offsetof(AnomalyWebSocketServerInfoV1, connected_clients)},
         {"published_messages", offsetof(AnomalyWebSocketServerInfoV1, published_messages)},
         {"dropped_messages", offsetof(AnomalyWebSocketServerInfoV1, dropped_messages)}},
        false);
    AppendStruct(
        output,
        "AnomalyWebSocketServiceV1",
        sizeof(AnomalyWebSocketServiceV1),
        alignof(AnomalyWebSocketServiceV1),
        {{"struct_size", offsetof(AnomalyWebSocketServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyWebSocketServiceV1, service_version)},
         {"user", offsetof(AnomalyWebSocketServiceV1, user)},
         {"publish_text", offsetof(AnomalyWebSocketServiceV1, publish_text)},
         {"server_info", offsetof(AnomalyWebSocketServiceV1, server_info)},
         {"set_port", offsetof(AnomalyWebSocketServiceV1, set_port)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcSchemaHashV1",
        sizeof(AnomalyIpcSchemaHashV1),
        alignof(AnomalyIpcSchemaHashV1),
        {{"bytes", offsetof(AnomalyIpcSchemaHashV1, bytes)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcEndpointDescriptorV1",
        sizeof(AnomalyIpcEndpointDescriptorV1),
        alignof(AnomalyIpcEndpointDescriptorV1),
        {{"struct_size", offsetof(AnomalyIpcEndpointDescriptorV1, struct_size)},
         {"endpoint_id", offsetof(AnomalyIpcEndpointDescriptorV1, endpoint_id)},
         {"major_version", offsetof(AnomalyIpcEndpointDescriptorV1, major_version)},
         {"minor_version", offsetof(AnomalyIpcEndpointDescriptorV1, minor_version)},
         {"request_schema", offsetof(AnomalyIpcEndpointDescriptorV1, request_schema)},
         {"response_schema", offsetof(AnomalyIpcEndpointDescriptorV1, response_schema)},
         {"event_schema", offsetof(AnomalyIpcEndpointDescriptorV1, event_schema)},
         {"modes", offsetof(AnomalyIpcEndpointDescriptorV1, modes)},
         {"affinity", offsetof(AnomalyIpcEndpointDescriptorV1, affinity)},
         {"timeout_milliseconds",
          offsetof(AnomalyIpcEndpointDescriptorV1, timeout_milliseconds)},
         {"reentrancy", offsetof(AnomalyIpcEndpointDescriptorV1, reentrancy)},
         {"maximum_request_bytes",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_request_bytes)},
         {"maximum_response_bytes",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_response_bytes)},
         {"maximum_event_bytes",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_event_bytes)},
         {"maximum_queue_depth",
          offsetof(AnomalyIpcEndpointDescriptorV1, maximum_queue_depth)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcEndpointSelectorV1",
        sizeof(AnomalyIpcEndpointSelectorV1),
        alignof(AnomalyIpcEndpointSelectorV1),
        {{"struct_size", offsetof(AnomalyIpcEndpointSelectorV1, struct_size)},
         {"endpoint_id", offsetof(AnomalyIpcEndpointSelectorV1, endpoint_id)},
         {"major_version", offsetof(AnomalyIpcEndpointSelectorV1, major_version)},
         {"minimum_minor_version",
          offsetof(AnomalyIpcEndpointSelectorV1, minimum_minor_version)},
         {"request_schema", offsetof(AnomalyIpcEndpointSelectorV1, request_schema)},
         {"response_schema", offsetof(AnomalyIpcEndpointSelectorV1, response_schema)},
         {"event_schema", offsetof(AnomalyIpcEndpointSelectorV1, event_schema)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcRequestContextV1",
        sizeof(AnomalyIpcRequestContextV1),
        alignof(AnomalyIpcRequestContextV1),
        {{"struct_size", offsetof(AnomalyIpcRequestContextV1, struct_size)},
         {"reserved", offsetof(AnomalyIpcRequestContextV1, reserved)},
         {"request_id", offsetof(AnomalyIpcRequestContextV1, request_id)},
         {"caller_plugin_id", offsetof(AnomalyIpcRequestContextV1, caller_plugin_id)}},
        false);
    AppendStruct(
        output,
        "AnomalyIpcServiceV1",
        sizeof(AnomalyIpcServiceV1),
        alignof(AnomalyIpcServiceV1),
        {{"struct_size", offsetof(AnomalyIpcServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyIpcServiceV1, service_version)},
         {"user", offsetof(AnomalyIpcServiceV1, user)},
         {"register_endpoint", offsetof(AnomalyIpcServiceV1, register_endpoint)},
         {"unregister_endpoint", offsetof(AnomalyIpcServiceV1, unregister_endpoint)},
         {"invoke", offsetof(AnomalyIpcServiceV1, invoke)},
         {"invoke_async", offsetof(AnomalyIpcServiceV1, invoke_async)},
         {"cancel", offsetof(AnomalyIpcServiceV1, cancel)},
         {"subscribe", offsetof(AnomalyIpcServiceV1, subscribe)},
         {"unsubscribe", offsetof(AnomalyIpcServiceV1, unsubscribe)},
         {"publish", offsetof(AnomalyIpcServiceV1, publish)}},
        false);
    AppendStruct(
        output,
        "AnomalyCommandsServiceV1",
        sizeof(AnomalyCommandsServiceV1),
        alignof(AnomalyCommandsServiceV1),
        {{"struct_size", offsetof(AnomalyCommandsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyCommandsServiceV1, service_version)},
         {"user", offsetof(AnomalyCommandsServiceV1, user)},
         {"register_command", offsetof(AnomalyCommandsServiceV1, register_command)},
         {"unregister_command", offsetof(AnomalyCommandsServiceV1, unregister_command)},
         {"invoke", offsetof(AnomalyCommandsServiceV1, invoke)}},
        false);
    AppendStruct(
        output,
        "AnomalyNotificationsServiceV1",
        sizeof(AnomalyNotificationsServiceV1),
        alignof(AnomalyNotificationsServiceV1),
        {{"struct_size", offsetof(AnomalyNotificationsServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyNotificationsServiceV1, service_version)},
         {"user", offsetof(AnomalyNotificationsServiceV1, user)},
         {"post", offsetof(AnomalyNotificationsServiceV1, post)},
         {"dismiss", offsetof(AnomalyNotificationsServiceV1, dismiss)}},
        false);
    AppendStruct(
        output,
        "AnomalyHookRequestV1",
        sizeof(AnomalyHookRequestV1),
        alignof(AnomalyHookRequestV1),
        {{"struct_size", offsetof(AnomalyHookRequestV1, struct_size)},
         {"kind", offsetof(AnomalyHookRequestV1, kind)},
         {"target", offsetof(AnomalyHookRequestV1, target)},
         {"detour", offsetof(AnomalyHookRequestV1, detour)},
         {"label", offsetof(AnomalyHookRequestV1, label)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureServiceV1",
        sizeof(AnomalySignatureServiceV1),
        alignof(AnomalySignatureServiceV1),
        {{"struct_size", offsetof(AnomalySignatureServiceV1, struct_size)},
         {"service_version", offsetof(AnomalySignatureServiceV1, service_version)},
         {"user", offsetof(AnomalySignatureServiceV1, user)},
         {"resolve", offsetof(AnomalySignatureServiceV1, resolve)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureFieldV2",
        sizeof(AnomalySignatureFieldV2),
        alignof(AnomalySignatureFieldV2),
        {{"struct_size", offsetof(AnomalySignatureFieldV2, struct_size)},
         {"id", offsetof(AnomalySignatureFieldV2, id)},
         {"seed_offset", offsetof(AnomalySignatureFieldV2, seed_offset)},
         {"alignment", offsetof(AnomalySignatureFieldV2, alignment)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureMemberV2",
        sizeof(AnomalySignatureMemberV2),
        alignof(AnomalySignatureMemberV2),
        {{"struct_size", offsetof(AnomalySignatureMemberV2, struct_size)},
         {"field_index", offsetof(AnomalySignatureMemberV2, field_index)},
         {"addend", offsetof(AnomalySignatureMemberV2, addend)},
         {"width", offsetof(AnomalySignatureMemberV2, width)},
         {"access", offsetof(AnomalySignatureMemberV2, access)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureWitnessV2",
        sizeof(AnomalySignatureWitnessV2),
        alignof(AnomalySignatureWitnessV2),
        {{"struct_size", offsetof(AnomalySignatureWitnessV2, struct_size)},
         {"id", offsetof(AnomalySignatureWitnessV2, id)},
         {"pattern", offsetof(AnomalySignatureWitnessV2, pattern)},
         {"members", offsetof(AnomalySignatureWitnessV2, members)},
         {"member_count", offsetof(AnomalySignatureWitnessV2, member_count)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureLayoutRequestV2",
        sizeof(AnomalySignatureLayoutRequestV2),
        alignof(AnomalySignatureLayoutRequestV2),
        {{"struct_size", offsetof(AnomalySignatureLayoutRequestV2, struct_size)},
         {"module_name", offsetof(AnomalySignatureLayoutRequestV2, module_name)},
         {"section_name", offsetof(AnomalySignatureLayoutRequestV2, section_name)},
         {"fields", offsetof(AnomalySignatureLayoutRequestV2, fields)},
         {"field_count", offsetof(AnomalySignatureLayoutRequestV2, field_count)},
         {"witnesses", offsetof(AnomalySignatureLayoutRequestV2, witnesses)},
         {"witness_count", offsetof(AnomalySignatureLayoutRequestV2, witness_count)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureServiceV2",
        sizeof(AnomalySignatureServiceV2),
        alignof(AnomalySignatureServiceV2),
        {{"struct_size", offsetof(AnomalySignatureServiceV2, struct_size)},
         {"service_version", offsetof(AnomalySignatureServiceV2, service_version)},
         {"user", offsetof(AnomalySignatureServiceV2, user)},
         {"resolve", offsetof(AnomalySignatureServiceV2, resolve)},
         {"resolve_layout", offsetof(AnomalySignatureServiceV2, resolve_layout)}},
        false);
    AppendStruct(
        output,
        "AnomalySignatureServiceV3",
        sizeof(AnomalySignatureServiceV3),
        alignof(AnomalySignatureServiceV3),
        {{"struct_size", offsetof(AnomalySignatureServiceV3, struct_size)},
         {"service_version", offsetof(AnomalySignatureServiceV3, service_version)},
         {"user", offsetof(AnomalySignatureServiceV3, user)},
         {"resolve", offsetof(AnomalySignatureServiceV3, resolve)},
         {"resolve_layout", offsetof(AnomalySignatureServiceV3, resolve_layout)},
         {"resolve_batch", offsetof(AnomalySignatureServiceV3, resolve_batch)}},
        false);
    AppendStruct(
        output,
        "AnomalyHookServiceV1",
        sizeof(AnomalyHookServiceV1),
        alignof(AnomalyHookServiceV1),
        {{"struct_size", offsetof(AnomalyHookServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyHookServiceV1, service_version)},
         {"user", offsetof(AnomalyHookServiceV1, user)},
         {"create", offsetof(AnomalyHookServiceV1, create)},
         {"release", offsetof(AnomalyHookServiceV1, release)},
         {"begin_callback", offsetof(AnomalyHookServiceV1, begin_callback)},
         {"end_callback", offsetof(AnomalyHookServiceV1, end_callback)}},
        false);
    AppendStruct(
        output,
        "AnomalyPatchServiceV1",
        sizeof(AnomalyPatchServiceV1),
        alignof(AnomalyPatchServiceV1),
        {{"struct_size", offsetof(AnomalyPatchServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyPatchServiceV1, service_version)},
         {"user", offsetof(AnomalyPatchServiceV1, user)},
         {"apply", offsetof(AnomalyPatchServiceV1, apply)},
         {"release", offsetof(AnomalyPatchServiceV1, release)}},
        false);
    AppendStruct(
        output,
        "AnomalyEspCameraV1",
        sizeof(AnomalyEspCameraV1),
        alignof(AnomalyEspCameraV1),
        {{"struct_size", offsetof(AnomalyEspCameraV1, struct_size)},
         {"flags", offsetof(AnomalyEspCameraV1, flags)},
         {"position", offsetof(AnomalyEspCameraV1, position)},
         {"rotation", offsetof(AnomalyEspCameraV1, rotation)},
         {"horizontal_fov_degrees", offsetof(AnomalyEspCameraV1, horizontal_fov_degrees)},
         {"reserved", offsetof(AnomalyEspCameraV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyEspEntityBoundsV1",
        sizeof(AnomalyEspEntityBoundsV1),
        alignof(AnomalyEspEntityBoundsV1),
        {{"struct_size", offsetof(AnomalyEspEntityBoundsV1, struct_size)},
         {"flags", offsetof(AnomalyEspEntityBoundsV1, flags)},
         {"center", offsetof(AnomalyEspEntityBoundsV1, center)},
         {"extent", offsetof(AnomalyEspEntityBoundsV1, extent)}},
        false);
    AppendStruct(
        output,
        "AnomalyEspBoxStyleV1",
        sizeof(AnomalyEspBoxStyleV1),
        alignof(AnomalyEspBoxStyleV1),
        {{"struct_size", offsetof(AnomalyEspBoxStyleV1, struct_size)},
         {"flags", offsetof(AnomalyEspBoxStyleV1, flags)},
         {"color_rgba", offsetof(AnomalyEspBoxStyleV1, color_rgba)},
         {"outline_color_rgba", offsetof(AnomalyEspBoxStyleV1, outline_color_rgba)},
         {"thickness", offsetof(AnomalyEspBoxStyleV1, thickness)},
         {"outline_thickness", offsetof(AnomalyEspBoxStyleV1, outline_thickness)}},
        false);
    AppendStruct(
        output,
        "AnomalyUiGlassStyleV1",
        sizeof(AnomalyUiGlassStyleV1),
        alignof(AnomalyUiGlassStyleV1),
        {{"struct_size", offsetof(AnomalyUiGlassStyleV1, struct_size)},
         {"flags", offsetof(AnomalyUiGlassStyleV1, flags)},
         {"variant", offsetof(AnomalyUiGlassStyleV1, variant)},
         {"tint_rgba", offsetof(AnomalyUiGlassStyleV1, tint_rgba)},
         {"rounding", offsetof(AnomalyUiGlassStyleV1, rounding)},
         {"scale", offsetof(AnomalyUiGlassStyleV1, scale)},
         {"opacity", offsetof(AnomalyUiGlassStyleV1, opacity)},
         {"refraction_scale", offsetof(AnomalyUiGlassStyleV1, refraction_scale)}},
        false);
    AppendStruct(
        output,
        "AnomalyUiServiceV1",
        sizeof(AnomalyUiServiceV1),
        alignof(AnomalyUiServiceV1),
        {{"struct_size", offsetof(AnomalyUiServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyUiServiceV1, service_version)},
         {"user", offsetof(AnomalyUiServiceV1, user)},
         {"set_next_window_size", offsetof(AnomalyUiServiceV1, set_next_window_size)},
         {"begin_window", offsetof(AnomalyUiServiceV1, begin_window)},
         {"end_window", offsetof(AnomalyUiServiceV1, end_window)},
         {"text", offsetof(AnomalyUiServiceV1, text)},
         {"button", offsetof(AnomalyUiServiceV1, button)},
         {"draw_entity_bbox", offsetof(AnomalyUiServiceV1, draw_entity_bbox)},
         {"checkbox", offsetof(AnomalyUiServiceV1, checkbox)},
         {"slider_float", offsetof(AnomalyUiServiceV1, slider_float)},
         {"color_edit4", offsetof(AnomalyUiServiceV1, color_edit4)},
         {"draw_entity_box3d", offsetof(AnomalyUiServiceV1, draw_entity_box3d)},
         {"draw_entity_label", offsetof(AnomalyUiServiceV1, draw_entity_label)},
         {"separator", offsetof(AnomalyUiServiceV1, separator)},
         {"begin_child", offsetof(AnomalyUiServiceV1, begin_child)},
         {"end_child", offsetof(AnomalyUiServiceV1, end_child)},
         {"begin_table", offsetof(AnomalyUiServiceV1, begin_table)},
         {"table_next_row", offsetof(AnomalyUiServiceV1, table_next_row)},
         {"table_next_column", offsetof(AnomalyUiServiceV1, table_next_column)},
         {"end_table", offsetof(AnomalyUiServiceV1, end_table)},
         {"begin_menu", offsetof(AnomalyUiServiceV1, begin_menu)},
         {"end_menu", offsetof(AnomalyUiServiceV1, end_menu)},
         {"open_popup", offsetof(AnomalyUiServiceV1, open_popup)},
         {"begin_popup_modal", offsetof(AnomalyUiServiceV1, begin_popup_modal)},
         {"end_popup", offsetof(AnomalyUiServiceV1, end_popup)},
         {"close_current_popup", offsetof(AnomalyUiServiceV1, close_current_popup)},
         {"filter_match", offsetof(AnomalyUiServiceV1, filter_match)},
         {"frame_state", offsetof(AnomalyUiServiceV1, frame_state)},
         {"set_next_window_size_constraints",
          offsetof(AnomalyUiServiceV1, set_next_window_size_constraints)},
         {"get_window_size", offsetof(AnomalyUiServiceV1, get_window_size)},
         {"input_uint32", offsetof(AnomalyUiServiceV1, input_uint32)},
         {"input_double", offsetof(AnomalyUiServiceV1, input_double)},
         {"developer_mode_enabled", offsetof(AnomalyUiServiceV1, developer_mode_enabled)},
         {"input_text", offsetof(AnomalyUiServiceV1, input_text)},
         {"button_enabled", offsetof(AnomalyUiServiceV1, button_enabled)},
         {"same_line", offsetof(AnomalyUiServiceV1, same_line)},
         {"set_cursor_pos_x", offsetof(AnomalyUiServiceV1, set_cursor_pos_x)},
         {"text_link", offsetof(AnomalyUiServiceV1, text_link)},
         {"begin_tab_bar", offsetof(AnomalyUiServiceV1, begin_tab_bar)},
         {"begin_tab_item", offsetof(AnomalyUiServiceV1, begin_tab_item)},
         {"end_tab_item", offsetof(AnomalyUiServiceV1, end_tab_item)},
         {"end_tab_bar", offsetof(AnomalyUiServiceV1, end_tab_bar)},
         {"text_wrapped", offsetof(AnomalyUiServiceV1, text_wrapped)},
         {"begin_child_auto_fit_y", offsetof(AnomalyUiServiceV1, begin_child_auto_fit_y)},
         {"slider_float_enabled", offsetof(AnomalyUiServiceV1, slider_float_enabled)},
         {"set_next_item_width", offsetof(AnomalyUiServiceV1, set_next_item_width)},
         {"overlay_viewport_size", offsetof(AnomalyUiServiceV1, overlay_viewport_size)},
         {"overlay_glass", offsetof(AnomalyUiServiceV1, overlay_glass)},
         {"overlay_rect_filled", offsetof(AnomalyUiServiceV1, overlay_rect_filled)},
         {"overlay_line", offsetof(AnomalyUiServiceV1, overlay_line)},
         {"overlay_circle", offsetof(AnomalyUiServiceV1, overlay_circle)},
         {"overlay_ring_sector", offsetof(AnomalyUiServiceV1, overlay_ring_sector)},
         {"overlay_measure_text", offsetof(AnomalyUiServiceV1, overlay_measure_text)},
         {"overlay_text", offsetof(AnomalyUiServiceV1, overlay_text)},
         {"overlay_glass_ring_sector", offsetof(AnomalyUiServiceV1, overlay_glass_ring_sector)},
         {"draw_screen_primitives", offsetof(AnomalyUiServiceV1, draw_screen_primitives)}},
        false);
    AppendStruct(
        output,
        "AnomalyWindowSpecV1",
        sizeof(AnomalyWindowSpecV1),
        alignof(AnomalyWindowSpecV1),
        {{"struct_size", offsetof(AnomalyWindowSpecV1, struct_size)},
         {"flags", offsetof(AnomalyWindowSpecV1, flags)},
         {"id", offsetof(AnomalyWindowSpecV1, id)},
         {"title", offsetof(AnomalyWindowSpecV1, title)},
         {"initial_width", offsetof(AnomalyWindowSpecV1, initial_width)},
         {"initial_height", offsetof(AnomalyWindowSpecV1, initial_height)},
         {"minimum_width", offsetof(AnomalyWindowSpecV1, minimum_width)},
         {"minimum_height", offsetof(AnomalyWindowSpecV1, minimum_height)},
         {"maximum_width", offsetof(AnomalyWindowSpecV1, maximum_width)},
         {"maximum_height", offsetof(AnomalyWindowSpecV1, maximum_height)},
         {"default_open", offsetof(AnomalyWindowSpecV1, default_open)},
         {"reserved", offsetof(AnomalyWindowSpecV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyWindowStateV1",
        sizeof(AnomalyWindowStateV1),
        alignof(AnomalyWindowStateV1),
        {{"struct_size", offsetof(AnomalyWindowStateV1, struct_size)},
         {"flags", offsetof(AnomalyWindowStateV1, flags)},
         {"width", offsetof(AnomalyWindowStateV1, width)},
         {"height", offsetof(AnomalyWindowStateV1, height)},
         {"ui_generation", offsetof(AnomalyWindowStateV1, ui_generation)},
         {"open", offsetof(AnomalyWindowStateV1, open)},
         {"reserved", offsetof(AnomalyWindowStateV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyFontRequestV1",
        sizeof(AnomalyFontRequestV1),
        alignof(AnomalyFontRequestV1),
        {{"struct_size", offsetof(AnomalyFontRequestV1, struct_size)},
         {"flags", offsetof(AnomalyFontRequestV1, flags)},
         {"relative_path", offsetof(AnomalyFontRequestV1, relative_path)},
         {"size_pixels", offsetof(AnomalyFontRequestV1, size_pixels)},
         {"glyph_range", offsetof(AnomalyFontRequestV1, glyph_range)},
         {"reserved", offsetof(AnomalyFontRequestV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyFontStateV1",
        sizeof(AnomalyFontStateV1),
        alignof(AnomalyFontStateV1),
        {{"struct_size", offsetof(AnomalyFontStateV1, struct_size)},
         {"flags", offsetof(AnomalyFontStateV1, flags)},
         {"effective_size_pixels", offsetof(AnomalyFontStateV1, effective_size_pixels)},
         {"scale", offsetof(AnomalyFontStateV1, scale)},
         {"device_generation", offsetof(AnomalyFontStateV1, device_generation)},
         {"ready", offsetof(AnomalyFontStateV1, ready)},
         {"reserved", offsetof(AnomalyFontStateV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyTextureRequestV1",
        sizeof(AnomalyTextureRequestV1),
        alignof(AnomalyTextureRequestV1),
        {{"struct_size", offsetof(AnomalyTextureRequestV1, struct_size)},
         {"flags", offsetof(AnomalyTextureRequestV1, flags)},
         {"relative_path", offsetof(AnomalyTextureRequestV1, relative_path)},
         {"encoded_bytes", offsetof(AnomalyTextureRequestV1, encoded_bytes)},
         {"format", offsetof(AnomalyTextureRequestV1, format)},
         {"width", offsetof(AnomalyTextureRequestV1, width)},
         {"height", offsetof(AnomalyTextureRequestV1, height)},
         {"reserved", offsetof(AnomalyTextureRequestV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyTextureStateV1",
        sizeof(AnomalyTextureStateV1),
        alignof(AnomalyTextureStateV1),
        {{"struct_size", offsetof(AnomalyTextureStateV1, struct_size)},
         {"flags", offsetof(AnomalyTextureStateV1, flags)},
         {"width", offsetof(AnomalyTextureStateV1, width)},
         {"height", offsetof(AnomalyTextureStateV1, height)},
         {"device_generation", offsetof(AnomalyTextureStateV1, device_generation)},
         {"byte_size", offsetof(AnomalyTextureStateV1, byte_size)}},
        false);
    AppendStruct(
        output,
        "AnomalyInputSnapshotV1",
        sizeof(AnomalyInputSnapshotV1),
        alignof(AnomalyInputSnapshotV1),
        {{"struct_size", offsetof(AnomalyInputSnapshotV1, struct_size)},
         {"modifiers", offsetof(AnomalyInputSnapshotV1, modifiers)},
         {"sequence", offsetof(AnomalyInputSnapshotV1, sequence)},
         {"timestamp_milliseconds", offsetof(AnomalyInputSnapshotV1, timestamp_milliseconds)},
         {"mouse_x", offsetof(AnomalyInputSnapshotV1, mouse_x)},
         {"mouse_y", offsetof(AnomalyInputSnapshotV1, mouse_y)},
         {"mouse_wheel", offsetof(AnomalyInputSnapshotV1, mouse_wheel)},
         {"capture_flags", offsetof(AnomalyInputSnapshotV1, capture_flags)},
         {"keys", offsetof(AnomalyInputSnapshotV1, keys)},
         {"mouse_buttons", offsetof(AnomalyInputSnapshotV1, mouse_buttons)},
         {"reserved", offsetof(AnomalyInputSnapshotV1, reserved)}},
        false);
    AppendStruct(
        output,
        "AnomalyHotkeySpecV1",
        sizeof(AnomalyHotkeySpecV1),
        alignof(AnomalyHotkeySpecV1),
        {{"struct_size", offsetof(AnomalyHotkeySpecV1, struct_size)},
         {"modifiers", offsetof(AnomalyHotkeySpecV1, modifiers)},
         {"virtual_key", offsetof(AnomalyHotkeySpecV1, virtual_key)},
         {"flags", offsetof(AnomalyHotkeySpecV1, flags)},
         {"id", offsetof(AnomalyHotkeySpecV1, id)}},
        false);
    AppendStruct(
        output,
        "AnomalyWindowServiceV1",
        sizeof(AnomalyWindowServiceV1),
        alignof(AnomalyWindowServiceV1),
        {{"struct_size", offsetof(AnomalyWindowServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyWindowServiceV1, service_version)},
         {"user", offsetof(AnomalyWindowServiceV1, user)},
         {"register_window", offsetof(AnomalyWindowServiceV1, register_window)},
         {"release_window", offsetof(AnomalyWindowServiceV1, release_window)},
         {"set_open", offsetof(AnomalyWindowServiceV1, set_open)},
         {"toggle", offsetof(AnomalyWindowServiceV1, toggle)},
         {"state", offsetof(AnomalyWindowServiceV1, state)},
         {"begin", offsetof(AnomalyWindowServiceV1, begin)},
         {"end", offsetof(AnomalyWindowServiceV1, end)}},
        false);
    AppendStruct(
        output,
        "AnomalyFontServiceV1",
        sizeof(AnomalyFontServiceV1),
        alignof(AnomalyFontServiceV1),
        {{"struct_size", offsetof(AnomalyFontServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyFontServiceV1, service_version)},
         {"user", offsetof(AnomalyFontServiceV1, user)},
         {"request", offsetof(AnomalyFontServiceV1, request)},
         {"release", offsetof(AnomalyFontServiceV1, release)},
         {"state", offsetof(AnomalyFontServiceV1, state)},
         {"push", offsetof(AnomalyFontServiceV1, push)},
         {"pop", offsetof(AnomalyFontServiceV1, pop)}},
        false);
    AppendStruct(
        output,
        "AnomalyTextureServiceV1",
        sizeof(AnomalyTextureServiceV1),
        alignof(AnomalyTextureServiceV1),
        {{"struct_size", offsetof(AnomalyTextureServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyTextureServiceV1, service_version)},
         {"user", offsetof(AnomalyTextureServiceV1, user)},
         {"request", offsetof(AnomalyTextureServiceV1, request)},
         {"release", offsetof(AnomalyTextureServiceV1, release)},
         {"state", offsetof(AnomalyTextureServiceV1, state)},
         {"draw", offsetof(AnomalyTextureServiceV1, draw)},
         {"draw_overlay", offsetof(AnomalyTextureServiceV1, draw_overlay)}},
        false);
    AppendStruct(
        output,
        "AnomalyInputServiceV1",
        sizeof(AnomalyInputServiceV1),
        alignof(AnomalyInputServiceV1),
        {{"struct_size", offsetof(AnomalyInputServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyInputServiceV1, service_version)},
         {"user", offsetof(AnomalyInputServiceV1, user)},
         {"snapshot", offsetof(AnomalyInputServiceV1, snapshot)},
         {"was_pressed", offsetof(AnomalyInputServiceV1, was_pressed)},
         {"register_hotkey", offsetof(AnomalyInputServiceV1, register_hotkey)},
         {"release_hotkey", offsetof(AnomalyInputServiceV1, release_hotkey)},
         {"capture_state", offsetof(AnomalyInputServiceV1, capture_state)},
         {"mouse_delta", offsetof(AnomalyInputServiceV1, mouse_delta)}},
        false);
    AppendStruct(
        output,
        "AnomalyLocalizationServiceV1",
        sizeof(AnomalyLocalizationServiceV1),
        alignof(AnomalyLocalizationServiceV1),
        {{"struct_size", offsetof(AnomalyLocalizationServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyLocalizationServiceV1, service_version)},
         {"user", offsetof(AnomalyLocalizationServiceV1, user)},
         {"locale", offsetof(AnomalyLocalizationServiceV1, locale)},
         {"translate", offsetof(AnomalyLocalizationServiceV1, translate)}},
        false);
    AppendStruct(
        output,
        "AnomalyMaydayMoneyServiceV1",
        sizeof(AnomalyMaydayMoneyServiceV1),
        alignof(AnomalyMaydayMoneyServiceV1),
        {{"struct_size", offsetof(AnomalyMaydayMoneyServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyMaydayMoneyServiceV1, service_version)},
         {"user", offsetof(AnomalyMaydayMoneyServiceV1, user)},
         {"get_money", offsetof(AnomalyMaydayMoneyServiceV1, get_money)},
         {"set_money", offsetof(AnomalyMaydayMoneyServiceV1, set_money)},
         {"add_money", offsetof(AnomalyMaydayMoneyServiceV1, add_money)},
         {"get_debt", offsetof(AnomalyMaydayMoneyServiceV1, get_debt)},
         {"set_debt", offsetof(AnomalyMaydayMoneyServiceV1, set_debt)}},
        false);
    AppendStruct(
        output,
        "AnomalyMaydayFreecamServiceV1",
        sizeof(AnomalyMaydayFreecamServiceV1),
        alignof(AnomalyMaydayFreecamServiceV1),
        {{"struct_size", offsetof(AnomalyMaydayFreecamServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyMaydayFreecamServiceV1, service_version)},
         {"user", offsetof(AnomalyMaydayFreecamServiceV1, user)},
         {"set_active", offsetof(AnomalyMaydayFreecamServiceV1, set_active)},
         {"get_state", offsetof(AnomalyMaydayFreecamServiceV1, get_state)},
         {"set_input", offsetof(AnomalyMaydayFreecamServiceV1, set_input)}},
        false);
    AppendStruct(
        output,
        "AnomalyMaydayWeatherStateV1",
        sizeof(AnomalyMaydayWeatherStateV1),
        alignof(AnomalyMaydayWeatherStateV1),
        {{"struct_size", offsetof(AnomalyMaydayWeatherStateV1, struct_size)},
         {"hours", offsetof(AnomalyMaydayWeatherStateV1, hours)},
         {"rate", offsetof(AnomalyMaydayWeatherStateV1, rate)},
         {"time_locked", offsetof(AnomalyMaydayWeatherStateV1, time_locked)},
         {"kind_value", offsetof(AnomalyMaydayWeatherStateV1, kind_value)},
         {"severity", offsetof(AnomalyMaydayWeatherStateV1, severity)},
         {"override_active", offsetof(AnomalyMaydayWeatherStateV1, override_active)},
         {"override_kind_value", offsetof(AnomalyMaydayWeatherStateV1, override_kind_value)},
         {"override_severity", offsetof(AnomalyMaydayWeatherStateV1, override_severity)}},
        false);
    AppendStruct(
        output,
        "AnomalyMaydayWeatherServiceV1",
        sizeof(AnomalyMaydayWeatherServiceV1),
        alignof(AnomalyMaydayWeatherServiceV1),
        {{"struct_size", offsetof(AnomalyMaydayWeatherServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyMaydayWeatherServiceV1, service_version)},
         {"user", offsetof(AnomalyMaydayWeatherServiceV1, user)},
         {"get_state", offsetof(AnomalyMaydayWeatherServiceV1, get_state)},
         {"set_time", offsetof(AnomalyMaydayWeatherServiceV1, set_time)},
         {"set_time_locked", offsetof(AnomalyMaydayWeatherServiceV1, set_time_locked)},
         {"set_weather_override", offsetof(AnomalyMaydayWeatherServiceV1, set_weather_override)},
         {"get_kind_value", offsetof(AnomalyMaydayWeatherServiceV1, get_kind_value)}},
        false);
    AppendStruct(
        output,
        "AnomalyMaydayCombatStateV1",
        sizeof(AnomalyMaydayCombatStateV1),
        alignof(AnomalyMaydayCombatStateV1),
        {{"struct_size", offsetof(AnomalyMaydayCombatStateV1, struct_size)},
         {"enabled", offsetof(AnomalyMaydayCombatStateV1, enabled)},
         {"in_flight", offsetof(AnomalyMaydayCombatStateV1, in_flight)},
         {"holding_bat", offsetof(AnomalyMaydayCombatStateV1, holding_bat)},
         {"has_target", offsetof(AnomalyMaydayCombatStateV1, has_target)},
         {"target_distance_mm", offsetof(AnomalyMaydayCombatStateV1, target_distance_mm)},
         {"max_range_mm", offsetof(AnomalyMaydayCombatStateV1, max_range_mm)},
         {"attack_interval_ms", offsetof(AnomalyMaydayCombatStateV1, attack_interval_ms)},
         {"last_result", offsetof(AnomalyMaydayCombatStateV1, last_result)},
         {"attacks_sent", offsetof(AnomalyMaydayCombatStateV1, attacks_sent)}},
        false);
    AppendStruct(
        output,
        "AnomalyMaydayCombatServiceV1",
        sizeof(AnomalyMaydayCombatServiceV1),
        alignof(AnomalyMaydayCombatServiceV1),
        {{"struct_size", offsetof(AnomalyMaydayCombatServiceV1, struct_size)},
         {"service_version", offsetof(AnomalyMaydayCombatServiceV1, service_version)},
         {"user", offsetof(AnomalyMaydayCombatServiceV1, user)},
         {"set_options", offsetof(AnomalyMaydayCombatServiceV1, set_options)},
         {"set_enabled", offsetof(AnomalyMaydayCombatServiceV1, set_enabled)},
         {"get_state", offsetof(AnomalyMaydayCombatServiceV1, get_state)}},
        true);
    output.append("  },\n");

    output.append("  \"services\": [\n");
    AppendService(output, ANOMALY_CORE_SERVICE_V1_ID,
        ANOMALY_CORE_SERVICE_V1_VERSION, "AnomalyCoreServiceV1", false);
    AppendService(output, ANOMALY_PLUGIN_STATE_SERVICE_V1_ID,
        ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION, "AnomalyPluginStateServiceV1", false);
    AppendService(output, ANOMALY_CONFIG_SERVICE_V1_ID,
        ANOMALY_CONFIG_SERVICE_V1_VERSION, "AnomalyConfigServiceV1", false);
    AppendService(output, ANOMALY_STORAGE_SERVICE_V1_ID,
        ANOMALY_STORAGE_SERVICE_V1_VERSION, "AnomalyStorageServiceV1", false);
    AppendService(output, ANOMALY_JSON_SERVICE_V1_ID,
        ANOMALY_JSON_SERVICE_V1_VERSION, "AnomalyJsonServiceV1", false);
    AppendService(output, ANOMALY_PROFILE_SERVICE_V1_ID,
        ANOMALY_PROFILE_SERVICE_V1_VERSION, "AnomalyProfileServiceV1", false);
    AppendService(output, ANOMALY_RUNTIME_INFO_SERVICE_V1_ID,
        ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION, "AnomalyRuntimeInfoServiceV1", false);
    AppendService(output, ANOMALY_DIAGNOSTICS_SERVICE_V1_ID,
        ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION, "AnomalyDiagnosticsServiceV1", false);
    AppendService(output, ANOMALY_SCHEDULER_SERVICE_V1_ID,
        ANOMALY_SCHEDULER_SERVICE_V1_VERSION, "AnomalySchedulerServiceV1", false);
    AppendService(output, ANOMALY_IPC_SERVICE_V1_ID,
        ANOMALY_IPC_SERVICE_V1_VERSION, "AnomalyIpcServiceV1", false);
    AppendService(output, ANOMALY_WEBSOCKET_SERVICE_V1_ID,
        ANOMALY_WEBSOCKET_SERVICE_V1_VERSION, "AnomalyWebSocketServiceV1", false);
    AppendService(output, ANOMALY_COMMANDS_SERVICE_V1_ID,
        ANOMALY_COMMANDS_SERVICE_V1_VERSION, "AnomalyCommandsServiceV1", false);
    AppendService(output, ANOMALY_NOTIFICATIONS_SERVICE_V1_ID,
        ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION, "AnomalyNotificationsServiceV1", false);
    AppendService(output, ANOMALY_SIGNATURE_SERVICE_V1_ID,
        ANOMALY_SIGNATURE_SERVICE_V1_VERSION, "AnomalySignatureServiceV1", false);
    AppendService(output, ANOMALY_SIGNATURE_SERVICE_V2_ID,
        ANOMALY_SIGNATURE_SERVICE_V2_VERSION, "AnomalySignatureServiceV2", false);
    AppendService(output, ANOMALY_SIGNATURE_SERVICE_V3_ID,
        ANOMALY_SIGNATURE_SERVICE_V3_VERSION, "AnomalySignatureServiceV3", false);
    AppendService(output, ANOMALY_HOOK_SERVICE_V1_ID,
        ANOMALY_HOOK_SERVICE_V1_VERSION, "AnomalyHookServiceV1", false);
    AppendService(output, ANOMALY_PATCH_SERVICE_V1_ID,
        ANOMALY_PATCH_SERVICE_V1_VERSION, "AnomalyPatchServiceV1", false);
    AppendService(output, ANOMALY_UI_SERVICE_V1_ID,
        ANOMALY_UI_SERVICE_V1_VERSION, "AnomalyUiServiceV1", false);
    AppendService(output, ANOMALY_WINDOW_SERVICE_V1_ID,
        ANOMALY_WINDOW_SERVICE_V1_VERSION, "AnomalyWindowServiceV1", false);
    AppendService(output, ANOMALY_FONT_SERVICE_V1_ID,
        ANOMALY_FONT_SERVICE_V1_VERSION, "AnomalyFontServiceV1", false);
    AppendService(output, ANOMALY_TEXTURE_SERVICE_V1_ID,
        ANOMALY_TEXTURE_SERVICE_V1_VERSION, "AnomalyTextureServiceV1", false);
    AppendService(output, ANOMALY_INPUT_SERVICE_V1_ID,
        ANOMALY_INPUT_SERVICE_V1_VERSION, "AnomalyInputServiceV1", false);
    AppendService(output, ANOMALY_LOCALIZATION_SERVICE_V1_ID,
        ANOMALY_LOCALIZATION_SERVICE_V1_VERSION, "AnomalyLocalizationServiceV1", false);
    AppendService(output, ANOMALY_MAYDAY_MONEY_SERVICE_V1_ID,
        ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION, "AnomalyMaydayMoneyServiceV1", false);
    AppendService(output, ANOMALY_MAYDAY_FREECAM_SERVICE_V1_ID,
        ANOMALY_MAYDAY_FREECAM_SERVICE_V1_VERSION, "AnomalyMaydayFreecamServiceV1", false);
    AppendService(output, ANOMALY_MAYDAY_WEATHER_SERVICE_V1_ID,
        ANOMALY_MAYDAY_WEATHER_SERVICE_V1_VERSION, "AnomalyMaydayWeatherServiceV1", false);
    AppendService(output, ANOMALY_MAYDAY_COMBAT_SERVICE_V1_ID,
        ANOMALY_MAYDAY_COMBAT_SERVICE_V1_VERSION, "AnomalyMaydayCombatServiceV1", true);
    output.append("  ]\n");
    output.append("}\n");
    return output;
}

bool WriteFile(const char* path, const std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return stream.good();
}

bool ReadFile(const char* path, std::string& contents) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

void PrintUsage(const char* executable) {
    std::cerr << "Usage: " << executable << " [--write PATH | --check PATH]\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
    const std::string snapshot = BuildSnapshot();
    if (argc == 1) {
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            std::cerr << "Failed to set stdout to binary mode\n";
            return 1;
        }
        std::cout.write(snapshot.data(), static_cast<std::streamsize>(snapshot.size()));
        return std::cout.good() ? 0 : 1;
    }
    if (argc != 3) {
        PrintUsage(argv[0]);
        return 2;
    }

    const std::string_view option(argv[1]);
    if (option == "--write") {
        if (!WriteFile(argv[2], snapshot)) {
            std::cerr << "Failed to write ABI snapshot: " << argv[2] << '\n';
            return 1;
        }
        return 0;
    }
    if (option == "--check") {
        std::string expected;
        if (!ReadFile(argv[2], expected)) {
            std::cerr << "Failed to read ABI snapshot: " << argv[2] << '\n';
            return 1;
        }
        if (expected != snapshot) {
            std::size_t offset = 0u;
            while (offset < expected.size() && offset < snapshot.size() &&
                   expected[offset] == snapshot[offset]) {
                ++offset;
            }
            std::cerr << "ABI snapshot mismatch at byte " << offset << ": " << argv[2] << '\n';
            return 1;
        }
        return 0;
    }

    PrintUsage(argv[0]);
    return 2;
}