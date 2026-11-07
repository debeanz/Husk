/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Closing Shift's Android port (com.chillasart.theclosingshift.port) has Java classes of its own, which its C# calls by name and which
 * do not run here. What they do is written out from the DEX:
 *
 *   PortStorage     the folder saves go in: getExternalFilesDir(null), which the activity prepares before Unity starts
 *   PortConfig      a first run's GameConfig.cfg, copied out of the APK (without one the game has no key bindings: "no Input section")
 *   PortDisplay     the frame rates it may offer, and asking the screen for one
 *   ControllerBridge the controller. The activity gives controller events to this class and not to Unity, and the C# polls it each frame
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-gamepad.h"
#include "husk-tl-internal.h"
#include "husk-tl-jni.h"
#include "husk-tl-unity.h"

#define PORT "com/chillasart/theclosingshift/port/"
#define MASK_20 0xFFFFF                 /* poll() keeps its counters to 20 bits: all of them survive being a float */
#define STICK_FLAT 0.08f                /* what InputDevice.MotionRange.getFlat says for a stick here (husk-tl-gamepad.c) */

const char *tl_hle_ext_files(void);
int tl_frame_hz(void);          /* the frame rate games are held to, 60 or 120 (husk-tl-egl.c) */

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }

/* ------------------------------------------------------------- PortStorage, PortConfig */

static void PS_saveRoot(tl_jcall *c) { c->ret = vl(tl_jni_new_string(tl_hle_ext_files())); }

/* PortConfig.seed: each of the two folders the game reads its configuration from gets the APK's copy, unless it has one. */
static void seed_config(const char *apk)
{
    static const char *const folders[] = { "Data", "TheClosingShift" };
    tl_zip z;
    char err[200] = "";
    bool opened = false;
    const tl_zip_entry *e = NULL;
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        char dir[900], path[1000];
        struct stat st;
        snprintf(dir, sizeof(dir), "%s/%s", tl_hle_ext_files(), folders[i]);
        mkdir(dir, 0755);
        snprintf(path, sizeof(path), "%s/GameConfig.cfg", dir);
        if (stat(path, &st) == 0) continue;
        if (!opened) {
            if (!tl_zip_open(&z, apk, err, sizeof(err))) { tl_log_line("closing shift: could not read the APK for its configuration: %s", err); return; }
            opened = true;
            e = tl_zip_find(&z, "assets/port-defaults/GameConfig.cfg");
        }
        if (e && tl_zip_extract(&z, e, path, err, sizeof(err))) tl_log_line("closing shift: %s/GameConfig.cfg made from the APK's copy", folders[i]);
        else tl_log_line("closing shift: no GameConfig.cfg for %s: %s", folders[i], e ? err : "the APK has none");
    }
    if (opened) tl_zip_close(&z);
}

/* ------------------------------------------------------------- PortDisplay */

/* PortDisplay.rates: 30, then each refresh rate of the screen's modes and its half if that is 30 or more, in order, and a 0 at the end. The screen here
 * has the one rate games are held to, which the game's "Display" frame rate setting goes by. */
static void PD_frameRates(tl_jcall *c)
{
    static const int at_60[] = { 30, 60, 0 }, at_120[] = { 30, 60, 120, 0 };
    bool fast = tl_frame_hz() > 60;
    uint32_t n = fast ? 4 : 3;
    jobj *a = tl_jni_new_prim_array('I', n);
    memcpy(a->arr.data, fast ? at_120 : at_60, n * sizeof(int));
    c->ret = vl(a);
}
/* PortDisplay.setFrameRate asks Android for the screen mode nearest the game's cap. Here the screen's rate is Husk's setting, and the cap is the game's own to keep. */
static void PD_setFrameRate(tl_jcall *c) { tl_log_line("closing shift: the game caps itself at %d frames a second (0: the screen's rate, %d)", c->args[0].i, tl_frame_hz()); }
static void P_void(tl_jcall *c) { (void)c; }

/* ------------------------------------------------------------- ControllerBridge */

/* The bits of `buttons` and `presses`. Bits 11 to 14 are the D-pad's as keys and as a hat alike, 15 and 16 the triggers'. */
static int button_bit(int keycode)
{
    switch (keycode) {
    case 96: return 0;          /* A */
    case 97: case 4: return 1;  /* B, and Back */
    case 99: return 2;          /* X */
    case 100: return 3;         /* Y */
    case 102: return 4;         /* L1 */
    case 103: return 5;         /* R1 */
    case 106: return 6;         /* left stick */
    case 107: return 7;         /* right stick */
    case 108: return 8;         /* Start */
    case 109: return 9;         /* Select */
    case 19: return 11; case 20: return 12; case 21: return 13; case 22: return 14;     /* D-pad */
    case 104: return 15; case 105: return 16;                                           /* L2, R2 */
    default: return -1;
    }
}

static struct {
    pthread_mutex_t mu;
    int sequence, device, buttons, backs, generation, presses;
    float lx, ly, rx, ry, hx, hy, lt, rt, move_flat, look_flat;
    bool lt_held, rt_held;
} B = { .mu = PTHREAD_MUTEX_INITIALIZER, .device = -1 };

static void clear_state(void)
{
    B.presses = B.buttons = 0;
    B.lx = B.ly = B.rx = B.ry = B.hx = B.hy = B.lt = B.rt = 0;
    B.lt_held = B.rt_held = false;
}

static void select_device(int id)
{
    if (id == B.device) return;
    clear_state();
    B.device = id;
    B.generation++;
    B.move_flat = B.look_flat = STICK_FLAT;
    tl_log_line("closing shift: controller %d is the game's", id);
}

static int hat_bits(float x, float y)
{
    return (y < -0.5f ? 1 << 11 : 0) | (y > 0.5f ? 1 << 12 : 0) | (x < -0.5f ? 1 << 13 : 0) | (x > 0.5f ? 1 << 14 : 0);
}

static void sample_hat(float x, float y)
{
    B.presses |= hat_bits(x, y) & ~hat_bits(B.hx, B.hy);
    B.hx = x; B.hy = y;
}

static void sample_triggers(float lt, float rt)
{
    bool l = lt >= (B.lt_held ? 0.35f : 0.55f), r = rt >= (B.rt_held ? 0.35f : 0.55f);
    if (l && !B.lt_held) B.presses |= 1 << 15;
    if (r && !B.rt_held) B.presses |= 1 << 16;
    B.lt_held = l; B.rt_held = r;
}

static void bridge_key(jobj *ev, int device, int action, int keycode, int64_t down_ms, int64_t event_ms)
{
    (void)down_ms; (void)event_ms;
    tl_jni_unref(ev);
    int bit = button_bit(keycode);
    if (bit < 0) return;
    pthread_mutex_lock(&B.mu);
    if (action == 0) select_device(device);
    if (device == B.device) {
        int m = 1 << bit;
        if (action == 0) { if (!(B.buttons & m)) B.presses |= m; B.buttons |= m; }
        else if (action == 1) B.buttons &= ~m;
        B.sequence++;
    }
    pthread_mutex_unlock(&B.mu);
}

static void bridge_motion(jobj *ev, int device, int source, int64_t down_ms, int64_t event_ms)
{
    (void)source; (void)down_ms; (void)event_ms;
    float a[48];
    bool ok = tl_input_event_axes(ev, a);
    tl_jni_unref(ev);
    if (!ok) return;
    float lt = fmaxf(a[17], a[23]), rt = fmaxf(a[18], a[22]);
    pthread_mutex_lock(&B.mu);
    if (B.device != device) {
        float moved = fabsf(a[0]) + fabsf(a[1]) + fabsf(a[11]) + fabsf(a[14]) + fabsf(a[15]) + fabsf(a[16]) + lt + rt;
        if (B.device < 0 || moved > 0.25f) select_device(device);
    }
    if (B.device == device) {
        B.lx = a[0]; B.ly = a[1]; B.rx = a[11]; B.ry = a[14];
        sample_hat(a[15], a[16]);
        B.lt = lt; B.rt = rt;
        sample_triggers(lt, rt);
        B.sequence++;
    }
    pthread_mutex_unlock(&B.mu);
}

/* A controller coming or going reaches the Java through a listener: here the frame's poll looks. */
static void follow_connections(void)
{
    if (B.device >= 0 && !tl_pad_connected(B.device - TL_PAD_DEVICE_BASE)) {
        clear_state();
        B.device = -1;
        B.generation++;
        tl_log_line("closing shift: the game's controller is gone");
    }
    if (B.device < 0) for (int s = 0; s < TL_PADS; s++) if (tl_pad_connected(s)) { select_device(TL_PAD_DEVICE_BASE + s); break; }
}

/* The snapshot's layout is the C#'s to check: 17 floats, in this order. */
static void CB_poll(tl_jcall *c)
{
    jobj *a = tl_jni_new_prim_array('F', 17);
    float *f = (float *)a->arr.data;
    pthread_mutex_lock(&B.mu);
    follow_connections();
    f[0] = (float)(B.sequence & MASK_20);
    f[1] = (float)B.device;
    f[2] = B.device >= 0 ? 1.0f : 0.0f;
    f[3] = B.lx; f[4] = B.ly; f[5] = B.rx; f[6] = B.ry;
    f[7] = B.hx; f[8] = B.hy;
    f[9] = B.lt; f[10] = B.rt;
    f[11] = (float)B.buttons;
    f[12] = (float)B.backs;
    f[13] = (float)(B.generation & MASK_20);
    f[14] = B.move_flat; f[15] = B.look_flat;
    f[16] = (float)B.presses;
    B.presses = 0;
    pthread_mutex_unlock(&B.mu);
    c->ret = vl(a);
}
static void CB_cancel(tl_jcall *c) { (void)c; pthread_mutex_lock(&B.mu); B.presses = 0; B.generation++; pthread_mutex_unlock(&B.mu); }
static void CB_back(tl_jcall *c) { (void)c; pthread_mutex_lock(&B.mu); B.backs = (B.backs + 1) & MASK_20; pthread_mutex_unlock(&B.mu); }

/* ------------------------------------------------------------- */

static const tl_jhle k_port[] = {
    { PORT "PortStorage", "saveRoot", "()Ljava/lang/String;", PS_saveRoot },
    { PORT "PortDisplay", "frameRates", "()[I", PD_frameRates },
    { PORT "PortDisplay", "setFrameRate", "(I)V", PD_setFrameRate },
    { PORT "PortDisplay", "haptic", "()V", P_void },
    { PORT "ControllerBridge", "poll", "()[F", CB_poll },
    { PORT "ControllerBridge", "cancel", "()V", CB_cancel },
    { PORT "ControllerBridge", "back", "()V", CB_back },
    { NULL, NULL, NULL, NULL }
};

void tl_closing_shift_install(const char *apk)
{
    if (!tl_dexidx_has_class(PORT "PortStorage")) return;
    tl_log_line("closing shift: its port's Java classes are provided here");
    seed_config(apk);
    tl_jni_register_hle(k_port);
    static const tl_pad_sink sink = { bridge_key, bridge_motion };
    tl_unity_set_pad_bridge(&sink);
}
