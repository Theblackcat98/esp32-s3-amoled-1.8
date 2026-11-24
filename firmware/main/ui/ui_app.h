#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_guard.h"
#include "board_pmu.h"
#include "esp_err.h"
#include "session_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_app_init(void);

void ui_app_set_state(app_state_t state, const char *status_text);
void ui_app_set_agent_info(
    const char *name,
    const char *role,
    uint32_t    accent_rgb,
    bool        apply_accent,
    const char *orb_style);

void ui_app_set_caption(const char *text, bool append);
void ui_app_clear_caption(void);
void ui_app_set_muted(bool muted);

void ui_app_update_telemetry(
    bool                         wifi_ok,
    int                          rssi,
    uint32_t                     rtt_ms,
    const board_pmu_telemetry_t *pmu);

void ui_app_show_tool_confirm(const char *call_id, const char *name, const char *summary);
void ui_app_note_user_activity(void);
void ui_app_toggle_dim(void);
void ui_app_show_shutdown_notice(void);
void ui_app_sync_audio_prefs(int vol_pct, audio_echo_guard_mode_t mode, int shield_vol_pct);
void ui_app_show_toast(const char *msg);

#ifdef __cplusplus
}
#endif
