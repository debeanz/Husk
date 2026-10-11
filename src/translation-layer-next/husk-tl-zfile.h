/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A large deflated file read where it lies, in the APK, without unpacking it to disk (husk-tl-zfile.c). */
#ifndef HUSK_TL_ZFILE_H
#define HUSK_TL_ZFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct tl_zfile tl_zfile;

/* One pass over a raw deflate stream, writing an index of restart points (each with the 32 KiB of output before it) to
 * `index_path`. Done once; minutes of unpacking and gigabytes of disk become seconds and a few dozen MiB. */
bool tl_zfile_build_index(const uint8_t *deflate, uint64_t csize, uint64_t usize, const char *index_path, char *err, size_t errlen);
/* Whether the index at `index_path` describes this stream (sizes match, file whole). */
bool tl_zfile_index_ok(const char *index_path, uint64_t csize, uint64_t usize);

/* `placeholder` (a sparse file of the right size, so stat and seeking answer correctly) reads as the inflated stream from now on.
 * The deflate bytes must stay mapped for the life of the process (they are the APK's). */
bool tl_zfile_register(const char *placeholder, const uint8_t *deflate, uint64_t csize, uint64_t usize, const char *index_path);

/* The registered file a descriptor was opened on, as a reader of its own (one per descriptor, with its own position), or NULL. */
tl_zfile *tl_zfile_open_fd(int fd);
/* Bytes of the inflated file at `off`: as many as asked unless the end comes first; -1 if the stream is damaged. */
long tl_zfile_pread(tl_zfile *f, void *buf, size_t n, uint64_t off);
void tl_zfile_close(tl_zfile *f);
/* True once anything is registered: callers check this before looking further. */
extern volatile bool tl_zfile_any;

#endif
