/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Private interface between layer_status.c and its peripheral-side
 * behavior receiver (behaviors/animation_layer_status.c). Not part of the
 * public API in include/cormoran/animation/ - this is transport-hack
 * plumbing for the quarantined central->peripheral layer-status relay
 * (DESIGN.md #3.7, #3.8), not something other animations should call.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Applies a layer-status bitmask relayed from the central, on the
 * split-peripheral side. No-op on central/non-split builds.
 */
void zmk_animation_layer_status_set_status(uint32_t layer_status);

#ifdef __cplusplus
}
#endif
