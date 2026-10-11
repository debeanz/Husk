/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stores into code, done through the JIT region's writable view (husk-tl-codewrite.c). */
#ifndef HUSK_TL_CODEWRITE_H
#define HUSK_TL_CODEWRITE_H

#include <stdbool.h>

/* From now on, stores into the executable view are carried out through the writable one, and the guest may map executable
 * memory of its own (it gets a piece of the JIT region). For mod loaders that hook the game (Geode). */
void tl_codewrite_enable(void);
bool tl_codewrite_enabled(void);
/* Stores carried out so far, for the log. */
long tl_codewrite_count(void);

#include <stddef.h>
/* Set once tl_codewrite_enable has run: the guest's memcpy/memmove/memset look here before anything else. */
extern volatile bool tl_codewrite_active;
/* A copy or fill whose destination is code, done through the writable view. False (nothing done) for any other memory. */
bool tl_codewrite_copy(void *dst, const void *src, size_t n);
bool tl_codewrite_fill(void *dst, int c, size_t n);

#endif
