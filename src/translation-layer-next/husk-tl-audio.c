/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-audio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "husk-tl-bionic.h"
#include "husk-tl-cocos.h"

#define NBUF 3
#define FRAMES_PER_BUF 1024
#define RING_FRAMES 8192
#define MAXS 4

/*
 * One output per mixer thread. A game can have more than one: a Godot game with FMOD has the engine's own output (OpenSL, silence
 * mostly, at 44.1 kHz) and FMOD's (AudioTrack, everything you hear, at 48 kHz). Each gets its own AudioQueue at its own rate,
 * and iOS mixes them; poured into one ring they would interleave and play at the first one's rate.
 */
typedef struct stream {
    bool used, failed;
    pthread_t owner;
    AudioQueueRef q;
    AudioQueueBufferRef bufs[NBUF];
    int rate, channels;
    int16_t *ring;
    size_t head, count;           /* frames */
    int16_t prev[2];              /* the last frame, for 2x upsampling across writes */
    bool have_prev;
    struct timespec last;         /* when its writer last wrote */
} stream;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool paused;
    stream s[MAXS];
} A = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* The audio thread asks for samples; an empty ring is silence, never a wait. */
static void on_buffer(void *user, AudioQueueRef q, AudioQueueBufferRef b)
{
    stream *s = user;
    int16_t *out = b->mAudioData;
    pthread_mutex_lock(&A.mu);
    if (s->q != q || !s->ring) {                                            /* being closed */
        pthread_mutex_unlock(&A.mu);
        memset(out, 0, b->mAudioDataBytesCapacity);
        b->mAudioDataByteSize = b->mAudioDataBytesCapacity;
        return;
    }
    size_t want = FRAMES_PER_BUF, ch = (size_t)s->channels;
    size_t take = A.paused ? 0 : (s->count < want ? s->count : want);
    for (size_t i = 0; i < take; i++)
        for (size_t c = 0; c < ch; c++) out[i * ch + c] = s->ring[((s->head + i) % RING_FRAMES) * ch + c];
    s->head = (s->head + take) % RING_FRAMES;
    s->count -= take;
    pthread_cond_broadcast(&A.cv);
    pthread_mutex_unlock(&A.mu);
    if (take < want) memset(out + take * ch, 0, (want - take) * ch * sizeof(int16_t));
    b->mAudioDataByteSize = (UInt32)(want * ch * sizeof(int16_t));
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

/* Called with A.mu held, which is let go while the queue stops: its callback takes the lock too. */
static void close_queue(stream *s)
{
    AudioQueueRef q = s->q;
    int16_t *ring = s->ring;
    s->q = NULL; s->ring = NULL;
    s->head = s->count = 0;
    s->have_prev = false;
    pthread_cond_broadcast(&A.cv);
    pthread_mutex_unlock(&A.mu);
    if (q) { AudioQueueStop(q, true); AudioQueueDispose(q, true); }
    free(ring);
    pthread_mutex_lock(&A.mu);
}

static bool open_queue(stream *s, int rate, int channels)
{
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBitsPerChannel = 16; f.mChannelsPerFrame = (UInt32)channels; f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = (UInt32)(channels * 2);
    s->ring = calloc((size_t)RING_FRAMES * (size_t)channels, sizeof(int16_t));
    s->rate = rate; s->channels = channels;
    AudioQueueRef q = NULL;
    OSStatus st = AudioQueueNewOutput(&f, on_buffer, s, NULL, NULL, 0, &q);
    if (st != noErr) { tl_log_line("audio: AudioQueueNewOutput failed (%d)", (int)st); return false; }
    s->q = q;
    for (int i = 0; i < NBUF; i++) {
        AudioQueueAllocateBuffer(q, FRAMES_PER_BUF * (UInt32)channels * 2, &s->bufs[i]);
        memset(s->bufs[i]->mAudioData, 0, FRAMES_PER_BUF * (size_t)channels * 2);
        s->bufs[i]->mAudioDataByteSize = FRAMES_PER_BUF * (UInt32)channels * 2;
        AudioQueueEnqueueBuffer(q, s->bufs[i], 0, NULL);
    }
    st = A.paused ? noErr : AudioQueueStart(q, NULL);
    if (st != noErr) { tl_log_line("audio: AudioQueueStart failed (%d)", (int)st); return false; }
    int n = 0;
    for (int i = 0; i < MAXS; i++) n += A.s[i].used && A.s[i].q;
    tl_log_line("audio: output running, %d Hz, %d channel(s)%s", rate, channels, n > 1 ? " (mixed with the game's other output)" : "");
    return true;
}

static double since(const struct timespec *t, const struct timespec *now) { return (now->tv_sec - t->tv_sec) + (now->tv_nsec - t->tv_nsec) / 1e9; }

/* The calling thread's output, opened (or reopened at a new format) as needed. A new writer takes a free one, or the one written
 * to longest ago (its thread has gone: a player recreated). Called with A.mu held. */
static stream *stream_for(int rate, int channels)
{
    pthread_t me = pthread_self();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    stream *s = NULL;
    for (int i = 0; i < MAXS && !s; i++) if (A.s[i].used && pthread_equal(A.s[i].owner, me)) s = &A.s[i];
    if (!s) {
        for (int i = 0; i < MAXS && !s; i++) if (!A.s[i].used) s = &A.s[i];
        if (!s) {
            s = &A.s[0];
            for (int i = 1; i < MAXS; i++) if (since(&A.s[i].last, &now) > since(&s->last, &now)) s = &A.s[i];
        }
        s->used = true; s->owner = me; s->failed = false; s->have_prev = false;
    }
    s->last = now;
    if (s->q && (s->rate != rate || s->channels != channels)) close_queue(s);
    if (!s->q && !s->failed && !open_queue(s, rate, channels)) { close_queue(s); s->failed = true; }
    return s;
}

static void sleep_for(int frames, int rate)
{
    struct timespec ts = { 0, (long)((double)frames * 1e9 / rate) };
    nanosleep(&ts, NULL);
}

/* Copy into the caller's ring, blocking while it is full (that is the pacing). */
static void ring_write(const int16_t *samples, int frames, int channels, int rate)
{
    pthread_mutex_lock(&A.mu);
    stream *s = stream_for(rate, channels);
    if (s->failed || !s->q) { pthread_mutex_unlock(&A.mu); sleep_for(frames, rate); return; }
    int done = 0;
    while (done < frames) {
        while ((s->count >= RING_FRAMES || A.paused) && s->q) pthread_cond_wait(&A.cv, &A.mu);
        if (!s->q || !pthread_equal(s->owner, pthread_self())) break;      /* closed or taken over while waiting */
        size_t room = RING_FRAMES - s->count, n = (size_t)(frames - done) < room ? (size_t)(frames - done) : room;
        size_t tail = (s->head + s->count) % RING_FRAMES;
        for (size_t i = 0; i < n; i++)
            memcpy(&s->ring[((tail + i) % RING_FRAMES) * (size_t)channels], &samples[(size_t)(done + (int)i) * (size_t)channels], (size_t)channels * sizeof(int16_t));
        s->count += n;
        done += (int)n;
    }
    pthread_mutex_unlock(&A.mu);
}

/*
 * The mixer's write. A game mixing at a low rate (Unity's FMOD runs at 24 kHz) is played at twice that: each frame, then the
 * point halfway to the next. The iPhone played Subway Surfers' 24 kHz output high-pitched and broken while the same samples
 * were right on a Mac, and every game that sounds right there runs at 44.1 or 48 kHz -- so a queue is only ever opened
 * at one of those.
 */
static void host_write(const int16_t *samples, int frames, int channels, int rate)
{
    if (rate > 0 && rate * 2 <= 48000 && channels > 0 && channels <= 2 && frames > 0) {
        int16_t *up = malloc((size_t)frames * 2 * (size_t)channels * sizeof(int16_t));
        if (up) {
            /* The previous write's last frame, per writer. */
            pthread_mutex_lock(&A.mu);
            stream *s = stream_for(rate * 2, channels);
            int16_t prev[2] = { s->prev[0], s->prev[1] };
            bool have_prev = s->have_prev;
            for (int c = 0; c < channels; c++) s->prev[c] = samples[(size_t)(frames - 1) * channels + c];
            s->have_prev = true;
            pthread_mutex_unlock(&A.mu);
            for (int f = 0; f < frames; f++)
                for (int c = 0; c < channels; c++) {
                    int16_t cur = samples[(size_t)f * channels + c];
                    int16_t before = f > 0 ? samples[(size_t)(f - 1) * channels + c] : (have_prev ? prev[c] : cur);
                    up[(size_t)(2 * f) * channels + c] = (int16_t)(((int)before + cur) / 2);
                    up[(size_t)(2 * f + 1) * channels + c] = cur;
                }
            ring_write(up, frames * 2, channels, rate * 2);
            free(up);
            return;
        }
    }
    ring_write(samples, frames, channels, rate);
}

/* TL_AUDIO_MUTE: pace like a device but make no sound (for test runs on a Mac). TL_AUDIO_STATS=1 also logs, about once a second,
 * how loud the game's output was, so a silent run still shows whether the game is making sound. */
static void muted_write(const int16_t *samples, int frames, int channels, int rate)
{
    static int stats = -1, seen, peak;
    static long loud, total;
    if (stats < 0) stats = getenv("TL_AUDIO_STATS") != NULL;
    if (stats) {
        for (int i = 0; i < frames * channels; i++) {
            int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak) peak = v;
            if (v > 64) loud++;
        }
        total += frames * channels;
        if ((seen += frames) >= rate) {
            tl_log_line("audio: %d Hz x %d, peak %d, %.0f%% of samples audible", rate, channels, peak, 100.0 * loud / (total ? total : 1));
            seen = 0; peak = 0; loud = 0; total = 0;
        }
    }
    sleep_for(frames, rate);
}

void tl_audio_install(void) { tl_cocos_audio_hook = getenv("TL_AUDIO_MUTE") ? muted_write : host_write; }

void tl_audio_set_paused(bool paused)
{
    pthread_mutex_lock(&A.mu);
    A.paused = paused;
    for (int i = 0; i < MAXS; i++) {
        stream *s = &A.s[i];
        if (s->q) { if (paused) AudioQueuePause(s->q); else AudioQueueStart(s->q, NULL); }
    }
    pthread_cond_broadcast(&A.cv);
    pthread_mutex_unlock(&A.mu);
}
