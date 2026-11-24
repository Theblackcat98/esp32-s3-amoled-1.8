#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_guard.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_SLOT_BOOT_SHORT = 0,
    BTN_SLOT_BOOT_LONG  = 1,
    BTN_SLOT_PWR_SHORT  = 2,
    BTN_SLOT_PWR_LONG   = 3,
    BTN_SLOT_COUNT      = 4,
} app_btn_slot_t;

typedef enum {
    BTN_ACT_NONE           = 0,
    BTN_ACT_START_OR_MUTE  = 1,
    BTN_ACT_TOGGLE_SESSION = 2,
    BTN_ACT_TOGGLE_MUTE    = 3,
    BTN_ACT_INTERRUPT      = 4,
    BTN_ACT_VOL_UP         = 5,
    BTN_ACT_VOL_DOWN       = 6,
    BTN_ACT_CYCLE_GUARD    = 7,
    BTN_ACT_TOGGLE_DIM     = 8,
    BTN_ACT_POWER_OFF      = 9,
    BTN_ACT_COUNT          = 10,
} app_btn_action_t;

typedef struct {
    int                     speaker_volume_pct;
    audio_echo_guard_mode_t echo_guard_mode;
    int                     auto_shield_vol_pct;
    app_btn_action_t        btn_actions[BTN_SLOT_COUNT];
} app_prefs_t;

/** Initialize runtime preferences from Kconfig defaults + NVS overrides. */
esp_err_t app_prefs_init(void);

/** Thread-safe snapshot copy of all user preferences. */
void app_prefs_get(app_prefs_t *out);

/** Update speaker volume (clamped 0..100), optionally committing to NVS. Returns clamped value. */
int app_prefs_set_volume(int pct, bool save_nvs);

/** Update echo guard mode and auto-shield threshold (clamped 60..100), saving to NVS. */
void app_prefs_set_echo_guard(audio_echo_guard_mode_t mode, int auto_shield_vol_pct);

/** Assign a remappable action to a hardware button slot and persist to NVS. */
void app_prefs_set_btn_action(app_btn_slot_t slot, app_btn_action_t action);

/** Restore default hardware button bindings and persist to NVS. */
void app_prefs_reset_btn_defaults(void);

/** Return human-readable hardware button slot name. */
const char *app_prefs_btn_slot_name(app_btn_slot_t slot);

/** Return human-readable remappable action label. */
const char *app_prefs_btn_action_name(app_btn_action_t action);

#ifdef __cplusplus
}
#endif
