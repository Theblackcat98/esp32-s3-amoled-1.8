#include "audio_guard.h"

#include <string.h>

const char *audio_guard_mode_name(audio_echo_guard_mode_t mode) {
    switch (mode) {
        case AUDIO_GUARD_AUTO:   return "Auto";
        case AUDIO_GUARD_STRICT: return "Strict";
        case AUDIO_GUARD_ALWAYS: return "Always";
        default:                 return "Auto";
    }
}

int audio_guard_hangover_ms(audio_echo_guard_mode_t mode) {
    return (mode == AUDIO_GUARD_ALWAYS) ? 250 : 350;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void audio_guard_eval(const audio_guard_input_t *in, audio_guard_result_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!in) {
        out->req_onset_ms = 96;
        out->gate_db      = -49.0f;
        return;
    }

    const int vol        = clamp_int(in->volume_pct, 0, 100);
    const int shield_vol = clamp_int(in->auto_shield_vol_pct, 60, 100);
    const int margin_db  = (in->base_barge_margin_db > 0) ? in->base_barge_margin_db : 0;
    const int base_ms    = (in->base_barge_ms > 0) ? in->base_barge_ms : 220;

    if (!in->playback_busy && !in->in_hangover) {
        out->block_voice_trigger = false;
        out->shield_active       = false;
        out->gate_db             = in->noise_db + 7.0f;
        out->req_onset_ms        = 96;
        return;
    }

    const bool shield =
        (in->mode == AUDIO_GUARD_STRICT) ||
        (in->mode == AUDIO_GUARD_AUTO && vol >= shield_vol);

    if (shield) {
        out->block_voice_trigger = true;
        out->shield_active       = true;
        out->gate_db             = 0.0f;
        out->req_onset_ms        = base_ms;
        return;
    }

    if (!in->playback_busy && in->in_hangover) {
        const float echo_tail  = in->spk_env_db - 18.0f;
        const float floor_gate = in->noise_db + 12.0f;
        float       g          = (echo_tail > floor_gate) ? echo_tail : floor_gate;
        if (g > -6.0f) g = -6.0f;
        out->block_voice_trigger = false;
        out->shield_active       = false;
        out->gate_db             = g;
        out->req_onset_ms        = 160;
        return;
    }

    /* Active playback with full-duplex barge-in and speaker-coupled echo tracking */
    const float vol_over_60 = (vol > 60) ? (float)(vol - 60) : 0.0f;
    float       erle_db     = 22.0f - 0.35f * vol_over_60;
    if (erle_db < 6.0f) erle_db = 6.0f;
    if (erle_db > 24.0f) erle_db = 24.0f;

    const float noise_gate = in->noise_db + (float)margin_db + 8.0f;
    float       echo_gate  = (in->spk_env_db > -55.0f) ? (in->spk_env_db - erle_db + 6.0f) : -96.0f;
    if (echo_gate > -6.0f) echo_gate = -6.0f;

    out->block_voice_trigger = false;
    out->shield_active       = false;
    out->gate_db             = (echo_gate > noise_gate) ? echo_gate : noise_gate;
    out->req_onset_ms        = base_ms + (int)(vol_over_60 * 5.0f);
}
