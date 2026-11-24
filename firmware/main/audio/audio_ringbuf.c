#include "audio_ringbuf.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static inline size_t load_acq(const size_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void store_rel(size_t *p, size_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

bool audio_ringbuf_init(audio_ringbuf_t *rb, size_t capacity_samples, bool prefer_psram) {
    if (!rb || capacity_samples == 0) {
        return false;
    }
    memset(rb, 0, sizeof(*rb));
    const size_t buf_len = capacity_samples + 1;

#ifdef ESP_PLATFORM
    const size_t bytes   = buf_len * sizeof(int16_t);
    if (prefer_psram) {
        rb->buf = (int16_t *)heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!rb->buf) {
        rb->buf = (int16_t *)heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
    }
#else
    (void)prefer_psram;
    rb->buf = (int16_t *)calloc(buf_len, sizeof(int16_t));
#endif
    if (!rb->buf) {
        return false;
    }
    rb->buf_len  = buf_len;
    rb->capacity = capacity_samples;
    rb->head     = 0;
    rb->tail     = 0;
    return true;
}

void audio_ringbuf_free(audio_ringbuf_t *rb) {
    if (!rb) return;
    if (rb->buf) {
#ifdef ESP_PLATFORM
        heap_caps_free(rb->buf);
#else
        free(rb->buf);
#endif
    }
    memset(rb, 0, sizeof(*rb));
}

void audio_ringbuf_flush(audio_ringbuf_t *rb) {
    if (!rb || !rb->buf) return;
    const size_t h = load_acq(&rb->head);
    store_rel(&rb->tail, h);
}

void audio_ringbuf_reset(audio_ringbuf_t *rb) {
    if (!rb) return;
    store_rel(&rb->head, 0);
    store_rel(&rb->tail, 0);
}

size_t audio_ringbuf_available(const audio_ringbuf_t *rb) {
    if (!rb || !rb->buf || rb->buf_len == 0) return 0;
    const size_t h = load_acq(&rb->head);
    const size_t t = load_acq(&rb->tail);
    return (h >= t) ? (h - t) : (rb->buf_len - t + h);
}

size_t audio_ringbuf_free_space(const audio_ringbuf_t *rb) {
    if (!rb || !rb->buf || rb->buf_len == 0) return 0;
    return rb->capacity - audio_ringbuf_available(rb);
}

size_t audio_ringbuf_write(audio_ringbuf_t *rb, const int16_t *samples, size_t count) {
    if (!rb || !rb->buf || !samples || count == 0) return 0;

    const size_t h    = rb->head;
    const size_t t    = load_acq(&rb->tail);
    const size_t used = (h >= t) ? (h - t) : (rb->buf_len - t + h);
    const size_t avail_space = rb->capacity - used;
    const size_t to_write    = (count < avail_space) ? count : avail_space;
    if (to_write == 0) return 0;

    const size_t first = (to_write <= rb->buf_len - h) ? to_write : (rb->buf_len - h);
    memcpy(&rb->buf[h], samples, first * sizeof(int16_t));
    if (to_write > first) {
        memcpy(&rb->buf[0], samples + first, (to_write - first) * sizeof(int16_t));
    }

    size_t next_h = h + to_write;
    if (next_h >= rb->buf_len) next_h -= rb->buf_len;
    store_rel(&rb->head, next_h);
    return to_write;
}

void audio_ringbuf_write_overwrite(audio_ringbuf_t *rb, const int16_t *samples, size_t count) {
    if (!rb || !rb->buf || !samples || count == 0) return;

    if (count >= rb->capacity) {
        const int16_t *src = samples + (count - rb->capacity);
        memcpy(rb->buf, src, rb->capacity * sizeof(int16_t));
        store_rel(&rb->tail, 0);
        store_rel(&rb->head, rb->capacity);
        return;
    }

    const size_t free_sp = audio_ringbuf_free_space(rb);
    if (count > free_sp) {
        const size_t drop = count - free_sp;
        size_t next_t     = rb->tail + drop;
        if (next_t >= rb->buf_len) next_t -= rb->buf_len;
        store_rel(&rb->tail, next_t);
    }
    audio_ringbuf_write(rb, samples, count);
}

size_t audio_ringbuf_read(audio_ringbuf_t *rb, int16_t *out, size_t max_samples) {
    if (!rb || !rb->buf || !out || max_samples == 0) return 0;

    const size_t h     = load_acq(&rb->head);
    const size_t t     = rb->tail;
    const size_t avail = (h >= t) ? (h - t) : (rb->buf_len - t + h);
    const size_t to_read = (max_samples < avail) ? max_samples : avail;
    if (to_read == 0) return 0;

    const size_t first = (to_read <= rb->buf_len - t) ? to_read : (rb->buf_len - t);
    memcpy(out, &rb->buf[t], first * sizeof(int16_t));
    if (to_read > first) {
        memcpy(out + first, &rb->buf[0], (to_read - first) * sizeof(int16_t));
    }

    size_t next_t = t + to_read;
    if (next_t >= rb->buf_len) next_t -= rb->buf_len;
    store_rel(&rb->tail, next_t);
    return to_read;
}
