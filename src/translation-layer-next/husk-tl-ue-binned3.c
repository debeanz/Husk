/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unreal Engine 5's allocator (FMallocBinned3) reserves one block of addresses as it starts: 76 pools of 1 GiB, one per size of allocation, 76 GiB
 * in all, of which a pool only ever touches its first few MiB. An Android phone has the addresses for that. An iPhone app without the
 * extended-virtual-addressing entitlement has 64 GiB in one piece at most, as iOS keeps 0x1000000000-0x7000000000 for the GPU, so the engine runs out of memory
 * before anything else has happened. The size is compiled in, as a constant and as shifts where the allocator works out which pool an address is in.
 *
 * So the pools are made smaller where they are defined and used, before the library's constructors run: 512 MiB each, which is 38 GiB. A game would need half a
 * GiB of allocations of one size to notice. Everything the allocator does with the size, in a game's build as compiled:
 *
 *     mov  x0, #N, lsl #32                 the block (N units of 4 GiB)
 *     mov  wd, #0x40000000                 a pool, for the tables that track it
 *     lsr  xd, xn, #30                     which pool an offset is in
 *     ubfx xd, xn, #30, #32                the same
 *     lsr  xd, xn, #29; and xd, xd, #0x7fffffffe      the same, times two, to index a table of shorts
 *     lsl  xd, xn, #30                     where a pool starts
 *     add  xd, xn, xm, lsl #30             the same
 *     and  xd, xn, #0x3fffffffc0000000     the same, from an offset
 *     lsr  xd, xn, #32; cmp xd, #N-1 or #N     whether an address is in the block at all
 *
 * The last matters most: a big allocation goes to the system, and can land just past the smaller block. Asked whether that address is its own by the old
 * size, the allocator would take it for a pool's.
 */
#define _DARWIN_C_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How far around the block's size the allocator's code lies, in instructions. */
#define BEFORE (0x2000 / 4)
#define AFTER  (0x7000 / 4)

#define MOVZ_X0_LSL32(n) (0xD2C00000u | ((uint32_t)(n) << 5))

/* The block, in units of 4 GiB, to hold pools 2^s times smaller: N of those units hold 4N pools. */
static unsigned block_units(unsigned n, unsigned s) { return (n + (1u << s) - 1) >> s; }

static uint32_t set_ubfm(uint32_t w, unsigned immr, unsigned imms) { return (w & ~(uint32_t)0x003FFC00u) | (immr << 16) | (imms << 10); }

/*
 * One instruction of the allocator's as it is for pools 2^s times smaller, or as it was. `prev` and `next` are its neighbours, for the ones that
 * mean something only together.
 */
static uint32_t binned3_rewrite(uint32_t w, uint32_t prev, const uint32_t *next, size_t nnext, unsigned n, unsigned s)
{
    unsigned b = block_units(n, s);
    if (w == MOVZ_X0_LSL32(n)) return MOVZ_X0_LSL32(b);
    if ((w & 0xFFFFFFE0u) == 0x52A80000u) return 0x52A00000u | ((0x4000u >> s) << 5) | (w & 31);                    /* mov wd, #0x40000000 */
    if ((w & 0xFFFFFC00u) == 0xD35EFC00u) return set_ubfm(w, 30 - s, 63);                                           /* lsr xd, xn, #30 */
    if ((w & 0xFFFFFC00u) == 0xD35EF400u) return set_ubfm(w, 30 - s, 61 - s);                                       /* ubfx xd, xn, #30, #32 */
    if ((w & 0xFFFFFC00u) == 0xD3628400u) return set_ubfm(w, 34 + s, 33 + s);                                       /* lsl xd, xn, #30 */
    if ((w & 0xFFE0FC00u) == 0x8B007800u) return (w & ~(uint32_t)0xFC00u) | ((30 - s) << 10);                       /* add xd, xn, xm, lsl #30 */
    if ((w & 0xFFFFFC00u) == 0x92627C00u) return set_ubfm(w, 34 + s, 31 + s);                                       /* and xd, xn, #0x3fffffffc0000000 */
    if ((w & 0xFFFFFC00u) == 0xD35DFC00u) {                                                                         /* lsr xd, xn, #29 ... and xd, xd, #0x7fffffffe */
        unsigned d = w & 31;
        for (size_t i = 0; i < nnext && i < 3; i++)
            if ((next[i] & 0xFFFFFC00u) == 0x927F8400u && (next[i] & 31) == d && ((next[i] >> 5) & 31) == d) return set_ubfm(w, 29 - s, 63);
        return w;
    }
    /* cmp xn, #imm, right after lsr xn, xm, #32 */
    if ((w & 0xFFC0001Fu) == 0xF100001Fu && (prev & 0xFFFFFC00u) == 0xD360FC00u && (prev & 31) == ((w >> 5) & 31)) {
        unsigned imm = (w >> 10) & 0xFFF;
        if (imm == n - 1) return (w & ~(uint32_t)0x3FFC00u) | ((b - 1) << 10);
        if (imm == n) return (w & ~(uint32_t)0x3FFC00u) | (b << 10);
    }
    return w;
}

/* The instruction that sets the block's size, in code of `count` words, and the number of 4 GiB units it asks for; 0 if none, or more than one, looks like it. */
static size_t binned3_find(const uint32_t *code, size_t count, unsigned *n_out)
{
    size_t at = 0;
    unsigned n = 0, found = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        uint32_t w = code[i];
        /* mov x0, #N, lsl #32 before a call, with a pool's size close by: the allocator's constructor */
        if ((w & 0xFFE0001Fu) != 0xD2C00000u || (code[i + 1] & 0xFC000000u) != 0x94000000u) continue;
        unsigned u = (w >> 5) & 0xFFFF;
        if (u < 8 || u > 0x40) continue;
        bool pool = false;
        for (size_t j = i; j < count && j < i + 64 && !pool; j++) pool = (code[j] & 0xFFFFFFE0u) == 0x52A80000u;
        if (!pool) continue;
        at = i; n = u; found++;
    }
    if (found != 1) return 0;
    *n_out = n;
    return at;
}

#ifndef HUSK_BINNED3_TEST

#include <sys/mman.h>

#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

/* Whether this many addresses can be had in one piece right now, mapped as the engine maps them. */
static bool can_reserve(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return false;
    munmap(p, bytes);
    return true;
}

void tl_ue_fit_allocator(tl_lib *lib)
{
    const uint8_t *text = NULL;
    size_t bytes = 0;
    if (!tl_ld_code_range(lib, &text, &bytes)) return;
    const uint32_t *code = (const uint32_t *)text;
    size_t count = bytes / 4;
    unsigned n = 0;
    size_t at = binned3_find(code, count, &n);
    if (!at) return;
    if (can_reserve((size_t)n << 32)) { tl_log_line("ue: the allocator's %u GiB of addresses fit: left as it is", n * 4); return; }
    unsigned s = 0;
    for (unsigned t = 1; t <= 3 && !s; t++) if (can_reserve((size_t)block_units(n, t) << 32)) s = t;
    if (!s) { tl_log_line("ue: the allocator wants %u GiB of addresses and not even an eighth of that is free", n * 4); return; }
    size_t lo = at > BEFORE ? at - BEFORE : 0, hi = at + AFTER < count ? at + AFTER : count;
    ptrdiff_t delta = tl_xmem_delta();
    int changed = 0;
    for (size_t i = lo; i < hi; i++) {
        uint32_t was = code[i], now = binned3_rewrite(was, i ? code[i - 1] : 0, code + i + 1, hi - i - 1, n, s);
        if (now == was) continue;
        *(uint32_t *)((uint8_t *)(code + i) + delta) = now;
        changed++;
    }
    tl_xmem_flush((void *)(code + lo), (hi - lo) * 4);
    tl_log_line("ue: the allocator wants %u GiB of addresses, more than iOS has in one piece: its pools are %u MiB each instead of 1 GiB, in %u GiB (%d instructions)",
                n * 4, 1024u >> s, block_units(n, s) * 4, changed);
}

#endif
