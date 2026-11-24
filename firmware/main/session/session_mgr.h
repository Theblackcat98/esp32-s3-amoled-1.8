#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STATE_BOOTING = 0,
    APP_STATE_WIFI_WAIT,
    APP_STATE_READY,
    APP_STATE_CONNECTING,
    APP_STATE_LISTENING,
    APP_STATE_THINKING,
    APP_STATE_SPEAKING,
    APP_STATE_INTERRUPTED,
    APP_STATE_TOOL_CALLING,
    APP_STATE_ERROR,
    APP_STATE_RECONNECTING,
} app_state_t;

esp_err_t   session_mgr_init(void);
app_state_t session_mgr_get_state(void);

void session_mgr_start_session(void);
void session_mgr_stop_session(void);
void session_mgr_toggle_session(void);

void session_mgr_toggle_mute(void);
bool session_mgr_is_muted(void);

void     session_mgr_force_interrupt(void);
void     session_mgr_confirm_tool(const char *call_id, bool approved);
void     session_mgr_switch_agent(const char *direction);
uint32_t session_mgr_get_rtt_ms(void);

#ifdef __cplusplus
}
#endif
