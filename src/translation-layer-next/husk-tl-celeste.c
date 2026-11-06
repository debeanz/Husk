/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Celeste's Android port: the PC game on FNA, on .NET for Android.
 *
 * On a phone, its launcher takes the player's own copy of the game, patches Celeste.exe with MonoMod into Celeste.dll, and starts GameActivity, a C# subclass of SDLActivity whose
 * Main() calls Celeste.Main. That goes through libmonodroid and Mono.Android, which bind thousands of Java classes. But the game does not use them: only the launcher and the
 * activity do, and the patched game itself references just FNA, MonoMod and the base libraries. So it runs here as what it is underneath, an SDL3 game: SDL's driver (husk-tl-sdl.c)
 * provides the screen, input and sound, and in place of SDL_main this starts Mono itself, on SDL's thread, the way a Godot .NET game does, and does what CelesteLauncher.Run does.
 * The libraries of the Android side (libmonodroid, and anything added to the APK) are not loaded at all.
 *
 *   files/Celeste/          the player's copy: Celeste.exe and Content/
 *   files/dotnet/           the app's assemblies, out of its assembly store
 *   files/patcher/          what the patch is built from, out of the APK
 *   files/patched/          Celeste.dll, patched at the first launch
 */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "husk-tl-asmstore.h"
#include "husk-tl-bionic.h"
#include "husk-tl-internal.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"

void tl_dotnet_runtime_env(void);

static struct { bool on; char files[800], game[900], dotnet[900], patcher[900], patched[900], user[900], background[900], touch[900]; } C;

static void make_dirs(const char *path)
{
    char t[1024]; snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}
static bool exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static const char *from_documents(const char *path)
{
    const char *last = NULL;
    for (const char *p = path; (p = strstr(p, "/Documents/")); p++) last = p;
    return last ? last + strlen("/Documents/") : path;
}

/* The assemblies, once: a marker naming the store's size goes last, so that a partial unpacking or a new version of the app is done again. */
static bool unpack_assemblies(tl_zip *z, char *why, size_t whylen)
{
    const tl_zip_entry *e = tl_zip_find(z, "lib/arm64-v8a/libassembly-store.so");
    if (!e) { snprintf(why, whylen, "the APK has no assembly store"); return false; }
    char marker[1000], want[32], have[32] = "";
    snprintf(marker, sizeof(marker), "%s/.unpacked", C.dotnet);
    snprintf(want, sizeof(want), "%llu", (unsigned long long)e->usize);
    FILE *f = fopen(marker, "r");
    if (f) { if (!fgets(have, sizeof(have), f)) have[0] = 0; fclose(f); }
    if (!strcmp(have, want)) return true;

    char err[200] = "";
    const uint8_t *data; size_t len; bool owned = false;
    if (!tl_zip_data(z, e, 512u << 20, &data, &len, &owned, err, sizeof(err))) { snprintf(why, whylen, "cannot read the assembly store: %s", err); return false; }
    unlink(marker);
    make_dirs(C.dotnet);
    int n = tl_asmstore_unpack(data, len, C.dotnet, err, sizeof(err));
    if (owned) free((void *)data);
    if (n < 0) { snprintf(why, whylen, "cannot unpack the assembly store: %s", err); return false; }
    tl_log_line("celeste: %d assemblies unpacked out of the APK", n);
    f = fopen(marker, "w");
    if (f) { fputs(want, f); fclose(f); }
    return true;
}

static bool unpack_patcher(tl_zip *z, char *why, size_t whylen)
{
    static const char dir[] = "assets/patcher/";
    make_dirs(C.patcher);
    int copied = 0;
    for (size_t i = 0; i < z->count; i++) {
        const tl_zip_entry *e = &z->entries[i];
        if (strncmp(e->name, dir, sizeof(dir) - 1) || !e->name[sizeof(dir) - 1] || strchr(e->name + sizeof(dir) - 1, '/')) continue;
        char dest[1200], err[200] = "";
        snprintf(dest, sizeof(dest), "%s/%s", C.patcher, e->name + sizeof(dir) - 1);
        if (!tl_zip_extract(z, e, dest, err, sizeof(err))) { snprintf(why, whylen, "cannot copy %s: %s", e->name, err); return false; }
        copied++;
    }
    if (!copied) { snprintf(why, whylen, "the APK has no patcher"); return false; }
    return true;
}

bool tl_celeste_prepare(const char *apk, const char *data, char *why, size_t whylen)
{
    snprintf(C.files, sizeof(C.files), "%s/files", data);
    snprintf(C.game, sizeof(C.game), "%s/Celeste", C.files);
    snprintf(C.dotnet, sizeof(C.dotnet), "%s/dotnet", C.files);
    snprintf(C.patcher, sizeof(C.patcher), "%s/patcher", C.files);
    snprintf(C.patched, sizeof(C.patched), "%s/patched/Celeste.dll", C.files);
    snprintf(C.user, sizeof(C.user), "%s/userdata", C.files);
    snprintf(C.background, sizeof(C.background), "%s/background.png", C.files);
    snprintf(C.touch, sizeof(C.touch), "%s/touch_layout.txt", C.files);
    make_dirs(C.game);
    make_dirs(C.user);

    char exe[1000], content[1000];
    snprintf(exe, sizeof(exe), "%s/Celeste.exe", C.game);
    snprintf(content, sizeof(content), "%s/Content", C.game);
    if (!exists(exe) || !exists(content)) {
        snprintf(why, whylen, "This port runs your own copy of Celeste (the FNA build, which has FNA.dll beside Celeste.exe), and it is not here yet. In Files, copy Celeste.exe and the Content folder into Husk's folder: %s", from_documents(C.game));
        return false;
    }

    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) { snprintf(why, whylen, "cannot read the APK: %s", err); return false; }
    bool ok = unpack_assemblies(&z, why, whylen) && unpack_patcher(&z, why, whylen);
    tl_zip_close(&z);
    if (!ok) return false;

    tl_dotnet_runtime_env();
    /* OpenGL ES through ANGLE: what every other game here draws with. */
    setenv("FNA3D_FORCE_DRIVER", "OpenGL", 1);
    /* ES 3.0 has no base-vertex draws: FNA3D has its own way when it cannot find them. */
    setenv("TL_GL_NO_BASE_VERTEX", "1", 1);
    C.on = true;
    return true;
}

/* What the activity's static initialiser loads by System.loadLibrary, so that JNI_OnLoad runs: the .NET crypto library keeps the VM for its Java calls. */
void tl_celeste_before_main(void)
{
    if (!C.on) return;
    jvalue a; a.j = 0; a.l = tl_jni_new_string("System.Security.Cryptography.Native.Android");
    tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
    if (tl_jni_pending()) { tl_log_line("celeste: the .NET crypto library did not load"); tl_jni_clear(); }
}

bool tl_celeste_on(void) { return C.on; }

/* ------------------------------------------------------------------- Mono */

typedef struct {
    int (*coreclr_initialize)(const char *, const char *, int, const char **, const char **, void **, unsigned *);
    void *(*jit_init)(const char *);
    void *(*enter_gc_unsafe)(void **);
    void *(*get_root_domain)(void);
    void *(*thread_attach)(void *);
    void *(*domain_assembly_open)(void *, const char *);
    void *(*assembly_get_image)(void *);
    void *(*class_from_name)(void *, const char *, const char *);
    void *(*class_get_method_from_name)(void *, const char *, int);
    void *(*class_get_field_from_name)(void *, const char *);
    void *(*class_vtable)(void *, void *);
    int (*runtime_class_init)(void *);
    void (*field_static_set_value)(void *, void *, void *);
    void *(*runtime_invoke)(void *, void *, void **, void **);
    void *(*string_new)(void *, const char *);
    void *(*get_string_class)(void);
    void *(*array_new)(void *, void *, uintptr_t);
    char *(*array_addr_with_size)(void *, int, uintptr_t);
    void (*gc_wbarrier_set_arrayref)(void *, void *, void *);
    void *(*object_to_string)(void *, void **);
    char *(*string_to_utf8)(void *);
} mono_api;
static mono_api M;
static void *g_domain;

static bool mono_bind(void)
{
    tl_lib *lib = tl_ld_load("libmonosgen-2.0.so");
    if (!lib || !tl_ld_init(lib)) { tl_log_line("celeste: libmonosgen-2.0.so did not load"); return false; }
    bool ok = true;
#define BIND(field, name) do { *(void **)&M.field = tl_ld_sym(lib, name); if (!M.field) { tl_log_line("celeste: Mono has no %s", name); ok = false; } } while (0)
    BIND(coreclr_initialize, "coreclr_initialize");
    BIND(jit_init, "mono_jit_init");
    BIND(enter_gc_unsafe, "mono_threads_enter_gc_unsafe_region");
    BIND(get_root_domain, "mono_get_root_domain");
    BIND(thread_attach, "mono_thread_attach");
    BIND(domain_assembly_open, "mono_domain_assembly_open");
    BIND(assembly_get_image, "mono_assembly_get_image");
    BIND(class_from_name, "mono_class_from_name");
    BIND(class_get_method_from_name, "mono_class_get_method_from_name");
    BIND(class_get_field_from_name, "mono_class_get_field_from_name");
    BIND(class_vtable, "mono_class_vtable");
    BIND(runtime_class_init, "mono_runtime_class_init");
    BIND(field_static_set_value, "mono_field_static_set_value");
    BIND(runtime_invoke, "mono_runtime_invoke");
    BIND(string_new, "mono_string_new");
    BIND(get_string_class, "mono_get_string_class");
    BIND(array_new, "mono_array_new");
    BIND(array_addr_with_size, "mono_array_addr_with_size");
    BIND(gc_wbarrier_set_arrayref, "mono_gc_wbarrier_set_arrayref");
    BIND(object_to_string, "mono_object_to_string");
    BIND(string_to_utf8, "mono_string_to_utf8");
#undef BIND
    return ok;
}

static bool mono_start(void)
{
    char app_paths[2000], base[1000];
    char patched_dir[900];
    snprintf(patched_dir, sizeof(patched_dir), "%s", C.patched);
    char *s = strrchr(patched_dir, '/');
    if (s) *s = 0;
    snprintf(app_paths, sizeof(app_paths), "%s:%s", C.dotnet, patched_dir);
    snprintf(base, sizeof(base), "%s/", C.game);
    /* Every property here is also what AppContext.GetData answers: CelesteLauncher.Run and GameActivity.Main set these same names. */
    const char *keys[] = { "APP_PATHS", "APP_CONTEXT_BASE_DIRECTORY", "RUNTIME_IDENTIFIER", "System.Globalization.Invariant",
                           "CelesteAndroid.Platform", "CelesteAndroid.PrefPath", "CelesteAndroid.BackgroundPath", "CelesteAndroid.TouchLayoutPath",
                           "CelesteAndroid.ShowFps", "CelesteAndroid.HideTouch", "CelesteAndroid.TouchOpacity" };
    const char *vals[] = { app_paths, base, "android-arm64", "true",
                           "Android", C.user, C.background, C.touch,
                           "0", "0", "45" };
    void *host = NULL; unsigned domain_id = 0;
    int rc = M.coreclr_initialize(C.dotnet, "Celeste", (int)(sizeof(keys) / sizeof(keys[0])), keys, vals, &host, &domain_id);
    if (rc != 0) { tl_log_line("celeste: coreclr_initialize failed (%#x)", (unsigned)rc); return false; }
    /* On Mono that only records the properties and how to find assemblies. This starts the runtime, and attaches this thread to it. */
    g_domain = M.jit_init("Celeste");
    if (!g_domain) g_domain = M.get_root_domain();
    if (!g_domain) { tl_log_line("celeste: Mono did not start: it has no root domain"); return false; }
    M.thread_attach(g_domain);
    /* With cooperative suspend, mono_jit_init leaves this thread "blocking": the collector does not wait for it, and it may not touch managed state. A host stays out of that state to
     * use the runtime, and this thread now does for good, as what runs on it next is the game. Managed code polls for the collector; its native calls step back out by themselves. */
    void *stackdata;                                                /* on the stack: Mono measures the stack from this address */
    M.enter_gc_unsafe(&stackdata);
    tl_log_line("celeste: Mono is up");
    return true;
}

/* An exception out of managed code, in the log. True if there was one. */
static bool report(const char *what, void *exc)
{
    if (!exc) return false;
    void *inner = NULL;
    void *str = M.object_to_string(exc, &inner);
    char *text = str && !inner ? M.string_to_utf8(str) : NULL;
    tl_log_line("celeste: %s threw: %s", what, text ? text : "(and it could not be described)");
    return true;
}

static void *open_class(const char *assembly_path, const char *ns, const char *name)
{
    void *a = M.domain_assembly_open(g_domain, assembly_path);
    if (!a) { tl_log_line("celeste: cannot open %s", assembly_path); return NULL; }
    void *c = M.class_from_name(M.assembly_get_image(a), ns, name);
    if (!c) tl_log_line("celeste: %s has no class %s.%s", assembly_path, ns, name);
    return c;
}

/* GameInstaller.Patch: Celeste.exe and the port's Celeste.Android.mm.dll into Celeste.dll. A file that is there is finished, as it is renamed into place last. */
static bool patch_game(void)
{
    if (exists(C.patched)) return true;
    char dir[900], exe[1000], mod[1000], tmp[1000], lib[1000];
    snprintf(dir, sizeof(dir), "%s", C.patched);
    char *s = strrchr(dir, '/');
    if (s) { *s = 0; make_dirs(dir); }
    snprintf(exe, sizeof(exe), "%s/Celeste.exe", C.game);
    snprintf(mod, sizeof(mod), "%s/Celeste.Android.mm.dll", C.patcher);
    snprintf(tmp, sizeof(tmp), "%s.tmp", C.patched);
    snprintf(lib, sizeof(lib), "%s/Celeste.Patcher.dll", C.dotnet);
    tl_log_line("celeste: patching the game, which takes a moment and only happens once");

    void *cls = open_class(lib, "CelesteAndroid.Patcher", "CelestePatcher");
    void *m = cls ? M.class_get_method_from_name(cls, "Patch", 5) : NULL;
    if (!m) { tl_log_line("celeste: the patcher has no Patch method"); return false; }
    void *dirs = M.array_new(g_domain, M.get_string_class(), 1);
    M.gc_wbarrier_set_arrayref(dirs, M.array_addr_with_size(dirs, sizeof(void *), 0), M.string_new(g_domain, C.patcher));
    /* The last argument is its logger: left null, it writes to the console, which reaches the log. */
    void *args[5] = { M.string_new(g_domain, exe), M.string_new(g_domain, mod), M.string_new(g_domain, tmp), dirs, NULL };
    void *exc = NULL;
    M.runtime_invoke(m, NULL, args, &exc);
    if (report("the patcher", exc)) return false;
    if (rename(tmp, C.patched) != 0) { tl_log_line("celeste: cannot put the patched game in place: %s", strerror(errno)); return false; }
    tl_log_line("celeste: patched");
    return true;
}

/* CelesteLauncher.Run. */
static int run_game(void)
{
    if (chdir(C.game) != 0) tl_log_line("celeste: cannot enter %s: %s", C.game, strerror(errno));
    void *engine = open_class(C.patched, "Monocle", "Engine");
    void *game = engine ? open_class(C.patched, "Celeste", "Celeste") : NULL;
    if (!game) return -1;
    void *field = M.class_get_field_from_name(engine, "AssemblyDirectory");
    void *vt = M.class_vtable(g_domain, engine);
    if (field && vt) {
        M.runtime_class_init(vt);
        M.field_static_set_value(vt, field, M.string_new(g_domain, C.game));
    } else tl_log_line("celeste: Monocle.Engine has no AssemblyDirectory to set");
    void *main_method = M.class_get_method_from_name(game, "Main", 1);
    if (!main_method) { tl_log_line("celeste: Celeste.Celeste has no Main"); return -1; }
    void *argv = M.array_new(g_domain, M.get_string_class(), 0);
    void *args[1] = { argv };
    void *exc = NULL;
    tl_log_line("celeste: calling Celeste.Main");
    M.runtime_invoke(main_method, NULL, args, &exc);
    if (report("Celeste.Main", exc)) return -1;
    return 0;
}

/* What runs on SDL's thread in place of SDL_main. */
int tl_celeste_main(void)
{
    int (*set_hint)(const char *, const char *) = tl_ld_sym(NULL, "SDL_SetHint");
    if (set_hint) {
        set_hint("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight");
        set_hint("FNA3D_FORCE_DRIVER", "OpenGL");
    }
    if (!mono_bind() || !mono_start() || !patch_game()) return -1;
    return run_game();
}
