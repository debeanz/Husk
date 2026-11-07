/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A Mach-O executable that a guest's own loader maps through mmap and pread (husk-tl-macho.c). */
#ifndef HUSK_TL_MACHO_H
#define HUSK_TL_MACHO_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Read the executable's load commands and set aside memory for it. Must come before any library is loaded: see
 * tl_xmem_reserve_straddle. Returns false and says why in `err`.
 */
bool tl_macho_prepare(const char *path, char *err, size_t errlen);

/* Before tl_macho_prepare, for a guest loader that is not libmachismo.so: see tl_macho_mmap. */
void tl_macho_any_loader(void);

/* The linked address of a symbol the executable defines, from its symbol table (a Mach-O name, with its leading underscore). 0 if it has none. */
unsigned long long tl_macho_symbol_address(const char *path, const char *name);

/* The guest's mmap, when it concerns the image: true if handled, with the result in `*result`. A mapping of a file is left to the caller to read in. */
bool tl_macho_mmap(void *addr, size_t len, int prot, int flags, void **result);

/* How many of the n bytes at p are the image's code, which is read into through the writable view. */
size_t tl_macho_code_bytes(const void *p, size_t n);

/* Whether a range is the image's code, which is read into through the writable view. */
bool tl_macho_is_code(const void *p, size_t n);

#endif
