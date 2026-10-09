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

/* libunity's executable segment, as the loader mapped it (its executable view). */
typedef struct { const uint8_t *code; size_t size; } code_span;
static int unity_code_cb(uintptr_t bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    const char *slash = name ? strrchr(name, '/') : NULL;
    if (!name || strcmp(slash ? slash + 1 : name, "libunity.so")) return 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = (const uint8_t *)phdr + 56u * i;
        uint32_t type, flags; uint64_t vaddr, filesz;
        memcpy(&type, p, 4); memcpy(&flags, p + 4, 4); memcpy(&vaddr, p + 16, 8); memcpy(&filesz, p + 32, 8);
        if (type == 1 && (flags & 1)) { ((code_span *)user)->code = (const uint8_t *)(bias + vaddr); ((code_span *)user)->size = filesz; }   /* PT_LOAD, PF_X */
    }
    return 1;
}
/* Where an adrp + add pair (in place, as the loader left it: aimed at the writable view) points. */
static uint8_t *adrp_add_target(const uint32_t *w)
{
    int64_t imm = (int64_t)((((w[0] >> 5) & 0x7FFFF) << 2) | ((w[0] >> 29) & 3));
    if (imm & (1 << 20)) imm -= 1 << 21;
    uintptr_t page = ((uintptr_t)w & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
    return (uint8_t *)(page + ((w[1] >> 10) & 0xFFF));
}

/*
 * Unity 6 does not answer for SetStackTraceLogType by name, and a stripped player (engine code stripping relinks libunity for each
 * game) keeps its settings at addresses of its own. The setter is found by its code instead -- two adrp + add pairs that load the
 * overrides and the settings, then "ldr; cmn #-1; csel; str; ret" -- and the settings are set where the engine keeps them, overrides
 * included, so the game's own setting cannot take them back.
 */
static bool stacks_by_table(void)
{
    static const uint32_t tail[] = { 0xb8605908, 0x3100051f, 0x1a880028, 0xb8205928, 0xd65f03c0 };  /* ldr w8,[x8,w0,uxtw#2]; cmn w8,#1; csel; str w8,[x9,..]; ret */
    code_span cs = { 0 };
    tl_ld_iterate(unity_code_cb, &cs);
    if (!cs.code || cs.size < 64) return false;
    const uint8_t *hit = NULL;
    for (const uint8_t *p = cs.code + 16; p + sizeof(tail) <= cs.code + cs.size; p += 4) {
        p = memmem(p, (size_t)(cs.code + cs.size - p), tail, sizeof(tail));
        if (!p) break;
        if (((uintptr_t)p & 3) || p < cs.code + 16) continue;
        if (hit) return false;                                      /* more than one: not sure which */
        hit = p;
    }
    if (!hit) return false;
    const uint32_t *w = (const uint32_t *)hit - 4;
    if ((w[0] & 0x9F00001Fu) != 0x90000008u || (w[1] & 0xFFC003FFu) != 0x91000108u
        || (w[2] & 0x9F00001Fu) != 0x90000009u || (w[3] & 0xFFC003FFu) != 0x91000129u) return false;
    int32_t *overrides = (int32_t *)adrp_add_target(w), *types = (int32_t *)adrp_add_target(w + 2);
    for (int i = 0; i < 5; i++)                                     /* what they hold: StackTraceLogType values, -1 for no override */
        if (overrides[i] < -1 || overrides[i] > 2 || types[i] < 0 || types[i] > 2) return false;
    overrides[0] = overrides[4] = 1;            /* LogType.Error and LogType.Exception: StackTraceLogType.ScriptOnly, whatever is asked later */
    types[0] = types[4] = 1;
    return true;
}

/*
 * Where a C# "Nullable object must have a value" comes from. Even with stack traces asked for, a shipped IL2CPP game's exception
 * comes without one here: its code keeps no frame pointers, and the engine's own walk finds nothing. So the throw helper that
 * Nullable<T>.Value calls (System.ThrowHelper, found through IL2CPP's exported API, so any game) is watched: when it runs, the
 * stack is scanned for return addresses into libil2cpp -- words that point just past a call -- and they are logged as offsets in
 * the library, which a dump of the game's methods turns into the methods it went through.
 */
static struct { const uint8_t *lo, *hi, *base; atomic_int hits; } g_il;
static int il2cpp_code_cb(uintptr_t bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    (void)user;
    const char *slash = name ? strrchr(name, '/') : NULL;
    if (!name || strcmp(slash ? slash + 1 : name, "libil2cpp.so")) return 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = (const uint8_t *)phdr + 56u * i;
        uint32_t type, flags; uint64_t vaddr, filesz;
        memcpy(&type, p, 4); memcpy(&flags, p + 4, 4); memcpy(&vaddr, p + 16, 8); memcpy(&filesz, p + 32, 8);
        if (type == 1 && (flags & 1)) { g_il.lo = (const uint8_t *)(bias + vaddr); g_il.hi = g_il.lo + filesz; g_il.base = (const uint8_t *)bias; }
    }
    return 1;
}
static bool return_into_il2cpp(uint64_t v)
{
    if ((v & 3) || v < (uintptr_t)g_il.lo + 4 || v >= (uintptr_t)g_il.hi) return false;
    uint32_t prev = *(const uint32_t *)(uintptr_t)(v - 4);
    return (prev & 0xFC000000u) == 0x94000000u || (prev & 0xFFFFFC1Fu) == 0xD63F0000u;          /* bl, blr */
}
static void nullable_throw_cb(uint64_t *regs)
{
    if (atomic_fetch_add(&g_il.hits, 1) >= 4) return;
    /* tl_ld_probe's frame: the game's lr is at regs[83] and its sp just above the saved frames (regs + 672 bytes). */
    const uint64_t *sp = (const uint64_t *)((uint8_t *)regs + 672);
    const uint8_t *top = pthread_get_stackaddr_np(pthread_self());
    char line[1400];
    int n = snprintf(line, sizeof(line), "il2cpp: a C# Nullable without a value was read; return addresses in libil2cpp.so, innermost first:");
    if (return_into_il2cpp(regs[83])) n += snprintf(line + n, sizeof(line) - (size_t)n, " %#llx", (unsigned long long)(regs[83] - (uintptr_t)g_il.base));
    int frames = 0;
    for (const uint64_t *p = sp; (const uint8_t *)(p + 1) <= top && p < sp + 4096 && frames < 32 && n < (int)sizeof(line) - 24; p++) {
        if (!return_into_il2cpp(*p)) continue;
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %#llx", (unsigned long long)(*p - (uintptr_t)g_il.base));
        frames++;
    }
    tl_log_line("%s", line);
}
static bool pc_relative(uint32_t i)
{
    return (i & 0x1F000000u) == 0x10000000u || (i & 0x7C000000u) == 0x14000000u || (i & 0xFF000010u) == 0x54000000u
        || (i & 0x7E000000u) == 0x34000000u || (i & 0x7E000000u) == 0x36000000u || (i & 0x3B000000u) == 0x18000000u;
}
static void watch_nullable_throws(void)
{
    void *(*domain_get)(void) = (void *(*)(void))tl_ld_sym(NULL, "il2cpp_domain_get");
    void *(*assembly_open)(void *, const char *) = (void *(*)(void *, const char *))tl_ld_sym(NULL, "il2cpp_domain_assembly_open");
    void *(*get_image)(void *) = (void *(*)(void *))tl_ld_sym(NULL, "il2cpp_assembly_get_image");
    void *(*class_from_name)(void *, const char *, const char *) = (void *(*)(void *, const char *, const char *))tl_ld_sym(NULL, "il2cpp_class_from_name");
    void *(*method_from_name)(void *, const char *, int) = (void *(*)(void *, const char *, int))tl_ld_sym(NULL, "il2cpp_class_get_method_from_name");
    if (!domain_get || !assembly_open || !get_image || !class_from_name || !method_from_name) return;
    void *assembly = assembly_open(domain_get(), "mscorlib");
    void *image = assembly ? get_image(assembly) : NULL;
    void *helper = image ? class_from_name(image, "System", "ThrowHelper") : NULL;
    void *method = helper ? method_from_name(helper, "ThrowInvalidOperationException_InvalidOperation_NoValue", 0) : NULL;
    const uint8_t *fn = method ? *(const uint8_t *const *)method : NULL;                  /* MethodInfo starts with its code */
    tl_ld_iterate(il2cpp_code_cb, NULL);
    if (!fn || !g_il.lo || fn < g_il.lo || fn >= g_il.hi || pc_relative(*(const uint32_t *)fn)) {
        tl_log_line("il2cpp: cannot watch Nullable reads (ThrowHelper %s)", fn ? "not where expected" : "not found");
        return;
    }
    tl_lib *lib = tl_ld_lib_of(fn);
    if (lib && tl_ld_probe(lib, (uint64_t)(fn - (const uint8_t *)tl_ld_lib_base(lib)), nullable_throw_cb))
        tl_log_line("il2cpp: watching where C# reads a Nullable that has no value (ThrowHelper at %#llx)", (unsigned long long)(fn - g_il.base));
}

/*
 * A game that stops without an error -- a loading screen that never ends -- is waiting on something, and its log does not
 * say what. So a few C# methods are watched (found through IL2CPP's API: a game without them is not touched) and their calls
 * logged: for a coroutine's step (MoveNext) the step it is about to take, so the last step logged is where the game waits.
 * For any game, the scenes it loads; the rest is Dave the Diver's way from its intro to its title screen.
 */
enum { TR_CALLS, TR_STEPS, TR_SCENE, TR_TWO_INTS };
typedef struct { const char *assembly, *cls, *method; int argc, kind; } traced;
static const traced k_traced[] = {
    { "UnityEngine.CoreModule", "UnityEngine.SceneManagement.SceneManagerAPIInternal", "LoadSceneAsyncNameIndexInternal", 4, TR_SCENE },
    { "Assembly-CSharp", "LogoManager", "EndReached", 1, TR_CALLS },
    { "Assembly-CSharp", "LogoManager", "<Start>b__12_0", 1, TR_CALLS },
    { "Assembly-CSharp", "LogoManager", "OnLoadingStepUpdate", 2, TR_TWO_INTS },
    { "Assembly-CSharp", "LogoManager/<Start>d__12", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "LogoManager/<Enumerator>d__13", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "LogoManager/<AgeGradeEvent>d__14", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "LogoManager/<>c", "<AgeGradeEvent>b__14_0", 0, TR_CALLS },
    { "Assembly-CSharp", "GameBase/<StartGame>d__31", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "GameBase/<InitAfterSaveSystem>d__45", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "SceneLoaderManagedBehaviour/<Start>d__18", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "SceneLoaderManagedBehaviour/<>c", "<Start>b__18_0", 0, TR_CALLS },
    { "Assembly-CSharp", "DR.Save.MakeEndingBackupSave/<MakeFromSavedData>d__3", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "EntrySceneDispatcher", "Enter", 0, TR_CALLS },
    { "Assembly-CSharp", "EntrySceneDispatcher", "TryPrewarmJungleScene", 0, TR_CALLS },
    { "Assembly-CSharp", "SceneLoader", "GoToTitle", 1, TR_CALLS },
    { "Assembly-CSharp", "SceneLoader/<CoChangeSceneAsync>d__116", "MoveNext", 0, TR_STEPS },
    { "Assembly-CSharp", "SceneLoader/<CoLoadSceneAsync>d__113", "MoveNext", 0, TR_STEPS },
};
#define TRACED (sizeof(k_traced) / sizeof(k_traced[0]))
static struct { atomic_uint calls, lines; const void *obj[4]; int step[4]; } g_tr[TRACED];

static void trace_string(uint64_t v, char *out, size_t n)
{
    /* An Il2CppString: its class and monitor, a 32-bit length, then UTF-16. Shown as ASCII. */
    if (!v || (v & 7)) { snprintf(out, n, "null"); return; }
    int32_t len = *(const int32_t *)(uintptr_t)(v + 16);
    const uint16_t *s = (const uint16_t *)(uintptr_t)(v + 20);
    size_t k = 0;
    for (int32_t i = 0; i < len && k + 1 < n; i++) out[k++] = s[i] < 0x80 ? (char)s[i] : '?';
    out[k] = 0;
}
static void trace_hit(unsigned i, uint64_t *regs)
{
    const traced *t = &k_traced[i];
    unsigned call = atomic_fetch_add(&g_tr[i].calls, 1) + 1;
    if (atomic_load(&g_tr[i].lines) >= 40) return;
    char what[160] = "";
    if (t->kind == TR_STEPS) {
        /* A coroutine's state is its object's first field, after the 16-byte object header. Logged when it changes. */
        const void *obj = (const void *)(uintptr_t)regs[0];
        int step = obj ? *(const int32_t *)((const uint8_t *)obj + 16) : -99;
        int slot = -1;
        for (int k = 0; k < 4 && slot < 0; k++) if (g_tr[i].obj[k] == obj) slot = k;
        if (slot >= 0 && g_tr[i].step[slot] == step) return;
        if (slot < 0) { slot = (int)(call % 4); g_tr[i].obj[slot] = obj; }
        g_tr[i].step[slot] = step;
        snprintf(what, sizeof(what), " at step %d (%p)", step, obj);
    } else if (t->kind == TR_SCENE) {
        char name[120]; trace_string(regs[0], name, sizeof(name));
        snprintf(what, sizeof(what), " \"%s\" (build index %d)", name, (int)regs[1]);
    } else if (t->kind == TR_TWO_INTS) {
        snprintf(what, sizeof(what), " (%d, %d)", (int)regs[1], (int)regs[2]);
    } else if (!(call <= 3 || call == 10 || call == 100 || call == 1000 || call == 10000 || call == 100000)) {
        return;
    }
    atomic_fetch_add(&g_tr[i].lines, 1);
    tl_log_line("il2cpp: %s.%s%s, call %u", t->cls, t->method, what, call);
}
#define TRACE_CB(i) static void trace_cb##i(uint64_t *regs) { trace_hit(i, regs); }
TRACE_CB(0) TRACE_CB(1) TRACE_CB(2) TRACE_CB(3) TRACE_CB(4) TRACE_CB(5) TRACE_CB(6) TRACE_CB(7) TRACE_CB(8) TRACE_CB(9)
TRACE_CB(10) TRACE_CB(11) TRACE_CB(12) TRACE_CB(13) TRACE_CB(14) TRACE_CB(15) TRACE_CB(16) TRACE_CB(17) TRACE_CB(18) TRACE_CB(19)
static void (*const k_trace_cb[])(uint64_t *) = {
    trace_cb0, trace_cb1, trace_cb2, trace_cb3, trace_cb4, trace_cb5, trace_cb6, trace_cb7, trace_cb8, trace_cb9,
    trace_cb10, trace_cb11, trace_cb12, trace_cb13, trace_cb14, trace_cb15, trace_cb16, trace_cb17, trace_cb18, trace_cb19,
};
_Static_assert(TRACED <= sizeof(k_trace_cb) / sizeof(k_trace_cb[0]), "a callback for each watched method");

/* "Namespace.Outer/Nested": the class, through its nesting. */
static void *trace_class(void *image, const char *path)
{
    void *(*class_from_name)(void *, const char *, const char *) = (void *(*)(void *, const char *, const char *))tl_ld_sym(NULL, "il2cpp_class_from_name");
    void *(*nested)(void *, void **) = (void *(*)(void *, void **))tl_ld_sym(NULL, "il2cpp_class_get_nested_types");
    const char *(*class_name)(void *) = (const char *(*)(void *))tl_ld_sym(NULL, "il2cpp_class_get_name");
    if (!image || !class_from_name || !nested || !class_name) return NULL;
    char outer[160];
    const char *slash = strchr(path, '/');
    snprintf(outer, sizeof(outer), "%.*s", (int)(slash ? (size_t)(slash - path) : strlen(path)), path);
    char *dot = strrchr(outer, '.');
    void *k;
    if (dot) { *dot = 0; k = class_from_name(image, outer, dot + 1); }
    else k = class_from_name(image, "", outer);
    while (k && slash) {
        const char *name = slash + 1;
        slash = strchr(name, '/');
        size_t len = slash ? (size_t)(slash - name) : strlen(name);
        void *iter = NULL, *n, *found = NULL;
        while (!found && (n = nested(k, &iter)))
            if (strlen(class_name(n)) == len && !strncmp(class_name(n), name, len)) found = n;
        k = found;
    }
    return k;
}
static void watch_game_steps(void)
{
    void *(*domain_get)(void) = (void *(*)(void))tl_ld_sym(NULL, "il2cpp_domain_get");
    void *(*assembly_open)(void *, const char *) = (void *(*)(void *, const char *))tl_ld_sym(NULL, "il2cpp_domain_assembly_open");
    void *(*get_image)(void *) = (void *(*)(void *))tl_ld_sym(NULL, "il2cpp_assembly_get_image");
    void *(*method_from_name)(void *, const char *, int) = (void *(*)(void *, const char *, int))tl_ld_sym(NULL, "il2cpp_class_get_method_from_name");
    if (!domain_get || !assembly_open || !get_image || !method_from_name) return;
    tl_ld_iterate(il2cpp_code_cb, NULL);
    int watched = 0, missing = 0;
    for (unsigned i = 0; i < TRACED; i++) {
        void *assembly = assembly_open(domain_get(), k_traced[i].assembly);
        void *klass = assembly ? trace_class(get_image(assembly), k_traced[i].cls) : NULL;
        void *method = klass ? method_from_name(klass, k_traced[i].method, k_traced[i].argc) : NULL;
        const uint8_t *fn = method ? *(const uint8_t *const *)method : NULL;
        if (!fn) { missing++; continue; }
        tl_lib *lib = tl_ld_lib_of(fn);
        if (!lib || pc_relative(*(const uint32_t *)fn) || !tl_ld_probe(lib, (uint64_t)(fn - (const uint8_t *)tl_ld_lib_base(lib)), k_trace_cb[i])) {
            tl_log_line("il2cpp: cannot watch %s.%s", k_traced[i].cls, k_traced[i].method);
            continue;
        }
        watched++;
    }
    if (watched) tl_log_line("il2cpp: watching %d of the game's methods to show where it waits (%d are not in this game)", watched, missing);
}

/*
 * A shipped game usually tells Unity to log a C# exception without its stack, so the log says what went wrong but not where
 * ("InvalidOperationException: Nullable object must have a value." and nothing else). Asking for the script part of the stack
 * -- what Application.SetStackTraceLogType does -- for errors and exceptions puts the methods it went through back into the log.
 * Asked again now and then, in case the game sets it back.
 */
static void want_script_stacks(void)
{
    typedef void *(*resolve_fn)(const char *name);
    typedef void (*set_fn)(int log_type, int stack_type);
    static set_fn set;
    static int tries;
    static bool by_table;
    if (!by_table && !tries) {
        watch_nullable_throws();
        watch_game_steps();
        by_table = stacks_by_table();
        if (by_table) { tl_log_line("unity: C# exceptions will be logged with the methods they went through"); return; }
    }
    if (by_table) return;
    if (!set && tries < 200) {
        tries++;
        resolve_fn resolve = (resolve_fn)tl_ld_sym(NULL, "il2cpp_resolve_icall");
        set = resolve ? (set_fn)resolve("UnityEngine.Application::SetStackTraceLogType") : NULL;
        if (set) tl_log_line("unity: C# exceptions will be logged with the methods they went through");
        else if (tries == 5) tl_log_line("unity: Application.SetStackTraceLogType is not to be found; C# exceptions stay without their stacks");
    }
    if (set) { set(0, 1); set(4, 1); }          /* LogType.Error and LogType.Exception: StackTraceLogType.ScriptOnly */
}

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
    unsigned long frame = 0;
    while (render && !atomic_load(&U.stop)) {
        if (atomic_load(&U.paused)) {
            if (!was_paused) { NATIVE_VOID("nativeFocusChanged", "(Z)V", 0, 0); NATIVE_VOID("nativePause", "()Z", 0, 0); was_paused = true; tl_log_line("unity: paused"); }
            usleep(20000);
            continue;
        }
        if (was_paused) { NATIVE_VOID("nativeResume", "()V", 0, 0); NATIVE_VOID("nativeFocusChanged", "(Z)V", 1, 0); was_paused = false; tl_log_line("unity: resumed"); }
        kb_drain();
        if (++frame >= 3 && frame % 30 == 3) want_script_stacks();      /* once the first frame is out, the engine has registered its calls */
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
