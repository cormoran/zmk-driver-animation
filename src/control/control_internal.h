/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Private interface between control.c, power_policy.c,
 * overlay_queue.c and core/render.c. Not part of the public API in
 * include/cormoran/animation/.
 *
 * `control.c` owns the single `struct zmk_animation_control_state` (DESIGN.md
 * #3.4) and the DT-fixed animation lists; `power_policy.c` and
 * `overlay_queue.c` are given a read/write view onto pieces of that state
 * (and the DT lists) through this header instead of poking at file-scope
 * globals directly, so nothing here needs collision-prone unprefixed names
 * (DESIGN.md #4 item 4) or a second copy of the singleton-resolution logic.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#include <cormoran/animation/animation.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns true if the control singleton is enabled and running
 * (DESIGN.md's `enabled` field), i.e. rendering/overlay dispatch should
 * proceed. False (and all render/query calls below are no-ops / return
 * defaults) when disabled or not registered.
 */
bool zmk_animation_control_is_running(void);

/** Currently selected base animation device for the active power source. */
const struct device *zmk_animation_control_current_base(void);

/**
 * @brief Brightness multiplier (0.0..1.0) for the active power source, per
 * DESIGN.md #3.4/#3.6. Applied by render.c to `pixels[i].value.{r,g,b}`
 * after the base+overlay render, and nowhere else.
 */
float zmk_animation_control_current_brightness_multiplier(void);

/**
 * @brief Renders the currently active ad-hoc overlay (if any) on top of
 * `pixels`, and advances overlay_queue's internal state (finishing /
 * dequeuing as needed). No-op if no overlay is playing.
 */
void zmk_animation_overlay_render(struct zmk_animation_pixel *pixels, size_t num_pixels);

#ifdef __cplusplus
}
#endif
