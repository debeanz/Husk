/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The C side of the app, as Swift sees it: the JIT memory the translation layer
 * runs games from, on-device pairing for Built-in StikJIT, and the translation
 * layer itself.
 */
#ifndef HUSK_BRIDGE_H
#define HUSK_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* --- JIT memory (JIT/husk-ios-jit.c) --- */
#include "JIT/husk-ios-jit.h"

/* --- On-device pairing for Built-in StikJIT (JITPairing.swift) --- */
#include "HuskRPPairing.h"

/* --- The translation layer --- */
/* See docs/04-translation-layer.md. */
#include "../../translation-layer/husk-tl.h"
#include "../../translation-layer-next/husk-tl-unity-app.h"

#endif /* HUSK_BRIDGE_H */
