/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A Mach-O executable mapped by a guest's own loader. An Android port can run a game's Mac build this way: libmachismo.so reserves
 * an address range, maps each segment read/write, preads it in and mprotects it, as dyld would.
 *
 * On a phone that sequence can not work as written. Code must be in the JIT region, where it is executable and never writable;
 * data must be writable; and the two must stay the exact distance apart the linker left them, because the code reaches its data
 * with adrp. An ELF library gets there by the loader retargeting its adrps at the writable view, but here another loader is
 * applying the fixups, and a pointer it stored would not equal the address the code computes.
 *
 * The two views of the region make this simple, as the writable one directly follows the executable one:
 *
 *     ... executable view ... [ island pool ][ __TEXT ] | [ __DATA_CONST ][ __DATA ][ __LINKEDIT ] ... writable view ...
 *
 * __TEXT ends where the writable view begins, so the rest of the image lands in memory that really is writable, at the addresses
 * the code expects. Nothing new is asked of the system: every page was granted when the region was. The low end of the region is
 * kept back, as those same pages are what the data is.
 *
 * Writes the loader makes to __TEXT and to the island pool (its patches and trampolines) fault and are carried out through the
 * writable view (husk-tl-codewrite.c). mprotect on the region already does nothing while that is on.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-macho.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

/* The room libmachismo's loader leaves after the image for its islands, and where it notes their address. */
#define POOL_BYTES ((size_t)2 << 20)
#define RESULTS_POOL_BASE 0x17

/* Where Darwin's commpage is, which is also where machismo builds an imitation of it. On a phone the real one is there and in use. */
#define COMMPAGE_ADDR ((uintptr_t)0xfffffc000ull)

#define MH_MAGIC_64 0xfeedfacfu
#define FAT_MAGIC_BE 0xcafebabeu
#define CPU_ARM64 0x0100000cu
#define LC_SEGMENT_64 0x19u

/* Linux's flag values, which are what a guest passes. */
#define G_MAP_FIXED 0x10
#define G_MAP_ANON 0x20

static struct {
    bool armed, reserved, pool_noted;
    uint64_t vm_base;
    size_t total, text_bytes;
    uint8_t *pool, *text;
} M;

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t le64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

bool tl_macho_prepare(const char *path, char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno)); return false; }
    bool ok = false;
    uint8_t *cmds = NULL;
    uint8_t head[32];
    off_t slice = 0;
    if (pread(fd, head, sizeof(head), 0) != (ssize_t)sizeof(head)) { snprintf(err, errlen, "cannot read the executable's header"); goto out; }
    if (be32(head) == FAT_MAGIC_BE) {
        uint32_t n = be32(head + 4);
        bool found = false;
        for (uint32_t i = 0; i < n && i < 16 && !found; i++) {
            uint8_t a[20];
            if (pread(fd, a, sizeof(a), 8 + (off_t)i * 20) != (ssize_t)sizeof(a)) break;
            if (be32(a) == CPU_ARM64) { slice = be32(a + 8); found = true; }
        }
        if (!found || pread(fd, head, sizeof(head), slice) != (ssize_t)sizeof(head)) { snprintf(err, errlen, "the executable has no arm64 part"); goto out; }
    }
    if (le32(head) != MH_MAGIC_64 || le32(head + 4) != CPU_ARM64) { snprintf(err, errlen, "the executable is not arm64 Mach-O"); goto out; }
    uint32_t ncmds = le32(head + 16), sizecmds = le32(head + 20);
    if (sizecmds == 0 || sizecmds > (16u << 20) || !(cmds = malloc(sizecmds)) || pread(fd, cmds, sizecmds, slice + 32) != (ssize_t)sizecmds) { snprintf(err, errlen, "cannot read the executable's load commands"); goto out; }

    /* The shape this layout needs: one executable segment, first, and everything after it contiguous and not executable. */
    uint64_t base = 0, end = 0, text_end = 0;
    bool first = true;
    size_t off = 0;
    for (uint32_t i = 0; i < ncmds && off + 8 <= sizecmds; i++) {
        uint32_t cmd = le32(cmds + off), size = le32(cmds + off + 4);
        if (size < 8 || off + size > sizecmds) break;
        if (cmd == LC_SEGMENT_64 && size >= 72 && strncmp((const char *)cmds + off + 8, "__PAGEZERO", 16)) {
            uint64_t vm = le64(cmds + off + 24), vmsize = le64(cmds + off + 32);
            uint32_t initprot = le32(cmds + off + 60);
            char name[17]; memcpy(name, cmds + off + 8, 16); name[16] = 0;
            if (vmsize) {
                if (first) {
                    if (!(initprot & 4)) { snprintf(err, errlen, "segment %s comes first and is not executable", name); goto out; }
                    base = vm; text_end = vm + vmsize; first = false;
                } else {
                    if (initprot & 4) { snprintf(err, errlen, "segment %s is a second executable segment", name); goto out; }
                    if (vm != end) { snprintf(err, errlen, "segment %s does not follow the one before it", name); goto out; }
                }
                end = vm + vmsize;
            }
        }
        off += size;
    }
    if (first || end <= text_end || (text_end - base) % TL_XMEM_PAGE || (end - text_end) % TL_XMEM_PAGE) { snprintf(err, errlen, "the executable's segments are not laid out on 16 KiB pages"); goto out; }

    char why[200] = "";
    if (!tl_xmem_open(768u << 20, why, sizeof(why))) { snprintf(err, errlen, "%s", why); goto out; }
    uint8_t *high;
    M.text_bytes = (size_t)(text_end - base);
    M.total = (size_t)(end - base);
    if (!tl_xmem_reserve_straddle(M.total - M.text_bytes, POOL_BYTES + M.text_bytes, &high, why, sizeof(why))) { snprintf(err, errlen, "%s", why); goto out; }
    M.pool = high;
    M.text = high + POOL_BYTES;
    M.vm_base = base;
    M.armed = true;
    memset(M.pool + tl_xmem_delta(), 0, POOL_BYTES);
    tl_log_line("macho: %.1f MiB of code at %p, %.1f MiB of data after it, islands at %p", M.text_bytes / 1048576.0, (void *)M.text, (M.total - M.text_bytes) / 1048576.0, (void *)M.pool);
    ok = true;
out:
    free(cmds);
    close(fd);
    return ok;
}

bool tl_macho_is_code(const void *p, size_t n)
{
    const uint8_t *b = p;
    return M.reserved && b >= M.text && n <= M.text_bytes && b + n <= M.text + M.text_bytes;
}

/* machismo noted the island pool as the room after the image, which here is in the writable view and would not execute. */
static void note_pool(void)
{
    if (M.pool_noted) return;
    M.pool_noted = true;
    tl_lib *lib = tl_ld_find_lib("libmachismo.so");
    uint64_t *results = lib ? tl_ld_sym(lib, "machismo_load_results") : NULL;
    if (!results) { tl_log_line("macho: libmachismo.so has no machismo_load_results: its islands will not be executable"); return; }
    results[RESULTS_POOL_BASE] = (uint64_t)(uintptr_t)M.pool;
}

bool tl_macho_mmap(void *addr, size_t len, int prot, int flags, void **result)
{
    if (!M.armed) return false;
    /* The real commpage is there, and the system's own code reads it. "Continuing without commpage" is what machismo does next. */
    if ((uintptr_t)addr == COMMPAGE_ADDR) {
        tl_set_guest_errno(12);                                                                                            /* ENOMEM */
        *result = MAP_FAILED;
        return true;
    }
    /* A stack it makes just below the commpage, fixed if need be, and does not go on to use. Nothing of the guest's belongs that close to the system's page, so it gets ordinary memory. */
    if ((uintptr_t)addr + len == COMMPAGE_ADDR) {
        void *r = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) tl_set_guest_errno(12);                                                                       /* ENOMEM */
        *result = r;
        return true;
    }
    /* The reservation: PROT_NONE, anonymous, at the linked address, as long as the image and the pool. */
    if (!M.reserved) {
        if (prot == 0 && (flags & G_MAP_ANON) && !(flags & G_MAP_FIXED) && (uint64_t)(uintptr_t)addr == M.vm_base && len == M.total + POOL_BYTES) {
            M.reserved = true;
            tl_log_line("macho: the loader reserved the image at %p", (void *)M.text);
            *result = M.text;
            return true;
        }
        return false;
    }
    /* A segment, about to be read in. Pages stay where they are; what it expects of them is that they are zero. */
    uint8_t *a = addr;
    if ((flags & G_MAP_FIXED) && a >= M.text && len <= M.total && a + len <= M.text + M.total) {
        note_pool();
        uint8_t *text_end = M.text + M.text_bytes;
        if (a < text_end) {
            size_t n = len < (size_t)(text_end - a) ? len : (size_t)(text_end - a);
            memset(a + tl_xmem_delta(), 0, n);
            if (len > n) memset(text_end, 0, len - n);
        } else {
            memset(a, 0, len);
        }
        *result = addr;
        return true;
    }
    return false;
}
