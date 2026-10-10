/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Java side of an Unreal Engine 4 game, implemented in C: the methods of com.epicgames.ue4.GameActivity that the engine calls through JNI
 * (AndroidThunkJava_*), for the facts it asks for at start-up. A method with no implementation here logs once and returns zero, which is
 * what most of them (notifications, purchases, Google sign-in) want.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-bionic.h"

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vj(int64_t j) { jvalue v; v.j = j; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static jvalue vf(float f) { jvalue v; v.j = 0; v.f = f; return v; }
static const char *S(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }

#define CLS "com/epicgames/ue4/GameActivity"

static struct { char pkg[160], data[512], ext[700], apk[1024]; jobj *activity; } U;

/*
 * <meta-data> of the manifest, which the engine reads through GetMetaData*: the project's settings as the packager recorded them.
 * These are ARK's, and only the fallback: the game's own manifest is read first (tl_hle_manifest_meta).
 */
static const struct { const char *key, *value; } k_meta[] = {
    { "com.epicgames.ue4.GameActivity.EngineVersion", "4.26.2" }, { "com.epicgames.ue4.GameActivity.EngineBranch", "++UE4+Release-4.26" },
    { "com.epicgames.ue4.GameActivity.ProjectVersion", "1.0.0.0" }, { "com.epicgames.ue4.GameActivity.DepthBufferPreference", "24" },
    { "com.epicgames.ue4.GameActivity.bPackageDataInsideApk", "false" }, { "com.epicgames.ue4.GameActivity.bVerifyOBBOnStartUp", "false" },
    { "com.epicgames.ue4.GameActivity.bShouldHideUI", "true" }, { "com.epicgames.ue4.GameActivity.ProjectName", "ShooterGame" },
    { "com.epicgames.ue4.GameActivity.AppType", "" }, { "com.epicgames.ue4.GameActivity.bHasOBBFiles", "false" },
    { "com.epicgames.ue4.GameActivity.BuildConfiguration", "Shipping" }, { "com.epicgames.ue4.GameActivity.CookedFlavors", "ASTC" },
    { "com.epicgames.ue4.GameActivity.bValidateTextureFormats", "true" }, { "com.epicgames.ue4.GameActivity.bUseExternalFilesDir", "true" },
    { "com.epicgames.ue4.GameActivity.bPublicLogFiles", "true" }, { "com.epicgames.ue4.GameActivity.bUseDisplayCutout", "false" },
    { "com.epicgames.ue4.GameActivity.bAllowIMU", "true" }, { "com.epicgames.ue4.GameActivity.bSupportsVulkan", "false" },
    { "com.epicgames.ue4.GameActivity.StartupPermissions", "" },
};
const char *tl_hle_manifest_meta(const char *key);
int tl_frame_hz(void);         /* the frame rate games are held to, 60 or 120 (husk-tl-egl.c) */
const char *tl_ue4_meta(const char *key)
{
    /* Not in the manifest: GameActivity computes it from the display's metrics. x dpi, y dpi. */
    if (!strcmp(key, "ue4.displaymetrics.dpi")) return "440.0,440.0";
    /* Likewise AudioManager's answers (PROPERTY_OUTPUT_FRAMES_PER_BUFFER, PROPERTY_OUTPUT_SAMPLE_RATE) and the display's refresh rate. A zero buffer size is not an
     * answer the engine can survive: it rounds its callback buffer up to a multiple of it, and a multiple of zero is never reached. */
    if (!strcmp(key, "audiomanager.framesPerBuffer")) return "256";
    if (!strcmp(key, "audiomanager.optimalSampleRate")) return "48000";
    if (!strcmp(key, "ue4.display.getRefreshRate")) return tl_frame_hz() > 60 ? "120" : "60";
    /* A setting forced from the environment ("key=value;key=value", keys without the com.epicgames.ue4.GameActivity. prefix), for trying things. */
    const char *force = getenv("TL_UE4_META");
    if (force) {
        static char buf[160]; size_t pl = strlen("com.epicgames.ue4.GameActivity.");
        if (!strncmp(key, "com.epicgames.ue4.GameActivity.", pl)) {
            for (const char *p = force; *p; ) {
                const char *eq = strchr(p, '='), *end = strchr(p, ';'); if (!end) end = p + strlen(p);
                if (eq && eq < end && (size_t)(eq - p) == strlen(key + pl) && !strncmp(p, key + pl, eq - p)) { snprintf(buf, sizeof(buf), "%.*s", (int)(end - eq - 1), eq + 1); return buf; }
                p = *end ? end + 1 : end;
            }
        }
    }
    const char *m = tl_hle_manifest_meta(key);
    if (m) return m;
    for (size_t i = 0; i < sizeof(k_meta) / sizeof(k_meta[0]); i++) if (!strcmp(key, k_meta[i].key)) return k_meta[i].value;
    return NULL;
}
#define meta tl_ue4_meta
static void GA_hasMeta(tl_jcall *c) { c->ret = vz(meta(S(c->args[0].l)) != NULL); }
static void GA_metaString(tl_jcall *c) { const char *v = meta(S(c->args[0].l)); c->ret = vl(v ? tl_jni_new_string(v) : NULL); }
static void GA_metaBool(tl_jcall *c) { const char *v = meta(S(c->args[0].l)); c->ret = vz(v && !strcmp(v, "true")); }
static void GA_metaInt(tl_jcall *c) { const char *v = meta(S(c->args[0].l)); c->ret = vi(v ? atoi(v) : 0); }
static void GA_metaLong(tl_jcall *c) { const char *v = meta(S(c->args[0].l)); c->ret = vj(v ? atoll(v) : 0); }
static void GA_metaFloat(tl_jcall *c) { const char *v = meta(S(c->args[0].l)); c->ret = vf(v ? (float)atof(v) : 0); }

jobj *tl_hle_assets(void);
static void GA_assetManager(tl_jcall *c) { c->ret = vl(tl_jni_ref(tl_hle_assets())); }

/*
 * ClassLoader.findClass / loadClass, which native code uses (through the activity's loader, as FindClass sees only the system's) to reach the game's own
 * Java classes by their dotted names. The class is there if the framework or the APK has it.
 */
bool tl_dexidx_has_class(const char *name);
static void CL_findClass(tl_jcall *c)
{
    char name[300]; snprintf(name, sizeof(name), "%s", S(c->args[0].l));
    for (char *p = name; *p; p++) if (*p == '.') *p = '/';
    bool framework = !strncmp(name, "android/", 8) || !strncmp(name, "java/", 5) || !strncmp(name, "javax/", 6) || !strncmp(name, "dalvik/", 7);
    if (framework || tl_dexidx_has_class(name)) { c->ret = vl(tl_jni_class_object(name)); return; }
    tl_log_line("ue4: ClassLoader could not find %s", name);
    tl_jni_throw("java/lang/ClassNotFoundException", name);
    c->ret = vl(NULL);
}

/*
 * AndroidThunkJava_ForceQuit: the engine has met something it cannot go on after. Its log is compiled out of a shipping build, but a failed check or a
 * fatal error still writes its message into GErrorHist (FDebug::AssertFailed, before it reports the error and the error device quits): that message,
 * when the engine exports the array, and the call chain are printed, and the thread stops (on Android the process would be gone in a moment).
 */
#include <unistd.h>
#include "husk-tl-ld.h"
static void GA_forceQuit(tl_jcall *c)
{
    (void)c;
    tl_lib *ue = tl_ld_find_lib("libUE4.so");
    const uint16_t *hist = ue ? (const uint16_t *)tl_ld_sym(ue, "GErrorHist") : NULL;      /* TCHAR is UTF-16 on Android */
    if (hist && hist[0]) {
        char msg[1200]; size_t k = 0;
        for (size_t i = 0; hist[i] && i < 16384 && k + 1 < sizeof(msg); i++)
            msg[k++] = hist[i] >= 0x20 && hist[i] < 0x7f ? (char)hist[i] : hist[i] < 0x20 ? ' ' : '?';     /* line breaks as spaces: one log line */
        msg[k] = 0;
        tl_log_line("ue4: the engine's error: %s", msg);
    }
    tl_log_line("ue4: the engine asked to quit (AndroidThunkJava_ForceQuit). Called from:");
    void **fp = __builtin_frame_address(0);
    for (int i = 0; fp && i < 28; i++) {
        if (((uintptr_t)fp & 7) || (uintptr_t)fp < 0x100000) break;
        void *lr = fp[1];
        const char *lib = NULL; const void *sym = NULL;
        const char *name = tl_ld_symbol_at(lr, &lib, &sym);
        tl_log_line("ue4:   %p  %s  %s+%#lx", lr, lib ? lib : "?", name ? name : "?", sym ? (unsigned long)((const char *)lr - (const char *)sym) : 0ul);
        void **next = fp[0];
        if (next <= fp) break;
        fp = next;
    }
    for (;;) sleep(1000);
}

/*
 * AndroidThunkJava_InitHMDs: on Android the activity answers by calling back into the engine (nativeInitHMDs), and the engine's start-up waits for that.
 * There is no headset to report, so it is just the call.
 */
static void GA_initHMDs(tl_jcall *c)
{
    (void)c;
    tl_lib *lib = tl_ld_find_lib("libUE4.so");
    void (*fn)(void *, void *) = lib ? (void (*)(void *, void *))tl_ld_sym(lib, "Java_com_epicgames_ue4_GameActivity_nativeInitHMDs") : NULL;
    if (fn) fn(tl_jni_env(), U.activity);
    else tl_log_line("ue4: nativeInitHMDs is not provided by libUE4");
}

/* com.epicgames.mobile.eossdk.EOSSDK: the statics Epic Online Services' native library calls while it starts */
#define EOS "com/epicgames/mobile/eossdk/EOSSDK"
static void EOS_void(tl_jcall *c) { (void)c; }
static void EOS_context(tl_jcall *c) { c->ret = vl(U.activity ? tl_jni_ref(U.activity) : NULL); }
static void EOS_osVersion(tl_jcall *c) { c->ret = vl(tl_jni_new_string("14")); }
static void EOS_market(tl_jcall *c) { c->ret = vl(tl_jni_new_string("US")); }
static void EOS_nullString(tl_jcall *c) { c->ret.l = NULL; }
static void EOS_true(tl_jcall *c) { c->ret = vz(1); }
static void EOS_orientation(tl_jcall *c) { c->ret = vi(1); }
static void EOS_zeroLong(tl_jcall *c) { c->ret = vj(0); }

/* android.app.ActivityThread, which native code reaches through the hidden API to find the application's Context */
static void AT_current(tl_jcall *c)
{
    static jobj *thread;
    if (!thread) thread = tl_jni_new_object(tl_jni_class("android/app/ActivityThread"));
    c->ret = vl(tl_jni_ref(thread));
}

static void GA_get(tl_jcall *c) { c->ret = vl(U.activity ? tl_jni_ref(U.activity) : NULL); }
static void GA_packageName(tl_jcall *c) { c->ret = vl(tl_jni_new_string(U.pkg)); }
static void GA_false(tl_jcall *c) { c->ret = vz(0); }
static void GA_true(tl_jcall *c) { c->ret = vz(1); }
static void GA_zero(tl_jcall *c) { c->ret = vi(0); }
static void GA_nullObj(tl_jcall *c) { c->ret.l = NULL; }
static void GA_emptyString(tl_jcall *c) { c->ret = vl(tl_jni_new_string("")); }
static void GA_commandLine(tl_jcall *c) { c->ret = vl(tl_jni_new_string(getenv("TL_UE4_CMDLINE") ? getenv("TL_UE4_CMDLINE") : "")); }
static void GA_fontDir(tl_jcall *c) { c->ret = vl(tl_jni_new_string("/system/fonts/")); }
const char *tl_hle_android_id(void);
static void GA_androidId(tl_jcall *c) { c->ret = vl(tl_jni_new_string(tl_hle_android_id())); }
static void GA_refresh(tl_jcall *c) { c->ret = vi(tl_frame_hz()); }
/* The rates the screen offers: 60, and 120 when games may have it. */
static void GA_refreshRates(tl_jcall *c)
{
    int n = tl_frame_hz() > 60 ? 2 : 1;
    jobj *a = tl_jni_new_prim_array('I', (uint32_t)n);
    ((int *)a->arr.data)[0] = 60;
    if (n > 1) ((int *)a->arr.data)[1] = 120;
    c->ret = vl(a);
}
static void GA_orientation(tl_jcall *c) { c->ret = vi(1); }                    /* landscape */
static void GA_netType(tl_jcall *c) { c->ret = vi(1); }                        /* Wi-Fi */
static void GA_netTime(tl_jcall *c) { c->ret = vj(0); }
extern bool tl_pad_connected(int slot);
/* Any controller: the app gives each its own slot, and the first one connected need not be in the first. */
static void GA_gamepad(tl_jcall *c)
{
    bool any = false;
    for (int s = 0; s < 4 && !any; s++) any = tl_pad_connected(s);
    c->ret = vz(any);
}
static void GA_listInputDevices(tl_jcall *c) { c->ret = vl(tl_jni_new_string("")); }
static void GA_isOBBInAPK(tl_jcall *c) { c->ret = vz(0); }
/*
 * AndroidThunkJava_SetDesiredViewSize: the engine draws at its content scale factor (r.MobileContentScaleFactor), smaller than the screen, and
 * GameActivity has the SurfaceView's buffers that size (SurfaceHolder.setFixedSize), which Android scales up to the view. Given a window of the
 * screen's size, OpenGL's picture would sit in its bottom-left corner. With Vulkan, MoltenVK sizes the frames from the swapchain and the
 * layer already scales them.
 */
bool tl_egl_draws_on_window(void);
void tl_nwindow_set_buffer_size(int w, int h);
static void GA_desiredViewSize(tl_jcall *c)
{
    int w = c->args[0].i, h = c->args[1].i;
    bool gl = tl_egl_draws_on_window();
    tl_log_line("ue4: the engine draws at %dx%d%s", w, h, gl ? "; the window's frames are made that size and scaled up to the screen" : "");
    if (gl) tl_nwindow_set_buffer_size(w, h);
}
static void GA_boolObj(tl_jcall *c)                                            /* java.lang.Boolean true */
{
    jobj *b = tl_jni_new_object(tl_jni_class("java/lang/Boolean"));
    jvalue v = vz(1); tl_jni_set_field(b, "value", "Z", v);
    c->ret = vl(b);
}

/*
 * com.epicgames.unreal.GooglePlayGamesWrapper (Little Nightmares): Google Play Games, which this device does not have. Each request is answered as the
 * class answers it on a phone that is not signed in. PostLogin's sign-in check fails, so the saved games (snapshots) are never loaded: querying, loading
 * and writing them fail at once, inside the call. A sign-in, the player's identity, achievements and leaderboards fail when their task does. The engine
 * runs its online requests one after another, so one left unanswered holds up every later one: QuerySnapshots, unanswered, kept the game at "checking
 * for downloadable content" for good.
 */
#define GPGW "com/epicgames/unreal/GooglePlayGamesWrapper"
static void *gpgw_native(const char *name)
{
    void *fn = tl_jni_native(GPGW, name, NULL);
    char sym[160];
    snprintf(sym, sizeof(sym), "Java_com_epicgames_unreal_GooglePlayGamesWrapper_%s", name);
    if (!fn) fn = tl_ld_sym(NULL, sym);
    if (!fn) tl_log_line("ue4: Google Play Games: %s is not in the game's libraries", sym);
    return fn;
}
static void gpgw_said(const char *request, const char *answer)
{
    tl_log_line("ue4: Google Play Games: %s -> %s (not signed in)", request, answer);
}
/* native(long request) */
static void gpgw_fail(tl_jcall *c, const char *request, const char *native)
{
    gpgw_said(request, native);
    void (*fn)(void *, void *, int64_t) = (void (*)(void *, void *, int64_t))gpgw_native(native);
    if (fn) fn(tl_jni_env(), tl_jni_class_object(GPGW), c->args[0].j);
}
static void GPGW_nothing(tl_jcall *c) { (void)c; }
static void GPGW_querySnapshots(tl_jcall *c) { gpgw_fail(c, "QuerySnapshots", "nativeQuerySnapshotsFailure"); }
static void GPGW_loadSnapshot(tl_jcall *c) { gpgw_fail(c, "LoadSnapshot", "nativeLoadSnapshotFailure"); }
static void GPGW_writeSnapshot(tl_jcall *c) { gpgw_fail(c, "WriteSnapshot", "nativeWriteSnapshotFailure"); }
static void GPGW_login(tl_jcall *c) { gpgw_fail(c, "Login", "nativeLoginFailed"); }
static void GPGW_identity(tl_jcall *c) { gpgw_fail(c, "RequestIdentityData", "nativeLoginFailed"); }
static void GPGW_queryAchievements(tl_jcall *c) { gpgw_fail(c, "QueryAchievements", "nativeQueryAchievementsFailed"); }
static void GPGW_leaderboardScore(tl_jcall *c) { gpgw_fail(c, "RequestPlayerLeaderboardScore", "nativeLeaderboardRequestFailed"); }
/* nativeWriteAchievementsCompleted(long request, String[] written): none were */
static void GPGW_writeAchievements(tl_jcall *c)
{
    gpgw_said("WriteAchievements", "nativeWriteAchievementsCompleted, none written");
    void (*fn)(void *, void *, int64_t, void *) = (void (*)(void *, void *, int64_t, void *))gpgw_native("nativeWriteAchievementsCompleted");
    if (!fn) return;
    jobj *none = tl_jni_new_obj_array(tl_jni_class("java/lang/String"), 0);
    fn(tl_jni_env(), tl_jni_class_object(GPGW), c->args[0].j, none);
    tl_jni_unref(none);
}
/* nativeFlushLeaderboardsCompleted(long request, boolean all written): not */
static void GPGW_submitScores(tl_jcall *c)
{
    gpgw_said("SubmitLeaderboardsScores", "nativeFlushLeaderboardsCompleted(false)");
    void (*fn)(void *, void *, int64_t, uint8_t) = (void (*)(void *, void *, int64_t, uint8_t))gpgw_native("nativeFlushLeaderboardsCompleted");
    if (fn) fn(tl_jni_env(), tl_jni_class_object(GPGW), c->args[0].j, 0);
}

/*
 * AndroidThunkJava_GetInputDeviceInfo: what the engine learns of an input device it has not seen, before it takes the device's events. Java answers
 * from InputDevice (its descriptor, vendor and product, controller number and name), and for a device it cannot find, "Unknown"; never nothing. Told
 * nothing, the engine marks the device invalid: a controller's buttons and sticks went nowhere, and the game was never told one was connected.
 */
jobj *tl_input_device_object(int id);
static void GA_inputDeviceInfo(tl_jcall *c)
{
    int id = c->args[0].i;
    jobj *dev = tl_input_device_object(id);
    jobj *info = tl_jni_new_object(tl_jni_class("com/epicgames/ue4/GameActivity$InputDeviceInfo"));
    jvalue vendor = vi(0), product = vi(0), controller = vi(-1), name, descriptor;
    name.j = 0; descriptor.j = 0;
    if (dev) {
        vendor = tl_jni_call(dev, "getVendorId", "()I", NULL);
        product = tl_jni_call(dev, "getProductId", "()I", NULL);
        controller = tl_jni_call(dev, "getControllerNumber", "()I", NULL);
        name = tl_jni_call(dev, "getName", "()Ljava/lang/String;", NULL);
        descriptor = tl_jni_call(dev, "getDescriptor", "()Ljava/lang/String;", NULL);
        tl_jni_unref(dev);
    }
    if (!name.l) name.l = tl_jni_new_string("Unknown");
    if (!descriptor.l) descriptor.l = tl_jni_new_string("Unknown");
    tl_jni_set_field(info, "deviceId", "I", vi(id));
    tl_jni_set_field(info, "vendorId", "I", vendor);
    tl_jni_set_field(info, "productId", "I", product);
    tl_jni_set_field(info, "controllerId", "I", controller);
    tl_jni_set_field(info, "name", "Ljava/lang/String;", name);
    tl_jni_set_field(info, "descriptor", "Ljava/lang/String;", descriptor);
    static uint64_t said;
    if (id >= 0 && id < 64 && !(said & (1ull << id))) {
        said |= 1ull << id;
        tl_log_line("ue4: input device %d is \"%s\" (%04x:%04x, controller %d)", id, S(name.l), vendor.i, product.i, controller.i);
    }
    c->ret = vl(info);
}

/*
 * What a game's start-up is doing, for a game that stops on a screen: the functions of its own that its menus and start-up go through, logged as they
 * are called (the first few calls, then the 10th, 100th...; a state as it changes; a menu with the class of the widget pushed or popped). Each is
 * watched if the game has it -- these are Little Nightmares': its "checking for downloadable content" screen, its save slots, its user and controller
 * discovery and its DLC system.
 */
enum { TW_CALL, TW_INT1, TW_INT3, TW_WIDGET1, TW_TOP };
static const struct { const char *sym, *label; int kind; } k_watch[] = {
    { "_ZN17UAtlasMenuManager4PushEP16UAtlasMenuWidgetbbb19EAtlasViewportState", "menu: push", TW_WIDGET1 },
    { "_ZN17UAtlasMenuManager14PushWithFadingEP16UAtlasMenuWidgetbbb19EAtlasViewportState", "menu: push (fading)", TW_WIDGET1 },
    { "_ZN17UAtlasMenuManager3PopEv", "menu: pop", TW_TOP },
    { "_ZN17UAtlasMenuManager13PopWithFadingEv", "menu: pop (fading)", TW_TOP },
    { "_ZN17UAtlasMenuManager5ClearEv", "menu: clear", TW_TOP },
    { "_ZN17UAtlasMenuManager15ClearWithFadingEv", "menu: clear (fading)", TW_TOP },
    { "_ZN21UAtlasComplianceLayer30CheckingForContentWidgetPoppedEv", "compliance: the checking-for-content widget was popped", TW_CALL },
    { "_ZN21UAtlasComplianceLayer10InitializeEP18AAtlasBaseGameMode", "compliance: Initialize", TW_CALL },
    { "_ZN28UAtlasAndroidComplianceLayer20DiscoverPlatformUserEi", "compliance: DiscoverPlatformUser", TW_INT1 },
    { "_ZN21UAtlasComplianceLayer21SetUserDiscoveryStateE24EAtlasUserDiscoveryState", "compliance: user discovery state", TW_INT1 },
    { "_ZN21UAtlasComplianceLayer22SetMainControllerStateE25EAtlasMainControllerState", "compliance: main controller state", TW_INT1 },
    { "_ZN21UAtlasComplianceLayer29RequestMainControllerIdChangeEi", "compliance: main controller id", TW_INT1 },
    { "_ZN21UAtlasComplianceLayer45SetCurrentProcessedControllerAsMainControllerEv", "compliance: SetCurrentProcessedControllerAsMainController", TW_CALL },
    { "_ZN28UAtlasAndroidComplianceLayer29OnControllerConnectionChangedEbii", "compliance: controller connection (connected, user, controller)", TW_INT3 },
    { "_ZN28UAtlasAndroidComplianceLayer32PushControllerDisconnectedWidgetEv", "compliance: PushControllerDisconnectedWidget", TW_CALL },
    { "_ZN28UAtlasAndroidComplianceLayer23MenuManagerPoppedWidgetEP16UAtlasMenuWidget", "compliance: MenuManagerPoppedWidget", TW_WIDGET1 },
    { "_ZN21UAtlasComplianceLayer19IsStateSharePendingERbR16EAtlasSaveTargetR5FName", "compliance: IsStateSharePending", TW_CALL },
    { "_ZNK21UAtlasComplianceLayer16IsUserDiscoveredEv", "compliance: IsUserDiscovered", TW_CALL },
    { "_ZN22UAtlasSaveSlotsManager28LoadCurrentUserIndexToMemoryEv", "saves: LoadCurrentUserIndexToMemory", TW_CALL },
    { "_ZN22UAtlasSaveSlotsManager18SetActiveUserIndexEib", "saves: active user index", TW_INT1 },
    { "_ZN22UAtlasSaveSlotsManager19EnqueueStateRequestEP22AtlasStateAsyncRequest", "saves: EnqueueStateRequest", TW_CALL },
    { "_ZN22UAtlasSaveSlotsManager16OnLoadStatesDoneEP22AtlasStateAsyncRequest", "saves: OnLoadStatesDone", TW_CALL },
    { "_ZN12UGameHelpers19CheckForEnabledDLCsEP7UObject", "dlc: CheckForEnabledDLCs", TW_CALL },
    { "_ZN12UGameHelpers12IsDLCEnabledEP7UObject9EAtlasDLC", "dlc: IsDLCEnabled", TW_CALL },
    { "_ZN19AtlasDLCSystemEmpty5StartEv", "dlc: the (empty) DLC system starts", TW_CALL },
    { "_ZN18UAtlasGameInstance17StartGameInstanceEv", "game instance: StartGameInstance", TW_CALL },
    { "_ZN18UAtlasGameInstance18CreatePreloadTasksEP6UClassRK7FStringi", "game instance: CreatePreloadTasks", TW_CALL },
    { "_ZN18UAtlasGameInstance16StartPreLoadTaskEib", "game instance: StartPreLoadTask", TW_INT3 },
    { "_ZN18UAtlasGameInstance17PreloadedCallbackEP12FPreloadTask", "game instance: PreloadedCallback", TW_CALL },
};
#define WATCHED (sizeof(k_watch) / sizeof(k_watch[0]))
static struct { atomic_uint calls, lines; int64_t last; } g_watch[WATCHED];

/* An FName as text, and the class of an object (UObjectBase: its class at +0x10, a class's name at +0x18). */
static void ue_fname(const void *fname, char *out, size_t n)
{
    static void (*to_string)(const void *, void *);
    static void (*mem_free)(void *);
    if (!to_string) {
        tl_lib *ue = tl_ld_find_lib("libUE4.so");
        mem_free = ue ? (void (*)(void *))tl_ld_sym(ue, "_ZN7FMemory4FreeEPv") : NULL;
        to_string = ue ? (void (*)(const void *, void *))tl_ld_sym(ue, "_ZNK5FName8ToStringER7FString") : NULL;
    }
    snprintf(out, n, "?");
    if (!fname || !to_string) return;
    struct { uint16_t *data; int32_t num, max; } s = { NULL, 0, 0 };
    to_string(fname, &s);
    size_t k = 0;
    for (int32_t i = 0; s.data && i < s.num && s.data[i] && k + 1 < n; i++) out[k++] = s.data[i] < 0x80 ? (char)s.data[i] : '?';
    out[k] = 0;
    if (s.data && mem_free) mem_free(s.data);
}
static void ue_class_of(const void *obj, char *out, size_t n)
{
    if (!obj) { snprintf(out, n, "nothing"); return; }
    const uint8_t *cls = *(const uint8_t *const *)((const uint8_t *)obj + 0x10);
    ue_fname(cls ? cls + 0x18 : NULL, out, n);
}

static void watch_hit(unsigned i, uint64_t *regs)
{
    unsigned call = atomic_fetch_add(&g_watch[i].calls, 1) + 1;
    if (atomic_load(&g_watch[i].lines) >= 60) return;
    char what[200] = "";
    switch (k_watch[i].kind) {
    case TW_INT1:
        if (call > 1 && g_watch[i].last == (int64_t)(int32_t)regs[1]) return;
        g_watch[i].last = (int32_t)regs[1];
        snprintf(what, sizeof(what), " %d", (int)regs[1]);
        break;
    case TW_INT3:
        snprintf(what, sizeof(what), " (%d, %d, %d)", (int)regs[1], (int)regs[2], (int)regs[3]);
        break;
    case TW_WIDGET1: {
        char cls[120]; ue_class_of((const void *)(uintptr_t)regs[1], cls, sizeof(cls));
        snprintf(what, sizeof(what), " %s", cls);
        break;
    }
    case TW_TOP: {
        static void *(*top)(void *);
        if (!top) { tl_lib *ue = tl_ld_find_lib("libUE4.so"); top = ue ? (void *(*)(void *))tl_ld_sym(ue, "_ZNK17UAtlasMenuManager3TopEv") : NULL; }
        char cls[120] = "?";
        if (top && regs[0]) ue_class_of(top((void *)(uintptr_t)regs[0]), cls, sizeof(cls));
        snprintf(what, sizeof(what), " (on top: %s)", cls);
        break;
    }
    default:
        if (!(call <= 5 || call == 10 || call == 100 || call == 1000 || call == 10000 || call == 100000)) return;
        break;
    }
    atomic_fetch_add(&g_watch[i].lines, 1);
    tl_log_line("ue4: %s%s, call %u", k_watch[i].label, what, call);
}
#define W_CB(i) static void watch_cb##i(uint64_t *regs) { watch_hit(i, regs); }
W_CB(0) W_CB(1) W_CB(2) W_CB(3) W_CB(4) W_CB(5) W_CB(6) W_CB(7) W_CB(8) W_CB(9) W_CB(10) W_CB(11) W_CB(12) W_CB(13) W_CB(14) W_CB(15)
W_CB(16) W_CB(17) W_CB(18) W_CB(19) W_CB(20) W_CB(21) W_CB(22) W_CB(23) W_CB(24) W_CB(25) W_CB(26) W_CB(27) W_CB(28) W_CB(29) W_CB(30) W_CB(31)
static void (*const k_watch_cb[])(uint64_t *) = {
    watch_cb0, watch_cb1, watch_cb2, watch_cb3, watch_cb4, watch_cb5, watch_cb6, watch_cb7, watch_cb8, watch_cb9, watch_cb10, watch_cb11,
    watch_cb12, watch_cb13, watch_cb14, watch_cb15, watch_cb16, watch_cb17, watch_cb18, watch_cb19, watch_cb20, watch_cb21, watch_cb22,
    watch_cb23, watch_cb24, watch_cb25, watch_cb26, watch_cb27, watch_cb28, watch_cb29, watch_cb30, watch_cb31,
};
_Static_assert(WATCHED <= sizeof(k_watch_cb) / sizeof(k_watch_cb[0]), "a callback for each watched function");

/* An instruction that reads the pc (adr/adrp, branches, literal loads): one a probe cannot move into its stub. */
static bool pc_relative(uint32_t i)
{
    return (i & 0x1F000000u) == 0x10000000u || (i & 0x7C000000u) == 0x14000000u || (i & 0xFF000010u) == 0x54000000u
        || (i & 0x7E000000u) == 0x34000000u || (i & 0x7E000000u) == 0x36000000u || (i & 0x3B000000u) == 0x18000000u;
}

void tl_ue4_watch(void)
{
    tl_lib *ue = tl_ld_find_lib("libUE4.so");
    if (!ue) return;
    int n = 0;
    for (unsigned i = 0; i < WATCHED; i++) {
        const uint8_t *fn = tl_ld_sym(ue, k_watch[i].sym);
        if (!fn || pc_relative(*(const uint32_t *)fn)) continue;
        if (tl_ld_probe(ue, (uint64_t)(fn - (const uint8_t *)tl_ld_lib_base(ue)), k_watch_cb[i])) n++;
    }
    if (n) tl_log_line("ue4: watching %d of the game's start-up functions", n);
}

#define M_(c, n, s, f) { c, n, s, f }
static const tl_jhle k_hle[] = {
    M_(CLS, "AndroidThunkJava_HasMetaDataKey", "(Ljava/lang/String;)Z", GA_hasMeta),
    M_(CLS, "AndroidThunkJava_GetMetaDataString", "(Ljava/lang/String;)Ljava/lang/String;", GA_metaString),
    M_(CLS, "AndroidThunkJava_GetMetaDataBoolean", "(Ljava/lang/String;)Z", GA_metaBool),
    M_(CLS, "AndroidThunkJava_GetMetaDataInt", "(Ljava/lang/String;)I", GA_metaInt),
    M_(CLS, "AndroidThunkJava_GetMetaDataLong", "(Ljava/lang/String;)J", GA_metaLong),
    M_(CLS, "AndroidThunkJava_GetMetaDataFloat", "(Ljava/lang/String;)F", GA_metaFloat),
    M_(CLS, "Get", "()Lcom/epicgames/ue4/GameActivity;", GA_get),
    M_(CLS, "AndroidThunkJava_ForceQuit", "()V", GA_forceQuit),
    M_(CLS, "AndroidThunkJava_InitHMDs", "()V", GA_initHMDs),
    M_("android/app/ActivityThread", "currentActivityThread", "()Landroid/app/ActivityThread;", AT_current),
    M_("android/app/ActivityThread", "currentApplication", "()Landroid/app/Application;", EOS_context),
    M_("android/app/ActivityThread", "getApplication", "()Landroid/app/Application;", EOS_context),
    M_(EOS, "GetApplicationContext", "()Landroid/content/Context;", EOS_context),
    M_(EOS, "GetActivity", "()Landroid/app/Activity;", EOS_context),
    M_(EOS, "setUserAgent", "(Ljava/lang/String;)V", EOS_void),
    M_(EOS, "GetOSVersion", "()Ljava/lang/String;", EOS_osVersion),
    M_(EOS, "GetSalesMarketIdentifier", "()Ljava/lang/String;", EOS_market),
    M_(EOS, "Keychain_ReadValue", "(Ljava/lang/String;)Ljava/lang/String;", EOS_nullString),
    M_(EOS, "Keychain_WriteValue", "(Ljava/lang/String;Ljava/lang/String;)Z", EOS_true),
    M_(EOS, "getDeviceOrientation", "()I", EOS_orientation),
    M_(EOS, "verifyPlatformsOptions", "(Ljava/lang/String;)Z", EOS_true),
    M_(EOS, "PrewarmURL", "(Ljava/lang/String;)J", EOS_zeroLong),
    M_(EOS, "LaunchURL", "(Ljava/lang/String;)J", EOS_zeroLong),
    M_(CLS, "AndroidThunkJava_GetAssetManager", "()Landroid/content/res/AssetManager;", GA_assetManager),
    M_("java/lang/ClassLoader", "findClass", "(Ljava/lang/String;)Ljava/lang/Class;", CL_findClass),
    M_("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", CL_findClass),
    M_("dalvik/system/PathClassLoader", "findClass", "(Ljava/lang/String;)Ljava/lang/Class;", CL_findClass),
    M_("dalvik/system/PathClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", CL_findClass),
    M_(CLS, "getAppPackageName", "()Ljava/lang/String;", GA_packageName),
    M_(CLS, "isOBBInAPK", "()Z", GA_isOBBInAPK),
    M_(CLS, "AndroidThunkJava_SetDesiredViewSize", "(II)V", GA_desiredViewSize),
    M_(CLS, "AndroidThunkJava_GetInputDeviceInfo", "(I)Lcom/epicgames/ue4/GameActivity$InputDeviceInfo;", GA_inputDeviceInfo),
    M_(CLS, "isStandaloneMode", "()Ljava/lang/Boolean;", GA_boolObj),
    M_(CLS, "isValidGameActivity", "()Ljava/lang/Boolean;", GA_boolObj),
    M_(CLS, "AndroidThunkJava_GetCommandLine", "()Ljava/lang/String;", GA_commandLine),
    M_(CLS, "AndroidThunkJava_GetFontDirectory", "()Ljava/lang/String;", GA_fontDir),
    M_(CLS, "AndroidThunkJava_GetAndroidId", "()Ljava/lang/String;", GA_androidId),
    M_(CLS, "AndroidThunkJava_GetNativeDisplayRefreshRate", "()I", GA_refresh),
    M_(CLS, "AndroidThunkJava_GetSupportedNativeDisplayRefreshRates", "()[I", GA_refreshRates),
    M_(CLS, "AndroidThunkJava_GetDeviceOrientation", "()I", GA_orientation),
    M_(CLS, "AndroidThunkJava_GetNetworkConnectionType", "()I", GA_netType),
    M_(CLS, "AndroidThunkJava_GetNetworkTimeMillis", "()J", GA_netTime),
    M_(CLS, "AndroidThunkJava_IsGamepadAttached", "()Z", GA_gamepad),
    M_(CLS, "AndroidThunkJava_ListInputDevices", "(I)Ljava/lang/String;", GA_listInputDevices),
    M_(CLS, "AndroidThunkJava_GetFunnelId", "()Ljava/lang/String;", GA_emptyString),
    M_(CLS, "AndroidThunkJava_GetLoginId", "()Ljava/lang/String;", GA_emptyString),
    M_(CLS, "AndroidThunkJava_GetIntentExtrasString", "(Ljava/lang/String;)Ljava/lang/String;", GA_nullObj),
    M_(CLS, "AndroidThunkJava_GetIntentExtrasBoolean", "(Ljava/lang/String;)Z", GA_false),
    M_(CLS, "AndroidThunkJava_GetIntentExtrasInt", "(Ljava/lang/String;)I", GA_zero),
    M_(CLS, "AndroidThunkJava_HasIntentExtrasKey", "(Ljava/lang/String;)Z", GA_false),
    M_(CLS, "AndroidThunkJava_IsMusicActive", "()Z", GA_false),
    M_(CLS, "AndroidThunkJava_IsScreensaverEnabled", "()Z", GA_false),
    M_(CLS, "AndroidThunkJava_IsScreenCaptureDisabled", "()Z", GA_false),
    M_(CLS, "AndroidThunkJava_IapIsAllowedToMakePurchases", "()Z", GA_false),
    M_(CLS, "AndroidThunkJava_GooglePAD_Available", "()Z", GA_false),
    M_(CLS, "AndroidThunkJava_IsAllowedRemoteNotifications", "()Z", GA_false),
    M_(GPGW, "Initialize", "(Landroid/content/Context;)V", GPGW_nothing),
    M_(GPGW, "PostLogin", "(Landroid/app/Activity;)V", GPGW_nothing),
    M_(GPGW, "InitSnapshots", "(Landroid/app/Activity;)V", GPGW_nothing),
    M_(GPGW, "ShowAchievementsUI", "(Landroid/app/Activity;)V", GPGW_nothing),
    M_(GPGW, "ShowLeaderboardUI", "(Landroid/app/Activity;Ljava/lang/String;)V", GPGW_nothing),
    M_(GPGW, "QuerySnapshots", "(JLandroid/app/Activity;)V", GPGW_querySnapshots),
    M_(GPGW, "LoadSnapshot", "(JLandroid/app/Activity;Ljava/lang/String;)V", GPGW_loadSnapshot),
    M_(GPGW, "WriteSnapshot", "(JLandroid/app/Activity;Ljava/lang/String;[B)V", GPGW_writeSnapshot),
    M_(GPGW, "Login", "(JLandroid/app/Activity;Ljava/lang/String;Z)V", GPGW_login),
    M_(GPGW, "RequestIdentityData", "(JLandroid/app/Activity;Lcom/epicgames/unreal/GooglePlayGamesWrapper$AuthCodeSettings;)V", GPGW_identity),
    M_(GPGW, "QueryAchievements", "(JLandroid/app/Activity;)V", GPGW_queryAchievements),
    M_(GPGW, "WriteAchievements", "(JLandroid/app/Activity;[Ljava/lang/String;[I[I)V", GPGW_writeAchievements),
    M_(GPGW, "RequestPlayerLeaderboardScore", "(JLandroid/app/Activity;Ljava/lang/String;)V", GPGW_leaderboardScore),
    M_(GPGW, "SubmitLeaderboardsScores", "(JLandroid/app/Activity;[Ljava/lang/String;[J)V", GPGW_submitScores),
    { NULL, NULL, NULL, NULL }
};

void tl_ue4_hle_install(const char *pkg, const char *apk, const char *data, const char *ext_files)
{
    snprintf(U.pkg, sizeof(U.pkg), "%s", pkg);
    snprintf(U.apk, sizeof(U.apk), "%s", apk);
    snprintf(U.data, sizeof(U.data), "%s", data);
    snprintf(U.ext, sizeof(U.ext), "%s", ext_files);
    tl_jni_declare("android/app/NativeActivity", "android/app/Activity");
    tl_jni_declare(CLS, "android/app/NativeActivity");
    tl_jni_declare("com/epicgames/ue4/GameApplication", "android/app/Application");
    tl_jni_declare("java/lang/Boolean", "java/lang/Object");
    tl_jni_declare("com/epicgames/ue4/GameActivity$InputDeviceInfo", "java/lang/Object");
    tl_jni_register_hle(k_hle);
    /* The enum constants EOS reads off EOSOverlay.BrowserStatus: with no class initialiser to run they would be null, which it takes as a broken SDK. */
    static const char *const status[] = { "BEGIN_LOAD", "CLOSED", "CRASHED", "END_LOAD", "LOAD_ERROR" };
    for (size_t i = 0; i < sizeof(status) / sizeof(status[0]); i++) {
        jvalue v; v.j = 0; v.l = tl_jni_new_object(tl_jni_class("com/epicgames/mobile/eossdk/EOSOverlay$BrowserStatus"));
        tl_jni_set_static("com/epicgames/mobile/eossdk/EOSOverlay$BrowserStatus", status[i], "Lcom/epicgames/mobile/eossdk/EOSOverlay$BrowserStatus;", v);
    }
}

void tl_ue4_set_activity(jobj *activity) { U.activity = activity; }
