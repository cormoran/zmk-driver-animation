/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Private interface between engine.c (tick scheduler) and render.c
 * (per-tick render pipeline). Not part of the public API in
 * include/cormoran/animation/.
 */

#include <stddef.h>

#include <zephyr/device.h>

#include <cormoran/animation/animation.h>
#include <cormoran/animation/color.h>

/**
 * Render one frame into `pixels`: reset the buffer to black, then render
 * `animation` on top.
 *
 * Phase A has no control/power-policy layer yet, so `animation` is always
 * the `zmk,animation` chosen node itself.
 * TODO(Phase B): `animation` should always be the animation-control
 * singleton, which in turn picks the actually-active animation per the
 * power policy; this function signature will likely collapse to take no
 * animation argument once that lands.
 */
void zmk_animation_render(const struct device *animation, struct zmk_animation_pixel *pixels,
                          size_t num_pixels);
