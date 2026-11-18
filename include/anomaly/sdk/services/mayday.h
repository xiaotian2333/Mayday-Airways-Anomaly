#pragma once
#include "anomaly/sdk/base.h"

// Mayday Airways (Unity IL2CPP) built-in services: money and debt.
// The host publishes the service after GameAssembly.dll is available. il2cpp
// thread attachment, class resolution and argument marshalling stay inside the
// host; plugins only consume the high-level operations.
// Failures return ANOMALY_STATUS_V1_UNAVAILABLE with a stable diagnostic key:
// "status.unavailable" (game symbols not ready), "status.not_in_flight" (no
// active flight), or "status.attach_failed" (il2cpp attach failed). The plugin
// localizes those keys itself.

#define ANOMALY_MAYDAY_MONEY_SERVICE_V1_ID "mayday.money"
#define ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnomalyMaydayMoneyServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Reads the current money value from the SyncVar.
    AnomalyStatusV1 (ANOMALY_CALL *get_money)(void* user, int32_t* out_money);
    // Writes the SyncVar directly, without updating the game's accounting stats.
    AnomalyStatusV1 (ANOMALY_CALL *set_money)(void* user, int32_t value);
    // Adds money through the game's ServerAddMoney accounting path.
    AnomalyStatusV1 (ANOMALY_CALL *add_money)(void* user, int32_t delta);
    // Reads the current debt value.
    AnomalyStatusV1 (ANOMALY_CALL *get_debt)(void* user, int32_t* out_debt);
    // Adjusts debt through ServerAddDebt to reach the requested value.
    AnomalyStatusV1 (ANOMALY_CALL *set_debt)(void* user, int32_t value);
} AnomalyMaydayMoneyServiceV1;

// Mayday Airways free camera service (id="mayday.freecam"). The host resolves
// PlayerRig/PlayerLook/Transform; the plugin only supplies per-frame input.
// While active the host owns the camera and suspends the player look and
// movement components, restoring the original state on exit.

#define ANOMALY_MAYDAY_FREECAM_SERVICE_V1_ID "mayday.freecam"
#define ANOMALY_MAYDAY_FREECAM_SERVICE_V1_VERSION 1u

typedef struct AnomalyMaydayFreecamServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Requests activation (active != 0) or deactivation. On failure the message
    // is a stable diagnostic key: "status.unavailable" (game symbols or player
    // not ready) or "status.failed" (an il2cpp call failed).
    AnomalyStatusV1 (ANOMALY_CALL *set_active)(void* user, int32_t active);
    // out_state: 1 when the camera is owned by the free camera, 0 otherwise.
    AnomalyStatusV1 (ANOMALY_CALL *get_state)(void* user, int32_t* out_state);
    // Per-frame input: forward/right/up are in [-1, 1]; yaw/pitch are mouse
    // deltas in degrees; speed is in world units per second. Only applied while
    // the free camera is active.
    AnomalyStatusV1 (ANOMALY_CALL *set_input)(
        void* user, float forward, float right, float up,
        float yaw_delta, float pitch_delta, float speed);
} AnomalyMaydayFreecamServiceV1;

#ifdef __cplusplus
}
#endif
