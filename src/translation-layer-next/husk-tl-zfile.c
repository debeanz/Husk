/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A large deflated file read in place: random access into a ZIP entry without unpacking it.
 *
 * Deflate can only be read from the start, unless one knows where its blocks begin and what the 32 KiB of output before such a
 * place were (back-references reach that far). One pass over the stream records that every MiB or so -- the input offset, the
 * bit within its byte, the window -- in an index file (zlib's examples/zran.c). After that a read anywhere starts at the restart
 * point before it and inflates at most a MiB or two to get there; a reader that goes on reading where it stopped just continues.
 *
 * Slay the Spire 2's Android port carries its 1.8 GB game pack deflated inside a ZIP that the APK stores: unpacked, it would
 * need the space twice over (the APK is kept too). Read this way it costs an index of ~60 MiB.
 *
 * The file is presented at its own path as a sparse placeholder of the right size, so stat, open and seek need nothing special;
 * only reads are answered from here (husk-tl-bionic-io.c, and Godot's FileAccessHandler in husk-tl-jni-godot.c).
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-zfile.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "husk-tl-bionic.h"

#define WIN 32768u
#define SPAN (1u << 20)                 /* output between restart points */
#define MAGIC 0x5849425aU               /* "ZBIX" */

typedef struct { uint32_t magic, version; uint64_t usize, csize, count, table_off; } zhdr;
typedef struct { uint64_t out, in; uint32_t bits, pad; } zpoint;

typedef struct {
    dev_t dev; ino_t ino;
    const uint8_t *data;
    uint64_t csize, usize;
    int idx_fd;
    zpoint *pts;
    uint64_t npts;
} zsrc;

struct tl_zfile {
    zsrc *src;
    pthread_mutex_t lock;
    z_stream zs;
    bool live;
    uint64_t pos;                       /* output offset the stream has reached */
    uint64_t in_off;                    /* next input byte to hand zlib */
};

volatile bool tl_zfile_any;
static zsrc g_src[8];
static int g_nsrc;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------- the index */

static bool put_all(int fd, const void *p, size_t n, off_t at)
{
    while (n) { ssize_t w = pwrite(fd, p, n, at); if (w <= 0) return false; p = (const uint8_t *)p + w; n -= (size_t)w; at += w; }
    return true;
}

bool tl_zfile_build_index(const uint8_t *in, uint64_t csize, uint64_t usize, const char *index_path, char *err, size_t errlen)
{
    char part[1100];
    snprintf(part, sizeof(part), "%s.part", index_path);
    int fd = open(part, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { snprintf(err, errlen, "cannot create %s: %s", part, strerror(errno)); return false; }
    uint8_t *window = malloc(WIN);
    size_t cap = 4096, n = 0;
    zpoint *pts = malloc(cap * sizeof(*pts));
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    bool ok = window && pts && inflateInit2(&zs, -15) == Z_OK;
    zhdr h = { MAGIC, 1, usize, csize, 0, 0 };
    off_t at = sizeof(h);
    uint64_t totin = 0, totout = 0, last = 0, fed = 0;
    int ret = Z_OK;
    zs.avail_out = 0;
    while (ok && ret != Z_STREAM_END) {
        if (zs.avail_in == 0) {
            /* Block by block, the last one ends with everything out and nothing left in, before inflate says Z_STREAM_END. */
            if (fed >= csize) { if (totout == usize) break; snprintf(err, errlen, "the stream ends early"); ok = false; break; }
            uInt k = csize - fed > (1u << 30) ? (1u << 30) : (uInt)(csize - fed);
            zs.next_in = (Bytef *)(in + fed); zs.avail_in = k; fed += k;
        }
        do {
            if (zs.avail_out == 0) { zs.avail_out = WIN; zs.next_out = window; }
            totin += zs.avail_in; totout += zs.avail_out;
            ret = inflate(&zs, Z_BLOCK);
            totin -= zs.avail_in; totout -= zs.avail_out;
            if (ret == Z_NEED_DICT || ret == Z_MEM_ERROR || ret == Z_DATA_ERROR) { snprintf(err, errlen, "inflate failed (%d)", ret); ok = false; break; }
            if (ret == Z_BUF_ERROR) ret = Z_OK;                      /* needs more input: the outer loop gives it */
            if (ret == Z_STREAM_END) break;
            /* At the end of a block (not the last), a place a reader can start from. */
            if ((zs.data_type & 128) && !(zs.data_type & 64) && (totout == 0 || totout - last > SPAN)) {
                if (n == cap) { cap *= 2; pts = realloc(pts, cap * sizeof(*pts)); }
                pts[n] = (zpoint){ totout, totin, (uint32_t)(zs.data_type & 7), 0 };
                /* The 32 KiB of output before this place, oldest first: the circular buffer from where it will next write. */
                uint8_t w[WIN];
                unsigned left = zs.avail_out;
                if (left) memcpy(w, window + WIN - left, left);
                if (left < WIN) memcpy(w + left, window, WIN - left);
                if (!put_all(fd, w, WIN, at)) { snprintf(err, errlen, "writing the index: %s", strerror(errno)); ok = false; break; }
                at += WIN;
                n++;
                last = totout;
            }
        } while (zs.avail_in != 0);
    }
    inflateEnd(&zs);
    if (ok && totout != usize) { snprintf(err, errlen, "inflated to %llu bytes, not %llu", (unsigned long long)totout, (unsigned long long)usize); ok = false; }
    if (ok) {
        h.count = n; h.table_off = (uint64_t)at;
        ok = put_all(fd, pts, n * sizeof(*pts), at) && put_all(fd, &h, sizeof(h), 0);
        if (!ok) snprintf(err, errlen, "writing the index: %s", strerror(errno));
    }
    free(window);
    free(pts);
    if (close(fd) != 0) ok = false;
    if (ok && rename(part, index_path) != 0) { snprintf(err, errlen, "renaming the index: %s", strerror(errno)); ok = false; }
    if (!ok) unlink(part);
    return ok;
}

static bool read_header(int fd, zhdr *h)
{
    return pread(fd, h, sizeof(*h), 0) == (ssize_t)sizeof(*h) && h->magic == MAGIC && h->version == 1;
}

bool tl_zfile_index_ok(const char *index_path, uint64_t csize, uint64_t usize)
{
    int fd = open(index_path, O_RDONLY);
    if (fd < 0) return false;
    zhdr h;
    struct stat st;
    bool ok = read_header(fd, &h) && h.csize == csize && h.usize == usize && h.count && fstat(fd, &st) == 0
              && (uint64_t)st.st_size == h.table_off + h.count * sizeof(zpoint) && h.table_off == sizeof(h) + h.count * WIN;
    close(fd);
    return ok;
}

bool tl_zfile_register(const char *placeholder, const uint8_t *data, uint64_t csize, uint64_t usize, const char *index_path)
{
    struct stat st;
    if (stat(placeholder, &st) != 0 || (uint64_t)st.st_size != usize) { tl_log_line("zfile: %s is not the placeholder it should be", placeholder); return false; }
    int fd = open(index_path, O_RDONLY);
    zhdr h;
    if (fd < 0 || !read_header(fd, &h) || h.usize != usize || h.csize != csize) { if (fd >= 0) close(fd); tl_log_line("zfile: %s is not a usable index", index_path); return false; }
    zpoint *pts = malloc(h.count * sizeof(*pts));
    if (!pts || pread(fd, pts, h.count * sizeof(*pts), (off_t)h.table_off) != (ssize_t)(h.count * sizeof(*pts))) { free(pts); close(fd); return false; }
    pthread_mutex_lock(&g_lock);
    bool ok = g_nsrc < 8;
    if (ok) g_src[g_nsrc++] = (zsrc){ st.st_dev, st.st_ino, data, csize, usize, fd, pts, h.count };
    pthread_mutex_unlock(&g_lock);
    if (!ok) { free(pts); close(fd); return false; }
    tl_zfile_any = true;
    tl_log_line("zfile: %s reads from the APK (%.2f GB, %llu restart points)", placeholder, usize / 1e9, (unsigned long long)h.count);
    return true;
}

/* ---------------------------------------------------------------- reading */

tl_zfile *tl_zfile_open_fd(int fd)
{
    if (!tl_zfile_any) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    zsrc *src = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsrc && !src; i++) if (g_src[i].dev == st.st_dev && g_src[i].ino == st.st_ino) src = &g_src[i];
    pthread_mutex_unlock(&g_lock);
    if (!src) return NULL;
    tl_zfile *f = calloc(1, sizeof(*f));
    f->src = src;
    pthread_mutex_init(&f->lock, NULL);
    return f;
}

void tl_zfile_close(tl_zfile *f)
{
    if (!f) return;
    if (f->live) inflateEnd(&f->zs);
    pthread_mutex_destroy(&f->lock);
    free(f);
}

/* Start over at the restart point at or before `off`. */
static bool restart(tl_zfile *f, uint64_t off)
{
    zsrc *s = f->src;
    uint64_t lo = 0, hi = s->npts;
    while (hi - lo > 1) { uint64_t mid = (lo + hi) / 2; if (s->pts[mid].out <= off) lo = mid; else hi = mid; }
    /* Before the first restart point (if the stream's very start is not one): the start, with no window. */
    static const zpoint start = { 0, 0, 0, 0 };
    bool from_start = !s->npts || s->pts[0].out > off;
    const zpoint *p = from_start ? &start : &s->pts[lo];
    if (f->live) inflateEnd(&f->zs);
    f->live = false;
    memset(&f->zs, 0, sizeof(f->zs));
    if (inflateInit2(&f->zs, -15) != Z_OK) return false;
    f->live = true;
    if (p->bits) {
        if (p->in == 0) return false;
        inflatePrime(&f->zs, (int)p->bits, s->data[p->in - 1] >> (8 - p->bits));
    }
    if (!from_start) {
        uint8_t w[WIN];
        if (pread(s->idx_fd, w, WIN, (off_t)(sizeof(zhdr) + lo * WIN)) != (ssize_t)WIN) return false;
        inflateSetDictionary(&f->zs, w, WIN);
    }
    f->in_off = p->in;
    f->pos = p->out;
    return true;
}

/* Inflate up to n bytes into out (NULL: thrown away, for skipping). */
static long pull(tl_zfile *f, uint8_t *out, size_t n)
{
    uint8_t junk[16384];
    size_t got = 0;
    while (got < n) {
        if (f->zs.avail_in == 0) {
            uint64_t left = f->src->csize - f->in_off;
            if (!left) break;
            uInt k = left > (1u << 30) ? (1u << 30) : (uInt)left;
            f->zs.next_in = (Bytef *)(f->src->data + f->in_off);
            f->zs.avail_in = k;
            f->in_off += k;
        }
        size_t want = n - got;
        uint8_t *dst = out ? out + got : junk;
        if (!out && want > sizeof(junk)) want = sizeof(junk);
        if (want > (1u << 30)) want = 1u << 30;
        f->zs.next_out = dst;
        f->zs.avail_out = (uInt)want;
        int r = inflate(&f->zs, Z_NO_FLUSH);
        size_t made = want - f->zs.avail_out;
        got += made;
        f->pos += made;
        if (r == Z_STREAM_END) break;
        if (r == Z_OK || (r == Z_BUF_ERROR && (made || f->zs.avail_in == 0))) continue;   /* progress, or needs input */
        return -1;
    }
    return (long)got;
}

long tl_zfile_pread(tl_zfile *f, void *buf, size_t n, uint64_t off)
{
    if (!f) return -1;
    zsrc *s = f->src;
    if (off >= s->usize || !n) return 0;
    if (n > s->usize - off) n = (size_t)(s->usize - off);
    pthread_mutex_lock(&f->lock);
    long r = -1;
    /* Going on from where it is is cheaper than starting over, up to about one restart span ahead. */
    if (!f->live || off < f->pos || off - f->pos > SPAN + SPAN / 2) {
        if (!restart(f, off)) goto out;
    }
    while (f->pos < off) {
        long k = pull(f, NULL, (size_t)(off - f->pos));
        if (k <= 0) goto out;
    }
    r = pull(f, buf, n);
out:
    if (r < 0) { if (f->live) inflateEnd(&f->zs); f->live = false; tl_log_line("zfile: read of %zu bytes at %llu failed", n, (unsigned long long)off); }
    pthread_mutex_unlock(&f->lock);
    return r;
}
