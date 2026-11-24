#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t *buf;
    size_t   buf_len;   /* allocated capacity + 1 sentinel slot */
    size_t   capacity;  /* usable int16_t samples */
    size_t   head;      /* producer write index */
    size_t   tail;      /* consumer read index */
} audio_ringbuf_t;

bool   audio_ringbuf_init(audio_ringbuf_t *rb, size_t capacity_samples, bool prefer_psram);
void   audio_ringbuf_free(audio_ringbuf_t *rb);

/* Reset read pointer from consumer thread (safe lock-free SPSC flush). */
void   audio_ringbuf_flush(audio_ringbuf_t *rb);

/* Clear both indices when neither producer nor consumer is running. */
void   audio_ringbuf_reset(audio_ringbuf_t *rb);

size_t audio_ringbuf_available(const audio_ringbuf_t *rb);
size_t audio_ringbuf_free_space(const audio_ringbuf_t *rb);

/* Producer writes up to free_space samples; returns count written. */
size_t audio_ringbuf_write(audio_ringbuf_t *rb, const int16_t *samples, size_t count);

/* Single-thread circular pre-roll append: overwrites oldest samples when full. */
void   audio_ringbuf_write_overwrite(audio_ringbuf_t *rb, const int16_t *samples, size_t count);

/* Consumer reads up to max_samples; returns count read. */
size_t audio_ringbuf_read(audio_ringbuf_t *rb, int16_t *out, size_t max_samples);

#ifdef __cplusplus
}
#endif
