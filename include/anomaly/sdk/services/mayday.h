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

// Mayday Airways weather and time-of-day service (id="mayday.weather"). The
// host resolves WorldEnvironment / WorldManager / WeatherSystem and maps
// WeatherKind values by name at runtime; plugins never touch il2cpp.
// kind_index follows the game's WeatherKind declaration order
// (0=Calm, 1=Cloudy, 2=Rain, 3=Storm, 4=Fog, 5=Snow); get_kind_value maps an
// index to the runtime enum value. Failures return ANOMALY_STATUS_V1_UNAVAILABLE
// with stable diagnostic keys ("status.unavailable", "status.not_in_flight",
// "status.attach_failed", "status.failed") that the plugin localizes.

#define ANOMALY_MAYDAY_WEATHER_SERVICE_V1_ID "mayday.weather"
#define ANOMALY_MAYDAY_WEATHER_SERVICE_V1_VERSION 1u

typedef struct AnomalyMaydayWeatherStateV1 {
    uint32_t struct_size;
    // Time of day in hours [0, 24).
    float hours;
    // Current clock rate multiplier; 0 while time is locked.
    float rate;
    // 1 when the clock is frozen (rate 0), 0 otherwise.
    int32_t time_locked;
    // Current WeatherKind runtime enum value, or -1 when unavailable.
    int32_t kind_value;
    // Current weather severity [0, 1], or -1 when unavailable.
    float severity;
    // 1 when a dev weather override is active (weather locked), 0 otherwise.
    int32_t override_active;
    // Override WeatherKind runtime enum value, or -1.
    int32_t override_kind_value;
    // Override severity [0, 1], or -1.
    float override_severity;
} AnomalyMaydayWeatherStateV1;

typedef struct AnomalyMaydayWeatherServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Reads the combined time-of-day and weather state.
    AnomalyStatusV1 (ANOMALY_CALL *get_state)(
        void* user, AnomalyMaydayWeatherStateV1* out_state);
    // Sets the time of day in hours [0, 24).
    AnomalyStatusV1 (ANOMALY_CALL *set_time)(void* user, float hours);
    // Locks (locked != 0) or unlocks the clock. Locking caches the current
    // rate and freezes it; unlocking restores the cached rate.
    AnomalyStatusV1 (ANOMALY_CALL *set_time_locked)(void* user, int32_t locked);
    // Enables (active != 0) or clears the weather override. While active the
    // game holds kind_index/severity until cleared.
    AnomalyStatusV1 (ANOMALY_CALL *set_weather_override)(
        void* user, int32_t active, int32_t kind_index, float severity);
    // Maps a WeatherKind declaration index to the runtime enum value.
    AnomalyStatusV1 (ANOMALY_CALL *get_kind_value)(
        void* user, int32_t kind_index, int32_t* out_value);
} AnomalyMaydayWeatherServiceV1;

// Mayday Airways automatic bat service (id="mayday.combat"). The host resolves
// the local player, the held BatItem and the game's player list, then sends the
// game's own use command (Mirror Command) at the game's crew swing range.
// Automatic targeting deliberately excludes NPC passengers and the local
// player: the tool only swings at other crewmates (PlayerNet). While the host
// owns the session, the server-side swing is silently aimed at the selected
// crewmate, so the camera never turns.
//
// set_options must be called with the compatibility profile's crew swing range,
// attack interval and crew cone before enabling; those values come from the
// game build and are not guessed by the host. Failures return
// ANOMALY_STATUS_V1_UNAVAILABLE with a stable diagnostic key
// ("status.unavailable", "status.combat_no_symbols", "status.combat_no_options",
// "status.combat_hook_failed") that the plugin localizes.

#define ANOMALY_MAYDAY_COMBAT_SERVICE_V1_ID "mayday.combat"
#define ANOMALY_MAYDAY_COMBAT_SERVICE_V1_VERSION 1u

typedef struct AnomalyMaydayCombatStateV1 {
    uint32_t struct_size;
    // 1 while automatic swinging is enabled, 0 otherwise.
    int32_t enabled;
    // 1 when a local player exists (inside a flight), 0 otherwise.
    int32_t in_flight;
    // 1 when the local player's held item is a BatItem.
    int32_t holding_bat;
    // 1 when a passenger was found inside max_range_mm.
    int32_t has_target;
    // Distance from the player's eye to the target crewmate's chest, in
    // millimetres; -1 when no target is available.
    int32_t target_distance_mm;
    // Effective crew swing range and send interval configured by set_options.
    int32_t max_range_mm;
    int32_t attack_interval_ms;
    // 1 when this process hosts the Mirror server (silent aim is available),
    // 0 when joined to another host (attacks use the server-side aim cone).
    int32_t host_mode;
    // 1 while gyro mode is enabled: the player's avatar body spins one full
    // turn every 0.1 s. The camera, movement and the player root are untouched.
    int32_t gyro_enabled;
    // Entries in the crewmate list used for this scan.
    int32_t crew_count;
    // 0 = no swing observed, 1 = whiff, 2 = passenger hit, 3 = crew hit,
    // 4 = whoosh; derived from the last RpcSwing result.
    int32_t last_swing_result;
    // 0 = idle, 1 = the last attack command was sent, 2 = it failed.
    int32_t last_result;
    // Number of attack commands sent since the service was published.
    uint32_t attacks_sent;
} AnomalyMaydayCombatStateV1;

typedef struct AnomalyMaydayCombatServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Stores the compatibility values before enabling. max_range_mm is the
    // game's server-side crew swing limit, attack_interval_ms the send cadence
    // in [10, 1000] and crew_cone_deg the server-side crew hit cone (all
    // extracted from the game build).
    AnomalyStatusV1 (ANOMALY_CALL *set_options)(
        void* user, int32_t max_range_mm, int32_t attack_interval_ms,
        int32_t crew_cone_deg);
    // Enables (enabled != 0) or disables automatic swinging. Enabling also
    // installs the main-thread game hooks; disabling leaves them installed but
    // dormant until the host stops.
    AnomalyStatusV1 (ANOMALY_CALL *set_enabled)(void* user, int32_t enabled);
    // Enables (enabled != 0) gyro mode: the avatar body rotates one full turn
    // every 0.1 s while the camera and movement stay fully controllable. This
    // is independent of automatic swinging.
    AnomalyStatusV1 (ANOMALY_CALL *set_gyro_enabled)(void* user, int32_t enabled);
    // Reads the latest main-thread snapshot; no game calls are made here.
    AnomalyStatusV1 (ANOMALY_CALL *get_state)(
        void* user, AnomalyMaydayCombatStateV1* out_state);
} AnomalyMaydayCombatServiceV1;

// Mayday Airways voice injection service (id="mayday.voice"). The host resolves
// Mayday.Net.PlayerVoice (SendVoice), the VoiceCapture static constants, the
// SpeakerRoute enum values and the VoiceBackend session flag by name at
// runtime; plugins never touch il2cpp. send_frame submits one pre-encoded
// mu-law frame through the game's own voice pipeline (PlayerVoice.SendVoice),
// which handles sequencing and the Mirror broadcast to every session member.
// Route ids are stable host semantics (0=proximity, 1=PA, 2=radio); the host
// maps them onto the runtime SpeakerRoute values. PA/radio routes stay subject
// to the game's own permission and loudness quota checks. Failures return
// ANOMALY_STATUS_V1_UNAVAILABLE with stable diagnostic keys
// ("status.unavailable", "status.voice_no_symbols", "status.attach_failed",
// "status.failed") that the plugin localizes.

#define ANOMALY_MAYDAY_VOICE_SERVICE_V1_ID "mayday.voice"
#define ANOMALY_MAYDAY_VOICE_SERVICE_V1_VERSION 1u

typedef struct AnomalyMaydayVoiceStateV1 {
    uint32_t struct_size;
    // 1 while the local player's voice pipeline is ready, 0 otherwise.
    int32_t in_flight;
    // 1 when the session negotiated the Dissonance backend, 0 for the legacy
    // mu-law backend, -1 when the backend flag could not be read.
    int32_t backend_dissonance;
    // VoiceCapture.MuLawRate: PCM sample rate the mu-law frames represent.
    int32_t sample_rate;
    // VoiceCapture.MuLawPacketBytes: bytes per encoded voice packet.
    int32_t frame_bytes;
    // VoiceCapture.SendInterval in milliseconds: the game's frame cadence.
    int32_t send_interval_ms;
    // Runtime SpeakerRoute enum values resolved by name; -1 when unresolved.
    int32_t route_proximity;
    int32_t route_pa;
    int32_t route_radio;
    // 1 when this process hosts the Mirror server, 0 when joined.
    int32_t host_mode;
    // 1 when the Dissonance capture hook is installed (injection available),
    // 0 otherwise. The hook replaces the microphone capture data with the
    // registered PCM stream while it is registered.
    int32_t injection_available;
    // WaveFormat.SampleRate captured from the Dissonance capture callback;
    // -1 until the first capture callback delivered a format.
    int32_t injection_sample_rate;
    // Consumption position (in samples) the capture hook reached inside the
    // registered stream; only meaningful while injection is registered.
    int32_t injection_position;
} AnomalyMaydayVoiceStateV1;

typedef struct AnomalyMaydayVoiceServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Reads the latest snapshot; no game calls are made here, so it is safe
    // from the render domain. Call refresh_state from the game domain to
    // update the snapshot.
    AnomalyStatusV1 (ANOMALY_CALL *get_state)(
        void* user, AnomalyMaydayVoiceStateV1* out_state);
    // Refreshes the snapshot (local player presence, session backend, host
    // mode). Game domain only; cheap enough for every frame.
    AnomalyStatusV1 (ANOMALY_CALL *refresh_state)(void* user);
    // Submits one mu-law encoded frame on the given stable route id
    // (0=proximity, 1=PA, 2=radio). frame/length hold codec bytes; the game
    // handles sequencing and broadcast. Game domain only. Ineffective while
    // the session negotiated the Dissonance backend.
    AnomalyStatusV1 (ANOMALY_CALL *send_frame)(
        void* user, int32_t route_id, const void* frame, int32_t length);
    // Registers (enabled != 0) or releases (enabled == 0) the Dissonance
    // capture injection: while registered, the capture hook replaces the
    // microphone PCM with samples (mono float32, sample_rate) until sample_count
    // samples were consumed. Re-registering with a different samples pointer
    // restarts at zero; toggling enabled with the same pointer resumes. The
    // plugin owns the buffer lifetime and must release it (enabled == 0)
    // before freeing. Game domain only. Returns status.voice_no_symbols /
    // status.failed when the capture symbols or hook are unavailable.
    AnomalyStatusV1 (ANOMALY_CALL *set_injection)(
        void* user, int32_t enabled, const void* samples,
        int32_t sample_count, int32_t sample_rate);
    // Injection volume multiplier applied by the capture hook, in [0, 2].
    AnomalyStatusV1 (ANOMALY_CALL *set_injection_volume)(void* user, float volume);
} AnomalyMaydayVoiceServiceV1;

#ifdef __cplusplus
}
#endif
