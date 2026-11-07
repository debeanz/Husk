/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hades, in the Android port com.hades.port. libhades.so runs the game's iOS build (assets/Game.iOS.Netflix.arm64) and draws with Vulkan,
 * which here is MoltenVK. The game's data stays in the APK, which the library reads itself.
 *
 * The port's activity is Java of its own, which does not run here. What it does is written out from the DEX:
 *
 *   MainActivity.onCreate   nativeStart(apk, files dir, log, "", width, height, scale, refresh rate), and "unsupported" if that says no
 *   onResume, onPause       nativeLifecycle(paused)
 *   surfaceCreated          nativeSurface(surface)
 *   touch                   nativeTouch(action, finger, x, y, pressure): 0 down, 1 move, 2 up, 3 cancel
 *   Pads                    nativePadName(slot, name, family), nativePadState(slot, connected, buttons, 8 axes)
 *
 * The library's loader (macho_load) copies the iOS image out of the APK into anonymous memory, applies its fixups and its patches to the game's code, and
 * mprotects the result. Here that memory is the layout husk-tl-macho.c describes. Two more pieces of it are run and are not in the image: the stubs it makes
 * for names it does not have (0x18000 bytes, mapped on their own), and the patch code it builds, which it keeps in room after the image, in what here is the
 * writable view. That room's address is a field of its g_image, so the code goes to the pool below the image instead, within reach of the branches to it.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-hades.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-codewrite.h"
#include "husk-tl-gamepad.h"
#include "husk-tl-internal.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-macho.h"
#include "husk-tl-vulkan.h"
#include "husk-tl-xmem.h"

#define MA "com/hades/port/MainActivity"
#define PADS "com/hades/port/Pads"
#define HAPTICS "com/hades/port/Haptics"
#define REPORT "com/hades/port/Report"
#define IOS_BUILD "assets/Game.iOS.Netflix.arm64"
#define STICK_FLAT 0.08f                /* what InputDevice.MotionRange.getFlat says for a stick here (husk-tl-gamepad.c) */
#define DPAD_BITS 0x3C00
#define AXES 8
#define TRAP_STUBS_BYTES 0x18000        /* names_seal: what it mprotects */
#define IMAGE_PATCH_CODE 9              /* g_image, in 8-byte fields: where the patch code goes, and its size */
#define IMAGE_PATCH_BYTES 10

int tl_frame_hz(void);
const char *tl_hle_ext_files(void);
void tl_jni_hle_install(void);
void tl_hle_configure(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_hle_set_activity(jobj *a);

static struct {
    char apk[1024], data[512], pkg[128], files[600], binary[700], log[800];
    int width, height;
    jobj *activity;
    pthread_t ui;
    atomic_bool running, paused;
    void (*touch)(void *, void *, int, int, float, float, float);
    void (*lifecycle)(void *, void *, uint8_t);
    void (*pad_name)(void *, void *, int, void *, int);
    void (*pad_state)(void *, void *, int, uint8_t, int, void *);
} H;

static void *find_native(const char *cls, const char *name, const char *sig, const char *mangled)
{
    void *fn = tl_jni_native(cls, name, sig);
    if (!fn) {
        tl_lib *lib = tl_ld_find_lib("libhades.so");
        if (lib) fn = tl_ld_sym(lib, mangled);
    }
    if (!fn) tl_log_line("hades: libhades.so has no native %s%s", name, sig);
    return fn;
}

static void fail(const char *why)
{
    tl_log_line("hades: %s", why);
    if (tl_game_failed_hook) tl_game_failed_hook(why);
}

static void make_dirs(const char *path)
{
    char tmp[900];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* ------------------------------------------------------------- the port's Java that its library may call */

static void P_void(tl_jcall *c) { (void)c; }
static void P_zero(tl_jcall *c) { c->ret.j = 0; }
static void R_note(tl_jcall *c) { const char *s = c->args[0].l ? tl_jni_string(c->args[0].l) : NULL; tl_log_line("hades: %s", s ? s : ""); }

/* Pads.deviceOfSlot: which Android device a slot is, for its rumble. */
static void PD_deviceOfSlot(tl_jcall *c)
{
    int s = c->args[0].i;
    c->ret.j = 0;
    c->ret.i = tl_pad_connected(s) ? TL_PAD_DEVICE_BASE + s : -1;
}
static void PD_devicesOfSlot(tl_jcall *c)
{
    int s = c->args[0].i;
    bool have = tl_pad_connected(s);
    jobj *a = tl_jni_new_prim_array('I', have ? 1 : 0);
    if (have) ((int *)a->arr.data)[0] = TL_PAD_DEVICE_BASE + s;
    c->ret.j = 0;
    c->ret.l = a;
}

static const tl_jhle k_port[] = {
    { MA, "restart", "()V", P_void },
    { MA, "manageMods", "()V", P_void },
    { MA, "manageSaves", "()V", P_void },
    { PADS, "deviceOfSlot", "(I)I", PD_deviceOfSlot },
    { PADS, "devicesOfSlot", "(I)[I", PD_devicesOfSlot },
    { HAPTICS, "init", "(Landroid/content/Context;)V", P_void },
    { HAPTICS, "pause", "(Z)V", P_void },
    { HAPTICS, "phoneHasVibrator", "()Z", P_zero },
    { HAPTICS, "play", "(II[J[I)V", P_void },
    { HAPTICS, "stop", "(II)V", P_void },
    { REPORT, "note", "(Ljava/lang/String;)V", R_note },
    { REPORT, "flush", "()V", P_void },
    { REPORT, "bugReport", "(Landroid/app/Activity;)V", P_void },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------- controllers (Pads) */

static struct {
    pthread_mutex_t mu;
    bool told[TL_PADS];
    int buttons[TL_PADS];
    float axes[TL_PADS][AXES];
} P = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* Pads.bitOf. Bits 10 to 13 are the D-pad's, as keys or as a hat. */
static int button_bit(int keycode)
{
    switch (keycode) {
    case 96: return 0;                  /* A */
    case 97: return 1;                  /* B */
    case 99: return 2;                  /* X */
    case 100: return 3;                 /* Y */
    case 102: return 4;                 /* L1 */
    case 103: return 5;                 /* R1 */
    case 82: case 108: return 6;        /* Menu, Start */
    case 4: case 109: return 7;         /* Back, Select */
    case 106: return 8;                 /* left stick */
    case 107: return 9;                 /* right stick */
    case 19: return 10; case 20: return 11; case 21: return 12; case 22: return 13;     /* D-pad */
    case 3: case 110: return 14;        /* Home, Mode */
    default: return -1;
    }
}

static void send_state(int slot)
{
    if (!H.pad_state) return;
    jobj *a = tl_jni_new_prim_array('F', AXES);
    pthread_mutex_lock(&P.mu);
    memcpy(a->arr.data, P.axes[slot], sizeof(P.axes[slot]));
    int buttons = P.buttons[slot];
    pthread_mutex_unlock(&P.mu);
    H.pad_state(tl_jni_env(), tl_jni_class_object(PADS), slot, 1, buttons, a);
    tl_jni_unref(a);
    if (tl_jni_pending()) tl_jni_clear();
}

/* A controller is told about as it comes and goes, and shown as the Xbox controller it is shown as to every game (family 0: Xbox's button icons). */
static void pads_changed(void)
{
    if (!atomic_load(&H.running) || !H.pad_state || !H.pad_name) return;
    for (int s = 0; s < TL_PADS; s++) {
        bool now = tl_pad_connected(s);
        pthread_mutex_lock(&P.mu);
        bool was = P.told[s];
        P.told[s] = now;
        P.buttons[s] = 0;
        memset(P.axes[s], 0, sizeof(P.axes[s]));
        pthread_mutex_unlock(&P.mu);
        if (now == was) continue;
        void *env = tl_jni_env(), *cls = tl_jni_class_object(PADS);
        if (now) {
            H.pad_name(env, cls, s, tl_jni_new_string("Xbox Wireless Controller"), 0);
            send_state(s);
        } else {
            jobj *a = tl_jni_new_prim_array('F', AXES);
            H.pad_state(env, cls, s, 0, 0, a);
            tl_jni_unref(a);
        }
        if (tl_jni_pending()) tl_jni_clear();
    }
}

static void pad_key(jobj *ev, int device, int action, int keycode, int64_t down_ms, int64_t event_ms)
{
    (void)down_ms; (void)event_ms;
    tl_jni_unref(ev);
    int bit = button_bit(keycode), slot = device - TL_PAD_DEVICE_BASE;
    if (bit < 0 || slot < 0 || slot >= TL_PADS || !atomic_load(&H.running)) return;
    pthread_mutex_lock(&P.mu);
    if (action == 0) P.buttons[slot] |= 1 << bit; else if (action == 1) P.buttons[slot] &= ~(1 << bit);
    pthread_mutex_unlock(&P.mu);
    send_state(slot);
}

/* Pads.stick: a round dead zone, and what is left of the stick's travel made the whole of it. Up is positive. */
static void stick(float x, float y, float *out)
{
    y = -y;
    float len = hypotf(x, y);
    if (len <= STICK_FLAT || len <= 0) { out[0] = out[1] = 0; return; }
    float k = (len - STICK_FLAT) / (1.0f - STICK_FLAT) / len;
    out[0] = x * k; out[1] = y * k;
}

static void pad_motion(jobj *ev, int device, int source, int64_t down_ms, int64_t event_ms)
{
    (void)source; (void)down_ms; (void)event_ms;
    float a[48];
    bool ok = tl_input_event_axes(ev, a);
    tl_jni_unref(ev);
    int slot = device - TL_PAD_DEVICE_BASE;
    if (!ok || slot < 0 || slot >= TL_PADS || !atomic_load(&H.running)) return;
    pthread_mutex_lock(&P.mu);
    float *o = P.axes[slot];
    stick(a[0], a[1], o);               /* left stick: X, Y */
    stick(a[11], a[14], o + 2);         /* right stick: Z, RZ */
    o[4] = a[15]; o[5] = -a[16];        /* the hat, up positive */
    o[6] = fmaxf(a[17], a[23]);         /* L2: LTRIGGER, BRAKE */
    o[7] = fmaxf(a[18], a[22]);         /* R2: RTRIGGER, GAS */
    /* The hat is the D-pad's buttons too. */
    int b = P.buttons[slot] & ~DPAD_BITS;
    if (a[16] < -0.5f) b |= 1 << 10;
    if (a[16] > 0.5f) b |= 1 << 11;
    if (a[15] < -0.5f) b |= 1 << 12;
    if (a[15] > 0.5f) b |= 1 << 13;
    P.buttons[slot] = b;
    pthread_mutex_unlock(&P.mu);
    send_state(slot);
}

/* ------------------------------------------------------------- touch, and coming and going */

void tl_hades_touch(int phase, int id, float x, float y)
{
    if (!atomic_load(&H.running) || !H.touch) return;
    int action = phase == 0 ? 0 : phase == 1 ? 1 : phase == 2 ? 2 : 3;
    H.touch(tl_jni_env(), tl_jni_class_object(MA), action, id, x, y, 1.0f);
    if (tl_jni_pending()) tl_jni_clear();
}

void tl_hades_set_paused(bool paused)
{
    if (!atomic_load(&H.running) || !H.lifecycle) return;
    if (atomic_exchange(&H.paused, paused) == paused) return;
    tl_log_line("hades: %s", paused ? "pausing" : "resuming");
    H.lifecycle(tl_jni_env(), tl_jni_class_object(MA), paused);
    if (tl_jni_pending()) tl_jni_clear();
}

/* ------------------------------------------------------------- starting */

/*
 * names_seal is the last thing macho_load does before patches_apply, always. It flushes the trap stubs and makes them executable, which here they already are
 * (each store to them was carried out through the writable view, and flushed), so this takes its place and moves the patch code to where it can run.
 */
static uint64_t *g_image_fields;
static void before_patches(void)
{
    size_t room;
    void *pool = tl_macho_pool(&room);
    if (g_image_fields[IMAGE_PATCH_BYTES] > room) tl_log_line("hades: %llu bytes of patch code and %zu of room", (unsigned long long)g_image_fields[IMAGE_PATCH_BYTES], room);
    tl_log_line("hades: the game is loaded; its patch code goes at %p (it was to be at %p)", pool, (void *)(uintptr_t)g_image_fields[IMAGE_PATCH_CODE]);
    g_image_fields[IMAGE_PATCH_CODE] = (uint64_t)(uintptr_t)pool;
}

static bool hook_before_patches(void)
{
    tl_lib *lib = tl_ld_find_lib("libhades.so");
    uint32_t *seal = lib ? tl_ld_sym(lib, "names_seal") : NULL;
    g_image_fields = lib ? tl_ld_sym(lib, "g_image") : NULL;
    if (!seal || !g_image_fields) return false;
    uint32_t *rw = (uint32_t *)((uint8_t *)seal + tl_xmem_delta());
    rw[0] = 0x58000050u;                /* ldr x16, #8 */
    rw[1] = 0xD61F0200u;                /* br x16 */
    void (*target)(void) = before_patches;
    memcpy(rw + 2, &target, sizeof(target));
    tl_xmem_flush(seal, 16);
    return true;
}

static void *ui_main(void *arg)
{
    (void)arg;
    pthread_setname_np("UiThread");
    ((void *(*)(int))tl_bionic_find("ALooper_prepare"))(0);
    void *env = tl_jni_env(), *cls = tl_jni_class_object(MA);

    typedef uint8_t (*start_fn)(void *, void *, void *, void *, void *, void *, int, int, float, long);
    start_fn start = (start_fn)find_native(MA, "nativeStart", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IIFI)Z", "Java_com_hades_port_MainActivity_nativeStart");
    void (*surface)(void *, void *, void *) = find_native(MA, "nativeSurface", "(Landroid/view/Surface;)V", "Java_com_hades_port_MainActivity_nativeSurface");
    H.lifecycle = find_native(MA, "nativeLifecycle", "(Z)V", "Java_com_hades_port_MainActivity_nativeLifecycle");
    H.touch = find_native(MA, "nativeTouch", "(IIFFF)V", "Java_com_hades_port_MainActivity_nativeTouch");
    H.pad_name = find_native(PADS, "nativePadName", "(ILjava/lang/String;I)V", "Java_com_hades_port_Pads_nativePadName");
    H.pad_state = find_native(PADS, "nativePadState", "(IZI[F)V", "Java_com_hades_port_Pads_nativePadState");
    if (!start || !surface) { fail("This APK's libhades.so is not the one Husk knows how to start."); return NULL; }

    /* MainActivity.scaleFor: a point is about a 414th of the screen's height, from 2 to 4 pixels. */
    float scale = roundf((float)H.height / 414.0f);
    scale = fmaxf(2.0f, fminf(4.0f, scale));
    tl_log_line("hades: nativeStart, %dx%d at scale %.0f, %d Hz", H.width, H.height, scale, tl_frame_hz());
    uint8_t ok = start(env, cls, tl_jni_new_string(H.apk), tl_jni_new_string(H.files), tl_jni_new_string(H.log), tl_jni_new_string(""),
                       H.width, H.height, scale, (long)tl_frame_hz());
    if (tl_jni_pending()) { tl_log_line("hades: a Java exception is pending after nativeStart"); tl_jni_clear(); }
    /* The one reason its activity gives for this. */
    if (!ok) { fail("The port's library says this device does not support Vulkan 1.1. The log says more."); return NULL; }
    tl_log_line("hades: nativeStart returned");
    atomic_store(&H.running, true);

    /* onResume, then surfaceCreated */
    if (H.lifecycle) H.lifecycle(env, cls, 0);
    surface(env, cls, tl_jni_new_object(tl_jni_class("android/view/Surface")));
    if (tl_jni_pending()) { tl_log_line("hades: a Java exception is pending after nativeSurface"); tl_jni_clear(); }
    pads_changed();
    tl_log_line("hades: the surface is the game's");

    int (*poll_once)(int, int *, int *, void **) = tl_bionic_find("ALooper_pollOnce");
    for (;;) { poll_once(200, NULL, NULL, NULL); if (tl_jni_pending()) tl_jni_clear(); }
    return NULL;
}

/* The iOS build, for its layout (tl_macho_prepare). The loader in the library reads it for itself. */
static bool take_ios_build(void)
{
    tl_zip z;
    char err[200] = "";
    if (!tl_zip_open(&z, H.apk, err, sizeof(err))) { tl_log_line("hades: cannot read the APK: %s", err); return false; }
    const tl_zip_entry *e = tl_zip_find(&z, IOS_BUILD);
    bool ok = e != NULL;
    if (!e) tl_log_line("hades: the APK has no %s", IOS_BUILD);
    struct stat st;
    if (e && (stat(H.binary, &st) != 0 || (uint64_t)st.st_size != e->usize)) {
        ok = tl_zip_extract(&z, e, H.binary, err, sizeof(err));
        if (!ok) tl_log_line("hades: cannot copy out the game: %s", err);
    }
    tl_zip_close(&z);
    return ok;
}

bool tl_hades_start(const tl_ga_config *cfg)
{
    snprintf(H.apk, sizeof(H.apk), "%s", cfg->apk_path);
    snprintf(H.data, sizeof(H.data), "%s", cfg->data_dir);
    snprintf(H.pkg, sizeof(H.pkg), "%s", cfg->package_name);
    H.width = cfg->width > cfg->height ? cfg->width : cfg->height;
    H.height = cfg->width > cfg->height ? cfg->height : cfg->width;
    snprintf(H.files, sizeof(H.files), "%s/files", H.data);
    make_dirs(H.files);
    snprintf(H.binary, sizeof(H.binary), "%s/Game.iOS.Netflix.arm64", H.files);

    if (!tl_vk_available()) { fail("This game draws with Vulkan, and Husk could not load MoltenVK."); return false; }
    if (!take_ios_build()) { fail("This APK does not have the game's iOS build in it."); return false; }

    /* Every step of the loader's mapping goes to the log: a loader that does something new is seen there. */
    setenv("TL_MM_TRACE", "1", 1);
    char err[300] = "";
    tl_macho_any_loader();
    tl_macho_exec_mapping(TRAP_STUBS_BYTES);
    if (!tl_macho_prepare(H.binary, err, sizeof(err))) { fail(err); return false; }
    tl_codewrite_enable();
    tl_keep_stdio = true;

    tl_jni_init();
    tl_hle_configure(H.pkg, H.apk, H.data, H.width, H.height);
    tl_jni_hle_install();
    tl_jni_declare(MA, "android/app/Activity");
    tl_jni_register_hle(k_port);
    H.activity = tl_jni_new_object(tl_jni_class(MA));
    tl_hle_set_activity(H.activity);

    char runs[800];
    snprintf(runs, sizeof(runs), "%s/runs", tl_hle_ext_files());
    make_dirs(runs);
    snprintf(H.log, sizeof(H.log), "%.780s/husk.log", runs);
    unlink(H.log);

    /* MainActivity's static initialiser */
    jvalue a; a.j = 0; a.l = tl_jni_new_string("hades");
    tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
    if (tl_jni_pending()) { tl_jni_clear(); fail("libhades.so did not load. The log says why."); return false; }
    if (!hook_before_patches()) { fail("This APK's libhades.so is not the one Husk knows how to start."); return false; }
    tl_log_line("hades: libhades.so loaded");

    static const tl_pad_sink sink = { pad_key, pad_motion };
    tl_pad_set_sink(&sink);
    tl_pad_set_change_hook(pads_changed);
    return true;
}

bool tl_hades_run(void)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    return pthread_create(&H.ui, &a, ui_main, NULL) == 0;
}
