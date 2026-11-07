/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Game data that comes with an APK rather than in it: put where the game will look, before the game loads, for every engine.
 * On Android the app's own Java launcher or the Play Store does this. No Java launcher runs here.
 *
 *   ZIPs of app data     assets/data0, data1, ..., data.bin, anything.zip: at the top of assets/, unpacked into the data folder.
 *                        Their entries may be laid out as the app's folder (files/, shared_prefs/, data/data/<package>/, <package>/)
 *                        or shared storage (Android/obb/, Android/data/, sdcard/, storage/emulated/0/, obb/, a bare main.1.pkg.obb).
 *   OBBs in the APK      assets/...x.obb, Android/obb/..., obb/...: unpacked into the OBB folder.
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

/* Whether `need` bytes fit, and the reason for the player if they do not. */
static bool has_room(unsigned long long need, char *why, size_t whylen)
{
    struct statfs sf;
    if (statfs(tl_data_dir(), &sf) != 0) return true;
    unsigned long long have = (unsigned long long)sf.f_bavail * sf.f_bsize;
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

/* ------------------------------------------------------------------- at import */

char *husk_tl_bundle_unpack(const char *bundle, const char *dir, char *err, size_t errlen)
{
    tl_zip z;
    char why[200];
    if (!tl_zip_open(&z, bundle, why, sizeof(why))) { snprintf(err, errlen, "That file is not a bundle Husk can read: %s", why); return NULL; }
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
        if ((size_t)snprintf(to, sizeof(to), obb ? "%s/obb/%s" : "%s/%s", dir, base) >= sizeof(to)) { snprintf(err, errlen, "The path of %s is too long.", base); ok = false; break; }
        make_dirs(to, false);
        if (!tl_zip_extract(&z, e, to, fail, sizeof(fail))) { snprintf(err, errlen, "%s", fail); ok = false; break; }
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
