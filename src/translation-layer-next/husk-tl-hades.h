/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Hades, in its Android port (husk-tl-hades.c). The NativeActivity driver hands it over when the APK has libhades.so. */
#ifndef HUSK_TL_HADES_H
#define HUSK_TL_HADES_H

#include <stdbool.h>

#include "husk-tl-gameactivity.h"       /* tl_ga_config */

/* After the APK's libraries are known and before any is loaded. */
bool tl_hades_start(const tl_ga_config *cfg);
bool tl_hades_run(void);
void tl_hades_touch(int phase, int id, float x, float y);       /* surface pixels, y down; phase 0 down, 1 move, 2 up, 3 cancel */
void tl_hades_set_paused(bool paused);

#endif
