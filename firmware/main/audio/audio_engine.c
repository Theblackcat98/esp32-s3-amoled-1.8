#include "audio_engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_prefs.h"
#include "audio_guard.h"
#include "audio_resampler.h"
#include "audio_ringbuf.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_engine";

#define AUDIO_SAMPLE_RATE_HZ     16000
#define DOWNLINK_RB_SAMPLES      131072 /* 256 KB in PSRAM (~4.1s @ 16kHz) */
#define DOWNLINK_PREBUF_SAMPLES  2880   /* 180 ms jitter buffer prefill */
#define PREROLL_SAMPLES          5600   /* 350 ms @ 16 kHz */
#define HOLDBACK_MAX_SAMPLES     8000   /* 500 ms max holdback buffer */
#define RESAMPLE_SCRATCH_SAMPLES 8192
#define TX_FRAME_SAMPLES         256    /* 16 ms @ 16 kHz */
#define MAX_UTTERANCE_MS         30000

static audio_engine_callbacks_t s_cbs;
static void                    *s_cb_ctx = NULL;

static i2s_chan_handle_t        s_tx_chan   = NULL;
static i2s_chan_handle_t        s_rx_chan   = NULL;
static esp_codec_dev_handle_t   s_codec_dev = NULL;

static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t        *s_afe_data   = NULL;

static audio_ringbuf_t   s_downlink_rb;
static audio_ringbuf_t   s_preroll_rb;
static int16_t          *s_holdback_buf   = NULL;
static int16_t          *s_rs_scratch     = NULL;
static audio_resampler_t s_resampler;
static SemaphoreHandle_t s_dl_mutex       = NULL;
static SemaphoreHandle_t s_codec_ctrl_mux = NULL;

static volatile bool    s_session_active      = false;
static volatile bool    s_muted               = false;
static volatile bool    s_speaking            = false;
static volatile bool    s_playback_active     = false;
static volatile bool    s_downlink_done       = false;
static volatile bool    s_flush_requested     = false;
static volatile uint8_t s_mic_rms_pct         = 0;
static volatile uint8_t s_spk_rms_pct         = 0;
static volatile int     s_volume_pct          = APP_SPEAKER_VOLUME_PCT;
static volatile int     s_guard_mode          = AUDIO_GUARD_AUTO;
static volatile int     s_auto_shield_vol_pct = APP_ECHO_SHIELD_VOL_PCT;
static volatile int16_t s_spk_env_db_x10      = -960;
static volatile uint32_t s_spk_last_active_ms = 0;
static volatile bool    s_shield_active       = false;

static uint8_t compute_rms_pct(const int16_t *pcm, size_t samples, float *out_db) {
    if (!pcm || samples == 0) {
        if (out_db) *out_db = -96.0f;
        return 0;
    }
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int32_t s = pcm[i];
        sum_sq += (uint64_t)(s * s);
    }
    const float rms      = sqrtf((float)sum_sq / (float)samples);
    const float rms_norm = rms / 32768.0f;
    if (out_db) {
        *out_db = 20.0f * log10f(rms_norm + 1e-6f);
    }
    float pct = (rms / 6000.0f) * 100.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)pct;
}

static esp_err_t init_i2s_and_codec(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear        = true;
    chan_cfg.dma_desc_num      = 6;
    chan_cfg.dma_frame_num     = 256;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    const i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port      = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BSP_I2C_NUM,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) {
        ESP_LOGE(TAG, "Failed creating codec interfaces");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin      = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = {
            .pa_voltage        = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
        .no_dac_ref  = false,
        .mclk_div    = 256,
    };
    const audio_codec_if_t *es_if = es8311_codec_new(&es_cfg);
    if (!es_if) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es_if,
        .data_if  = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_codec_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .channel_mask    = 0,
        .sample_rate     = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple   = 256,
    };
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return ESP_FAIL;
    }

    esp_codec_dev_set_out_vol(s_codec_dev, __atomic_load_n(&s_volume_pct, __ATOMIC_ACQUIRE));
    esp_codec_dev_set_in_gain(s_codec_dev, (float)APP_MIC_GAIN_DB);

    /* Lock ES8311 reg 0x44 = 0x48: Left = DAC_ref (R), Right = ADC_mic (M) -> AFE "RM" */
    esp_codec_dev_write_reg(s_codec_dev, 0x44, 0x48);
    int reg44 = 0;
    esp_codec_dev_read_reg(s_codec_dev, 0x44, &reg44);
    ESP_LOGI(TAG, "ES8311 full-duplex 16kHz ready | reg 0x44=0x%02X (L=Ref, R=Mic)", reg44 & 0xFF);
    return ESP_OK;
}

static esp_err_t init_afe(void) {
    afe_config_t *cfg = afe_config_init("RM", NULL, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    cfg->aec_init               = true;
    cfg->aec_mode               = AEC_MODE_FD_LOW_COST;
    cfg->aec_filter_length      = 4;
    cfg->ns_init                = true;
    cfg->afe_ns_mode            = AFE_NS_MODE_WEBRTC;
    cfg->vad_init               = true;
    cfg->vad_mode               = VAD_MODE_3;
    cfg->vad_model_name         = NULL;
    cfg->vad_min_speech_ms      = 128;
    cfg->vad_min_noise_ms       = 600;
    cfg->wakenet_init           = false;
    cfg->se_init                = false;
    cfg->agc_init               = false;
    cfg->memory_alloc_mode      = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core     = 1;
    cfg->afe_perferred_priority = 6;

    s_afe_handle = esp_afe_handle_from_config(cfg);
    if (!s_afe_handle) {
        afe_config_free(cfg);
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        return ESP_FAIL;
    }

    s_afe_data = s_afe_handle->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "ESP-SR AFE ready (format=RM, feed=%d ch=%d, fetch=%d)",
        s_afe_handle->get_feed_chunksize(s_afe_data),
        s_afe_handle->get_feed_channel_num(s_afe_data),
        s_afe_handle->get_fetch_chunksize(s_afe_data));
    return ESP_OK;
}

static void audio_tx_task(void *arg) {
    (void)arg;
    int16_t mono_buf[TX_FRAME_SAMPLES];
    int16_t stereo_buf[TX_FRAME_SAMPLES * 2];
    bool    primed      = false;
    int16_t last_sample = 0;
    float   spk_env_db  = -96.0f;

    while (1) {
        if (__atomic_exchange_n(&s_flush_requested, false, __ATOMIC_ACQ_REL)) {
            audio_ringbuf_flush(&s_downlink_rb);
            primed     = false;
            spk_env_db = -96.0f;
            __atomic_store_n(&s_spk_env_db_x10, (int16_t)-960, __ATOMIC_RELEASE);
            __atomic_store_n(&s_spk_last_active_ms, 0U, __ATOMIC_RELEASE);
            __atomic_store_n(&s_playback_active, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_downlink_done, false, __ATOMIC_RELEASE);

            /* Smooth 16 ms fade-out to zero so interrupting mid-wave never clicks */
            for (size_t i = 0; i < TX_FRAME_SAMPLES; ++i) {
                const float g = 1.0f - ((float)(i + 1) / (float)TX_FRAME_SAMPLES);
                const int16_t s = (int16_t)((float)last_sample * g);
                stereo_buf[i * 2]     = s;
                stereo_buf[i * 2 + 1] = s;
            }
            last_sample = 0;
            __atomic_store_n(&s_spk_rms_pct, 0, __ATOMIC_RELEASE);
            esp_codec_dev_write(s_codec_dev, stereo_buf, sizeof(stereo_buf));
            continue;
        }

        const size_t avail   = audio_ringbuf_available(&s_downlink_rb);
        const bool   dl_done = __atomic_load_n(&s_downlink_done, __ATOMIC_ACQUIRE);

        if (!primed) {
            if (avail >= DOWNLINK_PREBUF_SAMPLES || (dl_done && avail > 0)) {
                primed = true;
                __atomic_store_n(&s_playback_active, true, __ATOMIC_RELEASE);
            }
        }

        size_t got = 0;
        if (primed) {
            got = audio_ringbuf_read(&s_downlink_rb, mono_buf, TX_FRAME_SAMPLES);
            if (got < TX_FRAME_SAMPLES) {
                if (dl_done) {
                    primed = false;
                    __atomic_store_n(&s_playback_active, false, __ATOMIC_RELEASE);
                    __atomic_store_n(&s_downlink_done, false, __ATOMIC_RELEASE);
                }
                for (size_t i = got; i < TX_FRAME_SAMPLES; ++i) {
                    mono_buf[i] = 0;
                }
            }
        } else {
            memset(mono_buf, 0, sizeof(mono_buf));
            if (dl_done && avail == 0) {
                __atomic_store_n(&s_playback_active, false, __ATOMIC_RELEASE);
                __atomic_store_n(&s_downlink_done, false, __ATOMIC_RELEASE);
            }
        }

        float spk_frame_db = -96.0f;
        if (got > 0) {
            last_sample = mono_buf[got - 1];
            __atomic_store_n(&s_spk_rms_pct, compute_rms_pct(mono_buf, got, &spk_frame_db), __ATOMIC_RELEASE);
            if (spk_frame_db > -52.0f) {
                const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000LL);
                __atomic_store_n(&s_spk_last_active_ms, (ms != 0U) ? ms : 1U, __ATOMIC_RELEASE);
            }
        } else {
            last_sample = 0;
            __atomic_store_n(&s_spk_rms_pct, 0, __ATOMIC_RELEASE);
        }

        if (spk_frame_db > spk_env_db) {
            spk_env_db = spk_frame_db;
        } else {
            spk_env_db -= 1.2f;
            if (spk_env_db < -96.0f) spk_env_db = -96.0f;
        }
        __atomic_store_n(&s_spk_env_db_x10, (int16_t)(spk_env_db * 10.0f), __ATOMIC_RELEASE);

        for (size_t i = 0; i < TX_FRAME_SAMPLES; ++i) {
            stereo_buf[i * 2]     = mono_buf[i];
            stereo_buf[i * 2 + 1] = mono_buf[i];
        }
        esp_codec_dev_write(s_codec_dev, stereo_buf, sizeof(stereo_buf));
    }
}

static void audio_rx_task(void *arg) {
    (void)arg;
    const int chunk_samples = s_afe_handle->get_feed_chunksize(s_afe_data);
    const int ch_num        = s_afe_handle->get_feed_channel_num(s_afe_data);
    const size_t rx_bytes   = (size_t)chunk_samples * (size_t)ch_num * sizeof(int16_t);

    int16_t *rx_buf = (int16_t *)heap_caps_malloc(rx_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!rx_buf) {
        rx_buf = (int16_t *)heap_caps_malloc(rx_bytes, MALLOC_CAP_8BIT);
    }
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate I2S RX buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (esp_codec_dev_read(s_codec_dev, rx_buf, (int)rx_bytes) == ESP_CODEC_DEV_OK) {
            s_afe_handle->feed(s_afe_data, rx_buf);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

static void flush_holdback_to_uplink(size_t *hold_count) {
    if (*hold_count == 0 || !s_cbs.on_uplink_frame) {
        *hold_count = 0;
        return;
    }
    size_t pos = 0;
    while (pos < *hold_count) {
        const size_t n = (*hold_count - pos > 640) ? 640 : (*hold_count - pos);
        s_cbs.on_uplink_frame(&s_holdback_buf[pos], n, s_cb_ctx);
        pos += n;
    }
    *hold_count = 0;
}

static void afe_fetch_task(void *arg) {
    (void)arg;
    float  noise_db        = -56.0f;
    bool   in_utter        = false;
    bool   confirmed       = false;
    int    onset_ms        = 0;
    int    speech_ms       = 0;
    int    silence_ms      = 0;
    int    utter_ms        = 0;
    size_t hold_count      = 0;

    while (1) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL || !res->data || res->data_size <= 0) {
            continue;
        }

        const size_t samples  = (size_t)res->data_size / sizeof(int16_t);
        const int    frame_ms = (int)((samples * 1000U) / AUDIO_SAMPLE_RATE_HZ);

        if (__atomic_load_n(&s_muted, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s_mic_rms_pct, 0, __ATOMIC_RELEASE);
            if (in_utter) {
                if (confirmed && s_cbs.on_speech_end) {
                    s_cbs.on_speech_end(s_cb_ctx);
                }
                in_utter   = false;
                confirmed  = false;
                hold_count = 0;
            }
            onset_ms = speech_ms = silence_ms = utter_ms = 0;
            continue;
        }

        float frame_db = -96.0f;
        __atomic_store_n(&s_mic_rms_pct, compute_rms_pct(res->data, samples, &frame_db), __ATOMIC_RELEASE);

        const bool session_active = __atomic_load_n(&s_session_active, __ATOMIC_ACQUIRE);
        const bool busy =
            __atomic_load_n(&s_speaking, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&s_playback_active, __ATOMIC_ACQUIRE);

        const audio_echo_guard_mode_t guard_mode =
            (audio_echo_guard_mode_t)__atomic_load_n(&s_guard_mode, __ATOMIC_ACQUIRE);
        const int vol_pct        = __atomic_load_n(&s_volume_pct, __ATOMIC_ACQUIRE);
        const int shield_vol_pct = __atomic_load_n(&s_auto_shield_vol_pct, __ATOMIC_ACQUIRE);
        const int16_t spk_x10    = __atomic_load_n(&s_spk_env_db_x10, __ATOMIC_ACQUIRE);
        const float   spk_env_db = (float)spk_x10 * 0.1f;
        const uint32_t last_spk  = __atomic_load_n(&s_spk_last_active_ms, __ATOMIC_ACQUIRE);
        const uint32_t now_ms    = (uint32_t)(esp_timer_get_time() / 1000LL);
        const int hangover_ms    = audio_guard_hangover_ms(guard_mode);
        const bool in_hangover =
            !busy && (last_spk != 0U) && ((uint32_t)(now_ms - last_spk) < (uint32_t)hangover_ms);

        const audio_guard_input_t guard_in = {
            .mode                 = guard_mode,
            .volume_pct           = vol_pct,
            .auto_shield_vol_pct  = shield_vol_pct,
            .playback_busy        = busy,
            .in_hangover          = in_hangover,
            .noise_db             = noise_db,
            .spk_env_db           = spk_env_db,
            .base_barge_margin_db = APP_BARGE_IN_MARGIN_DB,
            .base_barge_ms        = APP_BARGE_IN_MS,
        };
        audio_guard_result_t guard_res;
        audio_guard_eval(&guard_in, &guard_res);
        __atomic_store_n(&s_shield_active, guard_res.shield_active, __ATOMIC_RELEASE);

        if (!in_utter) {
            if (guard_res.block_voice_trigger) {
                onset_ms = 0;
                audio_ringbuf_flush(&s_preroll_rb);
                continue;
            }

            audio_ringbuf_write_overwrite(&s_preroll_rb, res->data, samples);

            if (!busy && !in_hangover && res->vad_state == VAD_SILENCE && frame_db < noise_db + 6.0f) {
                noise_db = noise_db * 0.92f + frame_db * 0.08f;
                if (noise_db < -72.0f) noise_db = -72.0f;
                if (noise_db > -28.0f) noise_db = -28.0f;
            }

            if (!session_active) {
                onset_ms = 0;
                continue;
            }

            const bool trigger_frame = (res->vad_state == VAD_SPEECH) && (frame_db > guard_res.gate_db);
            if (trigger_frame) {
                onset_ms += frame_ms;
                const int req_ms = guard_res.req_onset_ms;
                if (onset_ms >= req_ms) {
                    onset_ms   = 0;
                    in_utter   = true;
                    speech_ms  = req_ms;
                    silence_ms = 0;
                    utter_ms   = 0;

                    if (s_cbs.on_speech_start) {
                        s_cbs.on_speech_start(busy, s_cb_ctx);
                    }

                    hold_count = audio_ringbuf_read(&s_preroll_rb, s_holdback_buf, HOLDBACK_MAX_SAMPLES);
                    if (hold_count + samples <= HOLDBACK_MAX_SAMPLES) {
                        memcpy(&s_holdback_buf[hold_count], res->data, samples * sizeof(int16_t));
                        hold_count += samples;
                    }

                    if (busy) {
                        /* Barge-in already cleared 220ms+ gate; stream pre-roll immediately */
                        confirmed = true;
                        flush_holdback_to_uplink(&hold_count);
                    } else {
                        confirmed = (speech_ms >= APP_VAD_HOLDBACK_MS);
                        if (confirmed) {
                            flush_holdback_to_uplink(&hold_count);
                        }
                    }
                }
            } else {
                onset_ms = 0;
            }
            continue;
        }

        if (!session_active) {
            if (confirmed && s_cbs.on_speech_end) {
                s_cbs.on_speech_end(s_cb_ctx);
            }
            in_utter   = false;
            confirmed  = false;
            hold_count = 0;
            continue;
        }

        utter_ms += frame_ms;
        const bool voice_frame = (res->vad_state == VAD_SPEECH) || (frame_db > noise_db + 6.0f);
        if (voice_frame) {
            speech_ms += frame_ms;
            silence_ms = 0;
        } else {
            silence_ms += frame_ms;
        }

        if (!confirmed) {
            if (hold_count + samples <= HOLDBACK_MAX_SAMPLES) {
                memcpy(&s_holdback_buf[hold_count], res->data, samples * sizeof(int16_t));
                hold_count += samples;
            }
            if (speech_ms >= APP_VAD_HOLDBACK_MS) {
                confirmed = true;
                flush_holdback_to_uplink(&hold_count);
            } else if (silence_ms >= 350) {
                /* Transient noise burst before confirmation -> discard quietly */
                in_utter   = false;
                confirmed  = false;
                hold_count = 0;
                speech_ms = silence_ms = utter_ms = 0;
                continue;
            }
        } else if (s_cbs.on_uplink_frame) {
            s_cbs.on_uplink_frame(res->data, samples, s_cb_ctx);
        }

        if (silence_ms >= APP_VAD_SILENCE_MS || utter_ms >= MAX_UTTERANCE_MS) {
            if (confirmed && s_cbs.on_speech_end) {
                s_cbs.on_speech_end(s_cb_ctx);
            }
            in_utter   = false;
            confirmed  = false;
            hold_count = 0;
            speech_ms = silence_ms = utter_ms = 0;
        }
    }
}

esp_err_t audio_engine_init(const audio_engine_callbacks_t *cbs, void *cb_ctx) {
    if (cbs) {
        s_cbs = *cbs;
    }
    s_cb_ctx = cb_ctx;

    app_prefs_t prefs;
    app_prefs_get(&prefs);
    __atomic_store_n(&s_volume_pct, prefs.speaker_volume_pct, __ATOMIC_RELEASE);
    __atomic_store_n(&s_guard_mode, (int)prefs.echo_guard_mode, __ATOMIC_RELEASE);
    __atomic_store_n(&s_auto_shield_vol_pct, prefs.auto_shield_vol_pct, __ATOMIC_RELEASE);

    s_dl_mutex       = xSemaphoreCreateMutex();
    s_codec_ctrl_mux = xSemaphoreCreateMutex();
    if (!s_dl_mutex || !s_codec_ctrl_mux) return ESP_ERR_NO_MEM;

    if (!audio_ringbuf_init(&s_downlink_rb, DOWNLINK_RB_SAMPLES, true) ||
        !audio_ringbuf_init(&s_preroll_rb, PREROLL_SAMPLES, true)) {
        ESP_LOGE(TAG, "Failed allocating PSRAM audio ring buffers");
        return ESP_ERR_NO_MEM;
    }

    s_holdback_buf = (int16_t *)heap_caps_malloc(HOLDBACK_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rs_scratch   = (int16_t *)heap_caps_malloc(RESAMPLE_SCRATCH_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_holdback_buf || !s_rs_scratch) {
        ESP_LOGE(TAG, "Failed allocating PSRAM scratch buffers");
        return ESP_ERR_NO_MEM;
    }

    audio_resampler_init(&s_resampler, 24000, AUDIO_SAMPLE_RATE_HZ);

    esp_err_t err = init_i2s_and_codec();
    if (err != ESP_OK) return err;

    err = init_afe();
    if (err != ESP_OK) return err;

    xTaskCreatePinnedToCore(audio_tx_task, "audio_tx", 4096, NULL, 8, NULL, 1);
    xTaskCreatePinnedToCore(audio_rx_task, "audio_rx", 4096, NULL, 8, NULL, 1);
    xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 6144, NULL, 7, NULL, 1);
    return ESP_OK;
}

void audio_engine_set_session_active(bool active) {
    __atomic_store_n(&s_session_active, active, __ATOMIC_RELEASE);
}

void audio_engine_set_muted(bool muted) {
    __atomic_store_n(&s_muted, muted, __ATOMIC_RELEASE);
}

bool audio_engine_is_muted(void) {
    return __atomic_load_n(&s_muted, __ATOMIC_ACQUIRE);
}

void audio_engine_set_speaking(bool speaking) {
    __atomic_store_n(&s_speaking, speaking, __ATOMIC_RELEASE);
}

void audio_engine_push_downlink(const int16_t *pcm, size_t samples, int sample_rate) {
    if (!pcm || samples == 0 || !s_dl_mutex) return;

    xSemaphoreTake(s_dl_mutex, portMAX_DELAY);
    if (sample_rate > 0 && sample_rate != s_resampler.in_rate) {
        audio_resampler_init(&s_resampler, sample_rate, AUDIO_SAMPLE_RATE_HZ);
    }

    size_t offset = 0;
    while (offset < samples) {
        size_t chunk = samples - offset;
        if (chunk > 4096) chunk = 4096;
        const size_t out_n = audio_resampler_process(
            &s_resampler,
            &pcm[offset],
            chunk,
            s_rs_scratch,
            RESAMPLE_SCRATCH_SAMPLES);
        if (out_n > 0) {
            size_t written = audio_ringbuf_write(&s_downlink_rb, s_rs_scratch, out_n);
            /* Brief backpressure retry if network burst outruns 4s ring buffer */
            int retries = 0;
            while (written < out_n && retries < 20 && !__atomic_load_n(&s_flush_requested, __ATOMIC_ACQUIRE)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                written += audio_ringbuf_write(&s_downlink_rb, s_rs_scratch + written, out_n - written);
                ++retries;
            }
        }
        offset += chunk;
    }
    xSemaphoreGive(s_dl_mutex);
}

void audio_engine_mark_downlink_done(void) {
    __atomic_store_n(&s_downlink_done, true, __ATOMIC_RELEASE);
}

void audio_engine_flush_playback(void) {
    __atomic_store_n(&s_playback_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_downlink_done, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_spk_env_db_x10, (int16_t)-960, __ATOMIC_RELEASE);
    __atomic_store_n(&s_spk_last_active_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_flush_requested, true, __ATOMIC_RELEASE);

    if (s_dl_mutex && xSemaphoreTake(s_dl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        audio_resampler_reset(&s_resampler);
        xSemaphoreGive(s_dl_mutex);
    }
}

bool audio_engine_is_playback_active(void) {
    return __atomic_load_n(&s_playback_active, __ATOMIC_ACQUIRE);
}

void audio_engine_get_levels(uint8_t *mic_rms_pct, uint8_t *spk_rms_pct) {
    if (mic_rms_pct) *mic_rms_pct = __atomic_load_n(&s_mic_rms_pct, __ATOMIC_ACQUIRE);
    if (spk_rms_pct) *spk_rms_pct = __atomic_load_n(&s_spk_rms_pct, __ATOMIC_ACQUIRE);
}

void audio_engine_set_pa_enabled(bool enabled) {
    gpio_set_level(BSP_POWER_AMP_IO, enabled ? 1 : 0);
}

esp_err_t audio_engine_set_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    __atomic_store_n(&s_volume_pct, pct, __ATOMIC_RELEASE);
    if (!s_codec_dev) return ESP_ERR_INVALID_STATE;
    if (s_codec_ctrl_mux) xSemaphoreTake(s_codec_ctrl_mux, portMAX_DELAY);
    const int err = esp_codec_dev_set_out_vol(s_codec_dev, pct);
    if (s_codec_ctrl_mux) xSemaphoreGive(s_codec_ctrl_mux);
    return (err == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

int audio_engine_get_volume(void) {
    return __atomic_load_n(&s_volume_pct, __ATOMIC_ACQUIRE);
}

void audio_engine_set_echo_guard(audio_echo_guard_mode_t mode, int auto_shield_vol_pct) {
    const int safe_mode = (mode < AUDIO_GUARD_MODE_COUNT) ? (int)mode : (int)AUDIO_GUARD_AUTO;
    if (auto_shield_vol_pct < 60) auto_shield_vol_pct = 60;
    if (auto_shield_vol_pct > 100) auto_shield_vol_pct = 100;
    __atomic_store_n(&s_guard_mode, safe_mode, __ATOMIC_RELEASE);
    __atomic_store_n(&s_auto_shield_vol_pct, auto_shield_vol_pct, __ATOMIC_RELEASE);
}

bool audio_engine_is_echo_shield_active(void) {
    return __atomic_load_n(&s_shield_active, __ATOMIC_ACQUIRE);
}
