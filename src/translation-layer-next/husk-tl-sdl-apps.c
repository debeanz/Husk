/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Java a particular SDL game adds around SDL, implemented in C.
 *
 * SDL games are mostly one native library, but some put an Android interface around it (a start menu, a dialog for a name, a loading overlay) that the library calls
 * into and waits on. Each is a handful of methods; they are kept here, by package, and installed only for that game.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-codewrite.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-internal.h"
#include "husk-tl-ld.h"
#include "husk-tl-macho.h"

jobj *tl_assetstream_list(const char *path);
void tl_sdl_display_metrics(tl_jcall *c);
jobj *tl_assetstream_file_tree(void);
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static const char *Str(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }
static void H_void(tl_jcall *c) { (void)c; }
static void H_false(tl_jcall *c) { c->ret = vz(0); }

/* A native of the game's own library, by its JNI name. */
static void *game_native(const char *mangled)
{
    static const char *const libs[] = { "libmain.so", NULL };
    (void)libs;
    void *fn = NULL;
    extern void *tl_sdl_game_symbol(const char *name);
    fn = tl_sdl_game_symbol(mangled);
    if (!fn) tl_log_line("sdl: the game has no native %s", mangled);
    return fn;
}

/* The answer Java gives later, from another thread, once its dialog is dismissed: here a moment after the question, from a thread of its own. */
struct answer { void (*fn)(void *env, void *self, int a); void *self; int a; void (*fn_text)(void *env, void *self, uint8_t ok, void *text); char text[200]; };
static void *deliver(void *arg)
{
    struct answer *r = arg;
    usleep(250000);
    void *env = tl_jni_env();
    if (r->fn) r->fn(env, r->self, r->a);
    else if (r->fn_text) r->fn_text(env, r->self, 1, tl_jni_new_string(r->text));
    free(r);
    return NULL;
}
static void answer_later(struct answer *r)
{
    pthread_t t;
    if (pthread_create(&t, NULL, deliver, r) == 0) pthread_detach(t); else free(r);
}

/* ---------------------------------------------------------------- Brogue CE */

#define BROGUE "org/broguece/game/BrogueActivity"
static void BR_showStartMenu(tl_jcall *c)                              /* (hasSavedGame, ...): New Game */
{
    struct answer *r = calloc(1, sizeof(*r));
    r->fn = game_native("Java_org_broguece_game_BrogueActivity_nativeStartMenuResult");
    r->self = tl_jni_ref(c->self); r->a = 0;
    if (!r->fn) { free(r); return; }
    tl_log_line("sdl: Brogue start menu: new game");
    answer_later(r);
}
static void BR_showTextInput(tl_jcall *c)                              /* (title, default, maxLength, ...): accept the default */
{
    struct answer *r = calloc(1, sizeof(*r));
    r->fn_text = game_native("Java_org_broguece_game_BrogueActivity_nativeTextInputResult");
    r->self = tl_jni_ref(c->self);
    snprintf(r->text, sizeof(r->text), "%s", c->args[1].l ? Str(c->args[1].l) : "");
    if (!r->fn_text) { free(r); return; }
    answer_later(r);
}
static void BR_getInt(tl_jcall *c) { c->ret = vi(c->args[1].i); }       /* a setting not stored: its default */
static const tl_jhle k_brogue[] = {
    { BROGUE, "showStartMenu", "(ZZ)V", BR_showStartMenu },
    { BROGUE, "showTextInputDialog", "(Ljava/lang/String;Ljava/lang/String;IZ)V", BR_showTextInput },
    { BROGUE, "setOverlayVisible", "(Z)V", H_void },
    { BROGUE, "setLoadingVisible", "(Z)V", H_void },
    { BROGUE, "setLoadingProgress", "(I)V", H_void },
    { BROGUE, "setRestoringVisible", "(Z)V", H_void },
    { BROGUE, "getSettingBool", "(Ljava/lang/String;)Z", H_false },
    { BROGUE, "getSettingInt", "(Ljava/lang/String;I)I", BR_getInt },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------- Pekka Kana 2 */

#define PK2 "org/pgnapps/pk2/PK2Activity"
static void PK_listDir(tl_jcall *c)                                    /* (path): the entries of an asset folder */
{
    jvalue r; r.j = 0; r.l = tl_assetstream_list(Str(c->args[0].l));
    c->ret = r;
}
static const tl_jhle k_pk2[] = {
    { PK2, "externalPermitted", "()Z", H_false },                      /* no shared storage: levels come from the app */
    { PK2, "listDir", "(Ljava/lang/String;)[Ljava/lang/String;", PK_listDir },
    { "org/libsdl/app/SDLActivity", "setSurfaceViewFormat", "(I)V", H_void },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------------- LOVE */

/* LOVE (love2d.org) on Android: its GameActivity adds a few screen settings and the message box LOVE shows when the game's Lua fails. */
#define LOVE "org/love2d/android/GameActivity"
static void LV_dpi(tl_jcall *c) { jvalue v; v.j = 0; v.f = 1.0f; c->ret = v; }                 /* the surface is in real pixels */
static void LV_messagebox(tl_jcall *c)                                                          /* (flags, title, message, ...): said in the log, no button pressed */
{
    tl_log_line("love: message box \"%s\": %s", c->args[1].l ? Str(c->args[1].l) : "", c->args[2].l ? Str(c->args[2].l) : "");
    c->ret = vi(0);
}
static void LV_buildFileTree(tl_jcall *c) { jvalue v; v.j = 0; v.l = tl_assetstream_file_tree(); c->ret = v; }
static const tl_jhle k_love[] = {
    { LOVE, "buildFileTree", "()[Ljava/lang/String;", LV_buildFileTree },
    { LOVE, "getMetrics", "()Landroid/util/DisplayMetrics;", tl_sdl_display_metrics },
    { LOVE, "getImmersiveMode", "()Z", H_false },
    { LOVE, "setImmersiveMode", "(Z)V", H_void },
    { LOVE, "getDPIScale", "()F", LV_dpi },
    { LOVE, "messageboxShowMessageBox", "(ILjava/lang/String;Ljava/lang/String;[I[I[Ljava/lang/String;[I)I", LV_messagebox },
    { NULL, NULL, NULL, NULL }
};

/*
 * LuaJIT. Its compiler writes machine code into memory it makes executable, which the phone does not give a guest (only the loader gets executable memory); with the JIT
 * off, LuaJIT runs its interpreter, and that is what a game gets. Each new Lua state is switched off as the standard libraries are opened.
 */
static void (*g_real_openlibs)(void *L);
static int (*g_jit_setmode)(void *L, int idx, int mode);
static void luajit_openlibs(void *L)
{
    if (!g_real_openlibs) g_real_openlibs = tl_ld_sym(NULL, "luaL_openlibs");
    if (!g_jit_setmode) g_jit_setmode = tl_ld_sym(NULL, "luaJIT_setmode");
    if (g_real_openlibs) g_real_openlibs(L);
    if (g_jit_setmode) g_jit_setmode(L, 0, 0 /* LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF */);
}

/* -------------------------------------------------------- Mina the Hollower */

/*
 * Team Pacheworks' Android port of Mina the Hollower runs the game's Mac build: the player imports their own copy, libmain.so hands it to libmachismo.so (a Mach-O loader
 * built for bionic), and libgothic_patches.so draws it with OpenGL ES. Its Java does three things before SDL starts, done here: the imported files are in place, the
 * configuration is written out with this device's paths in it (ConfInstaller), and the paths are given to libmain.so (MainActivity.nativeSetPaths). Without those SDL_main
 * gives up and clears the screen forever, which looks like a game running.
 */
#define MINA_PACKAGE "com.teampacheworks.minathehollower"
#define MINA_APP "Mina the Hollower.app"
static struct { bool on; char conf[800], lib[800], ext[800], gamedata[800], apk[1024]; } MN;

static void make_dirs(const char *path)
{
    char t[1024]; snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}
static void make_parent(const char *path)
{
    char t[1024]; snprintf(t, sizeof(t), "%s", path);
    char *s = strrchr(t, '/');
    if (s && s != t) { *s = 0; make_dirs(t); }
}
static bool write_file(const char *path, const void *data, size_t len)
{
    make_parent(path);
    FILE *f = fopen(path, "wb");
    if (!f) { tl_log_line("mina: cannot write %s", path); return false; }
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

/* Every occurrence of a placeholder in `text`, replaced. */
static char *replace_all(char *text, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), count = 0;
    for (const char *p = text; (p = strstr(p, from)); p += fl) count++;
    char *out = malloc(strlen(text) + count * (tl > fl ? tl - fl : 0) + 1);
    if (!out) return text;
    char *w = out; const char *r = text, *hit;
    while ((hit = strstr(r, from))) { memcpy(w, r, (size_t)(hit - r)); w += hit - r; memcpy(w, to, tl); w += tl; r = hit + fl; }
    strcpy(w, r);
    free(text);
    return out;
}

/* One configuration file out of the APK, with this device's directories in it. The app's folder moves between installs, so it is written at every launch. */
static bool mina_conf(tl_zip *z, const char *asset, const char *name)
{
    char err[160], path[1000];
    const tl_zip_entry *e = tl_zip_find(z, asset);
    const uint8_t *data; size_t len; bool owned = false;
    if (!e || !tl_zip_data(z, e, 1u << 20, &data, &len, &owned, err, sizeof(err))) { tl_log_line("mina: the APK has no %s", asset); return false; }
    char *text = malloc(len + 1);
    if (!text) { if (owned) free((void *)data); return false; }
    memcpy(text, data, len); text[len] = 0;
    if (owned) free((void *)data);
    text = replace_all(text, "{CONF_DIR}", MN.conf);
    text = replace_all(text, "{LIB_DIR}", MN.lib);
    snprintf(path, sizeof(path), "%s/%s", MN.conf, name);
    bool ok = write_file(path, text, strlen(text));
    free(text);
    return ok;
}

/* The port's OpenGL ES shaders, which it ships in the APK and the engine reads as files. corpus.version says which set is on disk, and goes last so that a partial copy is redone. */
static bool mina_shaders(tl_zip *z)
{
    static const char dir[] = "assets/gamedata/shaders_gles/", ver[] = "assets/gamedata/corpus.version";
    char err[160], path[1200];
    const tl_zip_entry *ve = tl_zip_find(z, ver);
    const uint8_t *data = NULL; size_t len = 0; bool owned = false;
    if (!ve || !tl_zip_data(z, ve, 4096, &data, &len, &owned, err, sizeof(err))) { tl_log_line("mina: the APK has no %s", ver); return false; }
    snprintf(path, sizeof(path), "%s/corpus.version", MN.gamedata);
    bool current = false;
    FILE *f = fopen(path, "rb");
    if (f) {
        char have[4096]; size_t n = fread(have, 1, sizeof(have), f);
        current = n == len && !memcmp(have, data, len);
        fclose(f);
    }
    bool ok = true;
    if (!current) {
        unlink(path);
        int copied = 0;
        for (size_t i = 0; i < z->count && ok; i++) {
            const tl_zip_entry *e = &z->entries[i];
            size_t nl = strlen(e->name);
            if (strncmp(e->name, dir, sizeof(dir) - 1) || !nl || e->name[nl - 1] == '/') continue;
            char dest[1200]; snprintf(dest, sizeof(dest), "%s/shaders_gles/%s", MN.gamedata, e->name + sizeof(dir) - 1);
            make_parent(dest);
            ok = tl_zip_extract(z, e, dest, err, sizeof(err));
            if (ok) copied++; else tl_log_line("mina: %s: %s", e->name, err);
        }
        if (ok) ok = write_file(path, data, len);
        tl_log_line("mina: %d shader files copied out of the APK", copied);
    }
    if (owned) free((void *)data);
    return ok;
}

/* The end of a path from the app's Documents folder on, which is where Files shows it. */
static const char *from_documents(const char *path)
{
    const char *last = NULL;
    for (const char *p = path; (p = strstr(p, "/Documents/")); p++) last = p;
    return last ? last + strlen("/Documents/") : path;
}

/* SDL_main giving up, which it follows by clearing the screen for as long as the app stays open. Frames of that are not a game playing. */
static void mina_log_watch(const char *tag, const char *text)
{
    if (tag && text && !strcmp(tag, "mina/main") && strstr(text, "game did NOT boot") && tl_game_failed_hook)
        tl_game_failed_hook("The game's loader could not start it. The log says why.");
}

static bool mina_prepare(const char *apk, const char *data, char *why, size_t whylen)
{
    snprintf(MN.apk, sizeof(MN.apk), "%s", apk);
    snprintf(MN.conf, sizeof(MN.conf), "%s/files/conf", data);
    snprintf(MN.lib, sizeof(MN.lib), "%s/lib", data);
    snprintf(MN.ext, sizeof(MN.ext), "%s/sdcard/Android/data/%s/files", data, MINA_PACKAGE);
    snprintf(MN.gamedata, sizeof(MN.gamedata), "%s/files/gamedata", data);
    make_dirs(MN.gamedata);
    make_dirs(MN.ext);

    char binary[1100];
    snprintf(binary, sizeof(binary), "%s/" MINA_APP "/Contents/MacOS/MinaTheHollower", MN.gamedata);
    struct stat st;
    if (stat(binary, &st) != 0) {
        snprintf(why, whylen, "This port runs your own copy of the game's Mac version, and it is not here yet. In Files, copy \"" MINA_APP "\" into Husk's folder: %s", from_documents(MN.gamedata));
        return false;
    }

    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) { snprintf(why, whylen, "cannot read the APK: %s", err); return false; }
    bool ok = mina_conf(&z, "assets/conf/machismo.conf", "machismo.conf")
           && mina_conf(&z, "assets/conf/dylib_map.conf", "dylib_map.conf")
           && mina_conf(&z, "assets/conf/patches/gothic.conf", "patches/gothic.conf")
           && mina_shaders(&z);
    tl_zip_close(&z);
    if (!ok) { snprintf(why, whylen, "cannot set up the port's files: see the log"); return false; }

    /* libmain.so looks for the file before it dlopens it, in what would be the app's library folder; the library itself comes out of the APK, by its name. */
    char stub[900];
    snprintf(stub, sizeof(stub), "%s/libmachismo.so", MN.lib);
    if (!write_file(stub, "", 0)) { snprintf(why, whylen, "cannot write to %s", MN.lib); return false; }

    /* This phone's CPU has the atomics and load-acquire instructions the Mac build uses, so nothing needs rewriting down to older ones. Bionic's getauxval here does not list them. */
    setenv("MACHISMO_ISA_EMUL", "0", 1);
    setenv("MACHISMO_VERBOSE", "1", 1);

    /* The image's memory comes first: it is the low end of the JIT region. */
    char merr[300];
    if (!tl_macho_prepare(binary, merr, sizeof(merr))) { snprintf(why, whylen, "cannot make room for the game: %s", merr); return false; }
    tl_codewrite_enable();
    tl_log_watch = mina_log_watch;
    MN.on = true;
    return true;
}

static void mina_before_main(void *activity)
{
    jvalue a; a.j = 0; a.l = tl_jni_new_string("main");
    tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
    if (tl_jni_pending()) { tl_log_line("mina: loading libmain.so failed"); tl_jni_clear(); return; }
    void (*set_paths)(void *, void *, void *, void *, void *, void *, void *, void *) = game_native("Java_com_teampacheworks_mina_MainActivity_nativeSetPaths");
    if (!set_paths) return;
    set_paths(tl_jni_env(), activity, tl_jni_new_string(MN.conf), tl_jni_new_string(MN.lib), tl_jni_new_string(MN.ext), tl_jni_new_string(MN.apk), tl_jni_new_string(MN.gamedata), NULL);
}

/* ------------------------------------------------------------------ install */

/* What a game needs before anything of it is loaded. False stops the launch, with the reason in `why`. */
bool tl_sdl_app_prepare(const char *package, const char *apk, const char *data, char *why, size_t whylen)
{
    if (!strcmp(package, MINA_PACKAGE)) return mina_prepare(apk, data, why, whylen);
    return true;
}

/* What the game's activity does in onCreate, just before SDL_main's thread starts. */
void tl_sdl_app_before_main(void *activity)
{
    if (MN.on) mina_before_main(activity);
}

void tl_sdl_apps_install(const char *package)
{
    if (!strcmp(package, "org.broguece.game")) tl_jni_register_hle(k_brogue);
    if (tl_dexidx_has_class(LOVE)) { tl_jni_register_hle(k_love); tl_ld_interpose("luaL_openlibs", (void *)luajit_openlibs); }
    if (!strcmp(package, "org.pgnapps.pk2")) tl_jni_register_hle(k_pk2);
}
