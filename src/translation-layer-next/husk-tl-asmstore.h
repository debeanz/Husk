/* SPDX-License-Identifier: GPL-2.0-or-later */
/* .NET for Android's assembly store (husk-tl-asmstore.c). */
#ifndef HUSK_TL_ASMSTORE_H
#define HUSK_TL_ASMSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every assembly of the store in `file` written to the directory `dir`, which must exist. How many, or -1 with the reason in `err`. */
int tl_asmstore_unpack(const uint8_t *file, size_t len, const char *dir, char *err, size_t errlen);

#endif
