#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_resampler.h"
#include "audio_ringbuf.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void test_resampler_24k_to_16k_streaming(void) {
    enum { IN_N = 24000, OUT_MAX = 20000 };
    int16_t *in_buf  = (int16_t *)malloc(IN_N * sizeof(int16_t));
    int16_t *out_buf = (int16_t *)malloc(OUT_MAX * sizeof(int16_t));
    assert(in_buf && out_buf);

    /* 440 Hz tone @ 24 kHz */
    for (int i = 0; i < IN_N; ++i) {
        in_buf[i] = (int16_t)lround(12000.0 * sin(2.0 * M_PI * 440.0 * (double)i / 24000.0));
    }

    audio_resampler_t rs;
    audio_resampler_init(&rs, 24000, 16000);

    /* Process in awkward prime/odd chunk sizes to stress boundary state */
    const size_t chunk_sizes[] = {1, 7, 13, 64, 111, 257, 500, 1023};
    size_t in_pos  = 0;
    size_t out_pos = 0;
    size_t ci      = 0;
    while (in_pos < IN_N) {
        size_t n = chunk_sizes[ci % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
        if (in_pos + n > IN_N) n = IN_N - in_pos;
        const size_t max_out = audio_resampler_max_out(&rs, n);
        assert(out_pos + max_out <= OUT_MAX);
        out_pos += audio_resampler_process(&rs, &in_buf[in_pos], n, &out_buf[out_pos], OUT_MAX - out_pos);
        in_pos += n;
        ++ci;
    }

    assert(out_pos == 16000);

    /* Verify frequency at 440 Hz via DFT correlation after 10-sample filter settle */
    double re = 0.0, im = 0.0, energy = 0.0;
    for (size_t i = 32; i < out_pos; ++i) {
        const double ang = 2.0 * M_PI * 440.0 * (double)i / 16000.0;
        const double s   = (double)out_buf[i];
        re += s * cos(ang);
        im += s * sin(ang);
        energy += s * s;
    }
    const double mag_sq = (re * re + im * im) * 2.0 / (double)(out_pos - 32);
    const double purity = mag_sq / energy;
    assert(purity > 0.98);

    free(in_buf);
    free(out_buf);
    printf("[PASS] test_resampler_24k_to_16k_streaming (out=%zu, purity=%.4f)\n", out_pos, purity);
}

static void test_resampler_passthrough(void) {
    audio_resampler_t rs;
    audio_resampler_init(&rs, 16000, 16000);
    int16_t in_buf[5]  = {10, -200, 32767, -32768, 42};
    int16_t out_buf[5] = {0};
    const size_t n     = audio_resampler_process(&rs, in_buf, 5, out_buf, 5);
    assert(n == 5);
    for (int i = 0; i < 5; ++i) {
        assert(out_buf[i] == in_buf[i]);
    }
    printf("[PASS] test_resampler_passthrough\n");
}

static void test_ringbuf_wrap_flush_overwrite(void) {
    audio_ringbuf_t rb;
    assert(audio_ringbuf_init(&rb, 8, false));
    assert(audio_ringbuf_free_space(&rb) == 8);
    assert(audio_ringbuf_available(&rb) == 0);

    int16_t a[6] = {1, 2, 3, 4, 5, 6};
    assert(audio_ringbuf_write(&rb, a, 6) == 6);
    assert(audio_ringbuf_available(&rb) == 6);
    assert(audio_ringbuf_free_space(&rb) == 2);

    int16_t r[4] = {0};
    assert(audio_ringbuf_read(&rb, r, 4) == 4);
    assert(r[0] == 1 && r[3] == 4);
    assert(audio_ringbuf_available(&rb) == 2);

    int16_t b[6] = {7, 8, 9, 10, 11, 12};
    assert(audio_ringbuf_write(&rb, b, 6) == 6);
    assert(audio_ringbuf_available(&rb) == 8);
    assert(audio_ringbuf_free_space(&rb) == 0);

    int16_t all[8] = {0};
    assert(audio_ringbuf_read(&rb, all, 8) == 8);
    for (int i = 0; i < 8; ++i) {
        assert(all[i] == 5 + i);
    }

    /* Pre-roll overwrite behavior */
    int16_t seq[12];
    for (int i = 0; i < 12; ++i) seq[i] = (int16_t)(100 + i);
    audio_ringbuf_write_overwrite(&rb, seq, 5);
    audio_ringbuf_write_overwrite(&rb, seq + 5, 7);
    assert(audio_ringbuf_available(&rb) == 8);
    assert(audio_ringbuf_read(&rb, all, 8) == 8);
    for (int i = 0; i < 8; ++i) {
        assert(all[i] == 104 + i);
    }

    /* Flush */
    assert(audio_ringbuf_write(&rb, a, 5) == 5);
    audio_ringbuf_flush(&rb);
    assert(audio_ringbuf_available(&rb) == 0);
    assert(audio_ringbuf_free_space(&rb) == 8);

    audio_ringbuf_free(&rb);
    printf("[PASS] test_ringbuf_wrap_flush_overwrite\n");
}

int main(void) {
    test_resampler_24k_to_16k_streaming();
    test_resampler_passthrough();
    test_ringbuf_wrap_flush_overwrite();
    printf("ALL HOST DSP TESTS PASSED\n");
    return 0;
}
