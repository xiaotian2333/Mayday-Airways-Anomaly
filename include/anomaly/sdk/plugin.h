#pragma once
#include "anomaly/sdk/base.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/version.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AnomalyHostApiV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor; void* host_context;
    AnomalyAllocatorV1 allocator;
    AnomalyStatusV1 (ANOMALY_CALL *query_service)(void* host_context, AnomalyStringViewV1 service_id, uint32_t minimum_version, const void** service);
} AnomalyHostApiV1;
typedef void (ANOMALY_CALL *AnomalyPluginRenderCallbackV1)(
    void* plugin_context, const AnomalyUiServiceV1* ui);
typedef struct AnomalyPluginDescriptorV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor;
    AnomalyStringViewV1 id; AnomalyStringViewV1 name; AnomalyStringViewV1 author; AnomalyStringViewV1 version;
    // on_load 契约：返回 UNAVAILABLE 表示初始化未完成（例如所依赖的宿主服务尚未
    // 就绪）。宿主会丢弃 *plugin_context 并在服务就绪后重新调用 on_load，期间不会
    // 调用 on_unload；因此插件不得在返回 UNAVAILABLE 前保留任何需要清理的资源，
    // 或必须保证重复调用 on_load 幂等。
    AnomalyStatusV1 (ANOMALY_CALL *on_load)(const AnomalyHostApiV1* host, void** plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_start)(void* plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_stop)(void* plugin_context, uint32_t deadline_milliseconds);
    void (ANOMALY_CALL *on_unload)(void* plugin_context);
    void (ANOMALY_CALL *on_update)(void* plugin_context, double delta_seconds);
    AnomalyPluginRenderCallbackV1 on_draw;
    // Persistent game-window overlay. Runs every render frame while started,
    // including while the plugin catalog window is closed. Do not create UI
    // windows here; use draw_screen_primitives or other draw-only operations.
    AnomalyPluginRenderCallbackV1 on_overlay;
} AnomalyPluginDescriptorV1;
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyPluginEntryV1Fn)(AnomalyPluginDescriptorV1* descriptor);
#ifdef __cplusplus
}
#endif
