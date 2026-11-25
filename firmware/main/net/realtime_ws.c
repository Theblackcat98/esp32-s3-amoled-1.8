#include "realtime_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "audio_ringbuf.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "realtime_ws";

#define WS_RX_BUF_BYTES        (128U * 1024U)
#define WS_B64_DEC_BUF_BYTES   (48U * 1024U)
#define WS_TX_UP_RB_SAMPLES    16000 /* 1 second @ 16 kHz in PSRAM */
#define WS_TX_CHUNK_SAMPLES    640   /* 40 ms @ 16 kHz */
#define WS_TX_JSON_BUF_BYTES   4096
#define WS_CMD_QUEUE_LEN       16

typedef enum {
    WS_CMD_SESSION_UPDATE = 0,
    WS_CMD_AUDIO_COMMIT,
    WS_CMD_RESPONSE_CREATE,
    WS_CMD_RESPONSE_CANCEL,
    WS_CMD_TOOL_CONFIRM,
    WS_CMD_AGENT_SWITCH,
} ws_cmd_type_t;

typedef struct {
    ws_cmd_type_t type;
    char          str_a[64];
    char          str_b[32];
    bool          flag;
} ws_cmd_t;

static realtime_ws_callbacks_t s_cbs;
static void                   *s_cb_ctx = NULL;

static esp_websocket_client_handle_t s_client = NULL;
static volatile bool                 s_connected = false;
static volatile bool                 s_should_run = false;

static char          *s_rx_buf        = NULL;
static uint8_t       *s_b64_dec_buf   = NULL;
static char          *s_tx_json_buf   = NULL;
static char          *s_tx_b64_buf    = NULL;
static char           s_uri_buf[256]  = {0};
static char           s_auth_hdr[192] = {0};

static audio_ringbuf_t s_uplink_rb;
static int16_t        *s_tx_pcm_chunk = NULL;
static QueueHandle_t   s_cmd_queue    = NULL;
static int64_t         s_last_ping_us = 0;

static uint32_t parse_hex_rgb(const char *s, bool *ok) {
    if (ok) *ok = false;
    if (!s || s[0] != '#' || strlen(s) != 7) return 0;
    char *end = NULL;
    unsigned long v = strtoul(s + 1, &end, 16);
    if (!end || *end != '\0') return 0;
    if (ok) *ok = true;
    return (uint32_t)(v & 0xFFFFFFUL);
}

static bool send_text_blocking(const char *txt, int len) {
    if (!s_client || !s_connected || !txt || len <= 0) return false;
    const int sent = esp_websocket_client_send_text(s_client, txt, len, pdMS_TO_TICKS(1000));
    return sent >= 0;
}

static void send_session_update_now(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *sess = cJSON_CreateObject();
    cJSON *mods = cJSON_CreateArray();
    if (!root || !sess || !mods) {
        cJSON_Delete(root);
        cJSON_Delete(sess);
        cJSON_Delete(mods);
        return;
    }

    cJSON_AddItemToArray(mods, cJSON_CreateString("text"));
    cJSON_AddItemToArray(mods, cJSON_CreateString("audio"));

    cJSON_AddStringToObject(sess, "type", "realtime");
    cJSON_AddStringToObject(sess, "instructions", APP_AGENT_INSTRUCTIONS);
    cJSON_AddItemToObject(sess, "output_modalities", mods);

    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddItemToObject(root, "session", sess);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload) {
        send_text_blocking(payload, (int)strlen(payload));
        cJSON_free(payload);
    }
}

static void handle_json_event(const char *json_str, int len) {
    cJSON *root = cJSON_ParseWithLength(json_str, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "Failed parsing WS JSON (%d bytes)", len);
        return;
    }

    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *ev_type = cJSON_IsString(type_item) ? type_item->valuestring : "";

    if (strcmp(ev_type, "session.created") == 0) {
        int in_rate  = 16000;
        int out_rate = 24000;
        const cJSON *sess  = cJSON_GetObjectItemCaseSensitive(root, "session");
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(sess, "audio");
        const cJSON *in_o  = cJSON_GetObjectItemCaseSensitive(audio, "input");
        const cJSON *in_f  = cJSON_GetObjectItemCaseSensitive(in_o, "format");
        const cJSON *in_r  = cJSON_GetObjectItemCaseSensitive(in_f, "rate");
        if (cJSON_IsNumber(in_r)) in_rate = in_r->valueint;

        const cJSON *out_o = cJSON_GetObjectItemCaseSensitive(audio, "output");
        const cJSON *out_f = cJSON_GetObjectItemCaseSensitive(out_o, "format");
        const cJSON *out_r = cJSON_GetObjectItemCaseSensitive(out_f, "rate");
        if (cJSON_IsNumber(out_r)) out_rate = out_r->valueint;

        ESP_LOGI(TAG, "session.created (in=%d Hz, out=%d Hz)", in_rate, out_rate);
        ws_cmd_t cmd = {.type = WS_CMD_SESSION_UPDATE};
        xQueueSend(s_cmd_queue, &cmd, 0);
        if (s_cbs.on_session_ready) {
            s_cbs.on_session_ready(in_rate, out_rate, s_cb_ctx);
        }
    } else if (strcmp(ev_type, "session.updated") == 0) {
        ESP_LOGI(TAG, "session.updated confirmed");
    } else if (strcmp(ev_type, "response.created") == 0) {
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
        const cJSON *id   = cJSON_GetObjectItemCaseSensitive(resp, "id");
        if (cJSON_IsString(id) && s_cbs.on_response_created) {
            s_cbs.on_response_created(id->valuestring, s_cb_ctx);
        }
    } else if (strcmp(ev_type, "response.output_audio.delta") == 0) {
        const cJSON *rid   = cJSON_GetObjectItemCaseSensitive(root, "response_id");
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring[0] != '\0') {
            const size_t b64_len = strlen(delta->valuestring);
            size_t out_len       = 0;
            if (mbedtls_base64_decode(
                    s_b64_dec_buf,
                    WS_B64_DEC_BUF_BYTES,
                    &out_len,
                    (const unsigned char *)delta->valuestring,
                    b64_len) == 0 &&
                out_len >= 2 && s_cbs.on_audio_delta) {
                s_cbs.on_audio_delta(
                    cJSON_IsString(rid) ? rid->valuestring : "",
                    (const int16_t *)s_b64_dec_buf,
                    out_len / sizeof(int16_t),
                    s_cb_ctx);
            }
        }
    } else if (strcmp(ev_type, "response.output_text.delta") == 0) {
        const cJSON *rid   = cJSON_GetObjectItemCaseSensitive(root, "response_id");
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(delta) && s_cbs.on_text_delta) {
            s_cbs.on_text_delta(
                cJSON_IsString(rid) ? rid->valuestring : "",
                delta->valuestring,
                s_cb_ctx);
        }
    } else if (strcmp(ev_type, "response.done") == 0) {
        const cJSON *resp   = cJSON_GetObjectItemCaseSensitive(root, "response");
        const cJSON *rid    = cJSON_GetObjectItemCaseSensitive(resp, "id");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
        if (s_cbs.on_response_done) {
            s_cbs.on_response_done(
                cJSON_IsString(rid) ? rid->valuestring : "",
                cJSON_IsString(status) ? status->valuestring : "completed",
                s_cb_ctx);
        }
    } else if (strcmp(ev_type, "error") == 0) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        const char  *txt = cJSON_IsString(msg) ? msg->valuestring : "Server error";
        ESP_LOGE(TAG, "Server error: %s", txt);
        if (s_cbs.on_error) {
            s_cbs.on_error(txt, s_cb_ctx);
        }
    } else if (strcmp(ev_type, "harness.agent.info") == 0) {
        const cJSON *id     = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *name   = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *role   = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON *accent = cJSON_GetObjectItemCaseSensitive(root, "accent_color");
        const cJSON *style  = cJSON_GetObjectItemCaseSensitive(root, "orb_style");
        bool has_accent     = false;
        const uint32_t rgb  = parse_hex_rgb(cJSON_IsString(accent) ? accent->valuestring : NULL, &has_accent);
        if (s_cbs.on_harness_agent_info) {
            s_cbs.on_harness_agent_info(
                cJSON_IsString(id) ? id->valuestring : "",
                cJSON_IsString(name) ? name->valuestring : NULL,
                cJSON_IsString(role) ? role->valuestring : NULL,
                rgb,
                has_accent,
                cJSON_IsString(style) ? style->valuestring : NULL,
                s_cb_ctx);
        }
    } else if (strcmp(ev_type, "harness.tool.status") == 0) {
        const cJSON *cid   = cJSON_GetObjectItemCaseSensitive(root, "call_id");
        const cJSON *name  = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (s_cbs.on_harness_tool_status) {
            s_cbs.on_harness_tool_status(
                cJSON_IsString(cid) ? cid->valuestring : "",
                cJSON_IsString(name) ? name->valuestring : "tool",
                cJSON_IsString(state) ? state->valuestring : "running",
                s_cb_ctx);
        }
    } else if (strcmp(ev_type, "harness.tool.confirm") == 0) {
        const cJSON *cid     = cJSON_GetObjectItemCaseSensitive(root, "call_id");
        const cJSON *name    = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary");
        if (s_cbs.on_harness_tool_confirm) {
            s_cbs.on_harness_tool_confirm(
                cJSON_IsString(cid) ? cid->valuestring : "",
                cJSON_IsString(name) ? name->valuestring : "tool",
                cJSON_IsString(summary) ? summary->valuestring : "Allow action?",
                s_cb_ctx);
        }
    } else if (strcmp(ev_type, "conversation.item.input_audio_transcription.completed") == 0 ||
               strcmp(ev_type, "harness.user.transcript") == 0) {
        const cJSON *txt = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (!cJSON_IsString(txt)) {
            txt = cJSON_GetObjectItemCaseSensitive(root, "text");
        }
        if (cJSON_IsString(txt) && txt->valuestring[0] != '\0' && s_cbs.on_user_transcript) {
            s_cbs.on_user_transcript(txt->valuestring, s_cb_ctx);
        }
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *data = (const esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "WebSocket connected to %s", s_uri_buf);
            if (s_cbs.on_ws_connected) {
                s_cbs.on_ws_connected(true, s_cb_ctx);
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            s_connected = false;
            ESP_LOGW(TAG, "WebSocket disconnected (event=%ld)", (long)event_id);
            if (s_cbs.on_ws_connected) {
                s_cbs.on_ws_connected(false, s_cb_ctx);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket transport error (http_status=%d, errno=%d)",
                     data->error_handle.esp_ws_handshake_status_code,
                     data->error_handle.esp_transport_sock_errno);
            if (s_cbs.on_error) {
                s_cbs.on_error("WebSocket connection error", s_cb_ctx);
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x0A && s_last_ping_us > 0) {
                const uint32_t rtt_ms = (uint32_t)((esp_timer_get_time() - s_last_ping_us) / 1000LL);
                if (s_cbs.on_rtt_sample) {
                    s_cbs.on_rtt_sample(rtt_ms, s_cb_ctx);
                }
                break;
            }
            if (data->op_code != 0x01 && data->op_code != 0x00) {
                break;
            }
            if (!data->data_ptr || data->data_len <= 0) {
                break;
            }
            if (data->payload_offset + data->data_len >= (int)WS_RX_BUF_BYTES) {
                ESP_LOGW(TAG, "WS payload overflow (%d bytes), dropping", data->payload_offset + data->data_len);
                break;
            }
            memcpy(&s_rx_buf[data->payload_offset], data->data_ptr, (size_t)data->data_len);
            if (data->payload_offset + data->data_len >= data->payload_len && data->fin) {
                s_rx_buf[data->payload_len] = '\0';
                handle_json_event(s_rx_buf, data->payload_len);
            }
            break;

        default:
            break;
    }
}

static void ws_tx_task(void *arg) {
    (void)arg;
    ws_cmd_t cmd;

    while (1) {
        bool did_work = false;

        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            did_work = true;
            if (!s_connected) continue;

            switch (cmd.type) {
                case WS_CMD_SESSION_UPDATE:
                    send_session_update_now();
                    break;
                case WS_CMD_AUDIO_COMMIT: {
                    static const char kCommit[] = "{\"type\":\"input_audio_buffer.commit\"}";
                    send_text_blocking(kCommit, sizeof(kCommit) - 1);
                    break;
                }
                case WS_CMD_RESPONSE_CREATE: {
                    static const char kCreate[] = "{\"type\":\"response.create\"}";
                    send_text_blocking(kCreate, sizeof(kCreate) - 1);
                    break;
                }
                case WS_CMD_RESPONSE_CANCEL: {
                    static const char kCancel[] = "{\"type\":\"response.cancel\"}";
                    send_text_blocking(kCancel, sizeof(kCancel) - 1);
                    break;
                }
                case WS_CMD_TOOL_CONFIRM: {
                    const int n = snprintf(
                        s_tx_json_buf,
                        WS_TX_JSON_BUF_BYTES,
                        "{\"type\":\"harness.tool.confirm_result\",\"call_id\":\"%s\",\"approved\":%s}",
                        cmd.str_a,
                        cmd.flag ? "true" : "false");
                    if (n > 0) send_text_blocking(s_tx_json_buf, n);
                    break;
                }
                case WS_CMD_AGENT_SWITCH: {
                    const int n = snprintf(
                        s_tx_json_buf,
                        WS_TX_JSON_BUF_BYTES,
                        "{\"type\":\"harness.agent.switch\",\"direction\":\"%s\"}",
                        cmd.str_a);
                    if (n > 0) send_text_blocking(s_tx_json_buf, n);
                    break;
                }
            }
        }

        if (s_connected) {
            const size_t avail = audio_ringbuf_available(&s_uplink_rb);
            if (avail >= WS_TX_CHUNK_SAMPLES) {
                const size_t got = audio_ringbuf_read(&s_uplink_rb, s_tx_pcm_chunk, WS_TX_CHUNK_SAMPLES);
                if (got > 0) {
                    size_t b64_len = 0;
                    if (mbedtls_base64_encode(
                            (unsigned char *)s_tx_b64_buf,
                            2048,
                            &b64_len,
                            (const unsigned char *)s_tx_pcm_chunk,
                            got * sizeof(int16_t)) == 0) {
                        s_tx_b64_buf[b64_len] = '\0';
                        const int n = snprintf(
                            s_tx_json_buf,
                            WS_TX_JSON_BUF_BYTES,
                            "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                            s_tx_b64_buf);
                        if (n > 0 && n < WS_TX_JSON_BUF_BYTES) {
                            send_text_blocking(s_tx_json_buf, n);
                        }
                    }
                    did_work = true;
                }
            }
            const int64_t now_us = esp_timer_get_time();
            if (now_us - s_last_ping_us >= 5000000LL) {
                s_last_ping_us = now_us;
            }
        } else {
            audio_ringbuf_flush(&s_uplink_rb);
        }

        if (!did_work) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t realtime_ws_init(const realtime_ws_callbacks_t *cbs, void *cb_ctx) {
    if (cbs) s_cbs = *cbs;
    s_cb_ctx = cb_ctx;

    s_rx_buf       = (char *)heap_caps_malloc(WS_RX_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_b64_dec_buf  = (uint8_t *)heap_caps_malloc(WS_B64_DEC_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx_json_buf  = (char *)heap_caps_malloc(WS_TX_JSON_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx_b64_buf   = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx_pcm_chunk = (int16_t *)heap_caps_malloc(WS_TX_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_buf || !s_b64_dec_buf || !s_tx_json_buf || !s_tx_b64_buf || !s_tx_pcm_chunk) {
        return ESP_ERR_NO_MEM;
    }

    if (!audio_ringbuf_init(&s_uplink_rb, WS_TX_UP_RB_SAMPLES, true)) {
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(WS_CMD_QUEUE_LEN, sizeof(ws_cmd_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    snprintf(
        s_uri_buf,
        sizeof(s_uri_buf),
        "ws://%s:%d%s?model=%s",
        LOCAL_SERVER_HOST,
        LOCAL_SERVER_PORT,
        APP_WS_PATH,
        APP_WS_MODEL);

    if (strlen(LOCAL_SERVER_API_KEY) > 0) {
        snprintf(s_auth_hdr, sizeof(s_auth_hdr), "Authorization: Bearer %s\r\n", LOCAL_SERVER_API_KEY);
    } else {
        s_auth_hdr[0] = '\0';
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri                  = s_uri_buf,
        .headers              = (s_auth_hdr[0] != '\0') ? s_auth_hdr : NULL,
        .buffer_size          = 16384,
        .task_core_id_set     = true,
        .task_core_id         = 0,
        .task_prio            = 6,
        .task_stack           = 6144,
        .reconnect_timeout_ms = 2500,
        .network_timeout_ms   = 10000,
        .ping_interval_sec    = 5,
    };
    s_client = esp_websocket_client_init(&ws_cfg);
    if (!s_client) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL));
    xTaskCreatePinnedToCore(ws_tx_task, "ws_tx", 4096, NULL, 6, NULL, 0);
    return ESP_OK;
}

esp_err_t realtime_ws_connect(void) {
    if (!s_client) return ESP_ERR_INVALID_STATE;
    if (s_should_run) return ESP_OK;
    s_should_run = true;
    return esp_websocket_client_start(s_client);
}

void realtime_ws_disconnect(void) {
    s_should_run = false;
    s_connected  = false;
    audio_ringbuf_flush(&s_uplink_rb);
    if (s_client) {
        esp_websocket_client_stop(s_client);
    }
}

bool realtime_ws_is_connected(void) {
    return s_connected;
}

bool realtime_ws_send_audio_append(const int16_t *pcm_16k, size_t samples) {
    if (!s_connected || !pcm_16k || samples == 0) return false;
    return audio_ringbuf_write(&s_uplink_rb, pcm_16k, samples) == samples;
}

bool realtime_ws_send_audio_commit(void) {
    /* Flush any partial queued audio before committing */
    const size_t avail = audio_ringbuf_available(&s_uplink_rb);
    if (avail > 0 && avail < WS_TX_CHUNK_SAMPLES) {
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    const ws_cmd_t cmd = {.type = WS_CMD_AUDIO_COMMIT};
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}

bool realtime_ws_send_response_create(void) {
    const ws_cmd_t cmd = {.type = WS_CMD_RESPONSE_CREATE};
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}

bool realtime_ws_send_response_cancel(void) {
    const ws_cmd_t cmd = {.type = WS_CMD_RESPONSE_CANCEL};
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}

bool realtime_ws_send_tool_confirm_result(const char *call_id, bool approved) {
    ws_cmd_t cmd = {.type = WS_CMD_TOOL_CONFIRM, .flag = approved};
    snprintf(cmd.str_a, sizeof(cmd.str_a), "%s", call_id ? call_id : "");
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}

bool realtime_ws_send_agent_switch(const char *direction) {
    ws_cmd_t cmd = {.type = WS_CMD_AGENT_SWITCH};
    snprintf(cmd.str_a, sizeof(cmd.str_a), "%s", direction ? direction : "next");
    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}
