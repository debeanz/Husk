/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Game data that comes with an APK rather than in it: put where the game will look, before the game loads, for every engine.
 * On Android the app's own Java launcher or the Play Store does this. No Java launcher runs here.
 *
 *   ZIPs of app data     assets/data0, data1, ..., data.bin, anything.zip: at the top of assets/, unpacked into the data folder.
 *                        Their entries may be laid out as the app's folder (files/, shared_prefs/, data/data/<package>/, <package>/)
 *                        or shared storage (Android/obb/, Android/data/, sdcard/, storage/emulated/0/, obb/, a bare main.1.pkg.obb).
 *   OBBs in the APK      assets/...x.obb, Android/obb/..., obb/...: unpacked into the OBB folder.
 *   Asset packs          a bundle's split APK that holds only ZIPs of assetpacks/<pack>/... or files/assetpacks/<pack>/...: unpacked into the
 *                        packs folder as the bundle is added, and the APK not kept (husk_tl_bundle_unpack, below).
 *
 * The OBB folder is <the game's folder>/obb, the one place a game's OBBs are: where those out of an .xapk, added with the APK or added
 * by hand go, what the game is told (getObbDir), and where Android's path for it leads (tl_path_resolve). It is not in the data
 * folder, so that clearing the data or backing it up leaves the OBBs out.
 *
 * An asset of such a name is not always this. One that is not a ZIP, or a ZIP with nothing laid out as above, is the game's own
 * to read (through AAssetManager, as ever) and is left alone.
 *
 * Each ZIP is done once. A stamp in .husk-unpacked/ names its sizes and the files it put in place (save backups leave those out,
 * as they can be made again) and goes last. With no stamp, because an unpacking was cut short or a backup was restored, only
 * what is missing is unpacked, so nothing the game wrote is lost; a stamp for other sizes is a new version of the app, whose files replace the old.
 */
#define _DARWIN_C_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "husk-tl.h"
#include "husk-tl-bionic.h"
#include "husk-tl-internal.h"
#include "husk-tl-ld.h"

const char *tl_data_dir(void);
bool tl_data_dir_is_set(void);

#define SLACK (256ull << 20)            /* free space to leave besides what is written */
#define STAMPS ".husk-unpacked"

static char g_pkg[128];

/* ------------------------------------------------------------------ small things */

/* Every folder on the way to `path`; the last component too if `whole`. */
static void make_dirs(const char *path, bool whole)
{
    char t[1200];
    snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    if (whole) mkdir(t, 0755);
}

static long long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) if (tolower((unsigned char)s[a - b + i]) != suffix[i]) return false;
    return true;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* If `*p` starts with `prefix`, step over it. */
static bool skip_prefix(const char **p, const char *prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(*p, prefix, n)) return false;
    *p += n;
    return true;
}

/* The ZIP is someone else's: nothing in it may name a place outside the folder it is unpacked into. */
static bool safe_name(const char *name)
{
    if (!name[0] || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    for (const char *p = name;;) {
        const char *s = strchr(p, '/');
        size_t len = s ? (size_t)(s - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return false;
        if (!s) return true;
        p = s + 1;
    }
}

/* The free space where `folder` is, or as much as there could be when it cannot be told. */
static unsigned long long free_bytes(const char *folder)
{
    struct statfs sf;
    return statfs(folder, &sf) == 0 ? (unsigned long long)sf.f_bavail * sf.f_bsize : ~0ull - SLACK;
}

/* Whether `need` bytes fit, and the reason for the player if they do not. */
static bool has_room(unsigned long long need, char *why, size_t whylen)
{
    unsigned long long have = free_bytes(tl_data_dir());
    if (have >= need + SLACK) return true;
    snprintf(why, whylen, "The game's data needs %.2f GB of free space and the phone has %.2f GB. Free some space and start it again.", (need + SLACK) / 1e9, have / 1e9);
    return false;
}

/* ---------------------------------------------------------------- the package */

/* Read from the manifest, so that an engine has nothing to set up for any of this. */
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool pool_string(const uint8_t *pool, size_t size, uint32_t index, char *out, size_t n)
{
    if (size < 28 || index >= r32(pool + 8)) return false;
    uint32_t flags = r32(pool + 16), start = r32(pool + 20);
    size_t table = r16(pool + 2) + (size_t)index * 4;
    if (table + 4 > size) return false;
    size_t at = (size_t)start + r32(pool + table);
    size_t k = 0;
    if (flags & 0x100) {                                        /* UTF-8: its length in characters, then in bytes, then the bytes */
        if (at + 2 > size) return false;
        at += (pool[at] & 0x80) ? 2 : 1;
        if (at + 2 > size) return false;
        size_t len = pool[at];
        if (len & 0x80) len = ((len & 0x7f) << 8) | pool[at + 1], at += 2; else at += 1;
        if (at + len > size) return false;
        for (; k < len && k + 1 < n; k++) out[k] = (char)pool[at + k];
    } else {                                                    /* UTF-16 */
        if (at + 2 > size) return false;
        size_t len = r16(pool + at);
        at += 2;
        if (len & 0x8000) { if (at + 2 > size) return false; len = ((len & 0x7fff) << 16) | r16(pool + at); at += 2; }
        if (at + len * 2 > size) return false;
        for (; k < len && k + 1 < n; k++) out[k] = (char)r16(pool + at + k * 2);
    }
    out[k] = 0;
    return true;
}

static void read_package(const tl_zip *apk)
{
    const tl_zip_entry *e = tl_zip_find(apk, "AndroidManifest.xml");
    const uint8_t *data; size_t len; bool owned = false; char err[160];
    if (!e || !tl_zip_data(apk, e, 8u << 20, &data, &len, &owned, err, sizeof(err))) return;
    if (len > 8 && r16(data) == 0x0003) {
        const uint8_t *pool = NULL; size_t pool_size = 0;
        for (size_t off = r16(data + 2); off + 8 <= len; ) {
            uint16_t type = r16(data + off); uint32_t size = r32(data + off + 4);
            if (size < 8 || off + size > len) break;
            if (type == 0x0001) { pool = data + off; pool_size = size; }
            else if (type == 0x0102 && pool && size >= 36) {     /* the first element is <manifest> */
                const uint8_t *el = data + off;
                uint16_t astart = r16(el + 24), asize = r16(el + 26), acount = r16(el + 28);
                for (unsigned i = 0; i < acount; i++) {
                    size_t rel = 16 + (size_t)astart + (size_t)i * asize;
                    if (rel + 20 > size) break;
                    char name[32];
                    if (pool_string(pool, pool_size, r32(el + rel + 4), name, sizeof(name)) && !strcmp(name, "package"))
                        pool_string(pool, pool_size, r32(el + rel + 8), g_pkg, sizeof(g_pkg));
                }
                break;
            }
            off += size;
        }
    }
    if (owned) free((void *)data);
    /* It becomes part of paths. */
    if (!safe_name(g_pkg) || strchr(g_pkg, '/')) g_pkg[0] = 0;
}

/* ---------------------------------------------------------------------- OBBs */

/* A game's OBB folder, from its data folder: they sit side by side in the game's own folder. */
void tl_obb_folder(const char *data_dir, char *out, size_t n)
{
    const char *slash = strrchr(data_dir, '/');
    snprintf(out, n, "%.*s/obb", slash ? (int)(slash - data_dir) : 0, data_dir);
}

/* Its asset packs' folder, in its own folder too: where a bundle's packs are put as it is added (husk_tl_bundle_unpack). */
#define PACKS_FOLDER "assetpacks"
void tl_packs_folder(const char *data_dir, char *out, size_t n)
{
    const char *slash = strrchr(data_dir, '/');
    snprintf(out, n, "%.*s/" PACKS_FOLDER, slash ? (int)(slash - data_dir) : 0, data_dir);
}

/*
 * Android's OBB folder used to be kept under the data folder as well, holding a second name for each OBB beside the APK, which looked
 * like a second copy. What is there moves to the one folder: a name the OBB folder already has was only that second name, and goes.
 */
static void retire_old_obb_folder(void)
{
    static bool done;
    if (done || !g_pkg[0]) return;
    done = true;
    char old[1200], obb[1200];
    if ((size_t)snprintf(old, sizeof(old), "%s/sdcard/Android/obb/%s", tl_data_dir(), g_pkg) >= sizeof(old)) return;
    DIR *d = opendir(old);
    if (!d) return;
    tl_obb_folder(tl_data_dir(), obb, sizeof(obb));
    make_dirs(obb, true);
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char from[1600], to[1600];
        snprintf(from, sizeof(from), "%s/%s", old, ent->d_name);
        snprintf(to, sizeof(to), "%s/%s", obb, ent->d_name);
        if (file_size(to) >= 0) unlink(from);
        else if (rename(from, to) != 0) tl_log_line("bundled: cannot move %s to the OBB folder: %s", ent->d_name, strerror(errno));
    }
    closedir(d);
    rmdir(old);
    snprintf(old, sizeof(old), "%s/sdcard/Android/obb", tl_data_dir());
    rmdir(old);
}

/* An OBB in the APK: unpacked into the OBB folder unless it is there already. False only if it could not be. */
static bool place_obb(const tl_zip *apk, const tl_zip_entry *e, char *why, size_t whylen)
{
    const char *base = base_name(e->name);
    if (!base[0] || !safe_name(base)) { tl_log_line("bundled: skipping %s, which points outside the app's folder", e->name); return true; }
    if (!g_pkg[0]) { tl_log_line("bundled: %s has no OBB folder to go in, as the APK names no package", e->name); return true; }
    char obb[1200], to[1600], err[300] = "";
    tl_obb_folder(tl_data_dir(), obb, sizeof(obb));
    if ((size_t)snprintf(to, sizeof(to), "%s/%s", obb, base) >= sizeof(to)) { snprintf(why, whylen, "The path of %s is too long.", e->name); return false; }
    if (file_size(to) == (long long)e->usize) return true;
    if (!has_room(e->usize, why, whylen)) return false;
    tl_log_line("bundled: unpacking %s (%.2f GB) into the OBB folder, which only happens once", e->name, e->usize / 1e9);
    make_dirs(to, false);
    if (!tl_zip_extract(apk, e, to, err, sizeof(err))) { snprintf(why, whylen, "Could not unpack the game's data: %s", err); return false; }
    return true;
}

/* ------------------------------------------------------------- ZIPs of app data */

/* Whether the entry's first bytes are a ZIP's, inflating only those if the APK compressed it. */
static bool is_zip(const tl_zip *apk, const tl_zip_entry *e)
{
    static const uint8_t magic[4] = { 'P', 'K', 3, 4 };
    const uint8_t *raw = tl_zip_raw(apk, e);
    if (!raw || e->usize < sizeof(magic)) return false;
    if (e->method == 0) return e->csize >= sizeof(magic) && !memcmp(raw, magic, sizeof(magic));
    if (e->method != 8) return false;
    uint8_t head[sizeof(magic)];
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = (Bytef *)raw;
    zs.avail_in = e->csize > (1u << 30) ? (1u << 30) : (uInt)e->csize;
    zs.next_out = head;
    zs.avail_out = sizeof(head);
    inflate(&zs, Z_SYNC_FLUSH);
    bool ok = zs.avail_out == 0 && !memcmp(head, magic, sizeof(magic));
    inflateEnd(&zs);
    return ok;
}

/* The OBB folder, relative to the data folder beside it. */
#define OBB_ROUTE "../obb/"

/* Where an entry goes, relative to the data folder: what Android's paths for it are under Husk's. False if it does not fit `out`. */
static bool route(const char *name, char *out, size_t n)
{
    const char *p = name;
    char pre[160];
    skip_prefix(&p, "./");
    /* The app's own folder, however it is named. */
    snprintf(pre, sizeof(pre), "data/data/%s/", g_pkg);
    bool app = g_pkg[0] && skip_prefix(&p, pre);
    if (!app) { snprintf(pre, sizeof(pre), "data/user/0/%s/", g_pkg); app = g_pkg[0] && skip_prefix(&p, pre); }
    if (!app) { snprintf(pre, sizeof(pre), "%s/", g_pkg); app = g_pkg[0] && skip_prefix(&p, pre); }     /* or the OBB folder, zipped */
    /* A bare OBB, whichever of those it was in. */
    if (!strchr(p, '/') && ends_with(p, ".obb")) return (size_t)snprintf(out, n, OBB_ROUTE "%s", p) < n;
    if (app) return (size_t)snprintf(out, n, "%s", p) < n;
    /* Shared storage. */
    if (skip_prefix(&p, "sdcard/") || skip_prefix(&p, "storage/emulated/0/") || !strncmp(p, "Android/", 8)) {
        /* This game's OBB folder is its own; another app's stays where that app would look. */
        snprintf(pre, sizeof(pre), "Android/obb/%s/", g_pkg);
        if (g_pkg[0] && skip_prefix(&p, pre)) return (size_t)snprintf(out, n, OBB_ROUTE "%s", p) < n;
        return (size_t)snprintf(out, n, "sdcard/%s", p) < n;
    }
    if (skip_prefix(&p, "obb/")) {
        snprintf(pre, sizeof(pre), "%s/", g_pkg);
        if (g_pkg[0]) skip_prefix(&p, pre);
        return (size_t)snprintf(out, n, OBB_ROUTE "%s", p) < n;
    }
    return (size_t)snprintf(out, n, "%s", p) < n;
}

/* Whether a ZIP is laid out as app data at all. */
static bool is_app_data(const tl_zip *z)
{
    static const char *const roots[] = { "files/", "shared_prefs/", "databases/", "no_backup/", "sdcard/", OBB_ROUTE };
    for (size_t i = 0; i < z->count; i++) {
        char rel[1024];
        if (!safe_name(z->entries[i].name) || !route(z->entries[i].name, rel, sizeof(rel))) continue;
        for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) if (!strncmp(rel, roots[r], strlen(roots[r]))) return true;
    }
    return false;
}

static void write_stamp(const char *stamp, const char *line)
{
    FILE *f = fopen(stamp, "w");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

/* Everything in the ZIP to its place. `replace`: over what is there; otherwise only what is missing. The stamp goes last. */
static bool unpack_entries(const tl_zip *z, bool replace, const char *stamp, const char *line, char *why, size_t whylen)
{
    const char *data = tl_data_dir();
    /* What will be written, first: a failure for lack of room should come before anything is changed, not halfway. */
    unsigned long long need = 0;
    for (size_t i = 0; i < z->count; i++) {
        const tl_zip_entry *e = &z->entries[i];
        char rel[1024], to[1200];
        if (!safe_name(e->name) || !route(e->name, rel, sizeof(rel))) continue;
        snprintf(to, sizeof(to), "%s/%s", data, rel);
        if (replace || file_size(to) < 0) need += e->usize;
    }
    if (!has_room(need, why, whylen)) return false;

    char list_path[1100];
    snprintf(list_path, sizeof(list_path), "%s.part", stamp);
    FILE *list = fopen(list_path, "w");
    if (list) fprintf(list, "%s\n", line);
    size_t files = 0, written = 0;
    bool ok = true;
    unlink(stamp);
    for (size_t i = 0; i < z->count && ok; i++) {
        const tl_zip_entry *e = &z->entries[i];
        char rel[1024], to[1200], err[300] = "";
        if (!safe_name(e->name)) { tl_log_line("bundled: skipping %s, which points outside the app's folder", e->name); continue; }
        if (!route(e->name, rel, sizeof(rel)) || (size_t)snprintf(to, sizeof(to), "%s/%s", data, rel) >= sizeof(to)) { snprintf(why, whylen, "The path of %s is too long.", e->name); ok = false; break; }
        size_t len = strlen(to);
        if (to[len - 1] == '/') { to[len - 1] = 0; make_dirs(to, true); continue; }
        files++;
        if (list) fprintf(list, "%s\n", rel);
        if (!replace && file_size(to) >= 0) continue;
        make_dirs(to, false);
        if (e->usize > (64u << 20)) tl_log_line("bundled:   %s (%.2f GB)", rel, e->usize / 1e9);
        if (!tl_zip_extract(z, e, to, err, sizeof(err))) { snprintf(why, whylen, "Could not unpack the game's data: %s", err); ok = false; }
        else written++;
    }
    if (list) fclose(list);
    if (ok && (!list || rename(list_path, stamp) != 0)) write_stamp(stamp, line);
    if (!ok) unlink(list_path);
    tl_log_line("bundled: %zu of %zu files unpacked", written, files);
    return ok;
}

/* One asset. False only when it had to be unpacked and could not be: the game cannot start without it. `sure`: named for this. */
static bool unpack_one(const tl_zip *apk, const tl_zip_entry *e, bool sure, char *why, size_t whylen)
{
    const char *data = tl_data_dir();
    char stamp[1024], want[64], have[64] = "";
    snprintf(stamp, sizeof(stamp), "%s/%s/%s", data, STAMPS, e->name + strlen("assets/"));
    snprintf(want, sizeof(want), "%llu %llu", (unsigned long long)e->csize, (unsigned long long)e->usize);
    FILE *f = fopen(stamp, "r");
    if (f) { if (!fgets(have, sizeof(have), f)) have[0] = 0; fclose(f); have[strcspn(have, "\r\n")] = 0; }
    if (!strcmp(have, want)) return true;

    if (!is_zip(apk, e)) { if (sure) tl_log_line("bundled: %s is not a ZIP, so it is left for the game to read", e->name); return true; }
    char stamps[1100];
    snprintf(stamps, sizeof(stamps), "%s/%s", data, STAMPS);
    make_dirs(stamps, true);

    /* Read where it lies when the APK stores it. Compressed, it has to be written out first, as a ZIP is read by seeking. */
    tl_zip z;
    char err[300] = "", copy[1100] = "";
    bool opened;
    if (e->method == 0) {
        opened = tl_zip_open_mem(&z, tl_zip_raw(apk, e), e->csize, err, sizeof(err));
    } else {
        snprintf(copy, sizeof(copy), "%s/.husk-unpacking.zip", stamps);
        char room[300] = "";
        if (!has_room(e->usize, room, sizeof(room)) || !tl_zip_extract(apk, e, copy, err, sizeof(err))) {
            if (sure) { snprintf(why, whylen, "%s", room[0] ? room : err); return false; }
            tl_log_line("bundled: %s stays in the APK: %s", e->name, room[0] ? room : err);
            return true;
        }
        opened = tl_zip_open(&z, copy, err, sizeof(err));
    }
    bool ok = true;
    if (!opened) {
        tl_log_line("bundled: %s cannot be read as a ZIP (%s), so it is left for the game to read", e->name, err);
        write_stamp(stamp, want);
    } else {
        if (!is_app_data(&z)) {
            tl_log_line("bundled: %s is not laid out as app data, so it is left for the game to read", e->name);
            write_stamp(stamp, want);
        } else {
            tl_log_line("bundled: unpacking %s (%zu entries), which only happens once", e->name, z.count);
            ok = unpack_entries(&z, have[0] != 0, stamp, want, why, whylen);
        }
        tl_zip_close(&z);
    }
    if (copy[0]) unlink(copy);
    return ok;
}

/* ---------------------------------------------------------------------- the APKs */

enum kind { OTHER, OBB, ZIP_NAMED, ZIP_MAYBE };

static enum kind classify(const char *name)
{
    const char *p = name;
    if (ends_with(name, "/")) return OTHER;
    if (skip_prefix(&p, "assets/")) {
        if (ends_with(p, ".obb")) return OBB;
        if (strchr(p, '/')) return OTHER;
        if (ends_with(p, ".zip")) return ZIP_MAYBE;
        if (strncmp(p, "data", 4)) return OTHER;
        const char *q = p + 4;
        while (*q >= '0' && *q <= '9') q++;
        if (!*q) return q > p + 4 ? ZIP_NAMED : ZIP_MAYBE;      /* data0, and data */
        return *q == '.' ? ZIP_MAYBE : OTHER;                   /* data.bin, data0.dat */
    }
    return !strncmp(name, "Android/obb/", 12) || !strncmp(name, "obb/", 4) ? OBB : OTHER;
}

static bool scan_apk(int index, const tl_zip *apk, char *why, size_t whylen)
{
    if (!g_pkg[0]) read_package(apk);
    retire_old_obb_folder();
    for (size_t i = 0; i < apk->count; i++) {
        const tl_zip_entry *e = &apk->entries[i];
        enum kind k = classify(e->name);
        if (k == OTHER) continue;
        /* The same name in two APKs is the first's, as for any asset. */
        bool taken = false;
        for (int a = 0; a < index && !taken; a++) taken = tl_zip_find(tl_ld_apk_at(a), e->name) != NULL;
        if (taken) continue;
        if (!(k == OBB ? place_obb(apk, e, why, whylen) : unpack_one(apk, e, k == ZIP_NAMED, why, whylen))) return false;
    }
    return true;
}

bool tl_bundled_data_unpack(void)
{
    /* APKs can be added before the engine has said where its data goes: they are looked at once it has. */
    static int next;
    if (!tl_data_dir_is_set()) return true;
    for (const tl_zip *apk; (apk = tl_ld_apk_at(next)); next++) {
        char why[600] = "";
        if (!scan_apk(next, apk, why, sizeof(why))) {
            tl_log_line("bundled: %s", why);
            if (tl_game_failed_hook) tl_game_failed_hook(why);
            return false;
        }
    }
    return true;
}

/* ----------------------------------------------------------- asset packs, at import */

/*
 * Play Asset Delivery's packs, in a bundle. Google Play delivers a big game's data as asset packs, which Play Core installs under the app's files folder
 * (assetpacks/<pack>/<version>/<version>/assets/) and tells the game where. A bundle made without Google Play carries them itself, zipped in that layout, in
 * split APKs that hold nothing else, for its own Java to unzip on the first start. Two versions of Sifu's show the two ways of it, both 8.5 GB unzipped:
 *
 *   split1.apk..split4.apk hold assets/cache534/pakchunk1.zip..pakchunk39.zip, each a stored ZIP of assetpacks/...
 *   split1.apk..split4.apk hold assets/split1..split4, each a ZIP of files/assetpacks/... (the packs of a phone, as they lay) that the APK compresses whole
 *
 * No Java runs here, so it is done as the bundle is added: every pack goes into the game's packs folder, beside its OBB folder and, like that, not in the data
 * folder, so that clearing the data or backing it up leaves them alone. The APK that carried them is not kept, as all it holds is a second, zipped copy. The
 * runtime answers Play Core from that folder (husk-tl-nativeactivity.c).
 */

/* What an APK holds besides its contents: its manifest, signature and resource table. */
static bool apk_frame(const char *name)
{
    return !strncmp(name, "META-INF/", 9) || !strcmp(name, "AndroidManifest.xml") || !strcmp(name, "resources.arsc")
        || !strcmp(name, "stamp-cert-sha256") || ends_with(name, "/");
}

/* Where a zipped file goes, from the game's folder: assetpacks/..., with or without the files/ of the app's own folder in front. NULL if it is not a pack's. */
static const char *pack_path(const char *name)
{
    const char *p = name;
    skip_prefix(&p, "files/");
    return !strncmp(p, "assetpacks/", 11) && safe_name(p) ? p : NULL;
}

/* A ZIP with files in it, every one of them a pack's. Its folders (files/, _tmp/) are not looked at, as only files are written. */
static bool is_pack_zip(const tl_zip *z)
{
    size_t files = 0;
    for (size_t i = 0; i < z->count; i++) {
        const char *n = z->entries[i].name;
        if (ends_with(n, "/")) continue;
        if (!pack_path(n)) return false;
        files++;
    }
    return files > 0;
}

/* An entry of the APK that is a stored ZIP of packs, opened where it lies. */
static bool open_pack_zip(const tl_zip *apk, const tl_zip_entry *e, tl_zip *out)
{
    char err[160];
    const uint8_t *raw = e->method == 0 ? tl_zip_raw(apk, e) : NULL;
    if (!raw || !tl_zip_open_mem(out, raw, e->csize, err, sizeof(err))) return false;
    if (is_pack_zip(out)) return true;
    tl_zip_close(out);
    return false;
}

/* Whether an entry the APK compresses is a ZIP that starts with a pack's file. Only its start is inflated, as it can be gigabytes: the ZIP's own list of
 * files is at its end, so its first local headers are what can be read. The whole of it is checked once it is written out (take_packs). */
static bool compressed_pack_zip(const tl_zip *apk, const tl_zip_entry *e)
{
    enum { PEEK = 1 << 16 };
    const uint8_t *raw = e->method == 8 ? tl_zip_raw(apk, e) : NULL;
    if (!raw) return false;
    uint8_t *head = malloc(PEEK);
    if (!head) return false;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    size_t got = 0;
    if (inflateInit2(&zs, -MAX_WBITS) == Z_OK) {
        zs.next_in = (Bytef *)raw;
        zs.avail_in = e->csize > (1u << 30) ? (1u << 30) : (uInt)e->csize;
        zs.next_out = head;
        zs.avail_out = PEEK;
        inflate(&zs, Z_SYNC_FLUSH);
        got = PEEK - zs.avail_out;
        inflateEnd(&zs);
    }
    bool pack = false;
    for (size_t at = 0; at + 30 <= got && r32(head + at) == 0x04034b50u; ) {
        size_t nlen = r16(head + at + 26), xlen = r16(head + at + 28), csize = r32(head + at + 18);
        if (at + 30 + nlen > got || (r16(head + at + 6) & 8)) break;     /* past what was inflated, or a size only given after the data */
        char name[512];
        snprintf(name, sizeof(name), "%.*s", (int)nlen, (const char *)(head + at + 30));
        if (!ends_with(name, "/")) { pack = pack_path(name) != NULL; break; }
        at += 30 + nlen + xlen + csize;
    }
    free(head);
    return pack;
}

/* The bytes of the packs an APK carries when packs are all it carries; 0 when it holds anything else, and is kept as it is. `*written_out`: the biggest
 * ZIP that has to be written out on the way, for the room it takes for a moment. A compressed one is taken at its size unzipped, its files being stored
 * in it in every one seen. */
static unsigned long long carried_packs(const tl_zip *apk, unsigned long long *written_out)
{
    unsigned long long bytes = 0;
    *written_out = 0;
    for (size_t i = 0; i < apk->count; i++) {
        const tl_zip_entry *e = &apk->entries[i];
        if (apk_frame(e->name)) continue;
        tl_zip z;
        if (open_pack_zip(apk, e, &z)) {
            for (size_t k = 0; k < z.count; k++) bytes += z.entries[k].usize;
            tl_zip_close(&z);
        } else if (compressed_pack_zip(apk, e)) {
            bytes += e->usize;
            if (e->usize > *written_out) *written_out = e->usize;
        } else {
            return 0;
        }
    }
    return bytes;
}

/* An APK of the bundle, read where it lies when the bundle stores it. */
static bool open_stored_apk(const tl_zip *bundle, const tl_zip_entry *e, tl_zip *out)
{
    char err[160];
    const uint8_t *raw = e->method == 0 ? tl_zip_raw(bundle, e) : NULL;
    return raw && tl_zip_open_mem(out, raw, e->csize, err, sizeof(err));
}

/* Every file of a ZIP of packs into the game's folder `dir`. */
static bool unpack_pack_zip(const tl_zip *z, const char *dir, unsigned long long *bytes, char *err, size_t errlen)
{
    for (size_t k = 0; k < z->count; k++) {
        const tl_zip_entry *f = &z->entries[k];
        char to[1200], fail[300] = "";
        if (ends_with(f->name, "/")) continue;
        if ((size_t)snprintf(to, sizeof(to), "%s/%s", dir, pack_path(f->name)) >= sizeof(to)) { snprintf(err, errlen, "The path of %s is too long.", f->name); return false; }
        make_dirs(to, false);
        if (!tl_zip_extract(z, f, to, fail, sizeof(fail))) { snprintf(err, errlen, "Could not unpack %s: %s", base_name(f->name), fail); return false; }
        *bytes += f->usize;
    }
    return true;
}

/* The packs of an APK that carries only packs, into the packs folder of the game's folder `dir`. 1 if it was one and they are out, 0 if it is not
 * one, -1 if they could not be unpacked (`err` says why). */
static int take_packs(const tl_zip *apk, const char *apk_name, const char *dir, char *err, size_t errlen)
{
    unsigned long long written_out;
    if (!carried_packs(apk, &written_out)) return 0;
    size_t zips = 0;
    unsigned long long bytes = 0;
    for (size_t i = 0; i < apk->count; i++) {
        const tl_zip_entry *e = &apk->entries[i];
        if (apk_frame(e->name)) continue;
        tl_zip z;
        char copy[1200] = "", fail[300] = "";
        if (!open_pack_zip(apk, e, &z)) {
            /* Compressed whole: written out first, as a ZIP is read by seeking. */
            if ((size_t)snprintf(copy, sizeof(copy), "%s/.husk-pack.zip", dir) >= sizeof(copy)) { snprintf(err, errlen, "The path of %s is too long.", e->name); return -1; }
            if (!tl_zip_extract(apk, e, copy, fail, sizeof(fail))) { snprintf(err, errlen, "Could not unpack %s: %s", e->name, fail); unlink(copy); return -1; }
            if (!tl_zip_open(&z, copy, fail, sizeof(fail))) { snprintf(err, errlen, "Could not read %s: %s", e->name, fail); unlink(copy); return -1; }
            if (!is_pack_zip(&z)) {
                /* It started like one and is not: the APK stays, as any other split would. */
                tl_log_line("bundle: %s in %s holds more than asset packs, so the APK is kept", e->name, apk_name);
                tl_zip_close(&z);
                unlink(copy);
                return 0;
            }
        }
        bool ok = unpack_pack_zip(&z, dir, &bytes, err, errlen);
        tl_zip_close(&z);
        if (copy[0]) unlink(copy);
        if (!ok) return -1;
        zips++;
    }
    tl_log_line("bundle: %zu ZIPs of asset packs (%.2f GB) out of %s, into the game's " PACKS_FOLDER " folder; the APK is not kept", zips, bytes / 1e9, apk_name);
    return 1;
}

/* ------------------------------------------------------------------- at import */

char *husk_tl_bundle_unpack(const char *bundle, const char *dir, char *err, size_t errlen)
{
    tl_zip z;
    char why[200];
    if (!tl_zip_open(&z, bundle, why, sizeof(why))) { snprintf(err, errlen, "That file is not a bundle Husk can read: %s", why); return NULL; }
    /* The room it all takes, first, so that a bundle too big for the phone stops before minutes of unpacking rather than after: the APKs and OBBs kept,
     * and the packs unzipped. */
    unsigned long long need = 0, biggest_copy = 0;
    for (size_t i = 0; i < z.count; i++) {
        const tl_zip_entry *e = &z.entries[i];
        const char *base = base_name(e->name);
        bool apk = ends_with(base, ".apk"), obb = ends_with(base, ".obb");
        if (!(apk || obb) || !strncmp(e->name, "__MACOSX/", 9) || !safe_name(base)) continue;
        tl_zip a;
        unsigned long long packs = 0, written_out = 0;
        if (apk && open_stored_apk(&z, e, &a)) { packs = carried_packs(&a, &written_out); tl_zip_close(&a); }
        need += packs ? packs : e->usize;
        if (written_out > biggest_copy) biggest_copy = written_out;
    }
    need += biggest_copy;
    unsigned long long have = free_bytes(dir);
    if (have < need + SLACK) {
        snprintf(err, errlen, "This game needs %.2f GB of free space to be added, and the phone has %.2f GB. Free some space and add it again.", (need + SLACK) / 1e9, have / 1e9);
        tl_zip_close(&z);
        return NULL;
    }
    size_t cap = 256, len = 0;
    char *names = malloc(cap);
    bool ok = names != NULL, any = false;
    if (names) names[0] = 0;
    for (size_t i = 0; i < z.count && ok; i++) {
        const tl_zip_entry *e = &z.entries[i];
        const char *base = base_name(e->name);
        bool apk = ends_with(base, ".apk"), obb = ends_with(base, ".obb");
        if (!(apk || obb) || !strncmp(e->name, "__MACOSX/", 9) || !safe_name(base)) continue;
        char to[1200], fail[300] = "";
        /* An APK that only carries asset packs: the packs come out, and the APK is not kept. Read in place when the bundle stores it. */
        tl_zip a;
        if (apk && open_stored_apk(&z, e, &a)) {
            int took = take_packs(&a, base, dir, err, errlen);
            tl_zip_close(&a);
            if (took < 0) { ok = false; break; }
            if (took > 0) continue;
        }
        if ((size_t)snprintf(to, sizeof(to), obb ? "%s/obb/%s" : "%s/%s", dir, base) >= sizeof(to)) { snprintf(err, errlen, "The path of %s is too long.", base); ok = false; break; }
        make_dirs(to, false);
        if (!tl_zip_extract(&z, e, to, fail, sizeof(fail))) { snprintf(err, errlen, "%s", fail); ok = false; break; }
        /* A compressed one is looked at once it is out. */
        if (apk && e->method != 0 && tl_zip_open(&a, to, fail, sizeof(fail))) {
            int took = take_packs(&a, base, dir, err, errlen);
            tl_zip_close(&a);
            if (took != 0) unlink(to);
            if (took < 0) { ok = false; break; }
            if (took > 0) continue;
        }
        any = any || apk;
        size_t add = strlen(base) + 2;
        if (len + add > cap) {
            while (len + add > cap) cap *= 2;
            char *grown = realloc(names, cap);
            if (!grown) { snprintf(err, errlen, "Out of memory."); ok = false; break; }
            names = grown;
        }
        len += (size_t)snprintf(names + len, cap - len, "%s\n", base);
    }
    tl_zip_close(&z);
    if (ok && !any) { snprintf(err, errlen, "There are no APKs in that bundle."); ok = false; }
    if (!ok) { free(names); return NULL; }
    return names;
}
