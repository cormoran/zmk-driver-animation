/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>

#include "../control/control_internal.h"
#include "engine.h"

/**
 * Per-tick render pipeline (DESIGN.md #3.3): reset buffer to black, render
 * the base animation selected by the power policy, render the active ad-hoc
 * overlay on top (an overlay owns only the pixels it writes; unwritten
 * pixels keep the base result), then apply the brightness multiplier for
 * the current power source. Animations never see brightness; it is applied
 * only here, after both renders and before the caller converts to
 * `led_rgb`.
 */
void zmk_animation_render(struct zmk_animation_pixel *pixels, size_t num_pixels) {
    for (size_t i = 0; i < num_pixels; ++i) {
        pixels[i].value.r = 0;
        pixels[i].value.g = 0;
        pixels[i].value.b = 0;
    }

    if (!zmk_animation_control_is_running()) {
        return;
    }

    const struct device *base = zmk_animation_control_current_base();
    if (base != NULL) {
        zmk_animation_call_render_frame(base, pixels, num_pixels);
    }

    zmk_animation_overlay_render(pixels, num_pixels);

    float multiplier = zmk_animation_control_current_brightness_multiplier();
    if (multiplier >= 1.0f) {
        return;
    }
    for (size_t i = 0; i < num_pixels; ++i) {
        pixels[i].value.r *= multiplier;
        pixels[i].value.g *= multiplier;
        pixels[i].value.b *= multiplier;
    }
}
