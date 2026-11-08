/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-nativeactivity.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-egl.h"
#include "husk-tl-gamepad.h"
#include "husk-tl-hades.h"
#include "husk-tl-vulkan.h"
#include "husk-tl-jni.h"
#include "husk-tl-internal.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

void tl_jni_hle_install(void);
void tl_hle_configure(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_hle_set_activity(jobj *a);
jobj *tl_hle_assets(void);
void tl_set_data_dir(const char *dir);
void tl_obb_folder(const char *data_dir, char *out, size_t n);
void tl_packs_folder(const char *data_dir, char *out, size_t n);
void tl_nwindow_configure(int w, int h, void *layer);
void *tl_nwindow_get(void);
void tl_ue4_hle_install(const char *pkg, const char *apk, const char *data, const char *ext_files);
void tl_ue4_watch(void);
void tl_ue4_set_activity(jobj *activity);
void tl_ue4_set_screen(int width, int height);
const char *tl_ue4_meta(const char *key);
void tl_ue_set_five(bool on);
const char *tl_ue_lib_name(void);
void tl_ue_fit_allocator(tl_lib *lib);

#define CLS (N.ue5 ? "com/epicgames/unreal/GameActivity" : "com/epicgames/ue4/GameActivity")

/* android/native_activity.h */
typedef struct ANativeActivity ANativeActivity;
typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity *);
    void (*onResume)(ANativeActivity *);
    void *(*onSaveInstanceState)(ANativeActivity *, size_t *);
    void (*onPause)(ANativeActivity *);
    void (*onStop)(ANativeActivity *);
    void (*onDestroy)(ANativeActivity *);
    void (*onWindowFocusChanged)(ANativeActivity *, int);
    void (*onNativeWindowCreated)(ANativeActivity *, void *);
    void (*onNativeWindowResized)(ANativeActivity *, void *);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *, void *);
    void (*onNativeWindowDestroyed)(ANativeActivity *, void *);
    void (*onInputQueueCreated)(ANativeActivity *, void *);
    void (*onInputQueueDestroyed)(ANativeActivity *, void *);
    void (*onContentRectChanged)(ANativeActivity *, const void *);
    void (*onConfigurationChanged)(ANativeActivity *);
    void (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;
struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    void *vm, *env;
    void *clazz;
    const char *internalDataPath, *externalDataPath;
    int32_t sdkVersion;
    void *instance, *assetManager;
    const char *obbPath;
};

static struct {
    tl_ga_config cfg;
    char apk[1024], data[512], pkg[128], frame_dir[512], angle_egl[600], angle_gles[600];
    char internal_dir[600], ext_dir[700], obb_dir[800], obb_file[900], patch_file[900];
    jobj *activity;
    ANativeActivity na;
    ANativeActivityCallbacks callbacks;
    bool generic;                          /* a NativeActivity game of its own (no Unreal library): nothing of Unreal is set up for it */
    bool ue5;                              /* Unreal Engine 5: libUnreal.so, and its Java under com.epicgames.unreal */
    bool has_patch;                        /* the APK carries a second OBB, assets/patch.obb.png */
    bool vulkan, gl_only;                  /* Unreal: Vulkan (MoltenVK), or OpenGL ES because the game has shaders for nothing else */
    char main_lib[64];                     /* the library that exports ANativeActivity_onCreate */
    void *window;
    pthread_t ui;
    bool started;
} N;

static void load_library(const char *name)
{
    jvalue a; a.j = 0; a.l = tl_jni_new_string(name);
    tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
}

static void mkdirs(const char *path)
{
    char t[1024]; snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}


/* Where a stored (uncompressed) entry's bytes begin in the APK, and how many there are. */
static bool stored_range(const char *apk, const char *entry, unsigned long long *off, unsigned long long *size)
{
    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return false;
    const tl_zip_entry *e = tl_zip_find(&z, entry);
    bool ok = false;
    if (e && e->method == 0 && e->local_offset + 30 < z.size) {
        const uint8_t *h = z.map + e->local_offset;
        uint16_t nlen, elen; memcpy(&nlen, h + 26, 2); memcpy(&elen, h + 28, 2);
        *off = e->local_offset + 30 + nlen + elen; *size = e->usize; ok = true;
    }
    tl_zip_close(&z);
    return ok;
}

/*
 * Play Asset Delivery. A game too big for one APK has Google Play deliver the rest as asset packs, which Play Core installs under the app's files
 * directory (files/assetpacks/<pack>/<version>/<version>/assets/) and reports through its AssetPackManager: Little Nightmares' levels are its "levels"
 * pack, pakchunk1-Android_ETC2.pak, which its boot level mounts (MountAssetBundle) once Play Core says the pack is installed and where. Without it the
 * menus work and New Game stops: "Could not find SuperStruct BP_SwitchClass_C". A repackaged APK carries the packs itself, as that same files/ tree
 * zipped into a stored asset (assets/youtubeapi.html, 309 MB), for its own code to unpack on the first start.
 *
 * Here the packs are found in the APK and shown where Play Core would put them, each file read in place from the APK (a virtual file), and Play Core's
 * native API (play/asset_pack.h), which the engine's GooglePAD plugin calls, is answered from them: every pack the APK carries is installed.
 *
 * A bundle can carry packs in APKs of their own, zipped, which are unpacked as it is added into the game's packs folder, beside its OBB folder
 * (husk-tl-bundled.c). Those are installed too, read from there: Sifu's 39 packs, pakchunk1 to pakchunk39, are the whole game but its menus, and
 * without them it stops at its own "Download failed" screen.
 */
#define PACK_FILES 8
static struct { char pack[64], file[160]; unsigned long long off, size; } g_pack_file[PACK_FILES];
static int g_npack_files;
static char g_pack_root[700];

#define DISK_PACKS 128
static struct { char name[64], path[900]; unsigned long long bytes; } g_disk_pack[DISK_PACKS];
static int g_ndisk_packs;

bool tl_ue4_has_asset_packs(void) { return g_npack_files > 0 || g_ndisk_packs > 0; }

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The stored files under .../assetpacks/<pack>/.../assets/ in a zip that lies, stored, at `base` in the APK. */
static void packs_in_zip(const uint8_t *z, uint64_t len, uint64_t base, const char *where)
{
    if (len < 22) return;
    uint64_t eocd = 0; bool found = false;
    for (uint64_t i = len - 22, lo = len > 65557 ? len - 65557 : 0; ; i--) {
        if (rd32(z + i) == 0x06054b50u) { eocd = i; found = true; break; }
        if (i == lo) break;
    }
    if (!found) return;
    uint16_t n = rd16(z + eocd + 10);
    uint64_t o = rd32(z + eocd + 16);
    for (uint16_t k = 0; k < n && o + 46 <= len && rd32(z + o) == 0x02014b50u; k++) {
        uint16_t method = rd16(z + o + 10), nlen = rd16(z + o + 28), xlen = rd16(z + o + 30), clen = rd16(z + o + 32);
        uint32_t usize = rd32(z + o + 24), lho = rd32(z + o + 42);
        char name[512]; snprintf(name, sizeof(name), "%.*s", (int)nlen, (const char *)(z + o + 46));
        o += 46u + nlen + xlen + clen;
        const char *ap = strstr(name, "assetpacks/"), *as = ap ? strstr(ap, "/assets/") : NULL;
        if (!as || method != 0 || !usize || strchr(as + 8, '/') || g_npack_files >= PACK_FILES || (uint64_t)lho + 30 > len) continue;
        const char *pack = ap + 11, *slash = strchr(pack, '/');
        uint64_t data = (uint64_t)lho + 30 + rd16(z + lho + 26) + rd16(z + lho + 28);
        if (!slash || data + usize > len) continue;
        snprintf(g_pack_file[g_npack_files].pack, sizeof(g_pack_file[0].pack), "%.*s", (int)(slash - pack), pack);
        snprintf(g_pack_file[g_npack_files].file, sizeof(g_pack_file[0].file), "%s", as + 8);
        g_pack_file[g_npack_files].off = base + data;
        g_pack_file[g_npack_files].size = usize;
        tl_log_line("ue4: asset pack \"%s\": %s, %.1f MiB, in the APK's %s", g_pack_file[g_npack_files].pack, as + 8, usize / 1048576.0, where);
        g_npack_files++;
    }
}

static bool is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }

/* A pack's assets folder: <pack>/assets, or a folder or two further down, as Play Core lays it out (<pack>/<version>/<version>/assets). */
static bool pack_assets(const char *dir, int depth, char *out, size_t n)
{
    char p[1000];
    snprintf(p, sizeof(p), "%s/assets", dir);
    if (is_dir(p)) { snprintf(out, n, "%s", p); return true; }
    DIR *d = depth < 3 ? opendir(dir) : NULL;
    if (!d) return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d)); ) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        found = is_dir(p) && pack_assets(p, depth + 1, out, n);
    }
    closedir(d);
    return found;
}

static unsigned long long folder_bytes(const char *dir, int depth)
{
    unsigned long long total = 0;
    DIR *d = depth < 8 ? opendir(dir) : NULL;
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (e->d_name[0] == '.') continue;
        char p[1100]; struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st) == 0) total += S_ISDIR(st.st_mode) ? folder_bytes(p, depth + 1) : (unsigned long long)st.st_size;
    }
    closedir(d);
    return total;
}

/* The packs in the game's packs folder, each <pack>/.../assets/ with something in it. */
static void find_disk_packs(void)
{
    char root[800];
    tl_packs_folder(N.data, root, sizeof(root));
    DIR *d = opendir(root);
    if (!d) return;
    unsigned long long total = 0;
    for (struct dirent *e; g_ndisk_packs < DISK_PACKS && (e = readdir(d)); ) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(g_disk_pack[0].name)) continue;
        char dir[900], assets[900];
        snprintf(dir, sizeof(dir), "%s/%s", root, e->d_name);
        if (!is_dir(dir) || !pack_assets(dir, 0, assets, sizeof(assets))) continue;
        unsigned long long bytes = folder_bytes(assets, 0);
        if (!bytes) continue;
        snprintf(g_disk_pack[g_ndisk_packs].name, sizeof(g_disk_pack[0].name), "%s", e->d_name);
        snprintf(g_disk_pack[g_ndisk_packs].path, sizeof(g_disk_pack[0].path), "%s", assets);
        g_disk_pack[g_ndisk_packs].bytes = bytes;
        g_ndisk_packs++;
        total += bytes;
    }
    closedir(d);
    if (g_ndisk_packs) tl_log_line("ue4: %d asset packs installed in the game's packs folder (%.2f GB), as its bundle carried them", g_ndisk_packs, total / 1e9);
}

/* The packs the APK carries, each file made visible where Play Core would have installed it, and those in the game's packs folder. */
static void find_asset_packs(void)
{
    find_disk_packs();
    tl_zip z; char err[160];
    if (!tl_zip_open(&z, N.apk, err, sizeof(err))) return;
    for (size_t i = 0; i < z.count; i++) {
        const tl_zip_entry *e = &z.entries[i];
        if (e->method != 0 || e->usize < (1u << 20) || e->local_offset + 30 > z.size) continue;
        const uint8_t *h = z.map + e->local_offset;
        uint64_t start = e->local_offset + 30 + rd16(h + 26) + rd16(h + 28);
        if (start + e->usize > z.size || memcmp(z.map + start, "PK\3\4", 4)) continue;
        packs_in_zip(z.map + start, e->usize, start, e->name);
    }
    tl_zip_close(&z);
    snprintf(g_pack_root, sizeof(g_pack_root), "%s/assetpacks", N.internal_dir);
    for (int i = 0; i < g_npack_files; i++) {
        char dir[900], path[1100];
        snprintf(dir, sizeof(dir), "%s/%s/assets", g_pack_root, g_pack_file[i].pack);
        mkdirs(dir);
        /* An empty file of that name, so the directory lists it; opening it reads the APK (matching is by name). */
        snprintf(path, sizeof(path), "%s/%s", dir, g_pack_file[i].file);
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        tl_vfile_add(g_pack_file[i].file, N.apk, g_pack_file[i].off, g_pack_file[i].size);
    }
}

/* play/asset_pack.h */
enum { PAD_NO_ERROR = 0, PAD_INVALID_REQUEST = -3 };
enum { PAD_COMPLETED = 4 };
enum { PAD_STORAGE_FILES = 0 };
enum { PAD_CONFIRM_APPROVED = 2 };
typedef struct { int status; uint64_t bytes; } pad_state;
typedef struct { char path[900]; } pad_location;

static int disk_pack(const char *name)
{
    for (int i = 0; i < g_ndisk_packs && name; i++) if (!strcmp(g_disk_pack[i].name, name)) return i;
    return -1;
}
static uint64_t pack_bytes(const char *name)
{
    uint64_t total = 0;
    for (int i = 0; i < g_npack_files && name; i++) if (!strcmp(g_pack_file[i].pack, name)) total += g_pack_file[i].size;
    int d = total ? -1 : disk_pack(name);
    return d >= 0 ? g_disk_pack[d].bytes : total;
}
static int pad_init(void *vm, void *context) { (void)vm; (void)context; return PAD_NO_ERROR; }
static void pad_destroy(void) {}
static int pad_ok(void) { return PAD_NO_ERROR; }
static int pad_request(const char **packs, size_t n)
{
    for (size_t i = 0; i < n && packs; i++) if (packs[i]) tl_log_line("ue4: Play Asset Delivery: the game asks for \"%s\" (%s)", packs[i], pack_bytes(packs[i]) ? "installed" : "not in the APK or the packs folder");
    return PAD_NO_ERROR;
}
static int pad_removal(const char *name) { (void)name; return PAD_NO_ERROR; }
static int pad_get_state(const char *name, pad_state **out)
{
    uint64_t bytes = pack_bytes(name);
    if (out) *out = NULL;
    if (!bytes || !out) return PAD_INVALID_REQUEST;
    pad_state *s = calloc(1, sizeof(*s));
    if (!s) return PAD_INVALID_REQUEST;
    s->status = PAD_COMPLETED; s->bytes = bytes;
    *out = s;
    return PAD_NO_ERROR;
}
static void pad_state_destroy(pad_state *s) { free(s); }
static int pad_state_status(pad_state *s) { return s ? s->status : 0; }
static uint64_t pad_state_bytes(pad_state *s) { return s ? s->bytes : 0; }
static int pad_show(void *activity) { (void)activity; return PAD_NO_ERROR; }
static int pad_confirm_status(int *out) { if (out) *out = PAD_CONFIRM_APPROVED; return PAD_NO_ERROR; }
static int pad_get_location(const char *name, pad_location **out)
{
    if (out) *out = NULL;
    if (!pack_bytes(name) || !out) return PAD_INVALID_REQUEST;
    pad_location *l = calloc(1, sizeof(*l));
    if (!l) return PAD_INVALID_REQUEST;
    int d = disk_pack(name);
    bool in_apk = false;
    for (int i = 0; i < g_npack_files && !in_apk; i++) in_apk = !strcmp(g_pack_file[i].pack, name);
    if (!in_apk && d >= 0) snprintf(l->path, sizeof(l->path), "%s", g_disk_pack[d].path);
    else snprintf(l->path, sizeof(l->path), "%s/%s/assets", g_pack_root, name);
    tl_log_line("ue4: Play Asset Delivery: pack \"%s\" is at %s", name, l->path);
    *out = l;
    return PAD_NO_ERROR;
}
static void pad_location_destroy(pad_location *l) { free(l); }
static int pad_location_storage(pad_location *l) { (void)l; return PAD_STORAGE_FILES; }
static const char *pad_location_path(pad_location *l) { return l ? l->path : ""; }

/* A function of the game's made to jump to one of Husk's: ldr x16, #8; br x16; the address. A one-instruction wrapper (b elsewhere) has its target
 * redirected instead, as it has no room. */
static bool redirect(tl_lib *lib, const char *symbol, void *to)
{
    uint8_t *rx = (uint8_t *)tl_ld_sym(lib, symbol);
    if (!rx) return false;
    uint32_t first; memcpy(&first, rx, 4);
    if ((first & 0xFC000000u) == 0x14000000u) rx += (int64_t)((int32_t)(first << 6) >> 6) * 4;
    uint32_t *rw = (uint32_t *)(rx + tl_xmem_delta());
    uint64_t a = (uint64_t)(uintptr_t)to;
    rw[0] = 0x58000050u;
    rw[1] = 0xD61F0200u;
    memcpy(rw + 2, &a, 8);
    tl_xmem_flush(rx, 16);
    return true;
}

/*
 * Compute shaders. The context is ES 3.0, which has none, and an engine told 3.1 dispatches them all the same, to clear a buffer if nothing else. Its program cannot
 * link, which it takes as a fatal error: it stops drawing and the last frame stays up. A dispatch does nothing here instead. What it would have written stays as it
 * was, so an effect built on one may be missing, but the game goes on. The count says how much a game leans on them.
 */
static void no_compute_note(void)
{
    static atomic_ulong dispatches;
    unsigned long n = atomic_fetch_add(&dispatches, 1) + 1, power = 1;
    while (power * 10 <= n) power *= 10;
    if (n == power) tl_log_line("ue4: compute dispatch #%lu skipped: OpenGL ES 3.0 has no compute shaders", n);
}
static void no_compute_dispatch(void *rhi, unsigned x, unsigned y, unsigned z) { (void)rhi; (void)x; (void)y; (void)z; no_compute_note(); }
static void no_compute_dispatch_indirect(void *rhi, void *arguments, unsigned offset) { (void)rhi; (void)arguments; (void)offset; no_compute_note(); }

static void answer_play_core(void)
{
    tl_lib *ue = tl_ld_find_lib(tl_ue_lib_name());
    if (!ue || (!g_npack_files && !g_ndisk_packs)) return;
    static const struct { const char *sym; void *fn; } k_pad[] = {
        { "AssetPackManager_init", (void *)pad_init }, { "AssetPackManager_destroy", (void *)pad_destroy },
        { "AssetPackManager_onResume", (void *)pad_ok }, { "AssetPackManager_onPause", (void *)pad_ok },
        { "AssetPackManager_requestInfo", (void *)pad_request }, { "AssetPackManager_requestDownload", (void *)pad_request },
        { "AssetPackManager_cancelDownload", (void *)pad_request }, { "AssetPackManager_requestRemoval", (void *)pad_removal },
        { "AssetPackManager_getDownloadState", (void *)pad_get_state }, { "AssetPackDownloadState_destroy", (void *)pad_state_destroy },
        { "AssetPackDownloadState_getStatus", (void *)pad_state_status }, { "AssetPackDownloadState_getBytesDownloaded", (void *)pad_state_bytes },
        { "AssetPackDownloadState_getTotalBytesToDownload", (void *)pad_state_bytes },
        { "AssetPackManager_showCellularDataConfirmation", (void *)pad_show }, { "AssetPackManager_getShowCellularDataConfirmationStatus", (void *)pad_confirm_status },
        { "AssetPackManager_showConfirmationDialog", (void *)pad_show }, { "AssetPackManager_getShowConfirmationDialogStatus", (void *)pad_confirm_status },
        { "AssetPackManager_getAssetPackLocation", (void *)pad_get_location }, { "AssetPackLocation_destroy", (void *)pad_location_destroy },
        { "AssetPackLocation_getStorageMethod", (void *)pad_location_storage }, { "AssetPackLocation_getAssetsPath", (void *)pad_location_path },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof(k_pad) / sizeof(k_pad[0]); i++) n += redirect(ue, k_pad[i].sym, k_pad[i].fn);
    if (n) tl_log_line("ue4: Play Asset Delivery answered here (%d of Play Core's functions): the packs the game came with are installed", n);
}

/*
 * Which graphics API a game's data has shaders for. A cooked Unreal game cannot compile shaders on the phone: it runs on an API its pak
 * has shaders for, or it stops as it starts (CompileGlobalShaderMap fails the check, and the engine quits -- Little Nightmares, asked for
 * Vulkan with shaders only for OpenGL ES). The pak's file index, after its data at the end of the file, names the shader files by format:
 * ...GLSL_ES3_1_ANDROID... (or GLSL_ES2) for OpenGL ES, ...SF_VULKAN_ES31_ANDROID... for Vulkan. An encrypted index names neither.
 * 'V' when there are Vulkan shaders, 'G' when only OpenGL ES ones, 0 when it cannot be told.
 */
static int pak_shader_api(void)
{
    const char *entry = getenv("TL_UE4_OBB_ENTRY") ? getenv("TL_UE4_OBB_ENTRY") : "assets/main.obb.png";
    unsigned long long off = 0, size = 0;
    const char *path = N.apk;
    struct stat st;
    if (!stored_range(N.apk, entry, &off, &size)) {
        if (stat(N.obb_file, &st) != 0) return 0;
        path = N.obb_file; off = 0; size = (unsigned long long)st.st_size;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    const unsigned long long tail = size < (64ull << 20) ? size : (64ull << 20), chunk = 4ull << 20, overlap = 64;
    char *buf = malloc((size_t)(chunk + overlap));
    bool gl = false, vk = false;
    for (unsigned long long pos = size - tail; buf && pos < size && !vk; pos += chunk) {
        unsigned long long want = size - pos < chunk + overlap ? size - pos : chunk + overlap;
        ssize_t got = pread(fd, buf, (size_t)want, (off_t)(off + pos));
        if (got <= 0) break;
        if (memmem(buf, (size_t)got, "SF_VULKAN", 9)) vk = true;
        if (memmem(buf, (size_t)got, "GLSL_ES", 7)) gl = true;
    }
    free(buf);
    close(fd);
    return vk ? 'V' : gl ? 'G' : 0;
}

/* Vulkan when MoltenVK is there and the game's data allows it: ANGLE over Metal speaks OpenGL ES 3.0, and the engine's ES 3.1 shaders run on it
 * only through the shim (tl_egl_es31_shim). TL_UE4_NO_VULKAN and TL_UE4_FORCE_VULKAN decide it outright. */
static void choose_api(void)
{
    N.vulkan = N.gl_only = false;
    if (getenv("TL_UE4_NO_VULKAN")) { N.gl_only = true; return; }
    if (getenv("TL_UE4_FORCE_VULKAN")) { N.vulkan = true; return; }
    if (!tl_vk_available()) return;
    int api = pak_shader_api();
    if (api == 'G') {
        N.gl_only = true;
        tl_log_line("ue4: the game's data has shaders for OpenGL ES and none for Vulkan: it runs on OpenGL ES (ANGLE)");
        return;
    }
    N.vulkan = true;
    tl_log_line(api == 'V' ? "ue4: the game's data has Vulkan shaders: it runs on Vulkan (MoltenVK)"
                           : "ue4: which shaders the game's data has cannot be told (an encrypted index?): it runs on Vulkan (MoltenVK)");
}

/* A native of GameActivity: registered by the library, or found by its JNI name. */
static void *native_of(const char *name, const char *sig)
{
    void *fn = tl_jni_native(CLS, name, sig);
    if (!fn) {
        tl_lib *lib = tl_ld_find_lib(tl_ue_lib_name());
        char mangled[160];
        snprintf(mangled, sizeof(mangled), "Java_com_epicgames_%s_GameActivity_%s", N.ue5 ? "unreal" : "ue4", name);
        if (lib) fn = tl_ld_sym(lib, mangled);
    }
    if (!fn) tl_log_line("ue4: native %s%s is not provided by %s", name, sig, tl_ue_lib_name());
    return fn;
}
#define NATIVE(name, sig) native_of(name, sig)


/*
 * Make a function of the loaded engine return a fixed value: `mov w0, #value; ret` over its first two instructions, written through the writable view of
 * the image. For trying things -- here, to ask an engine that has decided to use a graphics API we do not have to use the other one -- not for shipping.
 */
static bool patch_return(tl_lib *lib, const char *symbol, unsigned value)
{
    uint32_t *rx = (uint32_t *)tl_ld_sym(lib, symbol);
    if (!rx) { tl_log_line("ue4: no symbol %s to patch", symbol); return false; }
    uint32_t *rw = (uint32_t *)((uint8_t *)rx + tl_xmem_delta());
    rw[0] = 0x52800000u | (value << 5);          /* mov w0, #value */
    rw[1] = 0xD65F03C0u;                           /* ret */
    tl_xmem_flush(rx, 8);
    tl_log_line("ue4: %s now returns %u", symbol, value);
    return true;
}

/*
 * An engine changed to clamp its frame rate to a range of the project's (Little Nightmares: FrameRateClampingValues, 20 to 60, in its
 * DefaultEngine.ini) ends UEngine::GetMaxTickRate with the clamp: the upper bound's type is loaded (ldrb w8, [x19, #...]), compared with
 * Open (cmp w8, #2), and the rate is held under the bound, with FLT_MAX standing in for no rate. The load is made "Open" (mov w8, #2):
 * the game's own cap (t.MaxFPS) and Husk's 120 Hz hold it, as they do in an engine without the clamp.
 */
static void lift_tick_rate_clamp(tl_lib *lib)
{
    uint32_t *rx = (uint32_t *)tl_ld_sym(lib, "_ZNK7UEngine14GetMaxTickRateEfb");
    if (!rx) return;
    for (int i = 0; i < 200; i++) {
        if (rx[i] == 0xD65F03C0u) break;                                   /* ret: the end */
        if ((rx[i] & 0xFFC003FFu) != 0x39400268u || rx[i + 1] != 0x7100091Fu) continue;    /* ldrb w8, [x19, #imm]; cmp w8, #2 */
        bool flt_max = false;
        for (int j = i + 2; j < i + 8; j++) if (rx[j] == 0x12B01008u) flt_max = true;      /* movn w8, #0x8080, lsl #16 (FLT_MAX) */
        if (!flt_max) continue;
        uint32_t *rw = (uint32_t *)((uint8_t *)(rx + i) + tl_xmem_delta());
        rw[0] = 0x52800048u;                                                /* mov w8, #2 */
        tl_xmem_flush(rx + i, 4);
        tl_log_line("ue4: the engine's frame rate clamp (its upper bound) is lifted: the game's own cap and Husk's %d Hz hold it", tl_frame_hz());
        return;
    }
}

/* The OBB the engine looks for: <package>.obb in the OBB directory, named main.<version>.<package>.obb. */
#define OBB_VERSION 42

/* Controllers reach the engine as the NDK's own gamepad events: buttons as key events (source GAMEPAD), sticks and triggers as joystick motion events. */
void tl_inq_key(int device, int source, int action, int keycode);
void tl_inq_axes(int device, const float *axes48);
static void na_pad_key(jobj *ev, int device, int action, int keycode, int64_t down_ms, int64_t event_ms)
{
    (void)down_ms; (void)event_ms;
    tl_inq_key(device, 0x00000401, action, keycode);
    tl_jni_unref(ev);
}
static void na_pad_motion(jobj *ev, int device, int source, int64_t down_ms, int64_t event_ms)
{
    (void)source; (void)down_ms; (void)event_ms;
    float a[48];
    if (tl_input_event_axes(ev, a)) tl_inq_axes(device, a);
    tl_jni_unref(ev);
}

/* A NativeActivity game that is not Unreal: load its libraries (the manifest names one; here, whichever of the app's own libraries exports ANativeActivity_onCreate). */
struct na_libs { char name[6][64]; int n; };
static void na_lib_cb(const char *name, uint64_t size, void *user)
{
    (void)size;
    struct na_libs *l = user;
    static const char *const support[] = { "libc++_shared", "libSDL", "libopenal", "libsentry", "libFirebase", "libcrashlytics", "libandroidx" };
    for (size_t i = 0; i < sizeof(support) / sizeof(support[0]); i++) if (!strncmp(name, support[i], strlen(support[i]))) return;
    if (l->n < 6) snprintf(l->name[l->n++], 64, "%s", name);
}
static bool start_generic(void)
{
    struct na_libs libs = { .n = 0 };
    tl_ld_apk_libs(na_lib_cb, &libs);
    if (tl_ld_has_lib("libc++_shared.so")) load_library("c++_shared");
    for (int i = 0; i < libs.n; i++) {
        char base[64]; snprintf(base, sizeof(base), "%s", libs.name[i] + 3); base[strlen(base) - 3] = 0;
        load_library(base);
        if (tl_jni_pending()) { tl_log_line("na: loading %s failed", libs.name[i]); tl_jni_clear(); continue; }
        tl_lib *l = tl_ld_find_lib(libs.name[i]);
        if (l && tl_ld_sym(l, "ANativeActivity_onCreate")) { snprintf(N.main_lib, sizeof(N.main_lib), "%s", libs.name[i]); break; }
    }
    if (!N.main_lib[0]) { tl_log_line("na: no library exports ANativeActivity_onCreate"); return false; }
    tl_log_line("na: native activity in %s", N.main_lib);
    N.started = true;
    return true;
}

/*
 * An Unreal save that is empty: what a game ended while rewriting it leaves behind (before Husk made rewrites crash-safe). A real
 * one never is -- it starts with a GVAS header -- and Minecraft Dungeons waits forever on its title screen for an empty
 * GlobalSave.sav it cannot read. Taken away, the game makes a new one, and the other saves (its characters) stay.
 */
static void drop_empty_saves(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 10) drop_empty_saves(p, depth + 1); continue; }
        size_t n = strlen(e->d_name);
        if (S_ISREG(st.st_mode) && st.st_size == 0 && n > 4 && !strcmp(e->d_name + n - 4, ".sav") && strstr(dir, "/SaveGames")) {
            if (unlink(p) == 0) tl_log_line("ue4: removed an empty save, %s (left by a save cut off part-way)", p);
        }
    }
    closedir(d);
}

/* Hades has an activity of its own, and is driven by husk-tl-hades.c. */
static bool g_hades;

bool tl_na_start(const tl_ga_config *cfg)
{
    N.cfg = *cfg;
    snprintf(N.apk, sizeof(N.apk), "%s", cfg->apk_path);
    snprintf(N.data, sizeof(N.data), "%s", cfg->data_dir);
    snprintf(N.pkg, sizeof(N.pkg), "%s", cfg->package_name);
    N.cfg.apk_path = N.apk; N.cfg.data_dir = N.data; N.cfg.package_name = N.pkg;
    if (cfg->frame_dir) { snprintf(N.frame_dir, sizeof(N.frame_dir), "%s", cfg->frame_dir); N.cfg.frame_dir = N.frame_dir; }
    if (cfg->angle_egl) { snprintf(N.angle_egl, sizeof(N.angle_egl), "%s", cfg->angle_egl); N.cfg.angle_egl = N.angle_egl; }
    if (cfg->angle_gles) { snprintf(N.angle_gles, sizeof(N.angle_gles), "%s", cfg->angle_gles); N.cfg.angle_gles = N.angle_gles; }

    snprintf(N.internal_dir, sizeof(N.internal_dir), "%s/files", N.data);
    snprintf(N.ext_dir, sizeof(N.ext_dir), "%s/sdcard/Android/data/%s/files", N.data, N.pkg);
    tl_obb_folder(N.data, N.obb_dir, sizeof(N.obb_dir));
    snprintf(N.obb_file, sizeof(N.obb_file), "%s/main.%d.%s.obb", N.obb_dir, OBB_VERSION, N.pkg);
    snprintf(N.patch_file, sizeof(N.patch_file), "%s/patch.%d.%s.obb", N.obb_dir, OBB_VERSION, N.pkg);
    mkdirs(N.internal_dir); mkdirs(N.ext_dir); mkdirs(N.obb_dir);

    tl_set_data_dir(cfg->data_dir);
    tl_nwindow_configure(cfg->width, cfg->height, cfg->metal_layer);
    int n = tl_dexidx_open(cfg->apk_path);
    tl_log_line("ue4: %d classes in the APK's DEX", n);
    if (!tl_ld_add_apk(cfg->apk_path)) return false;
    if (tl_ld_has_lib("libhades.so")) {
        g_hades = true;
        return tl_hades_start(&N.cfg);
    }
    N.ue5 = !tl_ld_has_lib("libUE4.so") && tl_ld_has_lib("libUnreal.so");
    tl_ue_set_five(N.ue5);
    N.generic = !tl_ld_has_lib("libUE4.so") && !N.ue5;
    if (!N.generic) {
        unsigned long long po = 0, ps = 0;
        N.has_patch = stored_range(N.apk, "assets/patch.obb.png", &po, &ps);
        tl_egl_es31_shim(true);
        choose_api();
        tl_egl_offscreen_windows(N.vulkan);       /* with Vulkan, the layer is Vulkan's and GL draws off-screen */
        tl_vk_stand_in_for_swappy(N.ue5 && N.vulkan);
        tl_vk_no_pipeline_cache(N.ue5 && N.vulkan);
    }
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every)) return false;
    tl_jni_init();
    tl_hle_configure(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_jni_hle_install();
    if (N.generic) {
        tl_jni_declare("android/app/NativeActivity", "android/app/Activity");
        N.activity = tl_jni_new_object(tl_jni_class("android/app/NativeActivity"));
        tl_hle_set_activity(N.activity);
        return start_generic();
    }
    tl_ue4_hle_install(cfg->package_name, cfg->apk_path, cfg->data_dir, N.ext_dir);
    tl_ue4_set_screen(cfg->width, cfg->height);
    drop_empty_saves(N.data, 0);

    N.activity = tl_jni_new_object(tl_jni_class(CLS));
    tl_hle_set_activity(N.activity);
    tl_ue4_set_activity(N.activity);

    /* GameActivity's static initialiser, and NativeActivity's loading of the library its manifest names. */
    const char *const libs[] = { "c++_shared", "EOSSDK", N.ue5 ? "Unreal" : "UE4", NULL };
    /* Loaded and not started, so that its allocator can be made to fit before its first constructor asks it for memory. */
    if (N.ue5) { tl_lib *five = tl_ld_load(tl_ue_lib_name()); if (five) tl_ue_fit_allocator(five); }
    for (int i = 0; libs[i]; i++) {
        if (!strcmp(libs[i], "EOSSDK") && !tl_ld_has_lib("libEOSSDK.so")) continue;      /* Epic Online Services: only the games that use it ship it */
        if (!strcmp(libs[i], "c++_shared") && N.ue5 && !tl_ld_has_lib("libc++_shared.so")) continue;      /* Unreal Engine 5 builds it in */
        load_library(libs[i]);
        if (tl_jni_pending()) { tl_log_line("ue4: loading lib%s.so failed", libs[i]); return false; }
    }
    /* The API chosen above (choose_api), made the engine's: asked for Vulkan even by a project that defaults to OpenGL ES (bDetectVulkanByDefault off), which
     * Minecraft Dungeons is; kept off it when the game has no Vulkan shaders, or TL_UE4_NO_VULKAN says so. */
    {
        tl_lib *ue = tl_ld_find_lib(tl_ue_lib_name());
        if (ue && N.vulkan) { patch_return(ue, "_ZN12FAndroidMisc15ShouldUseVulkanEv", 1); patch_return(ue, "_ZN12FAndroidMisc17IsVulkanAvailableEv", 1); }
        if (ue && N.gl_only) { patch_return(ue, "_ZN12FAndroidMisc15ShouldUseVulkanEv", 0); patch_return(ue, "_ZN12FAndroidMisc22ShouldUseDesktopVulkanEv", 0); }
        if (ue && !N.vulkan) {
            redirect(ue, "_ZN17FOpenGLDynamicRHI24RHIDispatchComputeShaderEjjj", (void *)no_compute_dispatch);
            redirect(ue, "_ZN17FOpenGLDynamicRHI32RHIDispatchIndirectComputeShaderEP16FRHIVertexBufferj", (void *)no_compute_dispatch_indirect);
        }
        /* Over OpenGL ES the engine paces its frames itself, sleeping before each swap for its sync interval -- counted in sixtieths of a second
         * (rhi.SyncInterval, 1 unless a game asks otherwise), whatever the screen's refresh rate. A game held to 120 Hz here stopped at 60 with its
         * own cap set to 120 (Little Nightmares: t.MaxFPS 120). With no interval the game's cap (t.MaxFPS) and Husk's 120 Hz are what hold it. */
        if (ue && !N.vulkan && tl_frame_hz() > 60) patch_return(ue, "_Z18RHIGetSyncIntervalv", 0);
        if (ue && tl_frame_hz() > 60) lift_tick_rate_clamp(ue);
    }
    tl_log_line("ue4: libraries loaded");
    tl_ue4_watch();
    find_asset_packs();
    answer_play_core();
    setenv("TL_PAD_DPAD", "keys", 0);          /* the D-pad as DPAD_* keys, which the engine maps like a stick's arrows */
    static const tl_pad_sink sink = { na_pad_key, na_pad_motion };
    tl_pad_set_sink(&sink);
    N.started = true;
    return true;
}

/*
 * A console command for an Unreal game ("r.Fog 0"), asked for on any thread. GameActivity.nativeConsoleCommand takes it, and hands it to the game thread. The game's
 * code runs only on the threads set up for it, so the activity's message loop makes the call (ui_main). A graphics menu's choices come this way (ModGraphics.swift).
 */
enum { CONSOLE_QUEUE = 128 };
static pthread_mutex_t g_console_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_console[CONSOLE_QUEUE];
static int g_nconsole;

void husk_native_console_command(const char *command)
{
    if (!command || !command[0]) return;
    char *copy = strdup(command);
    if (!copy) return;
    pthread_mutex_lock(&g_console_mu);
    if (g_nconsole < CONSOLE_QUEUE) { g_console[g_nconsole++] = copy; copy = NULL; }
    pthread_mutex_unlock(&g_console_mu);
    if (copy) { tl_log_line("ue4: too many console commands waiting: \"%s\" is dropped", copy); free(copy); }
}

static void run_console_commands(void *env, jobj *activity)
{
    char *batch[CONSOLE_QUEUE];
    pthread_mutex_lock(&g_console_mu);
    int n = g_nconsole;
    memcpy(batch, g_console, sizeof(char *) * (size_t)n);
    g_nconsole = 0;
    pthread_mutex_unlock(&g_console_mu);
    if (!n) return;
    typedef void (*command_fn)(void *env, void *self, void *command);
    command_fn run = (command_fn)NATIVE("nativeConsoleCommand", "(Ljava/lang/String;)V");
    for (int i = 0; i < n; i++) {
        tl_log_line("ue4: console command: %s%s", batch[i], run ? "" : " (this engine does not take them)");
        if (run) run(env, activity, tl_jni_new_string(batch[i]));
        free(batch[i]);
    }
}

static void *ui_main(void *arg)
{
    (void)arg;
    pthread_setname_np("UiThread");
    ((void *(*)(int))tl_bionic_find("ALooper_prepare"))(0);
    void *env = tl_jni_env();
    jobj *activity = N.activity;

    /* GameActivity.onCreate, before it hands over to NativeActivity: the facts the engine wants to know. */
    if (!N.generic) {
        if (N.ue5) {
            /* It has one more than an older engine: the product's name. */
            typedef void (*ver5_fn)(void *env, void *self, void *release, int sdk, void *make, void *model, void *build, void *language, void *product);
            ver5_fn set_version = (ver5_fn)NATIVE("nativeSetAndroidVersionInformation", "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
            if (set_version) set_version(env, activity, tl_jni_new_string("14"), 34, tl_jni_new_string("Google"), tl_jni_new_string("Pixel 8"),
                                         tl_jni_new_string("UD1A.230803.041"), tl_jni_new_string("en-US"), tl_jni_new_string("shiba"));
        } else {
            typedef void (*ver_fn)(void *env, void *self, void *release, int sdk, void *a, void *b, void *c, void *d);
            ver_fn set_version = (ver_fn)NATIVE("nativeSetAndroidVersionInformation", "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
            if (set_version) set_version(env, activity, tl_jni_new_string("14"), 34, tl_jni_new_string("Google"), tl_jni_new_string("Pixel 8"),
                                         tl_jni_new_string("shiba"), tl_jni_new_string("google"));
        }
        typedef void (*glob_fn)(void *env, void *self, uint8_t ext, uint8_t pub, void *internal_path, void *external_path, uint8_t obb_in_apk, void *apk);
        glob_fn set_global = (glob_fn)NATIVE("nativeSetGlobalActivity", "(ZZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V");
        if (set_global) set_global(env, activity, 1, 1, tl_jni_new_string(N.internal_dir), tl_jni_new_string(N.ext_dir), 0, tl_jni_new_string(N.apk));
        typedef void (*obb_fn)(void *env, void *self, void *project, void *package, int version, int patch, void *apptype);
        obb_fn set_obb = (obb_fn)NATIVE("nativeSetObbInfo", "(Ljava/lang/String;Ljava/lang/String;IILjava/lang/String;)V");
        if (set_obb) set_obb(env, activity, tl_jni_new_string(tl_ue4_meta("com.epicgames.ue4.GameActivity.ProjectName") ? tl_ue4_meta("com.epicgames.ue4.GameActivity.ProjectName") : "ShooterGame"), tl_jni_new_string(N.pkg), OBB_VERSION, N.has_patch ? OBB_VERSION : 0, tl_jni_new_string(""));
        typedef void (*obbp_fn)(void *env, void *self, void *a, void *b, void *c, void *d);
        obbp_fn set_obb_paths = (obbp_fn)NATIVE("nativeSetObbFilePaths", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        if (set_obb_paths) set_obb_paths(env, activity, tl_jni_new_string(N.obb_file), tl_jni_new_string(N.has_patch ? N.patch_file : ""), tl_jni_new_string(""), tl_jni_new_string(""));
        typedef void (*rules_fn)(void *env, void *self, void *vars);
        rules_fn set_rules = (rules_fn)NATIVE("nativeSetConfigRulesVariables", "([Ljava/lang/String;)V");
        if (set_rules) set_rules(env, activity, tl_jni_new_obj_array(tl_jni_class("java/lang/String"), 0));

    }

    /* NativeActivity: the system calls the library's entry point with the activity it describes. */
    tl_lib *lib = tl_ld_find_lib(N.generic ? N.main_lib : tl_ue_lib_name());
    void (*on_create)(ANativeActivity *, void *, size_t) = lib ? tl_ld_sym(lib, "ANativeActivity_onCreate") : NULL;
    if (!on_create) { tl_log_line("na: the library has no ANativeActivity_onCreate"); return NULL; }
    jobj *assets = tl_hle_assets();
    void *(*from_java)(void *, void *) = tl_bionic_find("AAssetManager_fromJava");
    N.na.callbacks = &N.callbacks;
    N.na.vm = tl_jni_vm(); N.na.env = env; N.na.clazz = activity;
    N.na.internalDataPath = N.internal_dir; N.na.externalDataPath = N.ext_dir;
    N.na.sdkVersion = 34;
    N.na.assetManager = from_java ? from_java(env, assets) : NULL;
    N.na.obbPath = N.obb_dir;
    tl_log_line("ue4: ANativeActivity_onCreate");
    on_create(&N.na, NULL, 0);
    if (tl_jni_pending()) { tl_log_line("ue4: a Java exception is pending after onCreate"); tl_jni_clear(); }
    tl_log_line("ue4: onCreate returned; callbacks start=%p resume=%p window=%p", (void *)N.callbacks.onStart, (void *)N.callbacks.onResume, (void *)N.callbacks.onNativeWindowCreated);

    N.window = tl_nwindow_get();
    if (N.callbacks.onStart) N.callbacks.onStart(&N.na);
    if (N.callbacks.onResume) N.callbacks.onResume(&N.na);
    if (N.callbacks.onInputQueueCreated) N.callbacks.onInputQueueCreated(&N.na, (void *)&N);          /* a queue nothing arrives through */
    if (!N.generic) {
        /* Unreal Engine 5 also takes whether the window's alpha is passed on: it is not. An older engine takes no notice of the third. */
        typedef void (*win_fn)(void *env, void *self, uint8_t portrait, int depth, int propagate_alpha);
        win_fn set_window_info = (win_fn)NATIVE("nativeSetWindowInfo", N.ue5 ? "(ZII)V" : "(ZI)V");
        if (set_window_info) set_window_info(env, activity, N.cfg.width < N.cfg.height, 24, 0);
        typedef void (*sv_fn)(void *env, void *self, int w, int h);
        sv_fn set_surface = (sv_fn)NATIVE("nativeSetSurfaceViewInfo", "(II)V");
        if (set_surface) set_surface(env, activity, N.cfg.width, N.cfg.height);
    }
    if (N.callbacks.onNativeWindowCreated) N.callbacks.onNativeWindowCreated(&N.na, N.window);
    if (N.callbacks.onNativeWindowResized) N.callbacks.onNativeWindowResized(&N.na, N.window);
    if (N.callbacks.onWindowFocusChanged) N.callbacks.onWindowFocusChanged(&N.na, 1);
    if (!N.generic) {
        typedef void (*void_fn)(void *env, void *self);
        void_fn resume_init = (void_fn)NATIVE("nativeResumeMainInit", "()V");
        if (resume_init) resume_init(env, activity);
    }
    tl_log_line("ue4: lifecycle delivered");

    /* The activity's message loop: console commands asked for, and otherwise nothing to serve beyond keeping the thread (and its looper) alive. */
    int (*poll_once)(int, int *, int *, void **) = tl_bionic_find("ALooper_pollOnce");
    for (;;) {
        poll_once(200, NULL, NULL, NULL);
        if (!N.generic) run_console_commands(env, activity);
        if (tl_jni_pending()) tl_jni_clear();
    }
    return NULL;
}

bool tl_na_run(void)
{
    if (g_hades) return tl_hades_run();
    if (!N.started) return false;
    /* The OBB is inside the APK: say where, so the engine finds it where it expects. */
    if (!N.generic) {
        unsigned long long off = 0, size = 0;
        const char *entry = getenv("TL_UE4_OBB_ENTRY") ? getenv("TL_UE4_OBB_ENTRY") : "assets/main.obb.png";
        char name[200]; snprintf(name, sizeof(name), "main.%d.%s.obb", OBB_VERSION, N.pkg);
        if (stored_range(N.apk, entry, &off, &size)) { tl_vfile_add(name, N.apk, off, size); tl_log_line("ue4: OBB %s is %llu bytes at %llu of the APK", name, size, off); }
        else tl_log_line("ue4: the APK has no stored %s; the engine will find no OBB", entry);
        /* A second one, for a game too big for one: read the same way. */
        snprintf(name, sizeof(name), "patch.%d.%s.obb", OBB_VERSION, N.pkg);
        if (N.has_patch && stored_range(N.apk, "assets/patch.obb.png", &off, &size)) { tl_vfile_add(name, N.apk, off, size); tl_log_line("ue4: OBB %s is %llu bytes at %llu of the APK", name, size, off); }
    }
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    if (pthread_create(&N.ui, &a, ui_main, NULL) != 0) return false;
    return true;
}

unsigned long tl_vk_frames_presented(void);
unsigned long tl_na_frames(void) { return tl_egl_frames_presented() + tl_vk_frames_presented(); }

void tl_inq_touch(int phase, int id, float x, float y);
void tl_na_touch(int phase, int id, float x, float y) { if (g_hades) tl_hades_touch(phase, id, x, y); else tl_inq_touch(phase, id, x, y); }
/* The app left the screen or came back: the engine is told as an activity is -- onPause and the window losing focus, onResume and gaining it -- so it stops drawing and
 * silences its audio instead of feeding a GPU iOS will no longer let it have. Only real changes are passed on. */
void tl_na_set_paused(bool paused)
{
    if (g_hades) { tl_hades_set_paused(paused); return; }
    static atomic_bool is_paused;
    if (!N.started || !N.callbacks.onResume) return;
    if (atomic_exchange(&is_paused, paused) == paused) return;
    tl_log_line("ue4: %s", paused ? "pausing" : "resuming");
    if (paused) {
        if (N.callbacks.onWindowFocusChanged) N.callbacks.onWindowFocusChanged(&N.na, 0);
        if (N.callbacks.onPause) N.callbacks.onPause(&N.na);
    } else {
        if (N.callbacks.onResume) N.callbacks.onResume(&N.na);
        if (N.callbacks.onWindowFocusChanged) N.callbacks.onWindowFocusChanged(&N.na, 1);
    }
}
