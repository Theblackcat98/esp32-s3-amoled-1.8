#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_guard.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_speech_start)(bool during_playback, void *ctx);
    void (*on_uplink_frame)(const int16_t *pcm_16k, size_t samples, void *ctx);
    void (*on_speech_end)(void *ctx);
} audio_engine_callbacks_t;

esp_err_t audio_engine_init(const audio_engine_callbacks_t *cbs, void *cb_ctx);

void audio_engine_set_session_active(bool active);
void audio_engine_set_muted(bool muted);
bool audio_engine_is_muted(void);

void audio_engine_set_speaking(bool speaking);
void audio_engine_push_downlink(const int16_t *pcm, size_t samples, int sample_rate);
void audio_engine_mark_downlink_done(void);
void audio_engine_flush_playback(void);
bool audio_engine_is_playback_active(void);

void audio_engine_get_levels(uint8_t *mic_rms_pct, uint8_t *spk_rms_pct);
void audio_engine_set_pa_enabled(bool enabled);

esp_err_t audio_engine_set_volume(int pct);
int       audio_engine_get_volume(void);
void      audio_engine_set_echo_guard(audio_echo_guard_mode_t mode, int auto_shield_vol_pct);
bool      audio_engine_is_echo_shield_active(void);

#ifdef __cplusplus
}
#endif
