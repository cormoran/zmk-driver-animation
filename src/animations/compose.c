/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation_compose

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/animation/animation.h>
#include <cormoran/animation/color.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * `zmk,animation-compose`: DT-defined static composition of child
 * animations, played either in parallel or sequentially (DESIGN.md #3.3 -
 * this is the compile-time composition surface; `control/overlay_queue.c`
 * is the only *runtime* sequencing mechanism).
 */

struct animation_compose_config {
    const struct device *const *animations;
    const uint32_t *durations_ms;
    uint8_t num_animations;
    bool parallel;
};

struct animation_compose_data {
    bool running;
    uint8_t current_index;
    struct k_mutex mutex;
};

static void render_parallel(const struct device *dev, struct zmk_animation_pixel *pixels,
                            size_t num_pixels) {
    const struct animation_compose_config *config = dev->config;
    struct animation_compose_data *data = dev->data;
    bool still_running = false;

    for (uint8_t i = 0; i < config->num_animations; ++i) {
        if (!zmk_animation_call_is_finished(config->animations[i])) {
            zmk_animation_call_render_frame(config->animations[i], pixels, num_pixels);
            still_running = still_running || !zmk_animation_call_is_finished(config->animations[i]);
        }
    }

    if (!still_running) {
        k_mutex_lock(&data->mutex, K_FOREVER);
        data->running = false;
        k_mutex_unlock(&data->mutex);
    }
}

static void render_sequential(const struct device *dev, struct zmk_animation_pixel *pixels,
                              size_t num_pixels) {
    const struct animation_compose_config *config = dev->config;
    struct animation_compose_data *data = dev->data;
    uint8_t current = data->current_index;

    zmk_animation_call_render_frame(config->animations[current], pixels, num_pixels);

    if (!zmk_animation_call_is_finished(config->animations[current])) {
        return;
    }

    k_mutex_lock(&data->mutex, K_FOREVER);
    if (data->running && data->current_index == current) {
        uint8_t next = current + 1;
        if (next >= config->num_animations) {
            data->running = false;
            data->current_index = 0;
            LOG_DBG("animation compose: all children finished");
        } else {
            data->current_index = next;
            zmk_animation_call_start(config->animations[next], config->durations_ms[next]);
            zmk_animation_request_frames(1);
        }
    }
    k_mutex_unlock(&data->mutex);
}

static void animation_compose_render_frame(const struct device *dev,
                                           struct zmk_animation_pixel *pixels, size_t num_pixels) {
    const struct animation_compose_config *config = dev->config;
    struct animation_compose_data *data = dev->data;

    if (!data->running) {
        return;
    }
    if (config->parallel) {
        render_parallel(dev, pixels, num_pixels);
    } else {
        render_sequential(dev, pixels, num_pixels);
    }
}

static void animation_compose_start(const struct device *dev, uint32_t request_duration_ms) {
    const struct animation_compose_config *config = dev->config;
    struct animation_compose_data *data = dev->data;

    k_mutex_lock(&data->mutex, K_FOREVER);
    if (data->running) {
        k_mutex_unlock(&data->mutex);
        return;
    }
    data->current_index = 0;
    data->running = true;

    uint8_t start_count = config->parallel ? config->num_animations : 1;
    for (uint8_t i = 0; i < start_count; ++i) {
        /* Sequential children each keep their own DT-configured duration;
         * only the parallel case is scaled down by request_duration_ms,
         * matching v1. Every child still honors its own
         * request_duration_ms via zmk_animation_duration_to_frames()
         * internally. */
        uint32_t duration = config->durations_ms[i];
        if (config->parallel && request_duration_ms != ZMK_ANIMATION_DURATION_FOREVER &&
            request_duration_ms > 0 && duration > request_duration_ms) {
            duration = request_duration_ms;
        }
        zmk_animation_call_start(config->animations[i], duration);
    }
    zmk_animation_request_frames(1);
    k_mutex_unlock(&data->mutex);
}

static void animation_compose_stop(const struct device *dev) {
    const struct animation_compose_config *config = dev->config;
    struct animation_compose_data *data = dev->data;

    k_mutex_lock(&data->mutex, K_FOREVER);
    if (data->running) {
        if (config->parallel) {
            for (uint8_t i = 0; i < config->num_animations; ++i) {
                zmk_animation_call_stop(config->animations[i]);
            }
        } else {
            zmk_animation_call_stop(config->animations[data->current_index]);
        }
    }
    data->current_index = 0;
    data->running = false;
    k_mutex_unlock(&data->mutex);
}

static bool animation_compose_is_finished(const struct device *dev) {
    struct animation_compose_data *data = dev->data;
    return !data->running;
}

static int animation_compose_init(const struct device *dev) {
    struct animation_compose_data *data = dev->data;
    return k_mutex_init(&data->mutex);
}

static const struct zmk_animation_api animation_compose_api = {
    .start = animation_compose_start,
    .stop = animation_compose_stop,
    .render_frame = animation_compose_render_frame,
    .is_finished = animation_compose_is_finished,
};

#define PHANDLE_TO_DEVICE(node_id, prop, idx) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define ANIMATION_COMPOSE_DEVICE(idx)                                                              \
                                                                                                    \
    static struct animation_compose_data animation_compose_##idx##_data;                          \
                                                                                                    \
    static const struct device *animation_compose_##idx##_animations[] = {                        \
        DT_INST_FOREACH_PROP_ELEM(idx, animations, PHANDLE_TO_DEVICE)};                           \
    static const uint32_t animation_compose_##idx##_durations[] = DT_INST_PROP(idx, durations_ms); \
                                                                                                    \
    static const struct animation_compose_config animation_compose_##idx##_config = {             \
        .animations = animation_compose_##idx##_animations,                                       \
        .durations_ms = animation_compose_##idx##_durations,                                      \
        .num_animations = DT_INST_PROP_LEN(idx, animations),                                      \
        .parallel = DT_INST_PROP(idx, parallel),                                                  \
    };                                                                                            \
                                                                                                    \
    DEVICE_DT_INST_DEFINE(idx, &animation_compose_init, NULL, &animation_compose_##idx##_data,     \
                          &animation_compose_##idx##_config, POST_KERNEL,                          \
                          CONFIG_APPLICATION_INIT_PRIORITY, &animation_compose_api);

DT_INST_FOREACH_STATUS_OKAY(ANIMATION_COMPOSE_DEVICE);
