#include "session_mgr.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_prefs.h"
#include "audio_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "realtime_ws.h"
#include "ui_app.h"
#include "wifi_mgr.h"

static const char *TAG = "session_mgr";

static SemaphoreHandle_t s_lock            = NULL;
static app_state_t       s_state           = APP_STATE_BOOTING;
static bool              s_want_session    = false;
static bool              s_auto_started    = false;
static int               s_out_rate        = 24000;
static uint32_t          s_rtt_ms          = 0;

static char              s_active_resp_id[64]     = {0};
static char              s_cancelling_resp_id[64] = {0};
static bool              s_awaiting_cancel        = false;
static bool              s_pending_response       = false;
static bool              s_caption_started        = false;
static bool              s_response_finished      = true;
static int64_t           s_vad_end_us             = 0;
static bool              s_first_audio_logged     = false;

static void set_state_locked(app_state_t st, const char *status_msg) {
    s_state = st;
    ui_app_set_state(st, status_msg);
}

static void on_speech_start_cb(bool during_playback, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_want_session || !realtime_ws_is_connected()) {
        xSemaphoreGive(s_lock);
        return;
    }

    if (during_playback || s_state == APP_STATE_SPEAKING || s_state == APP_STATE_THINKING) {
        ESP_LOGI(TAG, "Barge-in triggered (resp=%s)", s_active_resp_id);
        audio_engine_flush_playback();
        audio_engine_set_speaking(false);
        if (s_active_resp_id[0] != '\0') {
            snprintf(s_cancelling_resp_id, sizeof(s_cancelling_resp_id), "%s", s_active_resp_id);
            s_active_resp_id[0] = '\0';
            s_awaiting_cancel   = true;
            s_response_finished = true;
            realtime_ws_send_response_cancel();
        }
        set_state_locked(APP_STATE_INTERRUPTED, "Interrupted");
    } else if (s_state == APP_STATE_LISTENING) {
        ui_app_set_state(APP_STATE_LISTENING, "Hearing speech...");
    }
    xSemaphoreGive(s_lock);
}

static void on_uplink_frame_cb(const int16_t *pcm_16k, size_t samples, void *ctx) {
    (void)ctx;
    if (s_state == APP_STATE_INTERRUPTED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_state == APP_STATE_INTERRUPTED) {
            set_state_locked(APP_STATE_LISTENING, "Listening...");
        }
        xSemaphoreGive(s_lock);
    }
    realtime_ws_send_audio_append(pcm_16k, samples);
}

static void on_speech_end_cb(void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_want_session || !realtime_ws_is_connected()) {
        xSemaphoreGive(s_lock);
        return;
    }

    s_vad_end_us         = esp_timer_get_time();
    s_first_audio_logged = false;
    realtime_ws_send_audio_commit();

    if (s_awaiting_cancel || s_active_resp_id[0] != '\0') {
        s_pending_response = true;
    } else {
        realtime_ws_send_response_create();
    }
    set_state_locked(APP_STATE_THINKING, "Thinking...");
    xSemaphoreGive(s_lock);
}

static void on_ws_connected_cb(bool connected, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!connected) {
        audio_engine_set_session_active(false);
        audio_engine_set_speaking(false);
        audio_engine_flush_playback();
        s_active_resp_id[0]     = '\0';
        s_cancelling_resp_id[0] = '\0';
        s_awaiting_cancel       = false;
        s_pending_response      = false;
        s_response_finished     = true;

        if (!wifi_mgr_is_connected()) {
            set_state_locked(APP_STATE_WIFI_WAIT, "Waiting for Wi-Fi...");
        } else if (s_want_session) {
            set_state_locked(APP_STATE_RECONNECTING, "Reconnecting to server...");
        } else {
            set_state_locked(APP_STATE_READY, "Tap orb or Start to talk");
        }
    } else {
        set_state_locked(APP_STATE_CONNECTING, "Handshaking...");
    }
    xSemaphoreGive(s_lock);
}

static void on_session_ready_cb(int in_rate, int out_rate, void *ctx) {
    (void)ctx;
    (void)in_rate;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out_rate              = (out_rate > 0) ? out_rate : 24000;
    s_active_resp_id[0]     = '\0';
    s_cancelling_resp_id[0] = '\0';
    s_awaiting_cancel       = false;
    s_pending_response      = false;
    s_response_finished     = true;

    if (s_want_session) {
        audio_engine_set_session_active(true);
        set_state_locked(APP_STATE_LISTENING, "Listening...");
        ui_app_set_caption("Ready. Speak naturally anytime.", false);
    }
    xSemaphoreGive(s_lock);
}

static void on_response_created_cb(const char *resp_id, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_awaiting_cancel && resp_id && resp_id[0] != '\0') {
        snprintf(s_active_resp_id, sizeof(s_active_resp_id), "%s", resp_id);
        s_caption_started   = false;
        s_response_finished = false;
        if (s_vad_end_us > 0) {
            s_rtt_ms = (uint32_t)((esp_timer_get_time() - s_vad_end_us) / 1000LL);
        }
    }
    xSemaphoreGive(s_lock);
}

static void on_audio_delta_cb(const char *resp_id, const int16_t *pcm, size_t samples, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active_resp_id[0] == '\0' || !resp_id || strcmp(resp_id, s_active_resp_id) != 0) {
        xSemaphoreGive(s_lock);
        return;
    }

    if (!s_first_audio_logged && s_vad_end_us > 0) {
        const int64_t ttfa_ms = (esp_timer_get_time() - s_vad_end_us) / 1000LL;
        ESP_LOGI(TAG, "Metric: VAD-end -> first audio delta = %lld ms", ttfa_ms);
        s_rtt_ms             = (uint32_t)ttfa_ms;
        s_first_audio_logged = true;
    }

    if (s_state != APP_STATE_SPEAKING) {
        audio_engine_set_speaking(true);
        set_state_locked(APP_STATE_SPEAKING, "Speaking...");
    }
    const int rate = s_out_rate;
    xSemaphoreGive(s_lock);

    audio_engine_push_downlink(pcm, samples, rate);
}

static void on_text_delta_cb(const char *resp_id, const char *delta, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active_resp_id[0] == '\0' || !resp_id || strcmp(resp_id, s_active_resp_id) != 0) {
        xSemaphoreGive(s_lock);
        return;
    }
    const bool append = s_caption_started;
    s_caption_started = true;
    xSemaphoreGive(s_lock);

    ui_app_set_caption(delta, append);
}

static void on_response_done_cb(const char *resp_id, const char *status, void *ctx) {
    (void)ctx;
    (void)status;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_awaiting_cancel && resp_id && strcmp(resp_id, s_cancelling_resp_id) == 0) {
        s_awaiting_cancel       = false;
        s_cancelling_resp_id[0] = '\0';
        if (s_pending_response && s_want_session) {
            s_pending_response = false;
            realtime_ws_send_response_create();
            set_state_locked(APP_STATE_THINKING, "Thinking...");
        }
        xSemaphoreGive(s_lock);
        return;
    }

    if (s_active_resp_id[0] != '\0' && resp_id && strcmp(resp_id, s_active_resp_id) == 0) {
        s_active_resp_id[0] = '\0';
        s_response_finished = true;
        audio_engine_mark_downlink_done();
        if (!audio_engine_is_playback_active() && s_want_session) {
            audio_engine_set_speaking(false);
            set_state_locked(APP_STATE_LISTENING, "Listening...");
        }
    }
    xSemaphoreGive(s_lock);
}

static void on_ws_error_cb(const char *message, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_want_session) {
        ui_app_set_caption(message ? message : "Connection error", false);
        set_state_locked(realtime_ws_is_connected() ? APP_STATE_ERROR : APP_STATE_RECONNECTING,
                         message ? message : "Connection error");
    }
    xSemaphoreGive(s_lock);
}

static void on_harness_agent_info_cb(
    const char *id,
    const char *name,
    const char *role,
    uint32_t    accent_rgb,
    bool        has_accent,
    const char *orb_style,
    void       *ctx) {
    (void)id;
    (void)ctx;
    ui_app_set_agent_info(name, role, accent_rgb, has_accent, orb_style);
}

static void on_harness_tool_status_cb(const char *call_id, const char *name, const char *state, void *ctx) {
    (void)call_id;
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (state && strcmp(state, "running") == 0) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Running: %s", name ? name : "tool");
        set_state_locked(APP_STATE_TOOL_CALLING, buf);
        ui_app_set_caption(buf, false);
    } else if (s_state == APP_STATE_TOOL_CALLING && s_want_session) {
        set_state_locked(APP_STATE_THINKING, "Finishing tool...");
    }
    xSemaphoreGive(s_lock);
}

static void on_harness_tool_confirm_cb(const char *call_id, const char *name, const char *summary, void *ctx) {
    (void)ctx;
    ui_app_show_tool_confirm(call_id, name, summary);
}

static void session_supervisor_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        xSemaphoreTake(s_lock, portMAX_DELAY);

        const bool wifi_ok = wifi_mgr_is_connected();
        if (!wifi_ok && s_state != APP_STATE_WIFI_WAIT) {
            if (s_want_session) {
                audio_engine_set_session_active(false);
                audio_engine_set_speaking(false);
                audio_engine_flush_playback();
            }
            set_state_locked(APP_STATE_WIFI_WAIT, "Waiting for Wi-Fi...");
        } else if (wifi_ok && s_state == APP_STATE_WIFI_WAIT) {
            if (!s_auto_started) {
                s_auto_started = true;
                ESP_LOGI(TAG, "Wi-Fi ready -> auto_connect=%d", APP_AUTO_CONNECT_BOOT);
                if (APP_AUTO_CONNECT_BOOT) {
                    s_want_session = true;
                    wifi_mgr_set_low_latency(true);
                    realtime_ws_connect();
                    set_state_locked(APP_STATE_CONNECTING, "Connecting...");
                    xSemaphoreGive(s_lock);
                    continue;
                }
            }
            if (s_want_session) {
                realtime_ws_connect();
                set_state_locked(APP_STATE_CONNECTING, "Connecting...");
            } else {
                set_state_locked(APP_STATE_READY, "Tap orb or Start to talk");
            }
        } else if (s_state == APP_STATE_SPEAKING && s_response_finished && !audio_engine_is_playback_active()) {
            audio_engine_set_speaking(false);
            if (s_want_session && realtime_ws_is_connected()) {
                set_state_locked(APP_STATE_LISTENING, "Listening...");
            }
        }

        xSemaphoreGive(s_lock);
    }
}

esp_err_t session_mgr_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    const audio_engine_callbacks_t audio_cbs = {
        .on_speech_start = on_speech_start_cb,
        .on_uplink_frame = on_uplink_frame_cb,
        .on_speech_end   = on_speech_end_cb,
    };
    ESP_ERROR_CHECK(audio_engine_init(&audio_cbs, NULL));

    const realtime_ws_callbacks_t ws_cbs = {
        .on_ws_connected         = on_ws_connected_cb,
        .on_session_ready        = on_session_ready_cb,
        .on_response_created     = on_response_created_cb,
        .on_audio_delta          = on_audio_delta_cb,
        .on_text_delta           = on_text_delta_cb,
        .on_response_done        = on_response_done_cb,
        .on_error                = on_ws_error_cb,
        .on_harness_agent_info   = on_harness_agent_info_cb,
        .on_harness_tool_status  = on_harness_tool_status_cb,
        .on_harness_tool_confirm = on_harness_tool_confirm_cb,
        .on_rtt_sample           = NULL,
    };
    ESP_ERROR_CHECK(realtime_ws_init(&ws_cbs, NULL));

    set_state_locked(APP_STATE_WIFI_WAIT, "Connecting to Wi-Fi...");
    xTaskCreatePinnedToCore(session_supervisor_task, "session_sup", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}

app_state_t session_mgr_get_state(void) {
    return s_state;
}

void session_mgr_start_session(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGI(TAG, "Session start requested (wifi=%d)", wifi_mgr_is_connected());
    s_want_session = true;
    if (!wifi_mgr_is_connected()) {
        set_state_locked(APP_STATE_WIFI_WAIT, "Waiting for Wi-Fi...");
        xSemaphoreGive(s_lock);
        return;
    }
    wifi_mgr_set_low_latency(true);
    set_state_locked(APP_STATE_CONNECTING, "Connecting to LAN server...");
    realtime_ws_connect();
    xSemaphoreGive(s_lock);
}

void session_mgr_stop_session(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGI(TAG, "Session stop requested");
    s_want_session = false;
    audio_engine_set_session_active(false);
    audio_engine_set_speaking(false);
    audio_engine_flush_playback();
    realtime_ws_disconnect();
    wifi_mgr_set_low_latency(false);
    s_active_resp_id[0]     = '\0';
    s_cancelling_resp_id[0] = '\0';
    s_awaiting_cancel       = false;
    s_pending_response      = false;
    s_response_finished     = true;
    set_state_locked(wifi_mgr_is_connected() ? APP_STATE_READY : APP_STATE_WIFI_WAIT,
                     wifi_mgr_is_connected() ? "Tap orb or Start to talk" : "Waiting for Wi-Fi...");
    ui_app_clear_caption();
    xSemaphoreGive(s_lock);
}

void session_mgr_toggle_session(void) {
    if (s_want_session) {
        session_mgr_stop_session();
    } else {
        session_mgr_start_session();
    }
}

void session_mgr_toggle_mute(void) {
    const bool next = !audio_engine_is_muted();
    audio_engine_set_muted(next);
    ui_app_set_muted(next);
}

bool session_mgr_is_muted(void) {
    return audio_engine_is_muted();
}

void session_mgr_force_interrupt(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_want_session) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_state == APP_STATE_SPEAKING || s_state == APP_STATE_THINKING) {
        audio_engine_flush_playback();
        audio_engine_set_speaking(false);
        if (s_active_resp_id[0] != '\0') {
            snprintf(s_cancelling_resp_id, sizeof(s_cancelling_resp_id), "%s", s_active_resp_id);
            s_active_resp_id[0] = '\0';
            s_awaiting_cancel   = true;
            s_response_finished = true;
            realtime_ws_send_response_cancel();
        }
        set_state_locked(APP_STATE_INTERRUPTED, "Interrupted");
    }
    xSemaphoreGive(s_lock);
}

void session_mgr_confirm_tool(const char *call_id, bool approved) {
    realtime_ws_send_tool_confirm_result(call_id, approved);
}

void session_mgr_switch_agent(const char *direction) {
    if (realtime_ws_is_connected()) {
        realtime_ws_send_agent_switch(direction);
    }
}

uint32_t session_mgr_get_rtt_ms(void) {
    return s_rtt_ms;
}

void session_mgr_set_volume(int pct, bool save_nvs, bool show_toast) {
    const int actual = app_prefs_set_volume(pct, save_nvs);
    audio_engine_set_volume(actual);

    app_prefs_t prefs;
    app_prefs_get(&prefs);
    ui_app_sync_audio_prefs(actual, prefs.echo_guard_mode, prefs.auto_shield_vol_pct);

    if (show_toast) {
        char buf[40];
        const bool shielded =
            (prefs.echo_guard_mode == AUDIO_GUARD_STRICT) ||
            (prefs.echo_guard_mode == AUDIO_GUARD_AUTO && actual >= prefs.auto_shield_vol_pct);
        snprintf(buf, sizeof(buf), shielded ? "Vol %d%% (Shield)" : "Volume %d%%", actual);
        ui_app_show_toast(buf);
    }
}

void session_mgr_adjust_volume(int delta_pct, bool show_toast) {
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    session_mgr_set_volume(prefs.speaker_volume_pct + delta_pct, true, show_toast);
}

void session_mgr_set_echo_guard(audio_echo_guard_mode_t mode, int auto_shield_vol_pct, bool show_toast) {
    app_prefs_set_echo_guard(mode, auto_shield_vol_pct);

    app_prefs_t prefs;
    app_prefs_get(&prefs);
    audio_engine_set_echo_guard(prefs.echo_guard_mode, prefs.auto_shield_vol_pct);
    ui_app_sync_audio_prefs(prefs.speaker_volume_pct, prefs.echo_guard_mode, prefs.auto_shield_vol_pct);

    if (show_toast) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Echo Guard: %s", audio_guard_mode_name(prefs.echo_guard_mode));
        ui_app_show_toast(buf);
    }
}

void session_mgr_cycle_echo_guard(bool show_toast) {
    app_prefs_t prefs;
    app_prefs_get(&prefs);
    const audio_echo_guard_mode_t next =
        (audio_echo_guard_mode_t)(((int)prefs.echo_guard_mode + 1) % AUDIO_GUARD_MODE_COUNT);
    session_mgr_set_echo_guard(next, prefs.auto_shield_vol_pct, show_toast);
}
