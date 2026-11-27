/*
 * PocketGBA host: runs the emulator on its own thread, paces it, owns the audio
 * output (OpenSL ES on Android) and exposes a tiny C API for Dart FFI.
 */
#define _GNU_SOURCE
#include "gba.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#define HAVE_OPENSL 1
#endif

#define RING_FRAMES 16384            /* power of two */
#define FRAME_US 16743               /* 280896 cycles @ 16.78 MHz */
#define OUT_RATE 48000
#define OUT_CHUNK 480                /* output frames per OpenSL buffer (10 ms) */
#define NBUF 3

typedef struct Host {
    GBA *g;
    pthread_t th;
    int th_started;
    pthread_mutex_t mu;
    atomic_int running, paused, speed, has_rom, volume;
    atomic_uint keys;
    uint32_t front[GBA_W * GBA_H];
    atomic_uint frame_id;

    int16_t ring[RING_FRAMES * 2];
    atomic_uint rw, rr;
    double apos;
    float last[2];

#ifdef HAVE_OPENSL
    SLObjectItf engine_obj, mix_obj, player_obj;
    SLEngineItf engine;
    SLPlayItf play;
    SLAndroidSimpleBufferQueueItf bq;
    int16_t out[NBUF][OUT_CHUNK * 2];
    int out_idx;
    int audio_ok;
#endif
} Host;

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* audio: ring -> resampler (32768 Hz -> 48000 Hz, adaptive rate)       */
/* ------------------------------------------------------------------ */
static void ring_push(Host *h, const int16_t *s, int n) {
    unsigned w = atomic_load_explicit(&h->rw, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&h->rr, memory_order_acquire);
    int space = RING_FRAMES - (int)(w - r);
    if (n > space) n = space;
    for (int i = 0; i < n; i++) {
        unsigned p = (w + (unsigned)i) & (RING_FRAMES - 1);
        h->ring[p * 2] = s[i * 2];
        h->ring[p * 2 + 1] = s[i * 2 + 1];
    }
    atomic_store_explicit(&h->rw, w + (unsigned)n, memory_order_release);
}

/* fills `frames` stereo frames at OUT_RATE.  Runs on the audio thread. */
static void audio_render(Host *h, int16_t *out, int frames) {
    unsigned w = atomic_load_explicit(&h->rw, memory_order_acquire);
    unsigned r = atomic_load_explicit(&h->rr, memory_order_relaxed);
    int avail = (int)(w - r);
    const int target = 2200;
    double err = (double)(avail - target) / (double)target;
    if (err > 1) err = 1;
    if (err < -1) err = -1;
    double step = ((double)GBA_AUDIO_RATE / (double)OUT_RATE) * (1.0 + err * 0.012);
    float vol = (float)atomic_load(&h->volume) / 100.0f;
    if (atomic_load(&h->paused) || !atomic_load(&h->has_rom)) vol = 0;
    double pos = h->apos;
    for (int i = 0; i < frames; i++) {
        int i0 = (int)pos;
        float f = (float)(pos - i0);
        float l, rr;
        if (i0 + 1 < (int)(w - r)) {
            unsigned p0 = (r + (unsigned)i0) & (RING_FRAMES - 1), p1 = (r + (unsigned)i0 + 1) & (RING_FRAMES - 1);
            l = h->ring[p0 * 2] * (1 - f) + h->ring[p1 * 2] * f;
            rr = h->ring[p0 * 2 + 1] * (1 - f) + h->ring[p1 * 2 + 1] * f;
            h->last[0] = l; h->last[1] = rr;
            pos += step;
        } else { /* underrun: fade out */
            h->last[0] *= 0.995f; h->last[1] *= 0.995f;
            l = h->last[0]; rr = h->last[1];
        }
        out[i * 2] = (int16_t)(l * vol);
        out[i * 2 + 1] = (int16_t)(rr * vol);
    }
    int used = (int)pos;
    if (used > (int)(w - r)) used = (int)(w - r);
    pos -= used;
    h->apos = pos;
    atomic_store_explicit(&h->rr, r + (unsigned)used, memory_order_release);
}

#ifdef HAVE_OPENSL
static void bq_callback(SLAndroidSimpleBufferQueueItf bq, void *ctx) {
    Host *h = (Host *)ctx;
    int16_t *buf = h->out[h->out_idx];
    audio_render(h, buf, OUT_CHUNK);
    (*bq)->Enqueue(bq, buf, OUT_CHUNK * 4);
    h->out_idx = (h->out_idx + 1) % NBUF;
}

static void audio_start(Host *h) {
    h->audio_ok = 0;
    if (slCreateEngine(&h->engine_obj, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS) return;
    if ((*h->engine_obj)->Realize(h->engine_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    if ((*h->engine_obj)->GetInterface(h->engine_obj, SL_IID_ENGINE, &h->engine) != SL_RESULT_SUCCESS) return;
    if ((*h->engine)->CreateOutputMix(h->engine, &h->mix_obj, 0, NULL, NULL) != SL_RESULT_SUCCESS) return;
    if ((*h->mix_obj)->Realize(h->mix_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;

    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NBUF};
    SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_48, SL_PCMSAMPLEFORMAT_FIXED_16,
                            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&loc_bq, &fmt};
    SLDataLocator_OutputMix loc_mix = {SL_DATALOCATOR_OUTPUTMIX, h->mix_obj};
    SLDataSink snk = {&loc_mix, NULL};
    const SLInterfaceID ids[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};
    if ((*h->engine)->CreateAudioPlayer(h->engine, &h->player_obj, &src, &snk, 1, ids, req) != SL_RESULT_SUCCESS) return;
    if ((*h->player_obj)->Realize(h->player_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    if ((*h->player_obj)->GetInterface(h->player_obj, SL_IID_PLAY, &h->play) != SL_RESULT_SUCCESS) return;
    if ((*h->player_obj)->GetInterface(h->player_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &h->bq) != SL_RESULT_SUCCESS) return;
    if ((*h->bq)->RegisterCallback(h->bq, bq_callback, h) != SL_RESULT_SUCCESS) return;
    memset(h->out, 0, sizeof h->out);
    h->out_idx = 0;
    for (int i = 0; i < NBUF; i++) (*h->bq)->Enqueue(h->bq, h->out[i], OUT_CHUNK * 4);
    if ((*h->play)->SetPlayState(h->play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) return;
    h->audio_ok = 1;
}

static void audio_stop(Host *h) {
    if (h->play) (*h->play)->SetPlayState(h->play, SL_PLAYSTATE_STOPPED);
    if (h->player_obj) (*h->player_obj)->Destroy(h->player_obj);
    if (h->mix_obj) (*h->mix_obj)->Destroy(h->mix_obj);
    if (h->engine_obj) (*h->engine_obj)->Destroy(h->engine_obj);
    h->player_obj = h->mix_obj = h->engine_obj = NULL;
    h->play = NULL; h->bq = NULL; h->engine = NULL;
    h->audio_ok = 0;
}
#else
static void audio_start(Host *h) { (void)h; }
static void audio_stop(Host *h) { (void)h; }
#endif

/* ------------------------------------------------------------------ */
/* emulation thread                                                     */
/* ------------------------------------------------------------------ */
static void sleep_until(int64_t t) {
    int64_t d = t - now_us();
    if (d > 0) {
        struct timespec ts = {(time_t)(d / 1000000), (long)((d % 1000000) * 1000)};
        nanosleep(&ts, NULL);
    }
}

static void *emu_main(void *arg) {
    Host *h = (Host *)arg;
    int64_t next = now_us();
    static int16_t tmp[4096 * 2];
    while (atomic_load(&h->running)) {
        if (atomic_load(&h->paused) || !atomic_load(&h->has_rom)) {
            usleep(4000);
            next = now_us();
            continue;
        }
        int speed = atomic_load(&h->speed);
        if (speed < 1) speed = 1;
        if (speed > 8) speed = 8;
        int n = 0;
        pthread_mutex_lock(&h->mu);
        gba_set_keys(h->g, atomic_load(&h->keys));
        gba_run_frame(h->g);
        memcpy(h->front, gba_video(h->g), sizeof h->front);
        n = gba_audio_pop(h->g, tmp, 4096);
        pthread_mutex_unlock(&h->mu);
        if (speed == 1 && n > 0) ring_push(h, tmp, n);
        atomic_fetch_add(&h->frame_id, 1);

        next += FRAME_US / speed;
        int64_t t = now_us();
        if (next > t) sleep_until(next);
        else if (t - next > 100000) next = t; /* fell far behind: resync */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* exported API                                                         */
/* ------------------------------------------------------------------ */
#define API __attribute__((visibility("default")))

API Host *host_create(void) {
    Host *h = (Host *)calloc(1, sizeof(Host));
    if (!h) return NULL;
    h->g = gba_new();
    if (!h->g) { free(h); return NULL; }
    pthread_mutex_init(&h->mu, NULL);
    atomic_store(&h->speed, 1);
    atomic_store(&h->volume, 100);
    atomic_store(&h->paused, 1);
    atomic_store(&h->running, 1);
    if (pthread_create(&h->th, NULL, emu_main, h) == 0) h->th_started = 1;
    audio_start(h);
    return h;
}

API void host_destroy(Host *h) {
    if (!h) return;
    audio_stop(h);
    atomic_store(&h->running, 0);
    if (h->th_started) pthread_join(h->th, NULL);
    gba_free(h->g);
    pthread_mutex_destroy(&h->mu);
    free(h);
}

API int host_load_rom(Host *h, const uint8_t *data, int size) {
    pthread_mutex_lock(&h->mu);
    int rc = gba_load_rom(h->g, data, (size_t)size);
    atomic_store(&h->has_rom, rc == 0);
    gba_audio_clear(h->g);
    atomic_store(&h->rr, atomic_load(&h->rw));
    h->apos = 0;
    pthread_mutex_unlock(&h->mu);
    return rc;
}

API void host_reset(Host *h) {
    pthread_mutex_lock(&h->mu);
    gba_reset(h->g);
    gba_audio_clear(h->g);
    pthread_mutex_unlock(&h->mu);
}

API void host_set_keys(Host *h, uint32_t mask) { atomic_store(&h->keys, mask); }
API void host_set_paused(Host *h, int p) { atomic_store(&h->paused, p ? 1 : 0); }
API void host_set_speed(Host *h, int s) { atomic_store(&h->speed, s); }
API void host_set_volume(Host *h, int v) { atomic_store(&h->volume, v < 0 ? 0 : (v > 100 ? 100 : v)); }
API uint32_t host_frame_id(Host *h) { return atomic_load(&h->frame_id); }

/* copies the latest frame (RGBA8888, 240x160) into dst; returns its frame id */
API uint32_t host_copy_frame(Host *h, uint8_t *dst) {
    pthread_mutex_lock(&h->mu);
    memcpy(dst, h->front, sizeof h->front);
    uint32_t id = atomic_load(&h->frame_id);
    pthread_mutex_unlock(&h->mu);
    return id;
}

API int host_save_size(Host *h) {
    pthread_mutex_lock(&h->mu);
    int n = (int)gba_save_size(h->g);
    pthread_mutex_unlock(&h->mu);
    return n;
}

/* returns 1 when the save changed since the last call (and copies it into dst, up to cap bytes) */
API int host_take_save(Host *h, uint8_t *dst, int cap) {
    pthread_mutex_lock(&h->mu);
    int dirty = gba_save_take_dirty(h->g);
    if (dirty && dst) {
        size_t n = 0;
        const uint8_t *d = gba_save_data(h->g, &n);
        if ((int)n > cap) n = (size_t)cap;
        memcpy(dst, d, n);
    }
    pthread_mutex_unlock(&h->mu);
    return dirty;
}

API void host_copy_save(Host *h, uint8_t *dst, int cap) {
    pthread_mutex_lock(&h->mu);
    size_t n = 0;
    const uint8_t *d = gba_save_data(h->g, &n);
    if ((int)n > cap) n = (size_t)cap;
    memcpy(dst, d, n);
    pthread_mutex_unlock(&h->mu);
}

API void host_load_save(Host *h, const uint8_t *src, int size) {
    pthread_mutex_lock(&h->mu);
    gba_save_load(h->g, src, (size_t)size);
    gba_save_take_dirty(h->g);
    pthread_mutex_unlock(&h->mu);
}

API int host_state_size(void) { return (int)gba_state_size(); }

API void host_state_save(Host *h, uint8_t *dst) {
    pthread_mutex_lock(&h->mu);
    gba_state_save(h->g, dst);
    pthread_mutex_unlock(&h->mu);
}

API int host_state_load(Host *h, const uint8_t *src, int size) {
    pthread_mutex_lock(&h->mu);
    int rc = gba_state_load(h->g, src, (size_t)size);
    if (rc == 0) {
        atomic_store(&h->rr, atomic_load(&h->rw));
        h->apos = 0;
    }
    pthread_mutex_unlock(&h->mu);
    return rc;
}

API int host_audio_ok(Host *h) {
#ifdef HAVE_OPENSL
    return h->audio_ok;
#else
    (void)h;
    return 0;
#endif
}
