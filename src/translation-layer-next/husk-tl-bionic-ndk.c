/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The NDK's own libraries: libandroid (looper, assets, windows, sensors), libmediandk,
 * and zlib. EGL and GLES are in husk-tl-egl.c.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "husk-tl-internal.h"
#include "husk-tl-ld.h"

/* ---------------------------------------------------------------- looper */

/*
 * Android's ALooper is a per-thread event loop that other threads wake with a write
 * to a pipe. Unity's main thread polls it between frames. This one has the wake pipe
 * and nothing else registered on it: no file descriptors, no callbacks.
 */
typedef struct looper_fd { int fd, ident, events; void *callback, *data; struct looper_fd *next; } looper_fd;
typedef struct tl_looper { int wake[2]; atomic_int refs; pthread_t thread; struct tl_looper *next; pthread_mutex_t mu; looper_fd *fds; } tl_looper;
static __thread tl_looper *t_looper;

enum { ALOOPER_POLL_WAKE = -1, ALOOPER_POLL_CALLBACK = -2, ALOOPER_POLL_TIMEOUT = -3, ALOOPER_POLL_ERROR = -4 };
enum { ALOOPER_EVENT_INPUT = 1, ALOOPER_EVENT_OUTPUT = 2, ALOOPER_EVENT_ERROR = 4, ALOOPER_EVENT_HANGUP = 8 };

static tl_looper *looper_make(void)
{
    tl_looper *l = calloc(1, sizeof(*l));
    if (pipe(l->wake) == 0) {
        fcntl(l->wake[0], F_SETFL, O_NONBLOCK);
        fcntl(l->wake[1], F_SETFL, O_NONBLOCK);
    }
    atomic_init(&l->refs, 1);
    pthread_mutex_init(&l->mu, NULL);
    l->thread = pthread_self();
    return l;
}
static void *b_ALooper_prepare(int opts) { (void)opts; if (!t_looper) t_looper = looper_make(); return t_looper; }
static void *b_ALooper_forThread(void) { return t_looper; }
static int b_ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events, void **out_data);

/* For a driver whose thread plays Android's UI thread: give it the looper Android's main thread always has... */
void *tl_looper_prepare_here(void) { return b_ALooper_prepare(0); }
/* ...and run it: dispatch what is ready on it, waiting up to timeout_ms. ALOOPER_POLL_ERROR (-4) when there is none. */
int tl_looper_poll_here(int timeout_ms) { return b_ALooper_pollOnce(timeout_ms, NULL, NULL, NULL); }
static void b_ALooper_acquire(tl_looper *l) { if (l) atomic_fetch_add(&l->refs, 1); }
static void b_ALooper_release(tl_looper *l) { (void)l; }
static void b_ALooper_wake(tl_looper *l)
{
    if (!l) return;
    char c = 1;
    (void)!write(l->wake[1], &c, 1);
}

/* A file descriptor the looper watches: with a callback it is called from the polling thread; without one, its ident is returned. */
static int b_ALooper_addFd(tl_looper *l, int fd, int ident, int events, void *callback, void *data)
{
    if (!l || fd < 0) return -1;
    pthread_mutex_lock(&l->mu);
    looper_fd *f = l->fds;
    while (f && f->fd != fd) f = f->next;
    if (!f) { f = calloc(1, sizeof(*f)); f->next = l->fds; l->fds = f; }
    f->fd = fd; f->ident = ident; f->events = events; f->callback = callback; f->data = data;
    pthread_mutex_unlock(&l->mu);
    b_ALooper_wake(l);
    return 1;
}
static int b_ALooper_removeFd(tl_looper *l, int fd)
{
    if (!l) return -1;
    int found = 0;
    pthread_mutex_lock(&l->mu);
    for (looper_fd **pp = &l->fds; *pp; pp = &(*pp)->next)
        if ((*pp)->fd == fd) { looper_fd *dead = *pp; *pp = dead->next; free(dead); found = 1; break; }
    pthread_mutex_unlock(&l->mu);
    return found;
}

static int b_ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    tl_looper *l = t_looper;
    if (out_fd) *out_fd = -1;
    if (out_events) *out_events = 0;
    if (out_data) *out_data = NULL;
    if (!l) return ALOOPER_POLL_ERROR;
    struct pollfd pfds[32];
    looper_fd snap[31];
    int n = 0;
    pfds[n++] = (struct pollfd){ l->wake[0], POLLIN, 0 };
    pthread_mutex_lock(&l->mu);
    for (looper_fd *f = l->fds; f && n < 32; f = f->next) {
        short ev = (short)(((f->events & ALOOPER_EVENT_INPUT) ? POLLIN : 0) | ((f->events & ALOOPER_EVENT_OUTPUT) ? POLLOUT : 0));
        snap[n - 1] = *f;
        pfds[n++] = (struct pollfd){ f->fd, ev, 0 };
    }
    pthread_mutex_unlock(&l->mu);
    int r = poll(pfds, (nfds_t)n, timeout_ms);
    if (r < 0) return ALOOPER_POLL_ERROR;
    if (r == 0) return ALOOPER_POLL_TIMEOUT;
    if (pfds[0].revents & POLLIN) { char buf[64]; while (read(l->wake[0], buf, sizeof(buf)) > 0) {} }
    for (int i = 1; i < n; i++) {
        if (!pfds[i].revents) continue;
        looper_fd *f = &snap[i - 1];
        int ev = ((pfds[i].revents & POLLIN) ? ALOOPER_EVENT_INPUT : 0) | ((pfds[i].revents & POLLOUT) ? ALOOPER_EVENT_OUTPUT : 0)
               | ((pfds[i].revents & POLLERR) ? ALOOPER_EVENT_ERROR : 0) | ((pfds[i].revents & POLLHUP) ? ALOOPER_EVENT_HANGUP : 0);
        if (f->callback) {
            int keep = ((int (*)(int, int, void *))f->callback)(f->fd, ev, f->data);
            if (!keep) b_ALooper_removeFd(l, f->fd);
            return ALOOPER_POLL_CALLBACK;
        }
        if (out_fd) *out_fd = f->fd;
        if (out_events) *out_events = ev;
        if (out_data) *out_data = f->data;
        return f->ident;
    }
    return ALOOPER_POLL_WAKE;
}
static int b_ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    for (;;) {
        int r = b_ALooper_pollOnce(timeout_ms, out_fd, out_events, out_data);
        if (r != ALOOPER_POLL_CALLBACK) return r;
        timeout_ms = 0;
    }
}

/* ----------------------------------------------------------- configuration */

/* AConfiguration: an English, landscape, phone-sized device. */
typedef struct { char language[2], country[2]; int orientation, density, screen_long, screen_size, ui_mode_type, ui_mode_night; } tl_aconfig;
static tl_aconfig *b_AConfiguration_new(void)
{
    tl_aconfig *c = calloc(1, sizeof(*c));
    memcpy(c->language, "en", 2); memcpy(c->country, "US", 2);
    c->orientation = 2; c->density = 480; c->screen_long = 2; c->screen_size = 2;
    return c;
}
static void b_AConfiguration_delete(tl_aconfig *c) { free(c); }
static void b_AConfiguration_fromAssetManager(tl_aconfig *c, void *mgr) { (void)c; (void)mgr; }
static void b_AConfiguration_getLanguage(tl_aconfig *c, char *out) { out[0] = c->language[0]; out[1] = c->language[1]; }
static void b_AConfiguration_getCountry(tl_aconfig *c, char *out) { out[0] = c->country[0]; out[1] = c->country[1]; }
static int b_AConfiguration_getOrientation(tl_aconfig *c) { return c->orientation; }
static int b_AConfiguration_getDensity(tl_aconfig *c) { return c->density; }
static int b_AConfiguration_getScreenLong(tl_aconfig *c) { return c->screen_long; }
static int b_AConfiguration_getScreenSize(tl_aconfig *c) { return c->screen_size; }
static int b_AConfiguration_getUiModeType(tl_aconfig *c) { return c->ui_mode_type; }
static int b_AConfiguration_getUiModeNight(tl_aconfig *c) { return c->ui_mode_night; }
static int b_AConfiguration_getKeyboard(tl_aconfig *c) { (void)c; return 1; }
static int b_AConfiguration_getNavigation(tl_aconfig *c) { (void)c; return 1; }
static int b_AConfiguration_getTouchscreen(tl_aconfig *c) { (void)c; return 3; }
static int b_AConfiguration_getSdkVersion(tl_aconfig *c) { (void)c; return 34; }

/* ---------------------------------------------------------------- assets */

typedef struct { const uint8_t *data; size_t len; size_t pos; uint8_t *owned; } tl_asset;

static void *b_AAssetManager_fromJava(void *env, void *obj) { (void)env; (void)obj; static int mgr; return &mgr; }

static void *b_AAssetManager_open(void *mgr, const char *name, int mode)
{
    (void)mgr; (void)mode;
    char path[1024];
    snprintf(path, sizeof(path), "assets/%s", name);
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_FILE_TRACE") != NULL;
    if (trace) tl_log_line("assets: open %s", name);
    /* TL_ASSET_DIR: a file of that name under this directory stands in for the one in the APK (to change a game's config while debugging) */
    if (getenv("TL_ASSET_DIR")) {
        char alt[1100];
        snprintf(alt, sizeof(alt), "%s/%s", getenv("TL_ASSET_DIR"), name);
        FILE *fp = fopen(alt, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
            uint8_t *buf = malloc((size_t)n + 1);
            if (buf && fread(buf, 1, (size_t)n, fp) == (size_t)n) {
                fclose(fp);
                tl_asset *a = calloc(1, sizeof(*a));
                a->data = buf; a->len = (size_t)n; a->owned = buf;
                return a;
            }
            free(buf); fclose(fp);
        }
    }
    for (int i = 0;; i++) {
        const tl_zip *z = tl_ld_apk_at(i);
        if (!z) break;
        const tl_zip_entry *e = tl_zip_find(z, path);
        if (!e) continue;
        const uint8_t *data; size_t n; bool owned; char err[160];
        if (!tl_zip_data(z, e, (size_t)2 << 30, &data, &n, &owned, err, sizeof(err))) {
            tl_log_line("assets: %s: %s", name, err);
            return NULL;
        }
        tl_asset *a = calloc(1, sizeof(*a));
        a->data = data; a->len = n;
        if (owned) a->owned = (uint8_t *)data;
        return a;
    }
    return NULL;
}
/*
 * AAudio, the way FMOD (Minecraft's audio) prefers to play: a stream whose data callback the system calls for the next burst of
 * samples. The output is always 48 kHz stereo 16-bit; a thread of ours calls the game's callback for a burst and hands the result
 * to the host audio (tl_cocos_audio_hook, which blocks until the speakers have room -- that is what paces the game's mixer).
 * Recording is not offered.
 */
extern void (*tl_cocos_audio_hook)(const int16_t *samples, int frames, int channels, int rate);

enum { AA_OK = 0, AA_UNAVAILABLE = -899, AA_INVALID_STATE = -895, AA_FORMAT_I16 = 1, AA_FORMAT_FLOAT = 2, AA_FORMAT_I32 = 4,
       AA_STATE_OPEN = 2, AA_STATE_STARTED = 4, AA_STATE_PAUSED = 6, AA_STATE_FLUSHED = 8, AA_STATE_STOPPED = 10, AA_STATE_CLOSED = 12 };
#define AA_RATE 48000
#define AA_CHANNELS 2
#define AA_BURST 1024

typedef int (*aa_data_cb)(void *stream, void *user, void *data, int frames);
typedef struct aa_builder { aa_data_cb cb; void *user; int direction; int frames_per_cb; int format; int rate; int channels; } aa_builder;
typedef struct aa_stream {
    aa_data_cb cb; void *user;
    int burst;                       /* frames per data callback: what the game asked for, or AA_BURST */
    int format;                      /* the samples the game writes: 16-bit, float or 32-bit, as it asked */
    int rate, channels;              /* the rate and channel count it writes them at, as it asked */
    double phase;                    /* the resampler's place, in the game's frames, from the start of the next buffer */
    int16_t last[2];                 /* the game's last stereo frame, for resampling across buffers */
    bool have_last;
    atomic_int state, buffer_size;
    atomic_bool run;
    atomic_llong frames;             /* frames played so far: the stream's position */
    pthread_t thread;
    bool thread_started;
} aa_stream;

static int b_AAudio_createStreamBuilder(void **builder) { aa_builder *b = calloc(1, sizeof(*b)); *builder = b; return b ? AA_OK : AA_UNAVAILABLE; }
static int b_AAudioStreamBuilder_delete(aa_builder *b) { free(b); return AA_OK; }
static void b_AAudioStreamBuilder_setDataCallback(aa_builder *b, aa_data_cb cb, void *user) { b->cb = cb; b->user = user; }
static void b_AAudioStreamBuilder_setDirection(aa_builder *b, int d) { b->direction = d; }
static void b_AAudioStreamBuilder_ignore(aa_builder *b, int v) { (void)b; (void)v; }
static void b_AAudioStreamBuilder_setErrorCallback(aa_builder *b, void *cb, void *user) { (void)b; (void)cb; (void)user; }
static void b_AAudioStreamBuilder_setFramesPerDataCallback(aa_builder *b, int n) { b->frames_per_cb = n; }
/* The sample format the game will write. FMOD in Unity 6 asks for float; reading its float samples as 16-bit was a loud
 * buzz, and its buffers -- twice the size of 16-bit ones -- overran ours. */
static void b_AAudioStreamBuilder_setFormat(aa_builder *b, int f) { b->format = f; }
/* ...and the rate and channels it will write at. FMOD in Unity 6 asks for its project's 24 kHz and writes 24 kHz; played at
 * 48 kHz, that was everything twice as fast -- the music, and the game, which keeps time by it. */
static void b_AAudioStreamBuilder_setSampleRate(aa_builder *b, int r) { b->rate = r; }
static void b_AAudioStreamBuilder_setChannelCount(aa_builder *b, int c) { b->channels = c; }

/* The bytes one sample of a stream's format takes; the host output takes 16-bit. */
static int aa_sample_bytes(int format) { return format == AA_FORMAT_FLOAT || format == AA_FORMAT_I32 ? 4 : 2; }

/* The game's samples, as the 16-bit ones the host output plays. */
static const int16_t *aa_to_i16(int format, const void *src, int samples, int16_t *dst);

/*
 * The game's frames as the host plays them: 16-bit stereo at AA_RATE. A mono frame goes to both sides; another rate is
 * resampled, by straight lines between neighbouring frames, carrying the position and the last frame from one buffer to
 * the next so the joins are seamless. Returns the frames written to `out`, which has room for aa_out_room(s, frames).
 */
static int aa_out_room(const aa_stream *s, int frames) { return (int)((double)frames * AA_RATE / s->rate) + 4; }

static int aa_to_host(aa_stream *s, const void *src, int frames, int16_t *scratch, int16_t *out)
{
    const int16_t *pcm = aa_to_i16(s->format, src, frames * s->channels, scratch);
    /* stereo, in place in scratch when it is mono (scratch has room for frames * 2) */
    const int16_t *st = pcm;
    if (s->channels == 1) {
        if (pcm != scratch) { memcpy(scratch, pcm, (size_t)frames * sizeof(int16_t)); }
        for (int i = frames - 1; i >= 0; i--) { scratch[2 * i + 1] = scratch[i]; scratch[2 * i] = scratch[i]; }
        st = scratch;
    }
    if (s->rate == AA_RATE) {
        if (st != out) memcpy(out, st, (size_t)frames * 2 * sizeof(int16_t));
        return frames;
    }
    if (!s->have_last) { s->last[0] = st[0]; s->last[1] = st[1]; s->have_last = true; s->phase = 0; }
    double step = (double)s->rate / AA_RATE;          /* the game's frames per host frame */
    double p = s->phase;
    int n = 0, room = aa_out_room(s, frames);
    while (p < frames - 1 && n < room) {
        int i = (int)(p < 0 ? -1 : p);
        double t = p - i;
        for (int c = 0; c < 2; c++) {
            double a = i < 0 ? s->last[c] : st[2 * i + c], b = st[2 * (i + 1) + c];
            out[2 * n + c] = (int16_t)(a + (b - a) * t);
        }
        n++;
        p += step;
    }
    s->last[0] = st[2 * (frames - 1)]; s->last[1] = st[2 * (frames - 1) + 1];
    s->phase = p - frames;
    return n;
}

static const int16_t *aa_to_i16(int format, const void *src, int samples, int16_t *dst)
{
    if (format == AA_FORMAT_FLOAT) {
        const float *f = src;
        for (int i = 0; i < samples; i++) {
            float v = f[i] * 32767.0f;
            dst[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        return dst;
    }
    if (format == AA_FORMAT_I32) {
        const int32_t *w = src;
        for (int i = 0; i < samples; i++) dst[i] = (int16_t)(w[i] >> 16);
        return dst;
    }
    return src;
}

/*
 * Output only. A stream with a data callback is played by a thread of ours that asks the game for each burst; one without
 * (the blocking "write" way some engines use) is played by its AAudioStream_write calls.
 */
static int b_AAudioStreamBuilder_openStream(aa_builder *b, aa_stream **out)
{
    if (b->direction != 0) return AA_UNAVAILABLE;                      /* no recording */
    aa_stream *s = calloc(1, sizeof(*s));
    s->cb = b->cb; s->user = b->user;
    s->burst = b->frames_per_cb > 0 && b->frames_per_cb <= 8192 ? b->frames_per_cb : AA_BURST;
    s->format = b->format == AA_FORMAT_FLOAT || b->format == AA_FORMAT_I32 ? b->format : AA_FORMAT_I16;
    s->rate = b->rate >= 8000 && b->rate <= 192000 ? b->rate : AA_RATE;
    s->channels = b->channels == 1 ? 1 : AA_CHANNELS;
    atomic_store(&s->state, AA_STATE_OPEN); atomic_store(&s->buffer_size, s->burst * 2);
    *out = s;
    tl_log_line("aaudio: stream opened (%d Hz, %d channel(s), burst %d, %s samples, %s)", s->rate, s->channels, s->burst,
                s->format == AA_FORMAT_FLOAT ? "float" : s->format == AA_FORMAT_I32 ? "32-bit" : "16-bit",
                s->cb ? "data callback" : "blocking writes");
    return AA_OK;
}

static void *aa_pump(void *arg)
{
    aa_stream *s = arg;
    pthread_setname_np("aaudio-pump");
    /* What the game writes, in its format, rate and channels; the same as 16-bit; and as the host plays it. */
    size_t samples = (size_t)s->burst * (size_t)s->channels;
    void *buf = malloc(samples * (size_t)aa_sample_bytes(s->format));
    int16_t *scratch = malloc((size_t)s->burst * 2 * sizeof(int16_t));
    int16_t *out = malloc((size_t)aa_out_room(s, s->burst) * 2 * sizeof(int16_t));
    bool answered = false;
    while (atomic_load(&s->run)) {
        memset(buf, 0, samples * (size_t)aa_sample_bytes(s->format));
        int r = s->cb(s, s->user, buf, s->burst);
        if (!answered) { answered = true; tl_log_line("aaudio: the game's mixer answered its first callback (%d)", r); }
        if (!atomic_load(&s->run)) break;
        int n = aa_to_host(s, buf, s->burst, scratch, out);
        { static bool said; if (!said) { for (int i = 0; i < n * 2; i++) if (out[i]) { said = true; tl_log_line("aaudio: the game's first non-silent burst (sample %d = %d)", i, out[i]); break; } } }
        if (tl_cocos_audio_hook) tl_cocos_audio_hook(out, n, AA_CHANNELS, AA_RATE);
        else { struct timespec ts = { 0, (long)((double)s->burst * 1e9 / s->rate) }; nanosleep(&ts, NULL); }
        atomic_fetch_add(&s->frames, s->burst);
        if (r != 0) break;
    }
    free(buf);
    free(scratch);
    free(out);
    return NULL;
}

static int b_AAudioStream_requestStart(aa_stream *s)
{
    if (atomic_exchange(&s->run, true)) return AA_OK;
    atomic_store(&s->state, AA_STATE_STARTED);
    tl_log_line("aaudio: stream started");
    if (!s->cb) return AA_OK;                                          /* blocking writes: nothing to pump */
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1u << 20);
    s->thread_started = pthread_create(&s->thread, &a, aa_pump, s) == 0;
    pthread_attr_destroy(&a);
    return AA_OK;
}
static int aa_stop(aa_stream *s, int state)
{
    if (atomic_exchange(&s->run, false) && s->thread_started) { pthread_join(s->thread, NULL); s->thread_started = false; }
    atomic_store(&s->state, state);
    return AA_OK;
}
static int b_AAudioStream_requestStop(aa_stream *s) { return aa_stop(s, AA_STATE_STOPPED); }
static int b_AAudioStream_requestPause(aa_stream *s) { return aa_stop(s, AA_STATE_PAUSED); }
static int b_AAudioStream_requestFlush(aa_stream *s) { atomic_store(&s->state, AA_STATE_FLUSHED); return AA_OK; }
static int b_AAudioStream_close(aa_stream *s) { aa_stop(s, AA_STATE_CLOSED); tl_log_line("aaudio: stream closed"); free(s); return AA_OK; }
static int b_AAudioStream_release(aa_stream *s) { aa_stop(s, AA_STATE_CLOSED); return AA_OK; }
static int b_AAudioStream_getSampleRate(aa_stream *s) { return s->rate; }
static int b_AAudioStream_getChannelCount(aa_stream *s) { return s->channels; }
static int b_AAudioStream_getHardwareSampleRate(aa_stream *s) { (void)s; return AA_RATE; }
static int b_AAudioStream_getHardwareChannelCount(aa_stream *s) { (void)s; return AA_CHANNELS; }
static int b_AAudioStream_getFormat(aa_stream *s) { return s->format; }
static int b_AAudioStream_getHardwareFormat(aa_stream *s) { (void)s; return AA_FORMAT_I16; }
static int b_AAudioStream_getDeviceId(aa_stream *s) { (void)s; return 0; }
static int b_AAudioStream_getFramesPerBurst(aa_stream *s) { return s->burst; }
static int b_AAudioStream_getFramesPerDataCallback(aa_stream *s) { return s->burst; }
static int b_AAudioStream_getBufferCapacityInFrames(aa_stream *s) { return s->burst * 8; }
static int b_AAudioStream_getBufferSizeInFrames(aa_stream *s) { return atomic_load(&s->buffer_size); }
static int b_AAudioStream_setBufferSizeInFrames(aa_stream *s, int n) { atomic_store(&s->buffer_size, n); return n; }
static int b_AAudioStream_getXRunCount(aa_stream *s) { (void)s; return 0; }
static int b_AAudioStream_getState(aa_stream *s) { return atomic_load(&s->state); }
static int b_AAudioStream_getPerformanceMode(aa_stream *s) { (void)s; return 12; }      /* LOW_LATENCY */
static int b_AAudioStream_getSharingMode(aa_stream *s) { (void)s; return 1; }          /* SHARED */
static int b_AAudioStream_getUsage(aa_stream *s) { (void)s; return 14; }               /* GAME */
static int b_AAudioStream_getContentType(aa_stream *s) { (void)s; return 2; }          /* MUSIC */
static int b_AAudioStream_getInputPreset(aa_stream *s) { (void)s; return 6; }          /* VOICE_RECOGNITION, the default */
static int b_AAudioStream_getSessionId(aa_stream *s) { (void)s; return -1; }           /* NONE */
static int b_AAudioStream_getChannelMask(aa_stream *s) { (void)s; return 3; }          /* STEREO */
static bool b_AAudioStream_isMMapUsed(aa_stream *s) { (void)s; return false; }
static int64_t b_AAudioStream_getFramesWritten(aa_stream *s) { return atomic_load(&s->frames); }
static int64_t b_AAudioStream_getFramesRead(aa_stream *s) { return atomic_load(&s->frames); }
static int b_AAudioStream_getTimestamp(aa_stream *s, int clock, int64_t *frame, int64_t *ns)
{
    if (atomic_load(&s->state) != AA_STATE_STARTED || atomic_load(&s->frames) == 0) return AA_INVALID_STATE;
    struct timespec ts;
    clock_gettime(clock == 1 ? CLOCK_MONOTONIC : CLOCK_MONOTONIC, &ts);
    if (frame) *frame = atomic_load(&s->frames);
    if (ns) *ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    return AA_OK;
}
static int b_AAudioStream_waitForStateChange(aa_stream *s, int input, int *next, int64_t timeout_ns)
{
    /* State changes here happen at once, inside the request; a caller waiting for one either has it already, or waits a
     * moment and is told the state as it is. */
    int now = atomic_load(&s->state);
    if (now == input && timeout_ns > 0) {
        struct timespec ts = { 0, (long)(timeout_ns < 10000000 ? timeout_ns : 10000000) };
        nanosleep(&ts, NULL);
        now = atomic_load(&s->state);
    }
    if (next) *next = now;
    return AA_OK;
}
/* The blocking way: the game hands us frames, and the host's output takes them (it blocks until there is room). */
static int b_AAudioStream_write(aa_stream *s, const void *buf, int frames, int64_t timeout_ns)
{
    (void)timeout_ns;
    if (!buf || frames <= 0) return 0;
    if (atomic_load(&s->state) != AA_STATE_STARTED) return AA_INVALID_STATE;
    int16_t *scratch = malloc((size_t)frames * 2 * sizeof(int16_t));
    int16_t *out = malloc((size_t)aa_out_room(s, frames) * 2 * sizeof(int16_t));
    int n = scratch && out ? aa_to_host(s, buf, frames, scratch, out) : 0;
    if (tl_cocos_audio_hook) tl_cocos_audio_hook(out, n, AA_CHANNELS, AA_RATE);
    else { struct timespec ts = { 0, (long)((double)frames * 1e9 / s->rate) }; nanosleep(&ts, NULL); }
    free(scratch);
    free(out);
    atomic_fetch_add(&s->frames, frames);
    return frames;
}
static int b_AAudioStream_read(aa_stream *s, void *buf, int frames, long timeout) { (void)s; (void)buf; (void)frames; (void)timeout; return AA_UNAVAILABLE; }
static const char *b_AAudio_convertResultToText(int r) { return r == 0 ? "AAUDIO_OK" : "AAudio error"; }
static const char *b_AAudio_convertStreamStateToText(int st) { (void)st; return "AAUDIO_STREAM_STATE"; }

/* AAssetDir: the files directly under an asset directory, across the APKs */
typedef struct { char **names; size_t n, pos; } tl_assetdir;
static void *b_AAssetManager_openDir(void *mgr, const char *dir)
{
    (void)mgr;
    if (getenv("TL_FILE_TRACE")) tl_log_line("assets: openDir %s", dir);
    char prefix[1024];
    size_t pl = (size_t)snprintf(prefix, sizeof(prefix), "assets/%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "");
    tl_assetdir *d = calloc(1, sizeof(*d));
    size_t cap = 0;
    for (int i = 0;; i++) {
        const tl_zip *z = tl_ld_apk_at(i);
        if (!z) break;
        for (size_t k = 0; k < z->count; k++) {
            const char *name = z->entries[k].name;
            if (strncmp(name, prefix, pl) != 0 || !name[pl] || strchr(name + pl, '/')) continue;
            if (d->n == cap) { cap = cap ? cap * 2 : 64; d->names = realloc(d->names, cap * sizeof(char *)); }
            d->names[d->n++] = strdup(name + pl);
        }
    }
    return d;
}
static const char *b_AAssetDir_getNextFileName(tl_assetdir *d) { return d && d->pos < d->n ? d->names[d->pos++] : NULL; }
static void b_AAssetDir_close(tl_assetdir *d) { if (!d) return; for (size_t i = 0; i < d->n; i++) free(d->names[i]); free(d->names); free(d); }
static unsigned long b_deflateBound(void *strm, unsigned long n) { (void)strm; return n + (n >> 12) + (n >> 14) + (n >> 25) + 13; }

static void b_AAsset_close(tl_asset *a) { if (a) { free(a->owned); free(a); } }
static int b_AAsset_read(tl_asset *a, void *buf, size_t n)
{
    size_t left = a->len - a->pos;
    if (n > left) n = left;
    memcpy(buf, a->data + a->pos, n);
    a->pos += n;
    return (int)n;
}
static long b_AAsset_getLength(tl_asset *a) { return (long)a->len; }
static long b_AAsset_getRemainingLength(tl_asset *a) { return (long)(a->len - a->pos); }
static const void *b_AAsset_getBuffer(tl_asset *a) { return a->data; }
static long b_AAsset_seek(tl_asset *a, long off, int whence)
{
    long base = whence == 0 ? 0 : whence == 1 ? (long)a->pos : (long)a->len;
    long np = base + off;
    if (np < 0 || (size_t)np > a->len) return -1;
    a->pos = (size_t)np;
    return np;
}
static int b_AAsset_isAllocated(tl_asset *a) { return a->owned != NULL; }

/* ---------------------------------------------------------------- windows */

/*
 * The input queue a NativeActivity's glue attaches to its looper (Unreal Engine's does). Events are the NDK's opaque AInputEvent: here a small record the accessors
 * below read. A touch from the host is queued and a byte written to a pipe the looper watches, which is how the real queue wakes the thread that owns it.
 */
typedef struct tl_ievent {
    int type;                                   /* AINPUT_EVENT_TYPE_KEY 1, _MOTION 2 */
    int source, device, action, meta, flags, keycode, button_state, nptr;
    int pid[10]; float px[10], py[10];
    float axis[48]; bool has_axes;                /* a joystick event: every axis by Android's axis number */
    int64_t time;
    struct tl_ievent *next;
} tl_ievent;
static struct {
    int rd, wr, ident;
    bool ready, attached;
    pthread_mutex_t mu;
    tl_ievent *head, *tail;
    int nptr, pid[10]; float px[10], py[10];    /* the fingers down now, to say who else is touching when one changes */
} IQ = { .rd = -1, .wr = -1, .mu = PTHREAD_MUTEX_INITIALIZER };

static void iq_init(void)
{
    if (IQ.ready) return;
    int fds[2];
    if (pipe(fds) != 0) return;
    fcntl(fds[0], F_SETFL, O_NONBLOCK); fcntl(fds[1], F_SETFL, O_NONBLOCK);
    IQ.rd = fds[0]; IQ.wr = fds[1]; IQ.ready = true;
}
static void iq_push(tl_ievent *e)
{
    pthread_mutex_lock(&IQ.mu);
    if (IQ.tail) IQ.tail->next = e; else IQ.head = e;
    IQ.tail = e;
    pthread_mutex_unlock(&IQ.mu);
    char c = 1; (void)!write(IQ.wr, &c, 1);
}
/* A touch in surface pixels: phase 0 down, 1 move, 2 up, 3 cancel -- the same words the other engines are given. Safe from any thread. */
void tl_inq_touch(int phase, int id, float x, float y)
{
    iq_init();
    if (!IQ.ready || !IQ.attached) return;
    pthread_mutex_lock(&IQ.mu);
    int idx = -1;
    for (int i = 0; i < IQ.nptr; i++) if (IQ.pid[i] == id) idx = i;
    int action;
    if (phase == 0) {
        if (idx < 0 && IQ.nptr < 10) { idx = IQ.nptr++; IQ.pid[idx] = id; }
        if (idx < 0) { pthread_mutex_unlock(&IQ.mu); return; }
        IQ.px[idx] = x; IQ.py[idx] = y;
        action = IQ.nptr == 1 ? 0 /* DOWN */ : (5 /* POINTER_DOWN */ | (idx << 8));
    } else if (idx < 0) { pthread_mutex_unlock(&IQ.mu); return; }
    else if (phase == 1) { IQ.px[idx] = x; IQ.py[idx] = y; action = 2; }
    else if (phase == 2) { IQ.px[idx] = x; IQ.py[idx] = y; action = IQ.nptr == 1 ? 1 /* UP */ : (6 /* POINTER_UP */ | (idx << 8)); }
    else action = 3;
    tl_ievent *e = calloc(1, sizeof(*e));
    e->type = 2; e->source = 0x1002 /* touchscreen */; e->device = 1; e->action = action; e->nptr = IQ.nptr;
    for (int i = 0; i < IQ.nptr; i++) { e->pid[i] = IQ.pid[i]; e->px[i] = IQ.px[i]; e->py[i] = IQ.py[i]; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); e->time = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    if (phase == 2 || phase == 3) {                       /* the finger leaves the list after the event that says so */
        if (phase == 3) IQ.nptr = 0;
        else { for (int i = idx; i + 1 < IQ.nptr; i++) { IQ.pid[i] = IQ.pid[i + 1]; IQ.px[i] = IQ.px[i + 1]; IQ.py[i] = IQ.py[i + 1]; } IQ.nptr--; }
    }
    pthread_mutex_unlock(&IQ.mu);
    iq_push(e);
}

/* A controller button (action 0 down, 1 up, an Android key code) and the state of its sticks, triggers and hat (axes by Android axis number), from any thread. */
void tl_inq_key(int device, int source, int action, int keycode)
{
    iq_init();
    if (!IQ.ready || !IQ.attached) return;
    tl_ievent *e = calloc(1, sizeof(*e));
    e->type = 1; e->source = source; e->device = device; e->action = action; e->keycode = keycode;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); e->time = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    iq_push(e);
}
void tl_inq_axes(int device, const float *axes48)
{
    iq_init();
    if (!IQ.ready || !IQ.attached) return;
    tl_ievent *e = calloc(1, sizeof(*e));
    e->type = 2; e->source = 0x01000010 /* joystick */; e->device = device; e->action = 2 /* MOVE */; e->nptr = 1; e->has_axes = true;
    memcpy(e->axis, axes48, sizeof(e->axis));
    e->px[0] = e->axis[0]; e->py[0] = e->axis[1];
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); e->time = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    iq_push(e);
}

static void b_AInputQueue_attachLooper(void *q, void *looper, int ident, void *cb, void *data)
{
    (void)q;
    iq_init();
    if (!IQ.ready || !looper) return;
    IQ.ident = ident; IQ.attached = true;
    b_ALooper_addFd(looper, IQ.rd, ident, ALOOPER_EVENT_INPUT, cb, data);
}
static void b_AInputQueue_detachLooper(void *q) { (void)q; IQ.attached = false; if (t_looper && IQ.ready) b_ALooper_removeFd(t_looper, IQ.rd); }
static int32_t b_AInputQueue_hasEvents(void *q) { (void)q; return IQ.head != NULL; }
static int32_t b_AInputQueue_getEvent(void *q, void **ev)
{
    (void)q;
    pthread_mutex_lock(&IQ.mu);
    tl_ievent *e = IQ.head;
    if (e) { IQ.head = e->next; if (!IQ.head) IQ.tail = NULL; }
    pthread_mutex_unlock(&IQ.mu);
    if (ev) *ev = e;
    if (e && IQ.ready) { char c; (void)!read(IQ.rd, &c, 1); }
    return e ? 0 : -1;
}
static int32_t b_AInputQueue_preDispatchEvent(void *q, void *ev) { (void)q; (void)ev; return 0; }
static void b_AInputQueue_finishEvent(void *q, void *ev, int handled) { (void)q; (void)handled; free(ev); }

static int32_t b_AInputEvent_getType(const tl_ievent *e) { return e->type; }
static int32_t b_AInputEvent_getSource(const tl_ievent *e) { return e->source; }
static int32_t b_AInputEvent_getDeviceId(const tl_ievent *e) { return e->device; }
static int32_t b_AMotionEvent_getAction(const tl_ievent *e) { return e->action; }
static int32_t b_AMotionEvent_getFlags(const tl_ievent *e) { return e->flags; }
static int32_t b_AMotionEvent_getMetaState(const tl_ievent *e) { return e->meta; }
static int32_t b_AMotionEvent_getButtonState(const tl_ievent *e) { return e->button_state; }
static int32_t b_AMotionEvent_getEdgeFlags(const tl_ievent *e) { (void)e; return 0; }
static int64_t b_AMotionEvent_getEventTime(const tl_ievent *e) { return e->time; }
static int64_t b_AMotionEvent_getDownTime(const tl_ievent *e) { return e->time; }
static size_t b_AMotionEvent_getPointerCount(const tl_ievent *e) { return (size_t)e->nptr; }
static int32_t b_AMotionEvent_getPointerId(const tl_ievent *e, size_t i) { return i < (size_t)e->nptr ? e->pid[i] : 0; }
static float b_AMotionEvent_getX(const tl_ievent *e, size_t i) { return i < (size_t)e->nptr ? e->px[i] : 0.0f; }
static float b_AMotionEvent_getY(const tl_ievent *e, size_t i) { return i < (size_t)e->nptr ? e->py[i] : 0.0f; }
static float b_AMotionEvent_getPressure(const tl_ievent *e, size_t i) { (void)e; (void)i; return 1.0f; }
static float b_AMotionEvent_getSize(const tl_ievent *e, size_t i) { (void)e; (void)i; return 0.1f; }
static size_t b_AMotionEvent_getHistorySize(const tl_ievent *e) { (void)e; return 0; }
static float b_AMotionEvent_getAxisValue(const tl_ievent *e, int32_t axis, size_t i)
{
    if (e->has_axes && axis >= 0 && axis < 48) return e->axis[axis];
    if (axis == 0) return b_AMotionEvent_getX(e, i);
    if (axis == 1) return b_AMotionEvent_getY(e, i);
    return 0.0f;
}
static int32_t b_AKeyEvent_getAction(const tl_ievent *e) { return e->action; }
static int32_t b_AKeyEvent_getKeyCode(const tl_ievent *e) { return e->keycode; }
static int32_t b_AKeyEvent_getFlags(const tl_ievent *e) { return e->flags; }
static int32_t b_AKeyEvent_getMetaState(const tl_ievent *e) { return e->meta; }
static int32_t b_AKeyEvent_getRepeatCount(const tl_ievent *e) { (void)e; return 0; }
static int32_t b_AKeyEvent_getScanCode(const tl_ievent *e) { (void)e; return 0; }
static void b_ANativeActivity_noop(void *a) { (void)a; }
static void b_ANativeActivity_flags(void *a, uint32_t add, uint32_t remove) { (void)a; (void)add; (void)remove; }
static void b_ANativeActivity_input(void *a, uint32_t flags) { (void)a; (void)flags; }


/* texture: for a video decoder's window, the SurfaceTexture its Surface was made over (husk-tl-jni-hle.c); NULL for the screen. */
typedef struct tl_nwindow { atomic_int refs; int width, height, format; void *layer; void *texture; } tl_nwindow;
static tl_nwindow g_window = { 1, 1080, 2400, 1, NULL, NULL };
void *tl_surface_texture_of(void *surface);
void tl_surface_texture_frame(void *surface_texture, int64_t timestamp_ns);

void tl_nwindow_configure(int w, int h, void *layer) { g_window.width = w; g_window.height = h; g_window.layer = layer; }
void *tl_nwindow_get(void) { atomic_fetch_add(&g_window.refs, 1); return &g_window; }
void *tl_nwindow_native(void *window) { return window ? ((tl_nwindow *)window)->layer : NULL; }
int tl_nwindow_width(void *window) { return window ? ((tl_nwindow *)window)->width : 0; }
int tl_nwindow_height(void *window) { return window ? ((tl_nwindow *)window)->height : 0; }
/*
 * A game that draws its frames smaller than its window (Unreal at its content scale factor) asks Android to scale them up to
 * it: the app makes the window's layer that many pixels, scaled onto the same rectangle. The window still reports the size
 * it was given, which is what touches are measured in. 0x0 goes back to the window's size.
 */
static void (*g_buffer_size_handler)(int width, int height);
void husk_tl_set_buffer_size_handler(void (*handler)(int width, int height)) { g_buffer_size_handler = handler; }
void tl_nwindow_set_buffer_size(int w, int h) { if (g_buffer_size_handler) g_buffer_size_handler(w, h); }

static void *b_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env;
    /* A Surface over a SurfaceTexture is where a video decoder draws, not the screen: a window of its own, which says where its frames go. */
    void *st = tl_surface_texture_of(surface);
    tl_nwindow *w = st ? calloc(1, sizeof(*w)) : NULL;
    if (w) {
        atomic_init(&w->refs, 1);
        w->width = 16; w->height = 16; w->format = 1; w->texture = st;
        return w;
    }
    atomic_fetch_add(&g_window.refs, 1);
    return &g_window;
}
static void b_ANativeWindow_acquire(tl_nwindow *w) { if (w) atomic_fetch_add(&w->refs, 1); }
static void b_ANativeWindow_release(tl_nwindow *w) { if (w) atomic_fetch_sub(&w->refs, 1); }
static int b_ANativeWindow_getWidth(tl_nwindow *w) { return w ? w->width : 0; }
static int b_ANativeWindow_getHeight(tl_nwindow *w) { return w ? w->height : 0; }
static int b_ANativeWindow_getFormat(tl_nwindow *w) { return w ? w->format : 0; }
static int b_ANativeWindow_setBuffersGeometry(tl_nwindow *w, int width, int height, int format)
{
    (void)format;
    static atomic_bool said;
    if (w == &g_window && width > 0 && height > 0 && (width != w->width || height != w->height) && !atomic_exchange(&said, true))
        tl_log_line("ndk: the game asks for %dx%d frames on its %dx%d window (drawn at the window's size)", width, height, w->width, w->height);
    return 0;
}
static void *b_ANativeWindow_toSurface(void *env, void *w) { (void)env; (void)w; return NULL; }

/* ---------------------------------------------------------------- sensors */

static void *b_ASensorManager_getInstance(void) { static int m; return &m; }
static void *b_ASensorManager_getDefaultSensor(void *m, int type) { (void)m; (void)type; return NULL; }
static int b_ASensorManager_getSensorList(void *m, const void ***list) { (void)m; if (list) *list = NULL; return 0; }
static void *b_ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data)
{
    (void)m; (void)looper; (void)ident; (void)cb; (void)data;
    static int q; return &q;
}
static int b_ASensorManager_destroyEventQueue(void *m, void *q) { (void)m; (void)q; return 0; }
static int b_ASensorEventQueue_enableSensor(void *q, void *s) { (void)q; (void)s; return -1; }
static int b_ASensorEventQueue_disableSensor(void *q, void *s) { (void)q; (void)s; return -1; }
static int b_ASensorEventQueue_setEventRate(void *q, void *s, int us) { (void)q; (void)s; (void)us; return -1; }
static int b_ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }
static long b_ASensorEventQueue_getEvents(void *q, void *ev, size_t n) { (void)q; (void)ev; (void)n; return 0; }
static const char *b_ASensor_getName(void *s) { (void)s; return ""; }
static const char *b_ASensor_getVendor(void *s) { (void)s; return ""; }
static int b_ASensor_getType(void *s) { (void)s; return 0; }
static float b_ASensor_getResolution(void *s) { (void)s; return 0.f; }
static int b_ASensor_getMinDelay(void *s) { (void)s; return 0; }

/* ------------------------------------------------------------ media: a video that is already over */

/*
 * Husk decodes no video. A game that plays one through the NDK's media API -- Unity's VideoPlayer, for an intro or a cutscene --
 * was refused outright, and a game that waits for its intro to finish waited on a black screen forever (Dave the Diver). Now it
 * is given the shortest video there is: an extractor with one video track that has no samples, and a decoder whose only frame
 * comes with the end of the stream. The video "plays" one blank frame and ends, and the game goes on as it does when a video
 * finishes.
 *
 * Unity 6 checks a video before it plays it: it decodes the first frames to measure the frame rate, and a video whose only frame
 * is its last must last one frame -- a duration of 0 fails that check, and Unity drops the video without a word. So the track
 * lasts one frame at 30 frames a second. On OpenGL ES the frame goes to a SurfaceTexture, and Unity waits until it hears the
 * frame arrived (onFrameAvailable) before it takes the next step; releasing a frame to the screen announces it.
 *
 * A video that ends the moment it starts is one no real device plays, and a game can count on its video lasting longer than a
 * frame: Dave the Diver starts its intro with one "video ended" handler and swaps in another on the next frame, and only the
 * second lets its loading screen finish -- with the first, the loading bar stopped near its end forever. So the decoder that
 * draws to the screen holds its frame back for a moment (MEDIA_SHOWN_NS) after it is set up. The decoder Unity checks the video
 * with hands its frame back at once, since that check is made before the video plays.
 */
#define MEDIA_OK 0
#define MEDIA_ERR (-10000)
#define MEDIA_TRY_AGAIN (-1)            /* AMEDIACODEC_INFO_TRY_AGAIN_LATER */
#define MEDIA_FLAG_EOS 4u               /* AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM */

/* AMediaFormat: a few keys and their values. */
enum { MF_INT32 = 1, MF_INT64, MF_FLOAT, MF_STRING };
typedef struct { char key[40]; int kind; int64_t i; float f; char s[96]; } mf_entry;
typedef struct { uint32_t magic; int n; mf_entry e[32]; } mformat;
#define MF_MAGIC 0x464d4448u

static mformat *mf_new(void) { mformat *f = calloc(1, sizeof(*f)); if (f) f->magic = MF_MAGIC; return f; }
static mf_entry *mf_find(mformat *f, const char *key, int make)
{
    if (!f || f->magic != MF_MAGIC || !key) return NULL;
    for (int i = 0; i < f->n; i++) if (!strcmp(f->e[i].key, key)) return &f->e[i];
    if (!make || f->n >= 32) return NULL;
    mf_entry *e = &f->e[f->n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", key);
    return e;
}
static void mf_set_i(mformat *f, const char *k, int kind, int64_t v) { mf_entry *e = mf_find(f, k, 1); if (e) { e->kind = kind; e->i = v; } }
static void mf_set_s(mformat *f, const char *k, const char *v) { mf_entry *e = mf_find(f, k, 1); if (e) { e->kind = MF_STRING; snprintf(e->s, sizeof(e->s), "%s", v ? v : ""); } }

/* What the track says: tiny, one frame long. */
#define MEDIA_FPS 30
#define MEDIA_FRAME_US (1000000 / MEDIA_FPS)
static mformat *mf_video(void)
{
    mformat *f = mf_new();
    mf_set_s(f, "mime", "video/avc");
    mf_set_i(f, "width", MF_INT32, 16); mf_set_i(f, "height", MF_INT32, 16);
    mf_set_i(f, "stride", MF_INT32, 16); mf_set_i(f, "slice-height", MF_INT32, 16);
    mf_set_i(f, "color-format", MF_INT32, 21);                /* COLOR_FormatYUV420SemiPlanar */
    mf_set_i(f, "durationUs", MF_INT64, MEDIA_FRAME_US);
    mf_set_i(f, "frame-rate", MF_INT32, MEDIA_FPS);
    mf_set_i(f, "max-input-size", MF_INT32, 4096);
    return f;
}

static void *b_AMediaFormat_new(void) { return mf_new(); }
static int b_AMediaFormat_delete(mformat *f) { if (f && f->magic == MF_MAGIC) { f->magic = 0; free(f); } return MEDIA_OK; }
static bool b_AMediaFormat_getInt32(mformat *f, const char *k, int32_t *out)
{ mf_entry *e = mf_find(f, k, 0); if (!e || (e->kind != MF_INT32 && e->kind != MF_INT64)) return false; if (out) *out = (int32_t)e->i; return true; }
static bool b_AMediaFormat_getInt64(mformat *f, const char *k, int64_t *out)
{ mf_entry *e = mf_find(f, k, 0); if (!e || (e->kind != MF_INT32 && e->kind != MF_INT64)) return false; if (out) *out = e->i; return true; }
static bool b_AMediaFormat_getFloat(mformat *f, const char *k, float *out)
{ mf_entry *e = mf_find(f, k, 0); if (!e) return false; if (out) *out = e->kind == MF_FLOAT ? e->f : (float)e->i; return e->kind != MF_STRING; }
static bool b_AMediaFormat_getString(mformat *f, const char *k, const char **out)
{ mf_entry *e = mf_find(f, k, 0); if (!e || e->kind != MF_STRING) return false; if (out) *out = e->s; return true; }
static void b_AMediaFormat_setInt32(mformat *f, const char *k, int32_t v) { mf_set_i(f, k, MF_INT32, v); }
static void b_AMediaFormat_setInt64(mformat *f, const char *k, int64_t v) { mf_set_i(f, k, MF_INT64, v); }
static void b_AMediaFormat_setFloat(mformat *f, const char *k, float v) { mf_entry *e = mf_find(f, k, 1); if (e) { e->kind = MF_FLOAT; e->f = v; } }
static void b_AMediaFormat_setString(mformat *f, const char *k, const char *v) { mf_set_s(f, k, v); }
static const char *b_AMediaFormat_toString(mformat *f) { (void)f; return "{mime=video/avc, width=16, height=16, durationUs=33333, frame-rate=30}"; }

/* AMediaExtractor: one video track, no samples. */
typedef struct { uint32_t magic; } mextractor;
#define MX_MAGIC 0x584d4448u
static void *b_AMediaExtractor_new(void) { mextractor *x = calloc(1, sizeof(*x)); if (x) x->magic = MX_MAGIC; return x; }
static int b_AMediaExtractor_delete(mextractor *x) { if (x && x->magic == MX_MAGIC) { x->magic = 0; free(x); } return MEDIA_OK; }
static void media_note(const char *what)
{
    static int said;
    if (said++ < 8) tl_log_line("media: the game opened a video (%s); Husk plays no video, so it is told the video is already over", what);
}
static int b_AMediaExtractor_setDataSourceFd(mextractor *x, int fd, int64_t off, int64_t len)
{
    (void)x; (void)fd;
    char what[80]; snprintf(what, sizeof(what), "%lld bytes at %lld in a file", (long long)len, (long long)off);
    media_note(what);
    return MEDIA_OK;
}
static int b_AMediaExtractor_setDataSource(mextractor *x, const char *where) { (void)x; media_note(where ? where : "a location"); return MEDIA_OK; }
static size_t b_AMediaExtractor_getTrackCount(mextractor *x) { (void)x; return 1; }
static void *b_AMediaExtractor_getTrackFormat(mextractor *x, size_t i) { (void)x; (void)i; return mf_video(); }
static void *b_AMediaExtractor_getFileFormat(mextractor *x) { (void)x; return mf_video(); }
static int b_AMediaExtractor_selectTrack(mextractor *x, size_t i) { (void)x; (void)i; return MEDIA_OK; }
static ssize_t b_AMediaExtractor_readSampleData(mextractor *x, uint8_t *buf, size_t cap) { (void)x; (void)buf; (void)cap; return -1; }
static int64_t b_AMediaExtractor_getSampleTime(mextractor *x) { (void)x; return -1; }
static int b_AMediaExtractor_getSampleTrackIndex(mextractor *x) { (void)x; return -1; }
static uint32_t b_AMediaExtractor_getSampleFlags(mextractor *x) { (void)x; return 0; }
static int64_t b_AMediaExtractor_getSampleSize(mextractor *x) { (void)x; return -1; }
static bool b_AMediaExtractor_advance(mextractor *x) { (void)x; return false; }
static int b_AMediaExtractor_seekTo(mextractor *x, int64_t t, int mode) { (void)x; (void)t; (void)mode; return MEDIA_OK; }

/* AMediaCodec: takes what it is given, and answers with the end of the stream. */
typedef struct { int32_t offset, size; int64_t presentationTimeUs; uint32_t flags; } mbufinfo;
typedef struct { uint32_t magic; bool eos_in, eos_out; atomic_bool configured; int waits; int64_t out_pts, shown_at; tl_nwindow *window; uint8_t buf[4096]; } mcodec;
#define MC_MAGIC 0x434d4448u
#define MEDIA_SHOWN_NS 1000000000ll     /* how long a video drawn to the screen lasts: a second */
static int64_t media_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec; }

/* A decoder the game made and never set up is one whose game is stuck before playing: say so, once, a few seconds on. */
static void *codec_watch(void *arg)
{
    mcodec *c = arg;
    pthread_setname_np("husk-media-watch");
    sleep(4);
    if (c->magic == MC_MAGIC && !atomic_load(&c->configured))
        tl_log_line("media: a decoder made 4 s ago was never set up; the game is stuck before its video plays (waiting for the video's surface?)");
    return NULL;
}
static void *b_AMediaCodec_create(const char *what)
{
    static int said;
    bool tell = said++ < 4;
    if (tell) tl_log_line("media: a %s decoder was asked for; it will report the end of the stream", what ? what : "video");
    mcodec *c = calloc(1, sizeof(*c));
    if (c) c->magic = MC_MAGIC;
    if (c && tell) { pthread_t t; if (pthread_create(&t, NULL, codec_watch, c) == 0) pthread_detach(t); }
    return c;
}
/* Marked dead and kept: a decoder's own thread (asynchronous mode) may still look at it after the game has deleted it. */
static int b_AMediaCodec_delete(mcodec *c) { if (c && c->magic == MC_MAGIC) c->magic = 0; return MEDIA_OK; }
static int b_AMediaCodec_ok(void) { return MEDIA_OK; }
/* The window is where frames go: the screen of a video's SurfaceTexture, or none when the game takes the frames back in memory. */
static int b_AMediaCodec_configure(mcodec *c, mformat *f, tl_nwindow *w, void *crypto, uint32_t flags)
{
    (void)f; (void)crypto; (void)flags;
    if (!c || c->magic != MC_MAGIC) return MEDIA_ERR;
    if (w) atomic_fetch_add(&w->refs, 1);               /* kept for as long as the decoder: windows here are never freed */
    c->window = w && w->texture ? w : NULL;
    c->shown_at = c->window ? media_now() + MEDIA_SHOWN_NS : 0;
    atomic_store(&c->configured, true);
    static int said;
    if (said++ < 6) tl_log_line("media: the decoder was set up %s", c->window ? "to draw into the video's SurfaceTexture" : w ? "to draw into a window" : "to hand frames back in memory");
    return MEDIA_OK;
}
static int b_AMediaCodec_flush(mcodec *c) { if (c && c->magic == MC_MAGIC) { c->eos_in = c->eos_out = false; c->waits = 0; } return MEDIA_OK; }
static ssize_t b_AMediaCodec_dequeueInputBuffer(mcodec *c, int64_t timeout) { (void)c; (void)timeout; return 0; }
static uint8_t *b_AMediaCodec_getBuffer(mcodec *c, size_t i, size_t *size)
{
    (void)i;
    if (!c || c->magic != MC_MAGIC) { if (size) *size = 0; return NULL; }
    if (size) *size = sizeof(c->buf);
    return c->buf;
}
static int b_AMediaCodec_queueInputBuffer(mcodec *c, size_t i, int64_t off, size_t size, uint64_t t, uint32_t flags)
{
    (void)i; (void)off; (void)size; (void)t;
    if (c && c->magic == MC_MAGIC && (flags & MEDIA_FLAG_EOS)) c->eos_in = true;
    return MEDIA_OK;
}
static ssize_t b_AMediaCodec_dequeueOutputBuffer(mcodec *c, mbufinfo *info, int64_t timeout)
{
    if (!c || c->magic != MC_MAGIC) return MEDIA_TRY_AGAIN;
    /* The end, once: as soon as the end of the input has come, or after a few waits if the game never sends it -- and for a
     * video on the screen, not before it has been on the screen for a moment. */
    if (!c->eos_out && (c->eos_in || ++c->waits > 3) && (!c->shown_at || media_now() >= c->shown_at)) {
        c->eos_out = true;
        c->out_pts = 0;
        if (info) { info->offset = 0; info->size = 0; info->presentationTimeUs = c->out_pts; info->flags = MEDIA_FLAG_EOS; }
        static int said;
        if (said++ < 6) tl_log_line("media: the decoder handed back the video's only frame, with the end of the stream");
        return 0;
    }
    if (timeout > 0) usleep((useconds_t)(timeout < 10000 ? timeout : 10000));
    return MEDIA_TRY_AGAIN;
}
/* A frame released to the screen is drawn into the video's SurfaceTexture, and its listener hears of it. */
static void codec_drew(mcodec *c)
{
    if (!c || c->magic != MC_MAGIC || !c->window) return;
    static int said;
    if (said++ < 4) tl_log_line("media: the game showed the video's frame; telling it the frame arrived");
    tl_surface_texture_frame(c->window->texture, c->out_pts * 1000);
}
static int b_AMediaCodec_releaseOutputBuffer(mcodec *c, size_t i, bool render) { (void)i; if (render) codec_drew(c); return MEDIA_OK; }
static int b_AMediaCodec_releaseOutputBufferAtTime(mcodec *c, size_t i, int64_t t) { (void)i; (void)t; codec_drew(c); return MEDIA_OK; }
static void *b_AMediaCodec_getOutputFormat(mcodec *c) { (void)c; return mf_video(); }
static int b_AMediaCodec_getName(mcodec *c, char **out) { (void)c; if (out) *out = strdup("c2.husk.none"); return MEDIA_OK; }
static void *b_AMediaCodec_getInputFormat(mcodec *c) { (void)c; return mf_video(); }
static int b_AMediaCodec_err(void) { return MEDIA_ERR; }
static void *b_media_none(void) { return NULL; }
static bool b_media_false(void) { return false; }

/*
 * A decoder in asynchronous mode (setAsyncNotifyCallback, Android 9): it tells the game when an input buffer is free and when
 * output is ready, from a thread of its own. This one offers one input buffer as it starts and then, as soon as the end of the
 * input comes (or after a moment if it never does), the end of the stream.
 */
typedef struct {
    void (*input)(void *codec, void *user, int32_t index);
    void (*output)(void *codec, void *user, int32_t index, mbufinfo *info);
    void (*format)(void *codec, void *user, void *format);
    void (*error)(void *codec, void *user, int err, int32_t action, const char *detail);
} mcallbacks;
typedef struct { mcodec *c; mcallbacks cb; void *user; } masync;
static struct { mcodec *c; mcallbacks cb; void *user; } g_async[8];
static pthread_mutex_t g_async_mu = PTHREAD_MUTEX_INITIALIZER;

static int b_AMediaCodec_setAsyncNotifyCallback(mcodec *c, mcallbacks cb, void *user)
{
    pthread_mutex_lock(&g_async_mu);
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++) if (g_async[i].c == c || !g_async[i].c) slot = i;
    if (slot >= 0) { g_async[slot].c = c; g_async[slot].cb = cb; g_async[slot].user = user; }
    pthread_mutex_unlock(&g_async_mu);
    return slot >= 0 ? MEDIA_OK : MEDIA_ERR;
}
static void *async_run(void *arg)
{
    masync a = *(masync *)arg;
    free(arg);
    pthread_setname_np("husk-media");
    usleep(5000);
    if (a.cb.input) a.cb.input(a.c, a.user, 0);
    for (int i = 0; i < 20 && a.c->magic == MC_MAGIC && !a.c->eos_in; i++) usleep(5000);
    while (a.c->magic == MC_MAGIC && a.c->shown_at && media_now() < a.c->shown_at) usleep(10000);
    if (a.c->magic == MC_MAGIC && !a.c->eos_out && a.cb.output) {
        a.c->eos_out = true;
        mbufinfo info = { 0, 0, 0, MEDIA_FLAG_EOS };
        a.cb.output(a.c, a.user, 0, &info);
    }
    return NULL;
}
static int b_AMediaCodec_start(mcodec *c)
{
    masync *a = NULL;
    pthread_mutex_lock(&g_async_mu);
    for (int i = 0; i < 8; i++) if (g_async[i].c == c && c) {
        a = malloc(sizeof(*a));
        if (a) { a->c = c; a->cb = g_async[i].cb; a->user = g_async[i].user; }
        break;
    }
    pthread_mutex_unlock(&g_async_mu);
    if (a) {
        pthread_t t;
        if (pthread_create(&t, NULL, async_run, a) == 0) pthread_detach(t); else free(a);
    }
    return MEDIA_OK;
}

/* AMediaDataSource (Android 9): a game that reads the video itself, out of its own packages. */
typedef struct {
    uint32_t magic; void *user;
    ssize_t (*read_at)(void *user, int64_t off, void *buf, size_t size);
    ssize_t (*get_size)(void *user);
    void (*close)(void *user);
} msource;
#define MS_MAGIC 0x534d4448u
static void *b_AMediaDataSource_new(void) { msource *d = calloc(1, sizeof(*d)); if (d) d->magic = MS_MAGIC; return d; }
static void b_AMediaDataSource_delete(msource *d) { if (d && d->magic == MS_MAGIC) { d->magic = 0; free(d); } }
static void b_AMediaDataSource_setUserdata(msource *d, void *u) { if (d && d->magic == MS_MAGIC) d->user = u; }
static void b_AMediaDataSource_setReadAt(msource *d, void *f) { if (d && d->magic == MS_MAGIC) d->read_at = (ssize_t (*)(void *, int64_t, void *, size_t))f; }
static void b_AMediaDataSource_setGetSize(msource *d, void *f) { if (d && d->magic == MS_MAGIC) d->get_size = (ssize_t (*)(void *))f; }
static void b_AMediaDataSource_setClose(msource *d, void *f) { if (d && d->magic == MS_MAGIC) d->close = (void (*)(void *))f; }
static void b_AMediaDataSource_ignore(void) { }
static int b_AMediaExtractor_setDataSourceCustom(mextractor *x, msource *d)
{
    (void)x;
    char what[80];
    long long size = d && d->magic == MS_MAGIC && d->get_size ? (long long)d->get_size(d->user) : -1;
    snprintf(what, sizeof(what), "%lld bytes, read through the game's own reader", size);
    media_note(what);
    return MEDIA_OK;
}

/* The rest of AMediaFormat: doubles, sizes, rectangles and buffers are kept as numbers or not at all. */
static bool b_AMediaFormat_getDouble(mformat *f, const char *k, double *out)
{ mf_entry *e = mf_find(f, k, 0); if (!e || e->kind == MF_STRING) return false; if (out) *out = e->kind == MF_FLOAT ? e->f : (double)e->i; return true; }
static bool b_AMediaFormat_getSize(mformat *f, const char *k, size_t *out)
{ mf_entry *e = mf_find(f, k, 0); if (!e || (e->kind != MF_INT32 && e->kind != MF_INT64)) return false; if (out) *out = (size_t)e->i; return true; }
static void b_AMediaFormat_setDouble(mformat *f, const char *k, double v) { mf_entry *e = mf_find(f, k, 1); if (e) { e->kind = MF_FLOAT; e->f = (float)v; } }
static void b_AMediaFormat_setSize(mformat *f, const char *k, size_t v) { mf_set_i(f, k, MF_INT64, (int64_t)v); }
static bool b_AMediaFormat_getRect(mformat *f, const char *k, int32_t *l, int32_t *t, int32_t *r, int32_t *b)
{
    (void)k;
    if (!f || f->magic != MF_MAGIC) return false;
    if (l) *l = 0; if (t) *t = 0; if (r) *r = 15; if (b) *b = 15;
    return true;
}
static void b_AMediaFormat_ignore(void) { }
static void b_AMediaFormat_clear(mformat *f) { if (f && f->magic == MF_MAGIC) f->n = 0; }
static int b_AMediaFormat_copy(mformat *to, mformat *from)
{
    if (!to || !from || to->magic != MF_MAGIC || from->magic != MF_MAGIC) return MEDIA_ERR;
    *to = *from;
    return MEDIA_OK;
}
static int b_AMediaExtractor_getSampleFormat(mextractor *x, mformat *f) { (void)x; (void)f; return MEDIA_ERR; }
static int64_t b_AMediaExtractor_getCachedDuration(mextractor *x) { (void)x; return -1; }

#define KEY(sym, text) static const char *g_##sym = text;
KEY(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count") KEY(AMEDIAFORMAT_KEY_COLOR_FORMAT, "color-format")
KEY(AMEDIAFORMAT_KEY_COLOR_RANGE, "color-range") KEY(AMEDIAFORMAT_KEY_COLOR_STANDARD, "color-standard")
KEY(AMEDIAFORMAT_KEY_DURATION, "durationUs") KEY(AMEDIAFORMAT_KEY_ENCODER_DELAY, "encoder-delay")
KEY(AMEDIAFORMAT_KEY_FRAME_RATE, "frame-rate") KEY(AMEDIAFORMAT_KEY_HEIGHT, "height")
KEY(AMEDIAFORMAT_KEY_LANGUAGE, "language") KEY(AMEDIAFORMAT_KEY_MIME, "mime")
KEY(AMEDIAFORMAT_KEY_ROTATION, "rotation-degrees") KEY(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate")
KEY(AMEDIAFORMAT_KEY_SLICE_HEIGHT, "slice-height") KEY(AMEDIAFORMAT_KEY_STRIDE, "stride")
KEY(AMEDIAFORMAT_KEY_WIDTH, "width")

#define MEDIA_DATA(n)  TL_DATA(#n, &g_##n)

const tl_bionic_entry tl_tab_ndk[] = {
    /* zlib */
    TL_DIRECT(deflate), TL_DIRECT(deflateEnd), TL_DIRECT(deflateInit2_), TL_DIRECT(deflateInit_),
    TL_DIRECT(inflate), TL_DIRECT(inflateEnd), TL_DIRECT(inflateInit2_), TL_DIRECT(inflateInit_), TL_DIRECT(zError),
    TL_DIRECT(compressBound), TL_DIRECT(zlibVersion), TL_DIRECT(compress), TL_DIRECT(compress2), TL_DIRECT(uncompress), TL_DIRECT(crc32), TL_DIRECT(adler32),
    TL_DIRECT(inflateReset), TL_DIRECT(inflateReset2), TL_DIRECT(deflateReset), TL_DIRECT(inflateSync), TL_DIRECT(deflateParams), TL_DIRECT(deflateSetDictionary), TL_DIRECT(inflateSetDictionary),
    /* looper */
    TL_WRAP("ALooper_prepare", b_ALooper_prepare), TL_WRAP("ALooper_forThread", b_ALooper_forThread),
    TL_WRAP("ALooper_acquire", b_ALooper_acquire), TL_WRAP("ALooper_release", b_ALooper_release),
    TL_WRAP("ALooper_wake", b_ALooper_wake), TL_WRAP("ALooper_pollOnce", b_ALooper_pollOnce), TL_WRAP("ALooper_pollAll", b_ALooper_pollAll),
    TL_WRAP("ALooper_addFd", b_ALooper_addFd), TL_WRAP("ALooper_removeFd", b_ALooper_removeFd),
    /* configuration */
    TL_WRAP("AConfiguration_new", b_AConfiguration_new), TL_WRAP("AConfiguration_delete", b_AConfiguration_delete),
    TL_WRAP("AConfiguration_fromAssetManager", b_AConfiguration_fromAssetManager), TL_WRAP("AConfiguration_getLanguage", b_AConfiguration_getLanguage),
    TL_WRAP("AConfiguration_getCountry", b_AConfiguration_getCountry), TL_WRAP("AConfiguration_getOrientation", b_AConfiguration_getOrientation),
    TL_WRAP("AConfiguration_getDensity", b_AConfiguration_getDensity), TL_WRAP("AConfiguration_getScreenLong", b_AConfiguration_getScreenLong),
    TL_WRAP("AConfiguration_getScreenSize", b_AConfiguration_getScreenSize), TL_WRAP("AConfiguration_getUiModeType", b_AConfiguration_getUiModeType),
    TL_WRAP("AConfiguration_getUiModeNight", b_AConfiguration_getUiModeNight), TL_WRAP("AConfiguration_getKeyboard", b_AConfiguration_getKeyboard),
    TL_WRAP("AConfiguration_getNavigation", b_AConfiguration_getNavigation), TL_WRAP("AConfiguration_getTouchscreen", b_AConfiguration_getTouchscreen),
    TL_WRAP("AConfiguration_getSdkVersion", b_AConfiguration_getSdkVersion),
    /* assets */
    TL_WRAP("AAudio_createStreamBuilder", b_AAudio_createStreamBuilder), TL_WRAP("AAudio_convertResultToText", b_AAudio_convertResultToText),
    TL_WRAP("AAudioStreamBuilder_delete", b_AAudioStreamBuilder_delete), TL_WRAP("AAudioStreamBuilder_setDataCallback", b_AAudioStreamBuilder_setDataCallback),
    TL_WRAP("AAudioStreamBuilder_setDirection", b_AAudioStreamBuilder_setDirection), TL_WRAP("AAudioStreamBuilder_setErrorCallback", b_AAudioStreamBuilder_setErrorCallback),
    TL_WRAP("AAudioStreamBuilder_openStream", b_AAudioStreamBuilder_openStream), TL_WRAP("AAudioStreamBuilder_setDeviceId", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStreamBuilder_setBufferCapacityInFrames", b_AAudioStreamBuilder_ignore), TL_WRAP("AAudioStreamBuilder_setInputPreset", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStreamBuilder_setPerformanceMode", b_AAudioStreamBuilder_ignore), TL_WRAP("AAudioStreamBuilder_setUsage", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStreamBuilder_setSampleRate", b_AAudioStreamBuilder_setSampleRate), TL_WRAP("AAudioStreamBuilder_setChannelCount", b_AAudioStreamBuilder_setChannelCount),
    TL_WRAP("AAudioStreamBuilder_setFormat", b_AAudioStreamBuilder_setFormat), TL_WRAP("AAudioStreamBuilder_setSharingMode", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStream_requestStart", b_AAudioStream_requestStart), TL_WRAP("AAudioStream_requestStop", b_AAudioStream_requestStop),
    TL_WRAP("AAudioStream_close", b_AAudioStream_close), TL_WRAP("AAudioStream_getSampleRate", b_AAudioStream_getSampleRate),
    TL_WRAP("AAudioStream_getChannelCount", b_AAudioStream_getChannelCount), TL_WRAP("AAudioStream_getFormat", b_AAudioStream_getFormat),
    TL_WRAP("AAudioStream_getDeviceId", b_AAudioStream_getDeviceId), TL_WRAP("AAudioStream_getFramesPerBurst", b_AAudioStream_getFramesPerBurst),
    TL_WRAP("AAudioStream_getBufferCapacityInFrames", b_AAudioStream_getBufferCapacityInFrames), TL_WRAP("AAudioStream_getBufferSizeInFrames", b_AAudioStream_getBufferSizeInFrames),
    TL_WRAP("AAudioStream_setBufferSizeInFrames", b_AAudioStream_setBufferSizeInFrames), TL_WRAP("AAudioStream_getXRunCount", b_AAudioStream_getXRunCount),
    TL_WRAP("AAudioStream_getState", b_AAudioStream_getState), TL_WRAP("AAudioStream_read", b_AAudioStream_read),
    /* the rest of AAudio, which an engine that loads it with dlopen and dlsym (FMOD in Unity 6) looks up all of */
    TL_WRAP("AAudioStreamBuilder_setFramesPerDataCallback", b_AAudioStreamBuilder_setFramesPerDataCallback),
    TL_WRAP("AAudioStreamBuilder_setSamplesPerFrame", b_AAudioStreamBuilder_setChannelCount), TL_WRAP("AAudioStreamBuilder_setContentType", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStreamBuilder_setSessionId", b_AAudioStreamBuilder_ignore), TL_WRAP("AAudioStreamBuilder_setAllowedCapturePolicy", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStreamBuilder_setPrivacySensitive", b_AAudioStreamBuilder_ignore), TL_WRAP("AAudioStreamBuilder_setChannelMask", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStreamBuilder_setSpatializationBehavior", b_AAudioStreamBuilder_ignore), TL_WRAP("AAudioStreamBuilder_setIsContentSpatialized", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStreamBuilder_setPackageName", b_AAudioStreamBuilder_ignore), TL_WRAP("AAudioStreamBuilder_setAttributionTag", b_AAudioStreamBuilder_ignore),
    TL_WRAP("AAudioStream_requestPause", b_AAudioStream_requestPause), TL_WRAP("AAudioStream_requestFlush", b_AAudioStream_requestFlush),
    TL_WRAP("AAudioStream_release", b_AAudioStream_release), TL_WRAP("AAudioStream_write", b_AAudioStream_write),
    TL_WRAP("AAudioStream_waitForStateChange", b_AAudioStream_waitForStateChange), TL_WRAP("AAudioStream_getTimestamp", b_AAudioStream_getTimestamp),
    TL_WRAP("AAudioStream_getFramesWritten", b_AAudioStream_getFramesWritten), TL_WRAP("AAudioStream_getFramesRead", b_AAudioStream_getFramesRead),
    TL_WRAP("AAudioStream_getFramesPerDataCallback", b_AAudioStream_getFramesPerDataCallback),
    TL_WRAP("AAudioStream_getPerformanceMode", b_AAudioStream_getPerformanceMode), TL_WRAP("AAudioStream_getSharingMode", b_AAudioStream_getSharingMode),
    TL_WRAP("AAudioStream_getSamplesPerFrame", b_AAudioStream_getChannelCount), TL_WRAP("AAudioStream_getUsage", b_AAudioStream_getUsage),
    TL_WRAP("AAudioStream_getContentType", b_AAudioStream_getContentType), TL_WRAP("AAudioStream_getInputPreset", b_AAudioStream_getInputPreset),
    TL_WRAP("AAudioStream_getSessionId", b_AAudioStream_getSessionId), TL_WRAP("AAudioStream_getChannelMask", b_AAudioStream_getChannelMask),
    TL_WRAP("AAudioStream_isMMapUsed", b_AAudioStream_isMMapUsed), TL_WRAP("AAudio_convertStreamStateToText", b_AAudio_convertStreamStateToText),
    TL_WRAP("AAudioStream_getHardwareSampleRate", b_AAudioStream_getHardwareSampleRate), TL_WRAP("AAudioStream_getHardwareChannelCount", b_AAudioStream_getHardwareChannelCount),
    TL_WRAP("AAudioStream_getHardwareFormat", b_AAudioStream_getHardwareFormat),
    TL_WRAP("AAssetManager_openDir", b_AAssetManager_openDir), TL_WRAP("AAssetDir_getNextFileName", b_AAssetDir_getNextFileName),
    TL_WRAP("AAssetDir_close", b_AAssetDir_close), TL_WRAP("deflateBound", b_deflateBound),
    TL_WRAP("AAssetManager_fromJava", b_AAssetManager_fromJava), TL_WRAP("AAssetManager_open", b_AAssetManager_open),
    TL_WRAP("AAsset_close", b_AAsset_close), TL_WRAP("AAsset_read", b_AAsset_read), TL_WRAP("AAsset_getLength", b_AAsset_getLength),
    TL_WRAP("AAsset_getLength64", b_AAsset_getLength), TL_WRAP("AAsset_getRemainingLength", b_AAsset_getRemainingLength),
    TL_WRAP("AAsset_getRemainingLength64", b_AAsset_getRemainingLength), TL_WRAP("AAsset_getBuffer", b_AAsset_getBuffer),
    TL_WRAP("AAsset_seek", b_AAsset_seek), TL_WRAP("AAsset_seek64", b_AAsset_seek), TL_WRAP("AAsset_isAllocated", b_AAsset_isAllocated),
    /* windows */
    TL_WRAP("ANativeWindow_fromSurface", b_ANativeWindow_fromSurface), TL_WRAP("ANativeWindow_acquire", b_ANativeWindow_acquire),
    TL_WRAP("ANativeWindow_release", b_ANativeWindow_release), TL_WRAP("ANativeWindow_getWidth", b_ANativeWindow_getWidth),
    TL_WRAP("ANativeWindow_getHeight", b_ANativeWindow_getHeight), TL_WRAP("ANativeWindow_getFormat", b_ANativeWindow_getFormat),
    TL_WRAP("ANativeWindow_setBuffersGeometry", b_ANativeWindow_setBuffersGeometry),
    TL_WRAP("ANativeWindow_toSurface", b_ANativeWindow_toSurface),
    TL_WRAP("AInputQueue_attachLooper", b_AInputQueue_attachLooper), TL_WRAP("AInputQueue_detachLooper", b_AInputQueue_detachLooper),
    TL_WRAP("AInputQueue_hasEvents", b_AInputQueue_hasEvents), TL_WRAP("AInputQueue_getEvent", b_AInputQueue_getEvent),
    TL_WRAP("AInputQueue_preDispatchEvent", b_AInputQueue_preDispatchEvent), TL_WRAP("AInputQueue_finishEvent", b_AInputQueue_finishEvent),
    TL_WRAP("AInputEvent_getType", b_AInputEvent_getType), TL_WRAP("AInputEvent_getSource", b_AInputEvent_getSource), TL_WRAP("AInputEvent_getDeviceId", b_AInputEvent_getDeviceId),
    TL_WRAP("AMotionEvent_getAction", b_AMotionEvent_getAction), TL_WRAP("AMotionEvent_getFlags", b_AMotionEvent_getFlags), TL_WRAP("AMotionEvent_getMetaState", b_AMotionEvent_getMetaState),
    TL_WRAP("AMotionEvent_getButtonState", b_AMotionEvent_getButtonState), TL_WRAP("AMotionEvent_getEdgeFlags", b_AMotionEvent_getEdgeFlags),
    TL_WRAP("AMotionEvent_getEventTime", b_AMotionEvent_getEventTime), TL_WRAP("AMotionEvent_getDownTime", b_AMotionEvent_getDownTime),
    TL_WRAP("AMotionEvent_getPointerCount", b_AMotionEvent_getPointerCount), TL_WRAP("AMotionEvent_getPointerId", b_AMotionEvent_getPointerId),
    TL_WRAP("AMotionEvent_getX", b_AMotionEvent_getX), TL_WRAP("AMotionEvent_getY", b_AMotionEvent_getY), TL_WRAP("AMotionEvent_getRawX", b_AMotionEvent_getX), TL_WRAP("AMotionEvent_getRawY", b_AMotionEvent_getY), TL_WRAP("AMotionEvent_getPressure", b_AMotionEvent_getPressure),
    TL_WRAP("AMotionEvent_getSize", b_AMotionEvent_getSize), TL_WRAP("AMotionEvent_getHistorySize", b_AMotionEvent_getHistorySize), TL_WRAP("AMotionEvent_getAxisValue", b_AMotionEvent_getAxisValue),
    TL_WRAP("AKeyEvent_getAction", b_AKeyEvent_getAction), TL_WRAP("AKeyEvent_getKeyCode", b_AKeyEvent_getKeyCode), TL_WRAP("AKeyEvent_getFlags", b_AKeyEvent_getFlags),
    TL_WRAP("AKeyEvent_getMetaState", b_AKeyEvent_getMetaState), TL_WRAP("AKeyEvent_getRepeatCount", b_AKeyEvent_getRepeatCount), TL_WRAP("AKeyEvent_getScanCode", b_AKeyEvent_getScanCode),
    TL_WRAP("ANativeActivity_finish", b_ANativeActivity_noop), TL_WRAP("ANativeActivity_setWindowFormat", b_ANativeActivity_input), TL_WRAP("ANativeActivity_setWindowFlags", b_ANativeActivity_flags),
    TL_WRAP("ANativeActivity_showSoftInput", b_ANativeActivity_input), TL_WRAP("ANativeActivity_hideSoftInput", b_ANativeActivity_input),
    /* sensors */
    TL_WRAP("ASensorManager_getInstance", b_ASensorManager_getInstance), TL_WRAP("ASensorManager_getDefaultSensor", b_ASensorManager_getDefaultSensor),
    TL_WRAP("ASensorManager_getSensorList", b_ASensorManager_getSensorList), TL_WRAP("ASensorManager_createEventQueue", b_ASensorManager_createEventQueue),
    TL_WRAP("ASensorManager_destroyEventQueue", b_ASensorManager_destroyEventQueue), TL_WRAP("ASensorEventQueue_enableSensor", b_ASensorEventQueue_enableSensor),
    TL_WRAP("ASensorEventQueue_disableSensor", b_ASensorEventQueue_disableSensor), TL_WRAP("ASensorEventQueue_setEventRate", b_ASensorEventQueue_setEventRate),
    TL_WRAP("ASensorEventQueue_hasEvents", b_ASensorEventQueue_hasEvents), TL_WRAP("ASensorEventQueue_getEvents", b_ASensorEventQueue_getEvents),
    TL_WRAP("ASensor_getName", b_ASensor_getName), TL_WRAP("ASensor_getVendor", b_ASensor_getVendor), TL_WRAP("ASensor_getType", b_ASensor_getType),
    TL_WRAP("ASensor_getResolution", b_ASensor_getResolution), TL_WRAP("ASensor_getMinDelay", b_ASensor_getMinDelay),
    /* media */
    TL_WRAP("AMediaCodec_createDecoderByType", b_AMediaCodec_create), TL_WRAP("AMediaCodec_createCodecByName", b_AMediaCodec_create),
    TL_WRAP("AMediaCodec_configure", b_AMediaCodec_configure), TL_WRAP("AMediaCodec_start", b_AMediaCodec_start), TL_WRAP("AMediaCodec_stop", b_AMediaCodec_ok),
    TL_WRAP("AMediaCodec_flush", b_AMediaCodec_flush), TL_WRAP("AMediaCodec_delete", b_AMediaCodec_delete),
    TL_WRAP("AMediaCodec_createEncoderByType", b_media_none), TL_WRAP("AMediaCodec_getInputFormat", b_AMediaCodec_getInputFormat),
    TL_WRAP("AMediaCodec_getBufferFormat", b_AMediaCodec_getInputFormat), TL_WRAP("AMediaCodec_setParameters", b_AMediaCodec_ok),
    TL_WRAP("AMediaCodec_signalEndOfInputStream", b_AMediaCodec_ok), TL_WRAP("AMediaCodec_setAsyncNotifyCallback", b_AMediaCodec_setAsyncNotifyCallback),
    TL_WRAP("AMediaCodec_releaseCrypto", b_AMediaCodec_ok), TL_WRAP("AMediaCodec_queueSecureInputBuffer", b_AMediaCodec_err),
    TL_WRAP("AMediaCodec_createInputSurface", b_AMediaCodec_err), TL_WRAP("AMediaCodec_createPersistentInputSurface", b_AMediaCodec_err),
    TL_WRAP("AMediaCodec_setInputSurface", b_AMediaCodec_err), TL_WRAP("AMediaCodec_onAsyncNotifyCallback", b_AMediaCodec_ok),
    TL_WRAP("AMediaCrypto_isCryptoSchemeSupported", b_media_false), TL_WRAP("AMediaCrypto_requiresSecureDecoderComponent", b_media_false),
    TL_WRAP("AMediaCrypto_new", b_media_none), TL_WRAP("AMediaCrypto_delete", b_AMediaCodec_ok),
    TL_WRAP("AMediaDataSource_new", b_AMediaDataSource_new), TL_WRAP("AMediaDataSource_delete", b_AMediaDataSource_delete),
    TL_WRAP("AMediaDataSource_setUserdata", b_AMediaDataSource_setUserdata), TL_WRAP("AMediaDataSource_setReadAt", b_AMediaDataSource_setReadAt),
    TL_WRAP("AMediaDataSource_setGetSize", b_AMediaDataSource_setGetSize), TL_WRAP("AMediaDataSource_setClose", b_AMediaDataSource_setClose),
    TL_WRAP("AMediaDataSource_setGetAvailableSize", b_AMediaDataSource_ignore), TL_WRAP("AMediaDataSource_close", b_AMediaDataSource_ignore),
    TL_WRAP("AMediaDataSource_newUri", b_media_none),
    TL_WRAP("AMediaExtractor_setDataSourceCustom", b_AMediaExtractor_setDataSourceCustom),
    TL_WRAP("AMediaExtractor_getSampleFormat", b_AMediaExtractor_getSampleFormat), TL_WRAP("AMediaExtractor_getCachedDuration", b_AMediaExtractor_getCachedDuration),
    TL_WRAP("AMediaExtractor_getPsshInfo", b_media_none), TL_WRAP("AMediaExtractor_getSampleCryptoInfo", b_media_none),
    TL_WRAP("AMediaFormat_getDouble", b_AMediaFormat_getDouble), TL_WRAP("AMediaFormat_getSize", b_AMediaFormat_getSize),
    TL_WRAP("AMediaFormat_getRect", b_AMediaFormat_getRect), TL_WRAP("AMediaFormat_getBuffer", b_media_false),
    TL_WRAP("AMediaFormat_setDouble", b_AMediaFormat_setDouble), TL_WRAP("AMediaFormat_setSize", b_AMediaFormat_setSize),
    TL_WRAP("AMediaFormat_setRect", b_AMediaFormat_ignore), TL_WRAP("AMediaFormat_setBuffer", b_AMediaFormat_ignore),
    TL_WRAP("AMediaFormat_clear", b_AMediaFormat_clear), TL_WRAP("AMediaFormat_copy", b_AMediaFormat_copy),
    TL_WRAP("AMediaCodec_dequeueInputBuffer", b_AMediaCodec_dequeueInputBuffer), TL_WRAP("AMediaCodec_getInputBuffer", b_AMediaCodec_getBuffer),
    TL_WRAP("AMediaCodec_queueInputBuffer", b_AMediaCodec_queueInputBuffer), TL_WRAP("AMediaCodec_dequeueOutputBuffer", b_AMediaCodec_dequeueOutputBuffer),
    TL_WRAP("AMediaCodec_getOutputBuffer", b_AMediaCodec_getBuffer), TL_WRAP("AMediaCodec_releaseOutputBuffer", b_AMediaCodec_releaseOutputBuffer),
    TL_WRAP("AMediaCodec_releaseOutputBufferAtTime", b_AMediaCodec_releaseOutputBufferAtTime), TL_WRAP("AMediaCodec_getOutputFormat", b_AMediaCodec_getOutputFormat),
    TL_WRAP("AMediaCodec_setOutputSurface", b_AMediaCodec_ok), TL_WRAP("AMediaCodec_getName", b_AMediaCodec_getName),
    TL_WRAP("AMediaCodec_releaseName", b_AMediaCodec_ok),
    TL_WRAP("AMediaExtractor_new", b_AMediaExtractor_new), TL_WRAP("AMediaExtractor_delete", b_AMediaExtractor_delete),
    TL_WRAP("AMediaExtractor_setDataSource", b_AMediaExtractor_setDataSource), TL_WRAP("AMediaExtractor_setDataSourceFd", b_AMediaExtractor_setDataSourceFd),
    TL_WRAP("AMediaExtractor_getTrackCount", b_AMediaExtractor_getTrackCount), TL_WRAP("AMediaExtractor_getTrackFormat", b_AMediaExtractor_getTrackFormat),
    TL_WRAP("AMediaExtractor_getFileFormat", b_AMediaExtractor_getFileFormat), TL_WRAP("AMediaExtractor_selectTrack", b_AMediaExtractor_selectTrack),
    TL_WRAP("AMediaExtractor_unselectTrack", b_AMediaExtractor_selectTrack), TL_WRAP("AMediaExtractor_readSampleData", b_AMediaExtractor_readSampleData),
    TL_WRAP("AMediaExtractor_getSampleTime", b_AMediaExtractor_getSampleTime), TL_WRAP("AMediaExtractor_getSampleTrackIndex", b_AMediaExtractor_getSampleTrackIndex),
    TL_WRAP("AMediaExtractor_getSampleFlags", b_AMediaExtractor_getSampleFlags), TL_WRAP("AMediaExtractor_getSampleSize", b_AMediaExtractor_getSampleSize),
    TL_WRAP("AMediaExtractor_advance", b_AMediaExtractor_advance), TL_WRAP("AMediaExtractor_seekTo", b_AMediaExtractor_seekTo),
    TL_WRAP("AMediaFormat_new", b_AMediaFormat_new), TL_WRAP("AMediaFormat_delete", b_AMediaFormat_delete),
    TL_WRAP("AMediaFormat_getInt32", b_AMediaFormat_getInt32), TL_WRAP("AMediaFormat_getInt64", b_AMediaFormat_getInt64),
    TL_WRAP("AMediaFormat_getFloat", b_AMediaFormat_getFloat), TL_WRAP("AMediaFormat_getString", b_AMediaFormat_getString),
    TL_WRAP("AMediaFormat_setInt32", b_AMediaFormat_setInt32), TL_WRAP("AMediaFormat_setInt64", b_AMediaFormat_setInt64),
    TL_WRAP("AMediaFormat_setFloat", b_AMediaFormat_setFloat), TL_WRAP("AMediaFormat_setString", b_AMediaFormat_setString),
    TL_WRAP("AMediaFormat_toString", b_AMediaFormat_toString),
    MEDIA_DATA(AMEDIAFORMAT_KEY_CHANNEL_COUNT), MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_FORMAT), MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_RANGE),
    MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_STANDARD), MEDIA_DATA(AMEDIAFORMAT_KEY_DURATION), MEDIA_DATA(AMEDIAFORMAT_KEY_ENCODER_DELAY),
    MEDIA_DATA(AMEDIAFORMAT_KEY_FRAME_RATE), MEDIA_DATA(AMEDIAFORMAT_KEY_HEIGHT), MEDIA_DATA(AMEDIAFORMAT_KEY_LANGUAGE),
    MEDIA_DATA(AMEDIAFORMAT_KEY_MIME), MEDIA_DATA(AMEDIAFORMAT_KEY_ROTATION), MEDIA_DATA(AMEDIAFORMAT_KEY_SAMPLE_RATE),
    MEDIA_DATA(AMEDIAFORMAT_KEY_SLICE_HEIGHT), MEDIA_DATA(AMEDIAFORMAT_KEY_STRIDE), MEDIA_DATA(AMEDIAFORMAT_KEY_WIDTH),
    TL_END
};
