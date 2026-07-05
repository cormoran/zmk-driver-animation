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
 * Render one frame into `pixels`: reset the buffer to black, render the
 * control singleton's current base animation, render the active ad-hoc
 * overlay (if any) on top, then apply the brightness multiplier for the
 * current power source (DESIGN.md #3.3's "render pipeline per tick";
 * brightness is applied only at this stage, animations never see it).
 *
 * If no `zmk,animation-control` device is registered/ready (e.g. RPC-only
 * native_sim builds with zero animation devices), this renders black.
 */
void zmk_animation_render(struct zmk_animation_pixel *pixels, size_t num_pixels);
