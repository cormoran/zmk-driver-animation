/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation_solid

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/animation/animation.h>
#include <cormoran/animation/color.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct animation_solid_config {
    const size_t *pixel_map;
    size_t pixel_map_size;
    const struct zmk_color_hsl *colors;
    uint8_t num_colors;
    /* Default full-cycle duration in frames, from the `duration` DT
     * property, used when start() is not given an explicit duration. */
    uint32_t default_duration_frames;
    uint32_t transition_duration_frames;
};

struct animation_solid_data {
    /* Remaining frames to render, or ZMK_ANIMATION_DURATION_FOREVER. */
    uint32_t counter;
    /* Position within one full color cycle, wraps at
     * config->transition_duration_frames * config->num_colors. */
    uint32_t cycle_counter;

    struct zmk_color_hsl current_hsl;
    struct zmk_color_rgb current_rgb;
};

static void animation_solid_update_color(const struct device *dev) {
    const struct animation_solid_config *config = dev->config;
    struct animation_solid_data *data = dev->data;

    const size_t from = data->cycle_counter / config->transition_duration_frames;
    const size_t to = (from + 1) % config->num_colors;

    struct zmk_color_hsl next_hsl;

    zmk_interpolate_hsl(&config->colors[from], &config->colors[to], &next_hsl,
                        (data->cycle_counter % config->transition_duration_frames) /
                            (float)config->transition_duration_frames);

    data->current_hsl = next_hsl;
    zmk_hsl_to_rgb(&data->current_hsl, &data->current_rgb);

    data->cycle_counter =
        (data->cycle_counter + 1) % (config->transition_duration_frames * config->num_colors);
}

static void animation_solid_render_frame(const struct device *dev,
                                         struct zmk_animation_pixel *pixels, size_t num_pixels) {
    ARG_UNUSED(num_pixels);
    const struct animation_solid_config *config = dev->config;
    struct animation_solid_data *data = dev->data;

    uint32_t counter = data->counter;
    if (counter == 0) {
        return;
    }

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        pixels[config->pixel_map[i]].value = data->current_rgb;
    }

    if (config->num_colors == 1 && counter == ZMK_ANIMATION_DURATION_FOREVER) {
        /* Optimization: a single, unchanging color never needs another
         * frame once rendered. */
        return;
    }

    if (counter < ZMK_ANIMATION_DURATION_FOREVER) {
        data->counter = counter - 1;
    }
    zmk_animation_request_frames_cap(data->counter);

    animation_solid_update_color(dev);
}

static void animation_solid_start(const struct device *dev, uint32_t request_duration_ms) {
    const struct animation_solid_config *config = dev->config;
    struct animation_solid_data *data = dev->data;

    data->counter =
        zmk_animation_duration_to_frames(request_duration_ms, config->default_duration_frames);
    data->cycle_counter = 0;

    if (data->counter == ZMK_ANIMATION_DURATION_FOREVER && config->num_colors == 1) {
        /* Optimization: a single, unchanging color only needs one more
         * frame to be rendered once. */
        zmk_animation_request_frames(1);
    } else {
        zmk_animation_request_frames_cap(data->counter);
    }
    LOG_INF("Start animation solid");
}

static void animation_solid_stop(const struct device *dev) {
    struct animation_solid_data *data = dev->data;
    data->counter = 0;
    LOG_INF("Stop animation solid");
}

static bool animation_solid_is_finished(const struct device *dev) {
    struct animation_solid_data *data = dev->data;
    return data->counter == 0;
}

static int animation_solid_init(const struct device *dev) {
    const struct animation_solid_config *config = dev->config;
    struct animation_solid_data *data = dev->data;

    data->current_hsl = config->colors[0];
    zmk_hsl_to_rgb(&data->current_hsl, &data->current_rgb);

    return 0;
}

static const struct zmk_animation_api animation_solid_api = {
    .start = animation_solid_start,
    .stop = animation_solid_stop,
    .render_frame = animation_solid_render_frame,
    .is_finished = animation_solid_is_finished,
};

#define ANIMATION_SOLID_DEVICE(idx)                                                                \
                                                                                                   \
    static struct animation_solid_data animation_solid_##idx##_data;                               \
                                                                                                   \
    static size_t animation_solid_##idx##_pixel_map[] = DT_INST_PROP(idx, pixels);                 \
                                                                                                   \
    static uint32_t animation_solid_##idx##_colors[] = DT_INST_PROP(idx, colors);                  \
                                                                                                   \
    static const struct animation_solid_config animation_solid_##idx##_config = {                  \
        .pixel_map = &animation_solid_##idx##_pixel_map[0],                                        \
        .pixel_map_size = DT_INST_PROP_LEN(idx, pixels),                                           \
        .colors = (struct zmk_color_hsl *)animation_solid_##idx##_colors,                          \
        .num_colors = DT_INST_PROP_LEN(idx, colors),                                               \
        .default_duration_frames = DT_INST_PROP(idx, duration) * CONFIG_ZMK_ANIMATION_FPS,         \
        .transition_duration_frames = (DT_INST_PROP(idx, duration) * CONFIG_ZMK_ANIMATION_FPS) /   \
                                      DT_INST_PROP_LEN(idx, colors),                               \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &animation_solid_init, NULL, &animation_solid_##idx##_data,         \
                          &animation_solid_##idx##_config, POST_KERNEL,                            \
                          CONFIG_APPLICATION_INIT_PRIORITY, &animation_solid_api);

DT_INST_FOREACH_STATUS_OKAY(ANIMATION_SOLID_DEVICE);
