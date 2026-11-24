#include "audio_resampler.h"

#include <math.h>
#include <string.h>

void audio_resampler_init(audio_resampler_t *r, int32_t in_rate, int32_t out_rate) {
    if (!r) return;
    r->in_rate  = (in_rate > 0) ? in_rate : 24000;
    r->out_rate = (out_rate > 0) ? out_rate : 16000;
    audio_resampler_reset(r);
}

void audio_resampler_reset(audio_resampler_t *r) {
    if (!r) return;
    r->phase_num = 0;
    r->prev_raw2 = 0.0f;
    r->prev_raw1 = 0.0f;
    r->filt_prev = 0.0f;
    r->primed    = false;
}

size_t audio_resampler_max_out(const audio_resampler_t *r, size_t in_count) {
    if (!r || in_count == 0) return 0;
    if (r->in_rate == r->out_rate) return in_count;
    return (size_t)(((uint64_t)in_count * (uint64_t)r->out_rate) / (uint64_t)r->in_rate) + 2;
}

static inline int16_t clamp_i16(float v) {
    const long iv = lroundf(v);
    if (iv > 32767L) return 32767;
    if (iv < -32768L) return -32768;
    return (int16_t)iv;
}

size_t audio_resampler_process(
    audio_resampler_t *r,
    const int16_t     *in,
    size_t             in_count,
    int16_t           *out,
    size_t             out_cap) {
    if (!r || !in || !out || in_count == 0 || out_cap == 0) {
        return 0;
    }

    if (r->in_rate == r->out_rate) {
        const size_t n = (in_count < out_cap) ? in_count : out_cap;
        memcpy(out, in, n * sizeof(int16_t));
        return n;
    }

    const bool downsample = (r->in_rate > r->out_rate);
    const int64_t in_rate  = r->in_rate;
    const int64_t out_rate = r->out_rate;
    size_t out_idx         = 0;

    for (size_t i = 0; i < in_count; ++i) {
        const float x = (float)in[i];
        float       y = x;

        if (!r->primed) {
            r->prev_raw2 = x;
            r->prev_raw1 = x;
            r->filt_prev = x;
            r->primed    = true;
        } else {
            if (downsample) {
                /* 3-tap anti-aliasing low-pass [0.25, 0.5, 0.25] */
                y = 0.25f * r->prev_raw2 + 0.5f * r->prev_raw1 + 0.25f * x;
            }
            r->prev_raw2 = r->prev_raw1;
            r->prev_raw1 = x;
        }

        while (r->phase_num <= 0) {
            if (out_idx >= out_cap) {
                r->filt_prev = y;
                r->phase_num -= out_rate;
                return out_idx;
            }
            const float t      = (float)(out_rate + r->phase_num) / (float)out_rate;
            const float sample = r->filt_prev + t * (y - r->filt_prev);
            out[out_idx++]     = clamp_i16(sample);
            r->phase_num += in_rate;
        }

        r->filt_prev = y;
        r->phase_num -= out_rate;
    }

    return out_idx;
}
