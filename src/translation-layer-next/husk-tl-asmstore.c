/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * .NET for Android's assembly store: every managed assembly of an app, in one blob inside an ELF (libassembly-store.so), each LZ4 compressed.
 *
 *     "XABA", version, entry count, index entry count, index size
 *     index: (hash, descriptor number, flag) for each name, for looking one up
 *     descriptors: (mapping index, data offset, data size, debug offset, debug size, config offset, config size), 28 bytes each
 *     names: (length, bytes) for each descriptor, in the same order
 *     data: "XALZ", descriptor index, uncompressed size, LZ4 block; or a plain PE image
 *
 * Mono's own host reads this through libmonodroid. A host that starts Mono itself gives it files in a directory instead.
 */
#include "husk-tl-asmstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INDEX_ENTRY_SIZE 13
#define DESCRIPTOR_SIZE 28

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* One raw LZ4 block. The number of bytes written, or -1 if the block runs off either buffer. */
static long lz4_block(const uint8_t *src, size_t slen, uint8_t *dst, size_t dcap)
{
    size_t s = 0, d = 0;
    while (s < slen) {
        uint8_t token = src[s++];
        size_t lit = token >> 4;
        if (lit == 15) { uint8_t x; do { if (s >= slen) return -1; x = src[s++]; lit += x; } while (x == 255); }
        if (lit > slen - s || lit > dcap - d) return -1;
        memcpy(dst + d, src + s, lit);
        s += lit; d += lit;
        if (s >= slen) break;                                       /* the last sequence is literals only */
        if (slen - s < 2) return -1;
        size_t off = (size_t)src[s] | ((size_t)src[s + 1] << 8);
        s += 2;
        if (off == 0 || off > d) return -1;
        size_t len = token & 15;
        if (len == 15) { uint8_t x; do { if (s >= slen) return -1; x = src[s++]; len += x; } while (x == 255); }
        len += 4;
        if (len > dcap - d) return -1;
        for (size_t i = 0; i < len; i++, d++) dst[d] = dst[d - off];  /* byte by byte: a match may overlap what it writes */
    }
    return (long)d;
}

static bool write_atomically(const char *dir, const char *name, const uint8_t *data, size_t n)
{
    char path[1200], part[1210];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    remove(path);
    if (ok) ok = rename(part, path) == 0;
    if (!ok) remove(part);
    return ok;
}

int tl_asmstore_unpack(const uint8_t *file, size_t len, const char *dir, char *err, size_t errlen)
{
    const uint8_t *x = NULL;
    for (size_t i = 0; i + 20 <= len; i += 4) if (!memcmp(file + i, "XABA", 4)) { x = file + i; break; }
    if (!x) { snprintf(err, errlen, "no assembly store in this file"); return -1; }
    size_t room = len - (size_t)(x - file);
    uint32_t entries = rd32(x + 8), index_size = rd32(x + 16);
    if (entries == 0 || entries > 65536 || (uint64_t)index_size + 20 + (uint64_t)entries * DESCRIPTOR_SIZE > room) { snprintf(err, errlen, "the assembly store's header makes no sense"); return -1; }
    const uint8_t *desc = x + 20 + index_size;
    const uint8_t *np = desc + (size_t)entries * DESCRIPTOR_SIZE, *end = x + room;
    int written = 0;
    for (uint32_t i = 0; i < entries; i++) {
        if (end - np < 4) { snprintf(err, errlen, "the assembly store's names end early"); return -1; }
        uint32_t nl = rd32(np);
        np += 4;
        if (nl == 0 || nl >= 256 || (size_t)(end - np) < nl) { snprintf(err, errlen, "assembly store name %u is bad", i); return -1; }
        char name[256];
        memcpy(name, np, nl); name[nl] = 0;
        np += nl;
        if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) { snprintf(err, errlen, "assembly store name %s is not a plain file name", name); return -1; }

        const uint8_t *dp = desc + (size_t)i * DESCRIPTOR_SIZE;
        uint32_t off = rd32(dp + 4), size = rd32(dp + 8);
        if ((uint64_t)off + size > room || size < 12) { snprintf(err, errlen, "%s lies outside the store", name); return -1; }
        const uint8_t *data = x + off;
        bool ok;
        if (!memcmp(data, "XALZ", 4)) {
            uint32_t full = rd32(data + 8);
            if (full == 0 || full > (1u << 30)) { snprintf(err, errlen, "%s claims a size of %u", name, full); return -1; }
            uint8_t *out = malloc(full);
            if (!out) { snprintf(err, errlen, "no memory for %s", name); return -1; }
            long got = lz4_block(data + 12, size - 12, out, full);
            if (got != (long)full || out[0] != 'M' || out[1] != 'Z') { free(out); snprintf(err, errlen, "%s did not decompress", name); return -1; }
            ok = write_atomically(dir, name, out, full);
            free(out);
        } else {
            ok = write_atomically(dir, name, data, size);
        }
        if (!ok) { snprintf(err, errlen, "cannot write %s", name); return -1; }
        written++;
    }
    return written;
}
