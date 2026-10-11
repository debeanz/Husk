/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Godot .NET games: the engine with Mono (libmonosgen-2.0.so) beside it, and the C# assemblies the engine hands it.
 *
 * Godot's Android build loads Mono itself (dlopen, coreclr_initialize with APP_PATHS = <files>/.godot/mono/publish/arm64,
 * coreclr_create_delegate) on the GL thread's first step. What has to be in place before then:
 *
 *   - Mono's way of running: its JIT writes code into memory it maps executable, which on a phone is the JIT region
 *     written through its second view (husk-tl-codewrite.c). Threads stop for the collector at safepoints (coop), not
 *     by signals; a null reference is a compare in the code, not a fault. Neither needs a Linux signal frame.
 *   - Globalization without ICU: Android's libicuuc is not here, so .NET runs invariant.
 *   - The assemblies, where the engine looks for them.
 *
 * The last one is a launcher's job. Slay the Spire 2's Android port ships the PC game as a ZIP inside the APK and a
 * Kotlin launcher that, on first run, unpacks it into files/payloads/<id>/game, installs the bundled compatibility packs,
 * stages the base class library, the port's STS2Mobile.dll and the game's own assemblies into .godot/mono/publish/arm64,
 * writes the launcher's JSON (which the C# side reads to find the game), and starts Godot with --main-pack on the PCK.
 * That is reproduced here, file for file, so the C# side finds what it expects.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-godot.h"

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-codewrite.h"
#include "husk-tl-internal.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"

#define PUBLISH "/.godot/mono/publish/arm64"

static char FILES[600];

/* ------------------------------------------------------------------ files */

static void mkdirs(const char *path)
{
    char p[1100];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static void parent_dirs(const char *path)
{
    char p[1100];
    snprintf(p, sizeof(p), "%s", path);
    char *s = strrchr(p, '/');
    if (s) { *s = 0; mkdirs(p); }
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char sub[1100];
                snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
                rm_rf(sub);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static bool exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static long long file_size(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long long)st.st_size : -1; }

static bool write_file(const char *path, const void *data, size_t n)
{
    char part[1100];
    snprintf(part, sizeof(part), "%s.part", path);
    parent_dirs(path);
    FILE *f = fopen(part, "wb");
    if (!f) { tl_log_line("dotnet: cannot write %s (%s)", path, strerror(errno)); return false; }
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(part, path) == 0;
    if (!ok) { unlink(part); tl_log_line("dotnet: writing %s failed (%s)", path, strerror(errno)); }
    return ok;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; if (len) *len = (size_t)n; }
    return b;
}

static bool copy_file(const char *from, const char *to)
{
    size_t n;
    char *b = read_file(from, &n);
    if (!b) { tl_log_line("dotnet: cannot read %s", from); return false; }
    bool ok = write_file(to, b, n);
    free(b);
    return ok;
}

/* An entry of the APK written out (stored entries straight from the mapping, deflated ones inflated). */
static bool apk_extract(const tl_zip *apk, const char *name, const char *to)
{
    const tl_zip_entry *e = tl_zip_find(apk, name);
    if (!e) return false;
    char err[256] = "";
    parent_dirs(to);
    if (!tl_zip_extract(apk, e, to, err, sizeof(err))) { tl_log_line("dotnet: %s", err); return false; }
    return true;
}

/* ------------------------------------------------------------------- JSON */

/* The value of "key": "..." in a JSON text, first occurrence at or after `from` and before `until` (NULL: the end). */
static bool json_str(const char *from, const char *until, const char *key, char *out, size_t n)
{
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = from;
    while ((p = strstr(p, pat)) && (!until || p < until)) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != '"') return false;
        q++;
        size_t i = 0;
        while (*q && *q != '"' && i + 1 < n) {
            if (*q == '\\' && q[1]) q++;
            out[i++] = *q++;
        }
        out[i] = 0;
        return true;
    }
    return false;
}

/* A string as a JSON literal, the way org.json writes it: '/' escaped too. */
static void jstr(char *out, size_t n, const char *s)
{
    size_t o = 0;
    if (o + 1 < n) out[o++] = '"';
    for (; *s && o + 3 < n; s++) {
        if (*s == '"' || *s == '\\' || *s == '/') { out[o++] = '\\'; out[o++] = *s; }
        else if ((unsigned char)*s < 0x20) { o += (size_t)snprintf(out + o, n - o, "\\u%04x", *s); }
        else out[o++] = *s;
    }
    if (o + 1 < n) out[o++] = '"';
    out[o < n ? o : n - 1] = 0;
}

/* A JSON object written as org.json's toString(2) does: "key": value, two spaces in. */
typedef struct { char *b; size_t n, cap; int count; } jw;
static void jw_raw(jw *w, const char *s)
{
    size_t l = strlen(s);
    if (w->n + l + 1 > w->cap) { w->cap = (w->n + l + 1) * 2; w->b = realloc(w->b, w->cap); }
    memcpy(w->b + w->n, s, l + 1);
    w->n += l;
}
static void jw_key(jw *w, const char *k)
{
    char q[256];
    jw_raw(w, w->count++ ? ",\n  " : "{\n  ");
    jstr(q, sizeof(q), k);
    jw_raw(w, q);
    jw_raw(w, ": ");
}
static void jw_s(jw *w, const char *k, const char *v) { char q[2400]; jw_key(w, k); jstr(q, sizeof(q), v); jw_raw(w, q); }
static void jw_i(jw *w, const char *k, long long v) { char q[32]; jw_key(w, k); snprintf(q, sizeof(q), "%lld", v); jw_raw(w, q); }
static void jw_b(jw *w, const char *k, bool v) { jw_key(w, k); jw_raw(w, v ? "true" : "false"); }
static void jw_v(jw *w, const char *k, const char *raw) { jw_key(w, k); jw_raw(w, raw); }
static bool jw_save(jw *w, const char *path)
{
    jw_raw(w, w->count ? "\n}" : "{}");
    bool ok = write_file(path, w->b, w->n);
    free(w->b);
    memset(w, 0, sizeof(*w));
    return ok;
}

/* -------------------------------------------------------------- the runtime */

static void set_default_env(const char *k, const char *v)
{
    if (getenv(k)) { tl_log_line("dotnet: %s=%s (already set)", k, getenv(k)); return; }
    setenv(k, v, 0);
    tl_log_line("dotnet: %s=%s", k, v);
}

static void runtime_env(void)
{
    /* The collector stops threads at safepoints the JIT puts in every loop and call, never with a signal: a signal's frame
     * here is Darwin's, not the Linux one Mono would read registers from. */
    set_default_env("MONO_THREADS_SUSPEND", "coop");
    /* A null reference becomes a compare in the generated code instead of a fault Mono would have to be handed. */
    set_default_env("MONO_DEBUG", "explicit-null-checks");
    /* .NET on Android takes ICU from the system (libicuuc.so); there is none here. */
    set_default_env("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1");
    set_default_env("DOTNET_SYSTEM_GLOBALIZATION_PREDEFINED_CULTURES_ONLY", "0");
    /* Which assemblies Mono looked for and found, for the log. */
    set_default_env("MONO_LOG_LEVEL", "info");
    set_default_env("MONO_LOG_MASK", "asm");
    tl_codewrite_enable();
}

/* ------------------------------------------------------- Slay the Spire 2 */

static const char *const STS2_ARGS_LOG[] = { "-log", "Generic", "Info", "-log", "Network", "Info", "-log", "Actions", "Info",
                                             "-log", "GameSync", "Info", "-log", "VisualSync", "Info", NULL };

/* settings.save as the launcher's first-run setup leaves it (createDefaultSettingsJson, then the "recommended", "original"
 * and "touch" presets, which write the same values): the port's C# and the game read and keep it. */
static const char STS2_DEFAULT_SETTINGS[] =
    "{\n"
    "  \"schema_version\": 6,\n"
    "  \"fullscreen\": true,\n"
    "  \"aspect_ratio\": \"auto\",\n"
    "  \"target_display\": -1,\n"
    "  \"resize_windows\": true,\n"
    "  \"fps_limit\": 60,\n"
    "  \"msaa\": 0,\n"
    "  \"shader_compatibility_mode\": false,\n"
    "  \"vsync\": \"off\",\n"
    "  \"window_position\": {\n"
    "    \"X\": -1,\n"
    "    \"Y\": -1\n"
    "  },\n"
    "  \"window_size\": {\n"
    "    \"X\": 1920,\n"
    "    \"Y\": 1080\n"
    "  },\n"
    "  \"fullscreen_render_size\": {\n"
    "    \"X\": 0,\n"
    "    \"Y\": 0\n"
    "  },\n"
    "  \"preload_enabled\": true,\n"
    "  \"preload_startup_common_enabled\": true,\n"
    "  \"preload_startup_main_menu_enabled\": true,\n"
    "  \"preload_menu_hotspots_enabled\": false,\n"
    "  \"preload_vfx_mode\": \"off\",\n"
    "  \"preload_vfx_tree_warmup_enabled\": false,\n"
    "  \"preload_vfx_tree_warmup_scope\": \"safe\",\n"
    "  \"preload_vfx_tree_warmup_frames\": 3,\n"
    "  \"preload_vfx_retain_cache_enabled\": false,\n"
    "  \"preload_combat_animation_warmup_mode\": \"off\",\n"
    "  \"preload_combat_animation_warmup_frames\": 1,\n"
    "  \"preload_combat_hit_effect_warmup_enabled\": false,\n"
    "  \"preload_combat_code_enabled\": false,\n"
    "  \"preload_shader_mode\": \"off\",\n"
    "  \"preload_runtime_enabled\": true,\n"
    "  \"preload_protect_warm_cache_enabled\": true,\n"
    "  \"preload_gameplay_assets_enabled\": false,\n"
    "  \"preload_learned_assets_enabled\": true,\n"
    "  \"android_compat_pack_enabled\": true,\n"
    "  \"global_scale\": 1,\n"
    "  \"ui_font_scale_percent\": 100,\n"
    "  \"show_more_hand_card_text\": true,\n"
    "  \"show_more_hand_card_text_lift_height_percent\": 50,\n"
    "  \"touch_lift_preview\": true,\n"
    "  \"touch_lift_retap_action\": \"put_down\",\n"
    "  \"mobile_selection_confirmation\": true,\n"
    "  \"mobile_two_finger_inspect\": true,\n"
    "  \"mobile_tooltip_mode\": \"immediate\",\n"
    "  \"mobile_tooltip_long_press_ms\": 1000,\n"
    "  \"show_mobile_emoji_button\": true,\n"
    "  \"lan_multiplayer_enabled\": true,\n"
    "  \"lan_compatibility_mod_names\": [],\n"
    "  \"audio_compatibility_mode\": false,\n"
    "  \"log_level\": \"info\",\n"
    "  \"android_performance_overlay_enabled\": false,\n"
    "  \"android_high_refresh_rate_enabled\": false,\n"
    "  \"android_volume_up_soft_keyboard\": false,\n"
    "  \"android_flip_screen_180\": false,\n"
    "  \"android_screen_rotation_mode\": \"user_landscape\",\n"
    "  \"lan_use_custom_player_id\": false,\n"
    "  \"lan_use_custom_platform_player_id\": false,\n"
    "  \"lan_custom_player_id\": \"\",\n"
    "  \"lan_join_host\": \"\",\n"
    "  \"lan_join_port\": 33771,\n"
    "  \"max_multiplayer_players\": 4,\n"
    "  \"max_multiplayer_enabled\": true,\n"
    "  \"quick_sl_enabled\": true,\n"
    "  \"android_in_game_overlay_enabled\": false,\n"
    "  \"android_dev_tools_enabled\": false,\n"
    "  \"android_dev_inspector_writable\": false,\n"
    "  \"mod_settings\": null,\n"
    "  \"android_graphics_preset\": \"recommended\",\n"
    "  \"android_display_preset\": \"original\"\n"
    "}";

/* sanitizeId: lower case, runs of anything but [a-z0-9._-] become '-', no '-' or '.' at either end. */
static void sanitize_id(const char *in, char *out, size_t n)
{
    size_t o = 0;
    bool dash = false;
    for (const char *p = in; *p && o + 1 < n; p++) {
        char c = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (ok) { out[o++] = c; dash = false; }
        else if (!dash) { out[o++] = '-'; dash = true; }
    }
    out[o] = 0;
    size_t s = 0;
    while (out[s] == '-' || out[s] == '.') s++;
    memmove(out, out + s, strlen(out + s) + 1);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '-' || out[l - 1] == '.')) out[--l] = 0;
}

static bool sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    static uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) CC_SHA256_Update(&c, buf, (CC_LONG)n);
    fclose(f);
    uint8_t d[32];
    CC_SHA256_Final(d, &c);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return true;
}

typedef struct {
    char version[64], commit[64], branch[64], release_json[2048];
    char pay[160], pid[200], display[160];
    char game[900], acct[700], mods[700], logs[900];
    char pack_id[96], target[96], pack_dir[800], dll[1000], overlay[1000];
} sts2;

/* compat_packs: each assets/compat_packs/<x>.zip holds one folder with compat_manifest.json; it is installed as
 * files/compat-packs/<pack_id>, with the two fields the launcher adds. Reinstalled when the bundled zip changes. */
static void install_compat_packs(const tl_zip *apk)
{
    for (size_t i = 0; i < apk->count; i++) {
        const tl_zip_entry *e = &apk->entries[i];
        const char *pfx = "assets/compat_packs/";
        size_t pl = strlen(pfx), nl = strlen(e->name);
        if (strncmp(e->name, pfx, pl) || nl < pl + 5 || strcmp(e->name + nl - 4, ".zip")) continue;
        const uint8_t *d; size_t len; bool owned; char err[256] = "";
        if (!tl_zip_data(apk, e, 256u << 20, &d, &len, &owned, err, sizeof(err))) { tl_log_line("dotnet: %s", err); continue; }
        tl_zip z;
        if (!tl_zip_open_mem(&z, d, len, err, sizeof(err))) { tl_log_line("dotnet: %s: %s", e->name, err); if (owned) free((void *)d); continue; }
        /* The folder that holds the manifest. */
        char root[256] = "";
        const tl_zip_entry *man = NULL;
        for (size_t k = 0; k < z.count; k++) {
            const char *n = z.entries[k].name, *s = strrchr(n, '/');
            const char *base = s ? s + 1 : n;
            if (strcmp(base, "compat_manifest.json")) continue;
            if (!man || strlen(n) < strlen(man->name)) { man = &z.entries[k]; snprintf(root, sizeof(root), "%.*s", (int)(base - n), n); }
        }
        const uint8_t *md; size_t ml; bool mo;
        char pack_id[96] = "";
        if (man && tl_zip_data(&z, man, 4u << 20, &md, &ml, &mo, err, sizeof(err))) {
            char *txt = malloc(ml + 1); memcpy(txt, md, ml); txt[ml] = 0;
            char raw[96];
            if (json_str(txt, NULL, "pack_id", raw, sizeof(raw))) sanitize_id(raw, pack_id, sizeof(pack_id));
            if (pack_id[0]) {
                char dir[800], stamp[900], want[64];
                snprintf(dir, sizeof(dir), "%s/compat-packs/%s", FILES, pack_id);
                snprintf(stamp, sizeof(stamp), "%s/.husk_source", dir);
                snprintf(want, sizeof(want), "%llu %llu", (unsigned long long)e->csize, (unsigned long long)e->usize);
                char *have = read_file(stamp, NULL);
                char mpath[900];
                snprintf(mpath, sizeof(mpath), "%s/compat_manifest.json", dir);
                if (have && !strcmp(have, want) && exists(mpath)) {
                    tl_log_line("dotnet: compatibility pack %s is installed", pack_id);
                } else {
                    rm_rf(dir);
                    mkdirs(dir);
                    int files = 0;
                    for (size_t k = 0; k < z.count; k++) {
                        const tl_zip_entry *f = &z.entries[k];
                        size_t rl = strlen(root), fl = strlen(f->name);
                        if (strncmp(f->name, root, rl) || fl == rl || f->name[fl - 1] == '/' || strstr(f->name, "..") || f == man) continue;
                        char to[1100];
                        snprintf(to, sizeof(to), "%s/%s", dir, f->name + rl);
                        parent_dirs(to);
                        if (tl_zip_extract(&z, f, to, err, sizeof(err))) files++;
                        else tl_log_line("dotnet: %s", err);
                    }
                    /* The manifest, with what the launcher records about where it came from. */
                    size_t end = ml;
                    while (end && txt[end - 1] != '}') end--;
                    if (end) {
                        char tail[600], zipname[200], sumbuf[400];
                        const char *zn = strrchr(e->name, '/');
                        jstr(zipname, sizeof(zipname), zn ? zn + 1 : e->name);
                        uint8_t h[32]; char hex[65];
                        CC_SHA256(d, (CC_LONG)len, h);
                        for (int b = 0; b < 32; b++) snprintf(hex + 2 * b, 3, "%02x", h[b]);
                        jstr(sumbuf, sizeof(sumbuf), hex);
                        snprintf(tail, sizeof(tail), ",\n  \"installed_at_unix\": %lld,\n  \"installed_source\": {\n    \"kind\": \"bundled_asset\",\n"
                                 "    \"display_name\": %s,\n    \"zip_sha256\": %s\n  }\n}", (long long)time(NULL), zipname, sumbuf);
                        size_t body = end - 1;
                        while (body && (txt[body - 1] == ' ' || txt[body - 1] == '\n' || txt[body - 1] == '\r' || txt[body - 1] == '\t')) body--;
                        char *out = malloc(body + strlen(tail) + 1);
                        memcpy(out, txt, body); strcpy(out + body, tail);
                        write_file(mpath, out, strlen(out));
                        free(out);
                    }
                    write_file(stamp, want, strlen(want));
                    tl_log_line("dotnet: compatibility pack %s installed (%d files)", pack_id, files + 1);
                }
                free(have);
            }
            free(txt);
            if (mo) free((void *)md);
        }
        tl_zip_close(&z);
        if (owned) free((void *)d);
    }
}

/* The pack and target the launcher would pick for this game: the main pack's target whose sts2.dll hash is this one's,
 * else whose version (or list of versions) names this version. Offline-bootstrap packs are the fallback, not chosen. */
static bool choose_compat(sts2 *g, const char *dll_sha)
{
    char dir[800];
    snprintf(dir, sizeof(dir), "%s/compat-packs", FILES);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    int best = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char mp[1100];
        snprintf(mp, sizeof(mp), "%s/%s/compat_manifest.json", dir, de->d_name);
        char *txt = read_file(mp, NULL);
        if (!txt) continue;
        char kind[64] = "";
        json_str(txt, NULL, "pack_kind", kind, sizeof(kind));
        char quoted[96], sha[96];
        snprintf(quoted, sizeof(quoted), "\"%s\"", g->version);
        snprintf(sha, sizeof(sha), "\"%s\"", dll_sha);
        for (const char *t = strstr(txt, "\"target_id\""); t; ) {
            const char *next = strstr(t + 1, "\"target_id\"");
            char id[96] = "", ver[64] = "", dll[300] = "", pck[300] = "";
            json_str(t, next, "target_id", id, sizeof(id));
            json_str(t, next, "version", ver, sizeof(ver));
            json_str(t, next, "dll", dll, sizeof(dll));
            json_str(t, next, "overlay_pck", pck, sizeof(pck));
            size_t span = next ? (size_t)(next - t) : strlen(t);
            char *block = strndup(t, span);
            int score = strstr(block, sha) ? 400 : !strcmp(ver, g->version) ? 300 : strstr(block, quoted) ? 200 : 0;
            free(block);
            if (strstr(kind, "offline")) score = score ? 10 : 0;
            if (score > best && id[0] && dll[0]) {
                best = score;
                snprintf(g->pack_id, sizeof(g->pack_id), "%s", de->d_name);
                snprintf(g->target, sizeof(g->target), "%s", id);
                snprintf(g->pack_dir, sizeof(g->pack_dir), "%s/%s", dir, de->d_name);
                snprintf(g->dll, sizeof(g->dll), "%s/%s", g->pack_dir, dll);
                snprintf(g->overlay, sizeof(g->overlay), "%s/%s", g->pack_dir, pck);
            }
            t = next;
        }
        free(txt);
    }
    closedir(d);
    if (best) tl_log_line("dotnet: compatibility pack %s, target %s", g->pack_id, g->target);
    return best > 0;
}

/* release_info.json: version, commit, branch. */
static bool read_release(sts2 *g, const char *path)
{
    char *txt = read_file(path, NULL);
    if (!txt) return false;
    json_str(txt, NULL, "version", g->version, sizeof(g->version));
    json_str(txt, NULL, "commit", g->commit, sizeof(g->commit));
    json_str(txt, NULL, "branch", g->branch, sizeof(g->branch));
    /* Kept as written, for the manifests that quote it. */
    size_t l = strlen(txt);
    while (l && (txt[l - 1] == '\n' || txt[l - 1] == '\r' || txt[l - 1] == ' ')) txt[--l] = 0;
    snprintf(g->release_json, sizeof(g->release_json), "%s", txt);
    free(txt);
    return g->version[0] != 0;
}

/* The payload's identity, from its release_info.json and sts2.dll. */
static void name_payload(sts2 *g, const char *dll_sha)
{
    char raw[300];
    snprintf(raw, sizeof(raw), "sts2-%s-%s-%.12s", g->version, g->commit, dll_sha);
    sanitize_id(raw, g->pay, sizeof(g->pay));
    snprintf(g->pid, sizeof(g->pid), "profile-%s", g->pay);
    snprintf(g->display, sizeof(g->display), "%s (%s)", g->version, g->commit);
}

static const char *required[] = { "SlayTheSpire2.pck", "release_info.json", "data_sts2_windows_x86_64/sts2.dll",
                                   "data_sts2_windows_x86_64/sts2.deps.json", "data_sts2_windows_x86_64/sts2.runtimeconfig.json", NULL };

/* assets/payload/SlayTheSpire2.zip unpacked to files/payloads/<id>/game, once. */
static bool import_payload(const tl_zip *apk, sts2 *g, char *dll_sha)
{
    const tl_zip_entry *pe = tl_zip_find(apk, "assets/payload/SlayTheSpire2.zip");
    if (!pe) return false;
    char stamp[700], want[64];
    snprintf(stamp, sizeof(stamp), "%s/payloads/.husk_payload", FILES);
    snprintf(want, sizeof(want), "%llu %llu", (unsigned long long)pe->csize, (unsigned long long)pe->usize);

    /* Already there: the stamp names the payload id; the game dir is whole when its manifest exists. */
    char *have = read_file(stamp, NULL);
    if (have) {
        char size[64] = "", pay[160] = "";
        if (sscanf(have, "%63[^\n]\n%159s", size, pay) == 2 && !strcmp(size, want)) {
            char game[900], man[1000];
            snprintf(game, sizeof(game), "%s/payloads/%s/game", FILES, pay);
            snprintf(man, sizeof(man), "%s/.payload_manifest.json", game);
            char rel[1000], dll[1000];
            snprintf(rel, sizeof(rel), "%s/release_info.json", game);
            snprintf(dll, sizeof(dll), "%s/data_sts2_windows_x86_64/sts2.dll", game);
            if (exists(man) && read_release(g, rel) && sha256_file(dll, dll_sha)) {
                name_payload(g, dll_sha);
                snprintf(g->game, sizeof(g->game), "%s", game);
                free(have);
                tl_log_line("dotnet: the game is unpacked (%s)", g->pay);
                return true;
            }
        }
        free(have);
    }

    /* The bundled ZIP, read where it lies when the APK stores it (it does: it is 0.9 GB of already-compressed data). */
    char err[300] = "";
    tl_zip z;
    const uint8_t *zd = NULL; size_t zlen = 0; bool zowned = false;
    char src_copy[800] = "";
    if (pe->method == 0) {
        if (!tl_zip_data(apk, pe, (size_t)1 << 40, &zd, &zlen, &zowned, err, sizeof(err)) || !tl_zip_open_mem(&z, zd, zlen, err, sizeof(err))) {
            tl_log_line("dotnet: the bundled game: %s", err); return false;
        }
    } else {
        snprintf(src_copy, sizeof(src_copy), "%s/payload_import/bundled-source.zip", FILES);
        parent_dirs(src_copy);
        if (!tl_zip_extract(apk, pe, src_copy, err, sizeof(err)) || !tl_zip_open(&z, src_copy, err, sizeof(err))) {
            tl_log_line("dotnet: the bundled game: %s", err); return false;
        }
    }

    unsigned long long total = 0;
    for (size_t i = 0; i < z.count; i++) total += z.entries[i].usize;
    struct statfs sf;
    if (statfs(FILES, &sf) == 0) {
        unsigned long long free_b = (unsigned long long)sf.f_bavail * sf.f_bsize;
        if (free_b < total + (256ull << 20)) {
            tl_log_line("dotnet: the game needs %.1f GB unpacked and the phone has %.1f GB free -- free some space and start it again",
                        total / 1e9, free_b / 1e9);
            tl_zip_close(&z);
            return false;
        }
    }

    /* Everything under one folder is that folder's contents (the launcher flattens it too). */
    char top[256] = "";
    bool one_top = true;
    for (size_t i = 0; i < z.count && one_top; i++) {
        const char *n = z.entries[i].name, *s = strchr(n, '/');
        if (!strncmp(n, "__MACOSX/", 9)) continue;
        if (!s) { one_top = false; break; }
        if (!top[0]) snprintf(top, sizeof(top), "%.*s", (int)(s - n + 1), n);
        else if (strncmp(n, top, strlen(top))) one_top = false;
    }
    size_t strip = one_top ? strlen(top) : 0;

    char staging[700];
    snprintf(staging, sizeof(staging), "%s/payload_import/staging-husk", FILES);
    rm_rf(staging);
    mkdirs(staging);
    tl_log_line("dotnet: unpacking the game (%.2f GB, %zu files) -- the first start takes a while", total / 1e9, z.count);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool ok = true;
    int files = 0;
    for (size_t i = 0; i < z.count && ok; i++) {
        const tl_zip_entry *e = &z.entries[i];
        const char *n = e->name;
        size_t nl = strlen(n);
        if (!nl || n[nl - 1] == '/' || !strncmp(n, "__MACOSX/", 9) || strstr(n, "..") || strchr(n, ':')) continue;
        const char *base = strrchr(n, '/');
        if (!strcmp(base ? base + 1 : n, ".DS_Store")) continue;
        char to[1100];
        snprintf(to, sizeof(to), "%s/%s", staging, n + strip);
        parent_dirs(to);
        if (e->usize > (64u << 20)) tl_log_line("dotnet:   %s (%.2f GB)", n + strip, e->usize / 1e9);
        if (!tl_zip_extract(&z, e, to, err, sizeof(err))) { tl_log_line("dotnet: %s", err); ok = false; }
        else files++;
    }
    tl_zip_close(&z);
    if (zowned) free((void *)zd);
    if (src_copy[0]) unlink(src_copy);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!ok) { rm_rf(staging); return false; }
    tl_log_line("dotnet: %d files unpacked in %.1f s", files, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);

    for (int i = 0; required[i]; i++) {
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", staging, required[i]);
        if (file_size(p) <= 0) { tl_log_line("dotnet: the bundled game has no %s", required[i]); rm_rf(staging); return false; }
    }
    char pck[1000], magic[4] = "";
    snprintf(pck, sizeof(pck), "%s/SlayTheSpire2.pck", staging);
    FILE *pf = fopen(pck, "rb");
    if (pf) { if (fread(magic, 1, 4, pf) != 4) magic[0] = 0; fclose(pf); }
    if (memcmp(magic, "GDPC", 4)) { tl_log_line("dotnet: SlayTheSpire2.pck is not a Godot pack"); rm_rf(staging); return false; }

    char rel[1000], dll[1000];
    snprintf(rel, sizeof(rel), "%s/release_info.json", staging);
    snprintf(dll, sizeof(dll), "%s/data_sts2_windows_x86_64/sts2.dll", staging);
    if (!read_release(g, rel) || !sha256_file(dll, dll_sha)) { tl_log_line("dotnet: release_info.json or sts2.dll unreadable"); rm_rf(staging); return false; }
    name_payload(g, dll_sha);

    /* .payload_manifest.json, as the launcher writes it (less the PCK's own hash, which costs a full read of 1.8 GB). */
    char src_sha[65] = "";
    if (zd) {
        uint8_t h[32];
        CC_SHA256_CTX c; CC_SHA256_Init(&c);
        for (size_t o = 0; o < zlen; o += 1u << 30) CC_SHA256_Update(&c, zd + o, (CC_LONG)(zlen - o > (1u << 30) ? (1u << 30) : zlen - o));
        CC_SHA256_Final(h, &c);
        for (int b = 0; b < 32; b++) snprintf(src_sha + 2 * b, 3, "%02x", h[b]);
    }
    long long now = (long long)time(NULL);
    char buf[4096], q1[200], q2[200], q3[200], q4[200], q5[200];
    jstr(q1, sizeof(q1), g->version); jstr(q2, sizeof(q2), g->commit); jstr(q3, sizeof(q3), g->branch); jstr(q4, sizeof(q4), dll_sha);
    jstr(q5, sizeof(q5), src_sha);
    long long dll_size = file_size(dll), pck_size = file_size(pck);
    const char *hash = strstr(g->release_json, "\"main_assembly_hash\"");
    long long mah = 0;
    if (hash) { hash = strchr(hash, ':'); if (hash) mah = atoll(hash + 1); }
    jw w = {0};
    jw_i(&w, "schema", 1);
    jw_i(&w, "imported_at_unix", now);
    snprintf(buf, sizeof(buf), "{\n    \"kind\": \"bundled_zip\",\n    \"display_name\": \"SlayTheSpire2.zip\",\n    \"size\": %llu,\n    \"sha256\": %s\n  }",
             (unsigned long long)pe->usize, q5);
    jw_v(&w, "source", buf);
    snprintf(buf, sizeof(buf), "{\n    \"release_info\": %s,\n    \"version\": %s,\n    \"commit\": %s,\n    \"branch\": %s,\n    \"main_assembly_hash\": %lld,\n"
             "    \"sts2_dll_sha256\": %s,\n    \"sts2_dll_size\": %lld,\n    \"pck_sha256_after_patch\": \"\"\n  }", g->release_json, q1, q2, q3, mah, q4, dll_size);
    jw_v(&w, "identity", buf);
    snprintf(buf, sizeof(buf), "{\n    \"pck_size\": %lld,\n    \"release_info\": %s,\n    \"sts2_dll_sha256\": %s,\n    \"pck_sha256_after_patch\": \"\",\n"
             "    \"dll_size\": %lld,\n    \"file_count\": %d,\n    \"total_uncompressed_bytes\": %llu\n  }", pck_size, g->release_json, q4, dll_size, files, total);
    jw_v(&w, "game", buf);
    jw_v(&w, "compat", "{\n    \"required_port_mod_version\": \"0.1.0\",\n    \"payload_layout\": \"pc_zip_flat_v1\",\n    \"pck_patches\": {\n"
         "      \"schema\": 2,\n      \"sentry_autoload_disabled\": true,\n      \"sentry_bootstrap_supported\": true\n    }\n  }");
    char man[1000];
    snprintf(man, sizeof(man), "%s/.payload_manifest.json", staging);
    if (!jw_save(&w, man)) { rm_rf(staging); return false; }

    snprintf(g->game, sizeof(g->game), "%s/payloads/%s/game", FILES, g->pay);
    rm_rf(g->game);
    parent_dirs(g->game);
    if (rename(staging, g->game) != 0) { tl_log_line("dotnet: cannot move the game into place (%s)", strerror(errno)); rm_rf(staging); return false; }
    char st[300];
    snprintf(st, sizeof(st), "%s\n%s\n", want, g->pay);
    write_file(stamp, st, strlen(st));
    tl_log_line("dotnet: the game is in %s", g->game);
    return true;
}

/* instance.json and the launcher's three selection files. */
static void write_profile(sts2 *g)
{
    char p[1100];
    long long now = (long long)time(NULL);
    snprintf(g->acct, sizeof(g->acct), "%s/default/1", FILES);
    snprintf(g->mods, sizeof(g->mods), "%s/mods", FILES);
    snprintf(g->logs, sizeof(g->logs), "%s/instances/%s/logs", FILES, g->pid);
    mkdirs(g->acct); mkdirs(g->mods); mkdirs(g->logs);

    snprintf(p, sizeof(p), "%s/instances/%s/instance.json", FILES, g->pid);
    if (!exists(p)) {
        jw w = {0};
        jw_i(&w, "schema", 2); jw_s(&w, "id", g->pid); jw_s(&w, "display_name", g->display); jw_s(&w, "payload_id", g->pay);
        jw_s(&w, "compat_pack_id", g->pack_id); jw_s(&w, "compat_target_id", g->target); jw_s(&w, "save_mode", "global");
        jw_s(&w, "mods_mode", "global"); jw_i(&w, "created_at_unix", now); jw_i(&w, "updated_at_unix", now);
        jw_save(&w, p);
    }

    char rel[1000], settings[800];
    snprintf(rel, sizeof(rel), "%s/release_info.json", g->game);
    snprintf(settings, sizeof(settings), "%s/settings.save", g->acct);
    jw w = {0};
    jw_i(&w, "schema", 1); jw_s(&w, "data_dir", FILES);
    jw_s(&w, "selected_instance_id", g->pid); jw_s(&w, "selected_profile_id", g->pid); jw_s(&w, "instance_id", g->pid); jw_s(&w, "profile_id", g->pid);
    jw_s(&w, "display_name", g->display); jw_s(&w, "payload_id", g->pay); jw_s(&w, "payload_label", g->display);
    jw_s(&w, "payload_version", g->version); jw_s(&w, "payload_commit", g->commit); jw_s(&w, "selected_game_dir", g->game);
    jw_s(&w, "selected_release_info", rel); jw_s(&w, "save_mode", "global"); jw_s(&w, "mods_mode", "global");
    jw_s(&w, "selected_account_root", g->acct); jw_s(&w, "selected_settings_path", settings); jw_s(&w, "selected_mods_dir", g->mods);
    jw_s(&w, "selected_logs_dir", g->logs); jw_s(&w, "compat_pack_id", g->pack_id); jw_s(&w, "compat_target_id", g->target);
    jw_s(&w, "selected_compat_pack_dir", g->pack_dir); jw_s(&w, "selected_compat_target_id", g->target);
    jw_s(&w, "selected_compat_overlay_pck", g->overlay); jw_s(&w, "selected_compat_dll", g->dll);
    snprintf(p, sizeof(p), "%s/launcher/selected_instance.json", FILES);
    jw_save(&w, p);
    if (!exists(settings)) write_file(settings, STS2_DEFAULT_SETTINGS, sizeof(STS2_DEFAULT_SETTINGS) - 1);

    jw_i(&w, "schema", 2); jw_s(&w, "selected_game_version_id", g->pay); jw_s(&w, "selected_launch_profile_id", g->pid);
    jw_s(&w, "selected_game_dir", g->game);
    snprintf(p, sizeof(p), "%s/launcher/selected_game_version.json", FILES);
    jw_save(&w, p);

    jw_i(&w, "schema", 2); jw_b(&w, "compat_pack_enabled", g->pack_id[0] != 0); jw_s(&w, "selected_compat_pack_id", g->pack_id);
    jw_s(&w, "selected_compat_target_id", g->target); jw_s(&w, "selected_compat_pack_dir", g->pack_dir);
    jw_s(&w, "selected_compat_dll", g->dll); jw_s(&w, "selected_compat_overlay_pck", g->overlay);
    snprintf(p, sizeof(p), "%s/launcher/selected_compat_pack.json", FILES);
    jw_save(&w, p);

    /* The performance overlay is off unless asked for. */
    snprintf(p, sizeof(p), "%s/launcher/enable_debug_menu.flag", FILES);
    unlink(p);
}

static bool protected_name(const char *n)
{
    return !strncmp(n, "System.", 7) || !strcmp(n, "mscorlib.dll") || !strcmp(n, "netstandard.dll") || !strcmp(n, "Microsoft.CSharp.dll")
        || !strncmp(n, "Microsoft.VisualBasic", 21);
}

/* .godot/mono/publish/arm64: the base class library from assets/dotnet_bcl, the port's STS2Mobile.dll, the game's assemblies. */
static bool stage_assemblies(const tl_zip *apk, sts2 *g)
{
    char pub[700], p[1100];
    snprintf(pub, sizeof(pub), "%s" PUBLISH, FILES);
    mkdirs(pub);
    const char *pfx = "assets/dotnet_bcl/";
    size_t pl = strlen(pfx);

    /* The BCL, again only when the APK's changed. */
    unsigned long long sum = 0; int count = 0;
    for (size_t i = 0; i < apk->count; i++) {
        const tl_zip_entry *e = &apk->entries[i];
        if (strncmp(e->name, pfx, pl) || !e->name[pl] || strchr(e->name + pl, '/')) continue;
        sum += e->usize * 31 + e->csize; count++;
    }
    char want[64], stamp[800];
    snprintf(want, sizeof(want), "%d %llu", count, sum);
    snprintf(stamp, sizeof(stamp), "%s/.husk_bcl", pub);
    char *have = read_file(stamp, NULL);
    snprintf(p, sizeof(p), "%s/GodotSharp.dll", pub);
    bool fresh = !have || strcmp(have, want) || !exists(p);
    free(have);
    if (fresh) {
        int n = 0;
        for (size_t i = 0; i < apk->count; i++) {
            const tl_zip_entry *e = &apk->entries[i];
            const char *name = e->name + pl;
            if (strncmp(e->name, pfx, pl) || !*name || strchr(name, '/') || !strcmp(name, "STS2Mobile.dll")) continue;
            snprintf(p, sizeof(p), "%s/%s", pub, name);
            if (apk_extract(apk, e->name, p)) n++;
        }
        write_file(stamp, want, strlen(want));
        tl_log_line("dotnet: %d base class library files staged", n);
    }

    /* STS2Mobile.dll: the chosen compatibility target's, else the APK's own. */
    snprintf(p, sizeof(p), "%s/STS2Mobile.dll", pub);
    if (g->dll[0] && exists(g->dll)) copy_file(g->dll, p);
    else if (!apk_extract(apk, "assets/dotnet_bcl/STS2Mobile.dll", p)) tl_log_line("dotnet: no STS2Mobile.dll anywhere");

    /* The game's assemblies, from its data_* folder: all but native libraries and what the BCL already has. */
    char data[1000] = "";
    DIR *d = opendir(g->game);
    struct dirent *de;
    while (d && (de = readdir(d))) if (!strncmp(de->d_name, "data_", 5)) { snprintf(data, sizeof(data), "%s/%s", g->game, de->d_name); break; }
    if (d) closedir(d);
    if (!data[0]) { tl_log_line("dotnet: the game has no data_* folder"); return false; }
    d = opendir(data);
    int n = 0;
    while (d && (de = readdir(d))) {
        const char *name = de->d_name;
        size_t l = strlen(name);
        if (name[0] == '.' || (l > 3 && !strcmp(name + l - 3, ".so")) || protected_name(name)) continue;
        char inbcl[400];
        snprintf(inbcl, sizeof(inbcl), "%s%s", pfx, name);
        if (tl_zip_find(apk, inbcl)) continue;
        char from[1100];
        snprintf(from, sizeof(from), "%s/%s", data, name);
        snprintf(p, sizeof(p), "%s/%s", pub, name);
        if (file_size(p) == file_size(from)) continue;
        if (copy_file(from, p)) n++;
    }
    if (d) closedir(d);
    if (n) tl_log_line("dotnet: %d game assemblies staged", n);
    return true;
}

static bool prepare_sts2(const tl_zip *apk)
{
    sts2 g;
    memset(&g, 0, sizeof(g));
    char p[1100], dll_sha[65] = "";
    tl_log_line("dotnet: Slay the Spire 2's Android port -- doing what its launcher does on first start");
    snprintf(p, sizeof(p), "%s/tmp", FILES);
    mkdirs(p);
    setenv("TMPDIR", p, 1); setenv("TMP", p, 1); setenv("TEMP", p, 1);

    install_compat_packs(apk);
    if (!import_payload(apk, &g, dll_sha)) { tl_log_line("dotnet: the game could not be unpacked"); return false; }
    if (!choose_compat(&g, dll_sha)) tl_log_line("dotnet: no compatibility pack matches %s; using the APK's own", g.version);
    write_profile(&g);

    /* The overlay the port's C# loads from the data dir. */
    snprintf(p, sizeof(p), "%s/port_compat.pck", FILES);
    if (!(g.overlay[0] && exists(g.overlay) && copy_file(g.overlay, p))) apk_extract(apk, "assets/port_compat.pck", p);

    if (!stage_assemblies(apk, &g)) return false;

    char logf[1000], pck[1000];
    snprintf(logf, sizeof(logf), "%s/godot.log", g.logs);
    snprintf(pck, sizeof(pck), "%s/SlayTheSpire2.pck", g.game);
    tl_godot_add_arg("--rendering-method"); tl_godot_add_arg("gl_compatibility");
    tl_godot_add_arg("--log-file"); tl_godot_add_arg(logf);
    tl_godot_add_arg("--force-steam"); tl_godot_add_arg("off");
    for (int i = 0; STS2_ARGS_LOG[i]; i++) tl_godot_add_arg(STS2_ARGS_LOG[i]);
    tl_godot_add_arg("--main-pack"); tl_godot_add_arg(pck);
    return true;
}

/* What GodotApp's static initializer loads before the engine, by System.loadLibrary so that each one's JNI_OnLoad runs: the .NET
 * crypto library (Godot's own mono template; it keeps the VM for its Java calls), and in this port FMOD, which finds Java
 * the same way, and MonoMod's code-patching shim. */
static void preload_libraries(const tl_zip *apk)
{
    static const char *const names[] = { "fmod", "fmodstudio", "monomod_android_libc_shim", "System.Security.Cryptography.Native.Android", NULL };
    for (int i = 0; names[i]; i++) {
        char entry[200];
        snprintf(entry, sizeof(entry), "lib/arm64-v8a/lib%s.so", names[i]);
        if (!tl_zip_find(apk, entry)) continue;
        jvalue a; a.j = 0; a.l = tl_jni_new_string(names[i]);
        tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
        if (tl_jni_pending()) { tl_log_line("dotnet: System.loadLibrary(%s) failed", names[i]); tl_jni_clear(); }
        else tl_log_line("dotnet: lib%s.so loaded, as the launcher does", names[i]);
    }
}

/* -------------------------------------------------------------------- entry */

bool tl_godot_dotnet_prepare(const char *data_dir)
{
    const tl_zip *apk = tl_ld_apk_at(0);
    if (!apk || !tl_zip_find(apk, "lib/arm64-v8a/libmonosgen-2.0.so")) return true;     /* not a .NET game */
    snprintf(FILES, sizeof(FILES), "%s/files", data_dir);
    tl_log_line("dotnet: a Godot .NET game (Mono)");
    runtime_env();
    if (tl_zip_find(apk, "assets/payload/SlayTheSpire2.zip") && tl_zip_find(apk, "assets/dotnet_bcl/GodotSharp.dll") && !prepare_sts2(apk)) return false;
    preload_libraries(apk);
    return true;
}
