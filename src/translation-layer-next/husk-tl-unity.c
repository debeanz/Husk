/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-unity.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-egl.h"
#include "husk-tl-gamepad.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"

void tl_jni_hle_install(void);
void tl_hle_configure(const char *pkg, const char *apk, const char *data, int w, int h);
jobj *tl_hle_activity(void);
void tl_set_data_dir(const char *dir);
void tl_nwindow_configure(int w, int h, void *layer);

static struct {
    tl_unity_config cfg;
    jobj *activity;          /* the Context handed to the engine */
    jobj *player;            /* the UnityPlayer object natives are called on */
    const char *player_class; /* the class its natives were registered on: UnityPlayer, or a subclass in newer Unity */
    bool ui_looper;          /* the starting thread has Android's UI-thread looper and goes on running it */
    atomic_ulong frames;
    atomic_bool stop;
    atomic_bool paused;
    atomic_ullong perf_ns, perf_max_ns;     /* time inside nativeRender since the last snapshot, and the worst call */
    atomic_ulong perf_frames;
    struct timespec perf_since;
    pthread_t thread;
    bool thread_started;
} U;

typedef int32_t (*onload_fn)(void *vm, void *reserved);

void *tl_looper_prepare_here(void);

/* Whether a line about this native has been said already: a lookup made on every touch must not log on every touch. */
static bool said_once(const char *name)
{
    static char said[32][48]; static int nsaid; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    bool seen = false;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nsaid && !seen; i++) seen = !strcmp(said[i], name);
    if (!seen && nsaid < 32) snprintf(said[nsaid++], sizeof(said[0]), "%s", name);
    pthread_mutex_unlock(&mu);
    return seen;
}

static void *native_of(const char *cls, const char *name, const char *sig)
{
    void *fn = tl_jni_native(cls, name, sig);
    if (!fn) {
        /* Newer engines can register a method on another class than the one an older Unity used: take it from there. */
        const char *owner = tl_jni_native_owner(name, sig);
        if (owner) {
            fn = tl_jni_native(owner, name, sig);
            if (!said_once(name)) tl_log_line("unity: native %s%s is registered on %s, not %s; using it", name, sig, owner, cls);
        }
    }
    if (!fn) {
        /* ...or with other parameters (Unity 6's initJni). It is called with the arguments this driver has; one it
         * does not have arrives as null or zero. */
        const char *owner = tl_jni_native(cls, name, NULL) ? cls : tl_jni_native_owner(name, NULL);
        if (owner) {
            fn = tl_jni_native(owner, name, NULL);
            if (!said_once(name))
                tl_log_line("unity: native %s is registered on %s as %s, not %s; calling it with what this driver has",
                            name, owner, tl_jni_native_sig(owner, name), sig);
        }
    }
    if (!fn) tl_log_line("unity: native %s.%s%s was not registered", cls, name, sig);
    return fn;
}

/* Unity 2023 and later: the player is split into subclasses (for an Activity or a Service, and for GameActivity), and the
 * engine expects the UI thread to have a native looper ("Couldn't retrieve native ALooper for UI thread"). */
static bool newer_unity(void)
{
    static const char *const marks[] = {
        "com/unity3d/player/UnityPlayerForActivityOrService",
        "com/unity3d/player/UnityPlayerForGameActivity",
        "com/unity3d/player/UnityPlayerGameActivity",
    };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (tl_dexidx_has_class(marks[i])) { tl_log_line("unity: %s is in the APK: a Unity of 2023 or later", marks[i]); return true; }
    return false;
}

bool tl_unity_ui_looper(void) { return U.ui_looper; }

static void kb_install(void);

bool tl_unity_start(const tl_unity_config *cfg)
{
    U.cfg = *cfg;
    tl_set_data_dir(cfg->data_dir);
    tl_nwindow_configure(cfg->width, cfg->height, cfg->metal_layer);
    int n = tl_dexidx_open(cfg->apk_path);
    tl_log_line("unity: %d classes in the APK's DEX", n);
    U.player_class = "com/unity3d/player/UnityPlayer";
    /* This thread is the one Android would load the engine on: the UI thread. A newer Unity looks for that thread's
     * looper as its libraries load, so it gets one first, and the thread goes on running it (husk-tl-unity-app.c). */
    if (newer_unity()) {
        tl_looper_prepare_here();
        U.ui_looper = true;
        tl_log_line("unity: the starting thread is the UI thread, with a looper");
    }
    if (!tl_ld_add_apk(cfg->apk_path)) return false;
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every)) return false;
    tl_jni_init();
    tl_hle_configure(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_jni_hle_install();
    kb_install();

    /* new UnityPlayer(activity): the Java side's own constructor calls loadNative(dir),
     * which is System.load(dir + "/libmain.so") followed by NativeLoader.load(dir). */
    U.activity = tl_hle_activity();
    U.player = tl_jni_new_object(tl_jni_class("com/unity3d/player/UnityPlayer"));

    tl_lib *main_lib = tl_ld_load("libmain.so");
    if (!main_lib) return false;
    tl_ld_init(main_lib);
    onload_fn onload = (onload_fn)tl_ld_sym(main_lib, "JNI_OnLoad");
    if (!onload) { tl_log_line("unity: libmain.so has no JNI_OnLoad"); return false; }
    int32_t ver = onload(tl_jni_vm(), NULL);
    tl_log_line("unity: libmain JNI_OnLoad -> %#x", ver);
    if (tl_jni_pending()) { tl_log_line("unity: an exception is pending after libmain's JNI_OnLoad"); return false; }

    typedef uint8_t (*load_fn)(void *env, void *cls, void *dir);
    load_fn load = (load_fn)native_of("com/unity3d/player/NativeLoader", "load", "(Ljava/lang/String;)Z");
    if (!load) return false;
    jobj *dir = tl_jni_new_string("/data/app/lib/arm64");
    uint8_t ok = load(tl_jni_env(), tl_jni_class_object("com/unity3d/player/NativeLoader"), dir);
    tl_log_line("unity: NativeLoader.load -> %d", ok);
    /* Where the engine put the player's natives: UnityPlayer in older Unity, a subclass of it in newer ones. The player
     * object the natives are called on is made of that class, as Android would have made it. */
    if (ok && !tl_jni_native(U.player_class, "nativeRender", "()Z")) {
        const char *owner = tl_jni_native_owner("nativeRender", "()Z");
        if (owner) {
            tl_log_line("unity: the player's natives are on %s", owner);
            U.player_class = owner;
            U.player = tl_jni_new_object(tl_jni_class(owner));
        } else {
            tl_log_line("unity: no class has nativeRender registered: this Unity drives its frames some other way");
        }
    }
    return ok != 0;
}

/*
 * Call a registered UnityPlayer native: (env, player, a, b). Fixed arity on purpose: a
 * variadic pointer type would put the arguments on the stack, where guest code (compiled
 * for AAPCS64) does not look for them. Four more zeros fill the rest of the argument
 * registers, so a native that takes more parameters than the driver knows of (a newer
 * Unity's) finds null and zero there rather than whatever the registers last held.
 */
typedef void (*native_call_fn)(void *env, void *self, uintptr_t a, uintptr_t b,
                               uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f);
static void call_native_on(const char *cls, jobj *self, const char *name, const char *sig, uintptr_t a, uintptr_t b)
{
    void *fn = native_of(cls, name, sig);
    if (fn) ((native_call_fn)fn)(tl_jni_env(), self, a, b, 0, 0, 0, 0);
}
static void call_native(const char *name, const char *sig, uintptr_t a, uintptr_t b)
{
    call_native_on(U.player_class, U.player, name, sig, a, b);
}
#define NATIVE_VOID(name, sig, a, b) call_native(name, sig, (uintptr_t)(a), (uintptr_t)(b))

static void kb_drain(void);

static void *unity_main(void *arg)
{
    (void)arg;
    pthread_setname_np("UnityMain");
    tl_log_line("unity: UnityMain thread started");

    /* The jobs UnityPlayer queues for this thread, in the order it queues them. */
    jobj *surface = tl_jni_new_object(tl_jni_class("android/view/Surface"));
    NATIVE_VOID("nativeRecreateGfxState", "(ILandroid/view/Surface;)V", 0, surface);
    tl_log_line("unity: nativeRecreateGfxState done");
    NATIVE_VOID("nativeSendSurfaceChangedEvent", "()V", 0, 0);
    NATIVE_VOID("nativeResume", "()V", 0, 0);
    tl_log_line("unity: nativeResume done");
    NATIVE_VOID("nativeFocusChanged", "(Z)V", 1, 0);
    /* What Unity's Java side tells the engine as it starts on a phone: the media volume (AudioVolumeHandler reports it, and
     * an engine that never hears it takes the device as muted) and that its sound is not muted. */
    {
        jobj *volume = tl_jni_new_object(tl_jni_class("com/unity3d/player/AudioVolumeHandler"));
        call_native_on("com/unity3d/player/AudioVolumeHandler", volume, "onAudioVolumeChanged", "(I)V", 7, 0);
        NATIVE_VOID("nativeMuteMasterAudio", "(Z)V", 0, 0);
        tl_log_line("unity: told the engine the volume (7 of 15) and that sound is on");
    }

    void *render = native_of(U.player_class, "nativeRender", "()Z");
    bool was_paused = false;
    while (render && !atomic_load(&U.stop)) {
        if (atomic_load(&U.paused)) {
            if (!was_paused) { NATIVE_VOID("nativeFocusChanged", "(Z)V", 0, 0); NATIVE_VOID("nativePause", "()Z", 0, 0); was_paused = true; tl_log_line("unity: paused"); }
            usleep(20000);
            continue;
        }
        if (was_paused) { NATIVE_VOID("nativeResume", "()V", 0, 0); NATIVE_VOID("nativeFocusChanged", "(Z)V", 1, 0); was_paused = false; tl_log_line("unity: resumed"); }
        kb_drain();
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(tl_jni_env(), U.player);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        {
            unsigned long long ns = (unsigned long long)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
            atomic_fetch_add(&U.perf_ns, ns);
            atomic_fetch_add(&U.perf_frames, 1);
            unsigned long long m = atomic_load(&U.perf_max_ns);
            while (ns > m && !atomic_compare_exchange_weak(&U.perf_max_ns, &m, ns)) {}
        }
        atomic_fetch_add(&U.frames, 1);
        if (!keep) { tl_log_line("unity: nativeRender returned false: the engine asked to quit"); break; }
        usleep(2000);
    }
    return NULL;
}

bool tl_unity_run(void)
{
    /* The UnityPlayer constructor's last native step before it starts the thread. */
    /* initJni(Context) in older Unity. Unity 6 has initJni(Context, int, String): which kind of player this is (0, for
     * an Activity or a Service -- "Context Type: ActivityOrService" in its log) and the name of the player's class, which
     * it then looks up with Class.forName. */
    const char *init_sig = tl_jni_native_sig("com/unity3d/player/UnityPlayer", "initJni");
    if (init_sig && !strcmp(init_sig, "(Landroid/content/Context;ILjava/lang/String;)V")) {
        char dotted[200];
        snprintf(dotted, sizeof(dotted), "%s", U.player_class);
        for (char *p = dotted; *p; p++) if (*p == '/') *p = '.';
        void *fn = tl_jni_native("com/unity3d/player/UnityPlayer", "initJni", init_sig);
        tl_log_line("unity: initJni(context, 0, \"%s\")", dotted);
        ((native_call_fn)fn)(tl_jni_env(), U.player, (uintptr_t)U.activity, 0, (uintptr_t)tl_jni_new_string(dotted), 0, 0, 0);
    } else {
        NATIVE_VOID("initJni", "(Landroid/content/Context;)V", U.activity, 0);
    }
    tl_log_line("unity: initJni done");
    /* The rest of the UnityPlayer constructor: the helpers it builds, whose constructors each call a
     * native that gives the engine its reference to the helper's Java class. Without these the engine
     * holds NULL where it expects a class and fails later, far from the cause. */
    /* ...and the web request helper's class, which UnityPlayer's constructor hands the engine so it can find the request methods. */
    NATIVE_VOID("nativeInitWebRequest", "(Ljava/lang/Class;)V", tl_jni_class_object("com/unity3d/player/UnityWebRequest"), 0);
    tl_log_line("unity: nativeInitWebRequest done");
    jobj *cam = tl_jni_new_object(tl_jni_class("com/unity3d/player/Camera2Wrapper"));
    call_native_on("com/unity3d/player/Camera2Wrapper", cam, "initCamera2Jni", "()V", 0, 0);
    jobj *hfp = tl_jni_new_object(tl_jni_class("com/unity3d/player/HFPStatus"));
    call_native_on("com/unity3d/player/HFPStatus", hfp, "initHFPStatusJni", "()V", 0, 0);
    jobj *olock = tl_jni_new_object(tl_jni_class("com/unity3d/player/OrientationLockListener"));
    call_native_on("com/unity3d/player/OrientationLockListener", olock, "nativeUpdateOrientationLockState", "(I)V", 1, 0);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&U.thread, &a, unity_main, NULL) != 0) return false;
    U.thread_started = true;
    return true;
}

/* ----------------------------------------------------------------- touch */

extern jobj *tl_input_motion_event(int action, int count, const int *ids, const float *xs, const float *ys, int64_t down_ms, int64_t event_ms);

static struct {
    pthread_mutex_t lock;
    int n, ids[10];
    float x[10], y[10];
    int64_t down_ms;
} T = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int64_t uptime_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

void tl_unity_touch(int phase, int id, float x, float y)
{
    typedef uint8_t (*inject_fn)(void *env, void *self, void *event, uintptr_t source);
    inject_fn fn = (inject_fn)native_of(U.player_class, "nativeInjectEvent", "(Landroid/view/InputEvent;I)Z");
    if (!fn) fn = (inject_fn)native_of(U.player_class, "nativeInjectEvent", "(Landroid/view/InputEvent;)Z");
    if (!fn) return;
    pthread_mutex_lock(&T.lock);
    int idx = -1;
    for (int i = 0; i < T.n; i++) if (T.ids[i] == id) idx = i;
    int64_t now = uptime_ms();
    int action;
    if (phase == 0) {
        if (idx < 0 && T.n < 10) { idx = T.n++; T.ids[idx] = id; }
        if (idx < 0) { pthread_mutex_unlock(&T.lock); return; }
        T.x[idx] = x; T.y[idx] = y;
        if (T.n == 1) T.down_ms = now;
        action = T.n == 1 ? 0 /* ACTION_DOWN */ : (5 /* ACTION_POINTER_DOWN */ | (idx << 8));
    } else if (phase == 1) {
        if (idx < 0) { pthread_mutex_unlock(&T.lock); return; }
        T.x[idx] = x; T.y[idx] = y;
        action = 2; /* ACTION_MOVE */
    } else if (phase == 3) {
        action = 3; /* ACTION_CANCEL */
    } else {
        if (idx < 0) { pthread_mutex_unlock(&T.lock); return; }
        T.x[idx] = x; T.y[idx] = y;
        action = T.n == 1 ? 1 /* ACTION_UP */ : (6 /* ACTION_POINTER_UP */ | (idx << 8));
    }
    jobj *ev = tl_input_motion_event(action, T.n, T.ids, T.x, T.y, T.down_ms, now);
    /* An ended touch leaves the set after the event that reports it. */
    if (phase == 2 && idx >= 0) {
        float lx[10], ly[10]; int li[10];
        memcpy(lx, T.x, sizeof(lx)); memcpy(ly, T.y, sizeof(ly)); memcpy(li, T.ids, sizeof(li));
        int n = 0;
        for (int i = 0; i < T.n; i++) if (i != idx) { T.ids[n] = li[i]; T.x[n] = lx[i]; T.y[n] = ly[i]; n++; }
        T.n = n;
    } else if (phase == 3) {
        T.n = 0;
    }
    pthread_mutex_unlock(&T.lock);
    fn(tl_jni_env(), U.player, ev, 0);
    if (tl_jni_pending()) tl_jni_clear();
    tl_jni_unref(ev);
}

/* ------------------------------------------------------------ soft keyboard */

/*
 * TouchScreenKeyboard. A tapped text field has the engine call UnityPlayer.showSoftInput(text, type, autocorrection, multiline,
 * secure, alert, placeholder[, character limit[, hide the input field[, ...]]]) -- the arguments grew with Unity's versions -- and
 * on Android a dialog with an edit box comes up. The dialog reports back on the engine's own thread: the text as it changes
 * (nativeSetInputString), then Done (nativeSoftInputClosed) or Back (nativeSoftInputCanceled, then closed), with
 * nativeSetKeyboardIsVisible around it. Here the app's keyboard is the dialog: it is told what to show, and its text comes back
 * through a queue UnityMain empties before each frame.
 */
enum { KJ_TEXT, KJ_DONE, KJ_CANCEL, KJ_VISIBLE };
typedef struct kjob { int kind; char *text; bool flag; struct kjob *next; } kjob;
static struct {
    pthread_mutex_t mu;
    kjob *head, *tail;
    tl_unity_keyboard_fn hook;
    int limit;
    bool shown;
} KB = { .mu = PTHREAD_MUTEX_INITIALIZER };

void tl_unity_set_keyboard_handler(tl_unity_keyboard_fn fn) { KB.hook = fn; }

static void kb_queue(int kind, const char *text, bool flag)
{
    kjob *j = calloc(1, sizeof(*j));
    if (!j) return;
    j->kind = kind; j->text = text ? strdup(text) : NULL; j->flag = flag;
    pthread_mutex_lock(&KB.mu);
    if (KB.tail) KB.tail->next = j; else KB.head = j;
    KB.tail = j;
    pthread_mutex_unlock(&KB.mu);
}

/* On UnityMain, before a frame: what the keyboard did since the last one, in order. */
static void kb_drain(void)
{
    pthread_mutex_lock(&KB.mu);
    kjob *j = KB.head;
    KB.head = KB.tail = NULL;
    pthread_mutex_unlock(&KB.mu);
    while (j) {
        kjob *next = j->next;
        switch (j->kind) {
        case KJ_TEXT: NATIVE_VOID("nativeSetInputString", "(Ljava/lang/String;)V", tl_jni_new_string(j->text ? j->text : ""), 0); break;
        case KJ_DONE: NATIVE_VOID("nativeSoftInputClosed", "()V", 0, 0); break;
        case KJ_CANCEL: NATIVE_VOID("nativeSoftInputCanceled", "()V", 0, 0); NATIVE_VOID("nativeSoftInputClosed", "()V", 0, 0); break;
        case KJ_VISIBLE: NATIVE_VOID("nativeSetKeyboardIsVisible", "(Z)V", j->flag, 0); break;
        }
        if (tl_jni_pending()) tl_jni_clear();
        free(j->text);
        free(j);
        j = next;
    }
}

void tl_unity_keyboard_text(const char *utf8) { kb_queue(KJ_TEXT, utf8 ? utf8 : "", false); }
void tl_unity_keyboard_done(bool cancelled)
{
    KB.shown = false;
    kb_queue(cancelled ? KJ_CANCEL : KJ_DONE, NULL, false);
    kb_queue(KJ_VISIBLE, NULL, false);
}

static void kb_show(tl_jcall *c, int nargs)
{
    const char *text = tl_jni_string(c->args[0].l), *placeholder = nargs > 6 ? tl_jni_string(c->args[6].l) : NULL;
    int type = c->args[1].i, limit = nargs > 7 ? c->args[7].i : 0;
    bool multiline = c->args[3].z, secure = c->args[4].z;
    KB.limit = limit > 0 ? limit : 0;
    KB.shown = true;
    tl_log_line("unity: the game asked for the keyboard (type %d%s%s, limit %d)", type, multiline ? ", multi-line" : "",
                secure ? ", secure" : "", KB.limit);
    kb_queue(KJ_VISIBLE, NULL, true);
    if (KB.hook) KB.hook(1, text ? text : "", placeholder ? placeholder : "", type, KB.limit, (secure ? 1 : 0) | (multiline ? 2 : 0));
    else tl_log_line("unity: no keyboard to show");
}
static void kb_show7(tl_jcall *c) { kb_show(c, 7); }
static void kb_show8(tl_jcall *c) { kb_show(c, 8); }
static void kb_show9(tl_jcall *c) { kb_show(c, 9); }
static void kb_show10(tl_jcall *c) { kb_show(c, 10); }
static void kb_hide(tl_jcall *c)
{
    (void)c;
    if (!KB.shown) return;                       /* Unity hides a keyboard that was never shown as it starts */
    KB.shown = false;
    kb_queue(KJ_VISIBLE, NULL, false);
    if (KB.hook) KB.hook(2, NULL, NULL, 0, 0, 0);
}
static void kb_set_text(tl_jcall *c) { const char *t = tl_jni_string(c->args[0].l); if (KB.hook) KB.hook(3, t ? t : "", NULL, 0, KB.limit, 0); }
static void kb_set_limit(tl_jcall *c) { KB.limit = c->args[0].i > 0 ? c->args[0].i : 0; if (KB.hook) KB.hook(4, NULL, NULL, 0, KB.limit, 0); }
static void kb_noop(tl_jcall *c) { (void)c; }

static void kb_install(void)
{
    #define UP "com/unity3d/player/UnityPlayer"
    static const tl_jhle hle[] = {
        { UP, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V", kb_show7 },
        { UP, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;I)V", kb_show8 },
        { UP, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZ)V", kb_show9 },        /* 2020 */
        { UP, "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V", kb_show10 },
        { UP, "hideSoftInput", "()V", kb_hide },
        { UP, "setSoftInputStr", "(Ljava/lang/String;)V", kb_set_text },
        { UP, "setCharacterLimit", "(I)V", kb_set_limit },
        { UP, "setHideInputField", "(Z)V", kb_noop },
        { UP, "setSelection", "(II)V", kb_noop },
        { NULL, NULL, NULL, NULL }
    };
    #undef UP
    tl_jni_register_hle(hle);
}

/* ------------------------------------------------------------ controller */

/* The same entry point as a touch: the engine reads a KeyEvent or a joystick MotionEvent off what it is handed. */
static void inject(jobj *ev)
{
    typedef uint8_t (*inject_fn)(void *env, void *self, void *event, uintptr_t source);
    /* The newer players take (InputEvent, int), older ones only the event; the extra argument is harmless to those. */
    inject_fn fn = (inject_fn)native_of(U.player_class, "nativeInjectEvent", "(Landroid/view/InputEvent;I)Z");
    if (!fn) fn = (inject_fn)native_of(U.player_class, "nativeInjectEvent", "(Landroid/view/InputEvent;)Z");
    if (fn) { fn(tl_jni_env(), U.player, ev, 0); if (tl_jni_pending()) tl_jni_clear(); }
    tl_jni_unref(ev);
}
static void pad_key(jobj *ev, int device, int action, int keycode, int64_t down_ms, int64_t event_ms)
{ (void)device; (void)action; (void)keycode; (void)down_ms; (void)event_ms; inject(ev); }
static void pad_motion(jobj *ev, int device, int source, int64_t down_ms, int64_t event_ms)
{ (void)device; (void)source; (void)down_ms; (void)event_ms; inject(ev); }

void tl_unity_register_pad_sink(void)
{
    static const tl_pad_sink sink = { pad_key, pad_motion };
    tl_pad_set_sink(&sink);
}

void tl_unity_perf_snapshot(tl_unity_perf *out)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long long ns = atomic_exchange(&U.perf_ns, 0), mx = atomic_exchange(&U.perf_max_ns, 0);
    unsigned long n = atomic_exchange(&U.perf_frames, 0);
    double elapsed = U.perf_since.tv_sec ? (now.tv_sec - U.perf_since.tv_sec) + (now.tv_nsec - U.perf_since.tv_nsec) / 1e9 : 0;
    U.perf_since = now;
    out->fps = elapsed > 0.05 ? (double)n / elapsed : 0;
    out->mean_ms = n ? (double)ns / n / 1e6 : 0;
    out->max_ms = (double)mx / 1e6;
}

void tl_unity_set_paused(bool paused) { atomic_store(&U.paused, paused); }

unsigned long tl_unity_frames(void) { return atomic_load(&U.frames); }
void tl_unity_poke(int signo) { if (U.thread_started) pthread_kill(U.thread, signo); }

void tl_unity_stop(void)
{
    atomic_store(&U.stop, true);
    if (U.thread_started) pthread_join(U.thread, NULL);
}
