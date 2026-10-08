// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenSL ES, the way Unreal Engine plays through it: an engine, an output mix, and one player fed from an Android simple buffer queue.
 * The player's thread takes each queued buffer in turn, hands the samples to the host audio (tl_cocos_audio_hook, which blocks until the
 * speakers have room -- that is what paces the game's mixer) and then calls the game's callback so it can queue the next one.
 *
 * OpenSL's objects and interfaces are pointers to tables of functions, so each object here starts with the table pointer the guest
 * dereferences, and the interface the guest is given is the address of that pointer. Recording, effects and 3D audio are not offered.
 *
 * An interface is asked for by its ID, and Android compares IDs by what they say, not where they are: an engine that loads
 * libOpenSLES.so itself (FMOD in Unity does) may carry its own copies of the IDs instead of Android's, and must be answered all
 * the same -- refusing it is "FMOD failed to initialize the output device" and a game with no sound.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-bionic.h"

extern void (*tl_cocos_audio_hook)(const int16_t *samples, int frames, int channels, int rate);

enum {
    SL_OK = 0, SL_PARAMETER_INVALID = 2, SL_FEATURE_UNSUPPORTED = 12, SL_BUFFER_INSUFFICIENT = 7,
    SL_PLAYING = 3, SL_PAUSED = 2, SL_STOPPED = 1,
    SL_OBJECT_UNREALIZED = 1, SL_OBJECT_REALIZED = 2,
    SL_DATAFORMAT_PCM = 2, SL_DATALOCATOR_BUFFERQUEUE = 6, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE = 0x800007BD,
    SL_ANDROID_DATAFORMAT_PCM_EX = 4, SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT = 1, SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT = 2,
    SL_ANDROID_PCM_REPRESENTATION_FLOAT = 3,
};
/* the samples a player is fed */
enum { PCM_S16, PCM_U8, PCM_S32, PCM_F32 };

/* The interface identifiers: the guest imports them as variables holding a pointer to one of these, or carries its own copies. */
typedef struct { uint32_t time_low; uint16_t time_mid, time_hi, clock; uint8_t node[6]; } sl_iid;
static const sl_iid k_iid_engine   = { 0x8d97c260, 0xddd4, 0x11db, 0x958f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const sl_iid k_iid_play     = { 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const sl_iid k_iid_volume   = { 0x09e8ede0, 0xddde, 0x11db, 0xb4f6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const sl_iid k_iid_bq       = { 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const sl_iid k_iid_asbq     = { 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const sl_iid k_iid_androidcfg = { 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const sl_iid k_iid_record   = { 0xc5657aa0, 0xdddb, 0x11db, 0x82f7, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const sl_iid *const g_SL_IID_ENGINE = &k_iid_engine, *const g_SL_IID_PLAY = &k_iid_play, *const g_SL_IID_VOLUME = &k_iid_volume,
                    *const g_SL_IID_BUFFERQUEUE = &k_iid_bq, *const g_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &k_iid_asbq,
                    *const g_SL_IID_ANDROIDCONFIGURATION = &k_iid_androidcfg, *const g_SL_IID_RECORD = &k_iid_record;

typedef struct { const void *const *vt; } sl_if;     /* an interface: the guest sees a pointer to the table pointer */

/* The same interface: the same ID, wherever the guest keeps it. The first two fields tell every OpenSL ID apart. */
static bool iid_is(const sl_iid *a, const sl_iid *want)
{
    return a == want || (a && a->time_low == want->time_low && a->time_mid == want->time_mid);
}

/* FMOD queues (its buffer length x its buffer count) / Android's burst before it starts: 16 with Unity's 1024 x 4 and a burst of 256, and more with larger
 * buffers. A refused buffer fails FMOD's whole setup, so there is room for plenty. */
#define MAX_QUEUE 64
enum { K_ENGINE = 1, K_MIX, K_PLAYER };

typedef struct sl_object {
    const void *vt_object;                 /* must be first: SLObjectItf points here */
    int kind;
    int state;
    /* the interfaces an object can hand out */
    const void *vt_engine, *vt_play, *vt_volume, *vt_bq, *vt_config;
    /* a player's queue */
    pthread_mutex_t mu;
    struct { const void *data; uint32_t size; } q[MAX_QUEUE];
    int qhead, qcount;
    uint32_t qindex;
    void (*bq_cb)(void *caller, void *ctx);
    void *bq_ctx;
    int channels, rate, pcm, bytes;        /* the samples queued: channels, rate, PCM_*, bytes per sample */
    atomic_llong played;                   /* frames handed to the speakers, for GetPosition */
    atomic_int play_state;
    atomic_bool thread_run;
    pthread_t thread;
    bool thread_started;
} sl_object;

static sl_object *obj_of_object(const void *itf) { return (sl_object *)itf; }
#define OBJ_FROM(itf, field) ((sl_object *)((const char *)(itf) - offsetof(sl_object, field)))

/* ---- SLObjectItf */
static const char *kind_name(const sl_object *o) { static const char *const k[] = { "?", "engine", "output mix", "player" }; return k[o->kind >= 1 && o->kind <= K_PLAYER ? o->kind : 0]; }
static uint32_t o_Realize(const void *self, uint32_t async)
{
    sl_object *o = obj_of_object(self);
    o->state = SL_OBJECT_REALIZED;
    if (o->kind != K_PLAYER || tl_watch_here) tl_log_line("opensl: %s realized%s", kind_name(o), async ? " (asked asynchronously)" : "");
    return SL_OK;
}
static uint32_t o_Resume(const void *self, uint32_t async) { (void)self; (void)async; return SL_OK; }
static uint32_t o_GetState(const void *self, uint32_t *state) { if (state) *state = (uint32_t)obj_of_object(self)->state; return SL_OK; }
static uint32_t o_GetInterface(const void *self, const sl_iid *iid, const void **out)
{
    sl_object *o = obj_of_object(self);
    if (!out) return SL_PARAMETER_INVALID;
    if (iid_is(iid, &k_iid_engine) && o->kind == K_ENGINE) { *out = &o->vt_engine; return SL_OK; }
    if (iid_is(iid, &k_iid_play) && o->kind == K_PLAYER) { *out = &o->vt_play; return SL_OK; }
    if (iid_is(iid, &k_iid_volume) && (o->kind == K_PLAYER || o->kind == K_MIX)) { *out = &o->vt_volume; return SL_OK; }
    if ((iid_is(iid, &k_iid_asbq) || iid_is(iid, &k_iid_bq)) && o->kind == K_PLAYER) { *out = &o->vt_bq; return SL_OK; }
    if (iid_is(iid, &k_iid_androidcfg) && o->kind == K_PLAYER) { *out = &o->vt_config; return SL_OK; }
    *out = NULL;
    /* Once for each ID: a refusal here is often the whole of a game's sound. */
    static uint32_t said[32]; static int nsaid;
    uint32_t id = iid ? iid->time_low : 0;
    bool seen = false;
    for (int i = 0; i < nsaid && !seen; i++) seen = said[i] == id;
    if (!seen) {
        if (nsaid < 32) said[nsaid++] = id;
        tl_log_line("opensl: the game asked the %s for interface %08x, which Husk does not offer", kind_name(o), id);
    }
    return SL_FEATURE_UNSUPPORTED;
}
static uint32_t o_RegisterCallback(const void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_OK; }
static void o_AbortAsyncOperation(const void *self) { (void)self; }
static void player_stop_thread(sl_object *o);
static void o_Destroy(const void *self)
{
    sl_object *o = obj_of_object(self);
    tl_log_line("opensl: %s destroyed", kind_name(o));
    if (o->kind == K_PLAYER) player_stop_thread(o);
    if (o->kind == K_ENGINE) tl_watch_here = 0;
    free(o);
}
static uint32_t o_SetPriority(const void *self, int32_t p, uint32_t pre) { (void)self; (void)p; (void)pre; return SL_OK; }
static uint32_t o_GetPriority(const void *self, int32_t *p, uint32_t *pre) { (void)self; if (p) *p = 0; if (pre) *pre = 0; return SL_OK; }
static uint32_t o_SetLoss(const void *self, uint32_t n, const void *ids, uint32_t en) { (void)self; (void)n; (void)ids; (void)en; return SL_OK; }

static const void *const k_vt_object[] = { o_Realize, o_Resume, o_GetState, o_GetInterface, o_RegisterCallback, o_AbortAsyncOperation, o_Destroy, o_SetPriority, o_GetPriority, o_SetLoss };

static sl_object *new_object(int kind)
{
    sl_object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->vt_object = k_vt_object;
    o->kind = kind; o->state = SL_OBJECT_UNREALIZED;
    pthread_mutex_init(&o->mu, NULL);
    return o;
}

/* ---- SLPlayItf and the player's thread */

/* `n` samples of what the game queued, as 16-bit: into `tmp` unless they already are. */
static const int16_t *pcm_to_i16(int pcm, const void *src, int n, int16_t *tmp)
{
    switch (pcm) {
    case PCM_U8:  { const uint8_t *s = src; for (int i = 0; i < n; i++) tmp[i] = (int16_t)((s[i] - 128) * 256); return tmp; }
    case PCM_S32: { const int32_t *s = src; for (int i = 0; i < n; i++) tmp[i] = (int16_t)(s[i] >> 16); return tmp; }
    case PCM_F32: {
        const float *s = src;
        for (int i = 0; i < n; i++) { float v = s[i] * 32767.0f; tmp[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v); }
        return tmp;
    }
    default: return src;
    }
}

static void *player_pump(void *arg)
{
    sl_object *o = arg;
    pthread_setname_np("opensl-pump");
    int16_t *tmp = NULL; size_t tmp_n = 0;
    bool said = false;
    while (atomic_load(&o->thread_run)) {
        if (atomic_load(&o->play_state) != SL_PLAYING) { usleep(2000); continue; }
        pthread_mutex_lock(&o->mu);
        const void *data = NULL; uint32_t size = 0;
        if (o->qcount > 0) { data = o->q[o->qhead].data; size = o->q[o->qhead].size; }
        pthread_mutex_unlock(&o->mu);
        if (!data) { usleep(1000); continue; }
        int ch = o->channels > 0 ? o->channels : 2, rate = o->rate > 0 ? o->rate : 48000;
        int frames = (int)(size / ((uint32_t)o->bytes * (uint32_t)ch));
        const int16_t *pcm = data;
        if (o->pcm != PCM_S16) {
            size_t need = (size_t)frames * (size_t)ch;
            if (need > tmp_n) { free(tmp); tmp = malloc(need * sizeof(int16_t)); tmp_n = tmp ? need : 0; }
            pcm = tmp ? pcm_to_i16(o->pcm, data, frames * ch, tmp) : NULL;
        }
        if (!said) {
            for (int i = 0; pcm && i < frames * ch; i++) if (pcm[i]) { said = true; tl_log_line("opensl: the game's first sound (sample %d = %d)", i, pcm[i]); break; }
        }
        if (tl_cocos_audio_hook && pcm) tl_cocos_audio_hook(pcm, frames, ch, rate);
        else { struct timespec ts = { 0, (long)((double)frames * 1e9 / rate) }; nanosleep(&ts, NULL); }
        atomic_fetch_add(&o->played, frames);
        pthread_mutex_lock(&o->mu);
        if (o->qcount > 0) { o->qhead = (o->qhead + 1) % MAX_QUEUE; o->qcount--; o->qindex++; }
        pthread_mutex_unlock(&o->mu);
        if (o->bq_cb) o->bq_cb(&o->vt_bq, o->bq_ctx);
    }
    free(tmp);
    return NULL;
}
static void player_start_thread(sl_object *o)
{
    if (o->thread_started) return;
    atomic_store(&o->thread_run, true);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1u << 20);
    o->thread_started = pthread_create(&o->thread, &a, player_pump, o) == 0;
    pthread_attr_destroy(&a);
    tl_log_line("opensl: playing");
}
static void player_stop_thread(sl_object *o)
{
    if (!o->thread_started) return;
    atomic_store(&o->thread_run, false);
    pthread_join(o->thread, NULL);
    o->thread_started = false;
}

static uint32_t p_SetPlayState(const void *self, uint32_t state)
{
    sl_object *o = OBJ_FROM(self, vt_play);
    atomic_store(&o->play_state, (int)state);
    if (state == SL_PLAYING) player_start_thread(o);
    return SL_OK;
}
static uint32_t p_GetPlayState(const void *self, uint32_t *state) { sl_object *o = OBJ_FROM(self, vt_play); if (state) *state = (uint32_t)atomic_load(&o->play_state); return SL_OK; }
static uint32_t p_GetDuration(const void *self, uint32_t *ms) { (void)self; if (ms) *ms = 0xFFFFFFFFu; /* SL_TIME_UNKNOWN */ return SL_OK; }
static uint32_t p_GetPosition(const void *self, uint32_t *ms)
{
    sl_object *o = OBJ_FROM(self, vt_play);
    if (ms) *ms = (uint32_t)(atomic_load(&o->played) * 1000 / (o->rate > 0 ? o->rate : 48000));
    return SL_OK;
}
static uint32_t p_ok(void) { return SL_OK; }
static uint32_t p_get_zero(const void *self, uint32_t *v) { (void)self; if (v) *v = 0; return SL_OK; }
static const void *const k_vt_play[] = { p_SetPlayState, p_GetPlayState, p_GetDuration, p_GetPosition, p_ok, p_ok, p_get_zero, p_ok, p_ok,
                                         p_get_zero, p_ok, p_get_zero };

/* ---- SLAndroidSimpleBufferQueueItf */
static uint32_t q_Enqueue(const void *self, const void *buf, uint32_t size)
{
    sl_object *o = OBJ_FROM(self, vt_bq);
    pthread_mutex_lock(&o->mu);
    if (o->qcount >= MAX_QUEUE) {
        pthread_mutex_unlock(&o->mu);
        static bool said; if (!said) { said = true; tl_log_line("opensl: the game queued more than %d buffers; refused", MAX_QUEUE); }
        return SL_BUFFER_INSUFFICIENT;
    }
    int tail = (o->qhead + o->qcount) % MAX_QUEUE;
    o->q[tail].data = buf; o->q[tail].size = size;
    o->qcount++;
    pthread_mutex_unlock(&o->mu);
    return SL_OK;
}
static uint32_t q_Clear(const void *self)
{
    sl_object *o = OBJ_FROM(self, vt_bq);
    pthread_mutex_lock(&o->mu);
    o->qcount = 0;
    pthread_mutex_unlock(&o->mu);
    return SL_OK;
}
static uint32_t q_GetState(const void *self, uint32_t *state)       /* { count, index } */
{
    sl_object *o = OBJ_FROM(self, vt_bq);
    pthread_mutex_lock(&o->mu);
    if (state) { state[0] = (uint32_t)o->qcount; state[1] = o->qindex; }
    pthread_mutex_unlock(&o->mu);
    return SL_OK;
}
static uint32_t q_RegisterCallback(const void *self, void (*cb)(void *, void *), void *ctx)
{
    sl_object *o = OBJ_FROM(self, vt_bq);
    o->bq_cb = cb; o->bq_ctx = ctx;
    return SL_OK;
}
static const void *const k_vt_bq[] = { q_Enqueue, q_Clear, q_GetState, q_RegisterCallback };

/* ---- SLVolumeItf, SLAndroidConfigurationItf: accepted, and nothing done with them */
static uint32_t v_get_level(const void *self, int16_t *mb) { (void)self; if (mb) *mb = 0; return SL_OK; }
static uint32_t v_get_bool(const void *self, uint32_t *b) { (void)self; if (b) *b = 0; return SL_OK; }
static uint32_t v_get_pos(const void *self, int16_t *pm) { (void)self; if (pm) *pm = 0; return SL_OK; }
/* SetVolumeLevel, GetVolumeLevel, GetMaxVolumeLevel, SetMute, GetMute, EnableStereoPosition, IsEnabledStereoPosition,
 * SetStereoPosition, GetStereoPosition */
static const void *const k_vt_volume[] = { p_ok, v_get_level, v_get_level, p_ok, v_get_bool, p_ok, v_get_bool, p_ok, v_get_pos };
static uint32_t c_Config(const void *self, const void *key, const void *value, uint32_t size) { (void)self; (void)key; (void)value; (void)size; return SL_OK; }
static uint32_t c_JavaProxy(void) { return SL_FEATURE_UNSUPPORTED; }
/* SetConfiguration, GetConfiguration, and on Android 7 and later AcquireJavaProxy, ReleaseJavaProxy */
static const void *const k_vt_config[] = { c_Config, c_Config, c_JavaProxy, c_JavaProxy };

/* ---- SLEngineItf */
static uint32_t e_CreateMix(const void *self, const void **out, uint32_t n, const void *ids, const void *req)
{
    (void)self; (void)n; (void)ids; (void)req;
    sl_object *o = new_object(K_MIX);
    if (!o) return 3;
    o->vt_volume = k_vt_volume;
    tl_log_line("opensl: output mix created");
    *out = o;
    return SL_OK;
}
typedef struct { void *locator; void *format; } sl_data;
typedef struct { uint32_t type, channels, rate_mhz, bits, container, mask, endian, representation; } sl_pcm;  /* representation: PCM_EX only */
static uint32_t e_CreateAudioPlayer(const void *self, const void **out, const sl_data *src, const sl_data *snk, uint32_t n, const void *ids, const void *req)
{
    (void)self; (void)snk; (void)n; (void)ids; (void)req;
    sl_object *o = new_object(K_PLAYER);
    if (!o) return 3;
    o->vt_play = k_vt_play; o->vt_volume = k_vt_volume; o->vt_bq = k_vt_bq; o->vt_config = k_vt_config;
    o->channels = 2; o->rate = 48000; o->pcm = PCM_S16; o->bytes = 2;
    if (src && src->format) {
        const sl_pcm *f = src->format;
        if (f->type == SL_DATAFORMAT_PCM || f->type == SL_ANDROID_DATAFORMAT_PCM_EX) {
            o->channels = f->channels >= 1 && f->channels <= 8 ? (int)f->channels : 2;
            o->rate = f->rate_mhz >= 8000000 ? (int)(f->rate_mhz / 1000) : 48000;
            uint32_t bits = f->container ? f->container : f->bits;
            bool fl = f->type == SL_ANDROID_DATAFORMAT_PCM_EX && f->representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
            if (bits == 8) { o->pcm = PCM_U8; o->bytes = 1; }
            else if (bits == 32) { o->pcm = fl ? PCM_F32 : PCM_S32; o->bytes = 4; }
        }
    }
    atomic_store(&o->play_state, SL_STOPPED);
    static const char *const names[] = { "16-bit", "8-bit", "32-bit", "float" };
    tl_log_line("opensl: audio player (%d channel(s), %d Hz, %s samples)", o->channels, o->rate, names[o->pcm]);
    tl_watch_here = 0;
    *out = o;
    return SL_OK;
}
static uint32_t e_unsupported(const char *what) { tl_log_line("opensl: the game asked the engine to %s, which Husk does not offer", what); return SL_FEATURE_UNSUPPORTED; }
static uint32_t e_LED(void) { return e_unsupported("create an LED device"); }
static uint32_t e_Vibra(void) { return e_unsupported("create a vibration device"); }
static uint32_t e_Recorder(void) { return e_unsupported("create an audio recorder"); }
static uint32_t e_Midi(void) { return e_unsupported("create a MIDI player"); }
static uint32_t e_Listener(void) { return e_unsupported("create a 3D listener"); }
static uint32_t e_3DGroup(void) { return e_unsupported("create a 3D group"); }
static uint32_t e_Metadata(void) { return e_unsupported("create a metadata extractor"); }
static uint32_t e_Extension(void) { return e_unsupported("create an extension object"); }
/* What each kind of object offers, for a game that asks before it creates one. */
static int offered(uint32_t object_id, const sl_iid **out)
{
    switch (object_id) {
    case 0x1001: out[0] = &k_iid_engine; return 1;                                         /* SL_OBJECTID_ENGINE */
    case 0x1004: out[0] = &k_iid_play; out[1] = &k_iid_volume; out[2] = &k_iid_bq;          /* SL_OBJECTID_AUDIOPLAYER */
                 out[3] = &k_iid_asbq; out[4] = &k_iid_androidcfg; return 5;
    case 0x1009: out[0] = &k_iid_volume; return 1;                                          /* SL_OBJECTID_OUTPUTMIX */
    default: return 0;
    }
}
static uint32_t e_QueryCount(const void *self, uint32_t id, uint32_t *n)
{
    (void)self; const sl_iid *ids[8];
    if (!n) return SL_PARAMETER_INVALID;
    *n = (uint32_t)offered(id, ids);
    tl_log_line("opensl: the game asked which interfaces object type %#x has (%u)", id, *n);
    return SL_OK;
}
static uint32_t e_QueryOne(const void *self, uint32_t id, uint32_t index, const sl_iid **iid)
{
    (void)self; const sl_iid *ids[8];
    int n = offered(id, ids);
    if (!iid || index >= (uint32_t)n) return SL_PARAMETER_INVALID;
    *iid = ids[index];
    return SL_OK;
}
/* Android's engine has one extension, its API level -- which engines read to know what Android they are on. */
static const char k_extension[] = "ANDROID_SDK_LEVEL_34";
static uint32_t e_QueryExtCount(const void *self, uint32_t *n)
{
    (void)self;
    if (!n) return SL_PARAMETER_INVALID;
    *n = 1;
    tl_log_line("opensl: the game asked how many extensions the engine has (1: %s)", k_extension);
    return SL_OK;
}
static uint32_t e_QueryExt(const void *self, uint32_t index, char *name, int16_t *len)
{
    (void)self;
    if (!len) return SL_PARAMETER_INVALID;
    if (index != 0) { *len = 0; return SL_PARAMETER_INVALID; }
    int16_t need = (int16_t)sizeof(k_extension);                       /* with its NUL, as Android counts it */
    uint32_t r = SL_OK;
    if (name) {
        if (*len <= 0) r = SL_BUFFER_INSUFFICIENT;
        else if (need > *len) { memcpy(name, k_extension, (size_t)*len - 1); name[*len - 1] = 0; r = SL_BUFFER_INSUFFICIENT; }
        else memcpy(name, k_extension, sizeof(k_extension));
    }
    *len = need;
    return r;
}
static uint32_t e_IsExt(const void *self, const char *name, uint32_t *yes)
{
    (void)self;
    if (!name || !yes) return SL_PARAMETER_INVALID;
    *yes = !strcmp(name, k_extension);
    tl_log_line("opensl: the game asked whether the engine has extension %s (%s)", name, *yes ? "yes" : "no");
    return SL_OK;
}
static const void *const k_vt_engine[] = {
    e_LED, e_Vibra, e_CreateAudioPlayer, e_Recorder, e_Midi, e_Listener, e_3DGroup, e_CreateMix,
    e_Metadata, e_Extension, e_QueryCount, e_QueryOne, e_QueryExtCount, e_QueryExt, e_IsExt
};

static uint32_t b_slCreateEngine(const void **engine, uint32_t n, const void *opts, uint32_t nif, const void *ifs, const void *req)
{
    (void)n; (void)opts; (void)nif; (void)ifs; (void)req;
    sl_object *o = new_object(K_ENGINE);
    if (!o) return 3;
    o->vt_engine = k_vt_engine;
    /* Engines (FMOD) can stop between here and making a player without a word; until they make one, what this thread
     * asks of Android goes in the log. */
    tl_watch_here = 1;
    tl_log_line("opensl: engine created; logging what this thread asks of Android until it makes a player");
    *engine = o;
    return SL_OK;
}

#define SLID(name) TL_DATA(#name, &g_##name)
const tl_bionic_entry tl_tab_opensles[] = {
    TL_WRAP("slCreateEngine", b_slCreateEngine),
    SLID(SL_IID_ENGINE), SLID(SL_IID_PLAY), SLID(SL_IID_VOLUME), SLID(SL_IID_BUFFERQUEUE), SLID(SL_IID_ANDROIDSIMPLEBUFFERQUEUE),
    SLID(SL_IID_ANDROIDCONFIGURATION), SLID(SL_IID_RECORD),
    TL_END
};
