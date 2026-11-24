#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_ws_connected)(bool connected, void *ctx);
    void (*on_session_ready)(int in_rate, int out_rate, void *ctx);
    void (*on_response_created)(const char *resp_id, void *ctx);
    void (*on_audio_delta)(const char *resp_id, const int16_t *pcm, size_t samples, void *ctx);
    void (*on_text_delta)(const char *resp_id, const char *delta, void *ctx);
    void (*on_response_done)(const char *resp_id, const char *status, void *ctx);
    void (*on_error)(const char *message, void *ctx);
    void (*on_harness_agent_info)(
        const char *id,
        const char *name,
        const char *role,
        uint32_t    accent_rgb,
        bool        has_accent,
        const char *orb_style,
        void       *ctx);
    void (*on_harness_tool_status)(const char *call_id, const char *name, const char *state, void *ctx);
    void (*on_harness_tool_confirm)(const char *call_id, const char *name, const char *summary, void *ctx);
    void (*on_rtt_sample)(uint32_t rtt_ms, void *ctx);
} realtime_ws_callbacks_t;

esp_err_t realtime_ws_init(const realtime_ws_callbacks_t *cbs, void *cb_ctx);
esp_err_t realtime_ws_connect(void);
void      realtime_ws_disconnect(void);
bool      realtime_ws_is_connected(void);

bool realtime_ws_send_audio_append(const int16_t *pcm_16k, size_t samples);
bool realtime_ws_send_audio_commit(void);
bool realtime_ws_send_response_create(void);
bool realtime_ws_send_response_cancel(void);
bool realtime_ws_send_tool_confirm_result(const char *call_id, bool approved);
bool realtime_ws_send_agent_switch(const char *direction);

#ifdef __cplusplus
}
#endif
