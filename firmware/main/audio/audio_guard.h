#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_GUARD_AUTO       = 0,
    AUDIO_GUARD_STRICT     = 1,
    AUDIO_GUARD_ALWAYS     = 2,
    AUDIO_GUARD_MODE_COUNT = 3,
} audio_echo_guard_mode_t;

typedef struct {
    audio_echo_guard_mode_t mode;
    int                     volume_pct;
    int                     auto_shield_vol_pct;
    bool                    playback_busy;
    bool                    in_hangover;
    float                   noise_db;
    float                   spk_env_db;
    int                     base_barge_margin_db;
    int                     base_barge_ms;
} audio_guard_input_t;

typedef struct {
    bool  block_voice_trigger;
    bool  shield_active;
    float gate_db;
    int   req_onset_ms;
} audio_guard_result_t;

/** Return human-readable display name for an echo guard mode. */
const char *audio_guard_mode_name(audio_echo_guard_mode_t mode);

/** Return post-playback echo tail hangover duration in milliseconds. */
int audio_guard_hangover_ms(audio_echo_guard_mode_t mode);

/** Evaluate whether voice trigger should be blocked or dynamically gated. */
void audio_guard_eval(const audio_guard_input_t *in, audio_guard_result_t *out);

#ifdef __cplusplus
}
#endif
