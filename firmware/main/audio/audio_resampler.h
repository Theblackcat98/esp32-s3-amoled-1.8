#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t in_rate;
    int32_t out_rate;
    int64_t phase_num;
    float   prev_raw2;
    float   prev_raw1;
    float   filt_prev;
    bool    primed;
} audio_resampler_t;

void   audio_resampler_init(audio_resampler_t *r, int32_t in_rate, int32_t out_rate);
void   audio_resampler_reset(audio_resampler_t *r);

/* Maximum output samples that `in_count` input samples can produce. */
size_t audio_resampler_max_out(const audio_resampler_t *r, size_t in_count);

/* Process streaming mono PCM16 chunk; returns output sample count written to `out`. */
size_t audio_resampler_process(
    audio_resampler_t *r,
    const int16_t     *in,
    size_t             in_count,
    int16_t           *out,
    size_t             out_cap);

#ifdef __cplusplus
}
#endif
