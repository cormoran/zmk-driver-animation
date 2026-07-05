/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>

#include "engine.h"

/**
 * Per-tick render pipeline: reset buffer to black, render the active
 * animation on top, and leave the result in `pixels` for the caller to
 * convert to `led_rgb` and fan out to the drivers.
 *
 * TODO(Phase B): apply global brightness for the active power source here,
 * after the animation render and before the caller converts to led_rgb.
 * Animations never see brightness; it is applied only at this stage.
 */
void zmk_animation_render(const struct device *animation, struct zmk_animation_pixel *pixels,
                          size_t num_pixels) {
    for (size_t i = 0; i < num_pixels; ++i) {
        pixels[i].value.r = 0;
        pixels[i].value.g = 0;
        pixels[i].value.b = 0;
    }

    zmk_animation_call_render_frame(animation, pixels, num_pixels);
}
