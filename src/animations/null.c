/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation_none

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <cormoran/animation/animation.h>

/*
 * Null-object animation: initializes successfully (no -ENXIO-on-init
 * sentinel trick, DESIGN.md #4 item 9) and renders nothing. The render
 * pipeline already reset the buffer to black before calling render_frame,
 * so there is nothing to do here.
 */

static void animation_none_start(const struct device *dev, uint32_t request_duration_ms) {
    ARG_UNUSED(dev);
    ARG_UNUSED(request_duration_ms);
}

static void animation_none_stop(const struct device *dev) { ARG_UNUSED(dev); }

static void animation_none_render_frame(const struct device *dev,
                                        struct zmk_animation_pixel *pixels, size_t num_pixels) {
    ARG_UNUSED(dev);
    ARG_UNUSED(pixels);
    ARG_UNUSED(num_pixels);
}

static bool animation_none_is_finished(const struct device *dev) {
    ARG_UNUSED(dev);
    return true;
}

static int animation_none_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

static const struct zmk_animation_api animation_none_api = {
    .start = animation_none_start,
    .stop = animation_none_stop,
    .render_frame = animation_none_render_frame,
    .is_finished = animation_none_is_finished,
};

#define ANIMATION_NONE_DEVICE(idx)                                                                 \
    DEVICE_DT_INST_DEFINE(idx, &animation_none_init, NULL, NULL, NULL, POST_KERNEL,                \
                          CONFIG_APPLICATION_INIT_PRIORITY, &animation_none_api);

DT_INST_FOREACH_STATUS_OKAY(ANIMATION_NONE_DEVICE);
