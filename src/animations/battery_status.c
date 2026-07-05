/*
 * Copyright (c) 2025 cormoran
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation_battery_status

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>

#include <cormoran/animation/animation.h>
#include <cormoran/animation/color.h>
#include <cormoran/animation/control.h>

#include "../core/event_dispatch.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * `zmk,animation-battery-status`: breathing bar-graph of the current
 * battery percentage, plus a low-battery alert enqueued as an ad-hoc
 * overlay (DESIGN.md: "should go through overlay_queue.c's enqueue/
 * play_now, not a bespoke mechanism" - this uses
 * `zmk_animation_enqueue()`, the same entry point animations use in
 * general, rather than reaching into overlay_queue.c directly).
 */

struct animation_battery_status_config {
    const size_t *pixel_map;
    size_t pixel_map_size;
    uint32_t animation_duration_frames;
    uint8_t low_alert_start_threshold;
    uint8_t low_alert_stop_threshold;
    uint32_t low_alert_interval_ms;
    uint32_t low_alert_duration_ms;
    const struct zmk_color_hsl *color_high;
    const struct zmk_color_hsl *color_middle;
    const struct zmk_color_hsl *color_low;
};

struct animation_battery_status_data {
    uint32_t counter;
    int64_t last_alert_time;
};

static void animation_battery_status_render_frame(const struct device *dev,
                                                  struct zmk_animation_pixel *pixels,
                                                  size_t num_pixels) {
    ARG_UNUSED(num_pixels);
    const struct animation_battery_status_config *config = dev->config;
    struct animation_battery_status_data *data = dev->data;

    uint32_t counter = data->counter;
    if (counter == 0) {
        return;
    }

    uint8_t battery_level = zmk_battery_state_of_charge();
    uint32_t highest_point = counter % config->animation_duration_frames;

    /*
     * Per-pixel thresholds used to be computed as a precomputed
     * `unit = 100 / (pixel_map_size * 3)` (integer division) compared
     * against `battery_level <= (i * 3) * unit`. `unit` truncates to 0 for
     * any `pixel_map_size >= 34` (100 / 102 == 0 in integer division),
     * which collapsed every threshold to 0 and made every pixel render as
     * color_high regardless of the actual battery level. Comparing the
     * cross-multiplied form `battery_level * pixel_map_size * 3` against
     * `i * 3 * 100` is equivalent to `battery_level <= (i * 3) * (100 /
     * (pixel_map_size * 3))` for real-number division, but never divides,
     * so there is no truncation for any pixel_map_size >= 1 (both sides
     * are exact uint32_t products; pixel_map_size is DT_INST_PROP_LEN-
     * bounded to a small array size in practice, so `battery_level (<=
     * 100) * pixel_map_size * 3` cannot realistically overflow uint32_t).
     */
    uint32_t level_scaled = (uint32_t)battery_level * config->pixel_map_size * 3;

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        uint32_t point = i * config->animation_duration_frames / config->pixel_map_size;
        uint32_t gap = point < highest_point ? highest_point - point : point - highest_point;
        if (gap > config->animation_duration_frames / 2) {
            gap = config->animation_duration_frames - gap;
        }
        float ratio = 1.0f - (float)gap / (config->animation_duration_frames / 2);

        struct zmk_color_hsl color = {0};
        if (level_scaled <= (uint32_t)i * 3 * 100) {
            /* default (off): treat 0% as "no bar lit" */
        } else if (level_scaled < (uint32_t)(i * 3 + 1) * 100) {
            color = *config->color_low;
        } else if (level_scaled < (uint32_t)(i * 3 + 2) * 100) {
            color = *config->color_middle;
        } else {
            color = *config->color_high;
        }
        color.l = (uint8_t)(color.l * (0.5f + ratio / 2));

        struct zmk_color_rgb rgb;
        zmk_hsl_to_rgb(&color, &rgb);
        pixels[config->pixel_map[i]].value = rgb;
    }

    counter -= 1;
    data->counter = counter;
    zmk_animation_request_frames_cap(counter);
}

static void animation_battery_status_start(const struct device *dev, uint32_t request_duration_ms) {
    const struct animation_battery_status_config *config = dev->config;
    struct animation_battery_status_data *data = dev->data;

    LOG_INF("Start animation battery status");
    data->last_alert_time = k_uptime_get();
    data->counter =
        zmk_animation_duration_to_frames(request_duration_ms, config->animation_duration_frames);
    zmk_animation_request_frames_cap(data->counter);
}

static void animation_battery_status_stop(const struct device *dev) {
    struct animation_battery_status_data *data = dev->data;
    LOG_INF("Stop animation battery status");
    data->last_alert_time = k_uptime_get();
    data->counter = 0;
}

static bool animation_battery_status_is_finished(const struct device *dev) {
    struct animation_battery_status_data *data = dev->data;
    return data->counter == 0;
}

static void on_battery_state_changed(const struct device *dev) {
    const struct animation_battery_status_config *config = dev->config;
    struct animation_battery_status_data *data = dev->data;

    /*
     * v1 (git show main:src/animation_battery_level.c,
     * on_battery_status_change) only enqueued the low-battery alert while
     * `!data->running` - i.e. suppressed entirely whenever this exact
     * device instance was already actively playing, whether as the
     * selected bar-graph base animation or as the alert overlay itself
     * (re-entrancy guard). The Phase B port dropped that outer guard,
     * leaving only the low_alert_interval_ms rate limit, which is a
     * different check (time-since-last-alert, not "is this device already
     * on screen"): the alert could now be (re-)enqueued as an overlay on
     * top of this same animation's own bar-graph rendering.
     *
     * Judgement call: restore an equivalent guard rather than keep the
     * dropped behavior. `animation_battery_status_is_finished(dev)`
     * (data->counter == 0) is this device's own "not currently playing"
     * state and is the direct v2 analog of v1's per-device `running` flag
     * - using it here (instead of e.g. comparing against
     * zmk_animation_control_current_base()) keeps the check local to this
     * animation and correct for both roles data->counter covers (base and
     * overlay), matching v1 exactly rather than only checking "am I the
     * selected base".
     */
    if (!animation_battery_status_is_finished(dev)) {
        return;
    }

    uint8_t level = zmk_battery_state_of_charge();
    if (config->low_alert_stop_threshold < level && level < config->low_alert_start_threshold &&
        k_uptime_get() - data->last_alert_time > config->low_alert_interval_ms) {
        data->last_alert_time = k_uptime_get();
        zmk_animation_enqueue(dev, false, config->low_alert_duration_ms);
    }
}

static int animation_battery_status_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

static const struct zmk_animation_api animation_battery_status_api = {
    .start = animation_battery_status_start,
    .stop = animation_battery_status_stop,
    .render_frame = animation_battery_status_render_frame,
    .is_finished = animation_battery_status_is_finished,
};

#define ANIMATION_BATTERY_STATUS_DEVICE(idx)                                                       \
                                                                                                   \
    static struct animation_battery_status_data animation_battery_status_##idx##_data;             \
                                                                                                   \
    static const size_t animation_battery_status_##idx##_pixel_map[] = DT_INST_PROP(idx, pixels);  \
                                                                                                   \
    static const uint32_t animation_battery_status_##idx##_color_high =                            \
        DT_INST_PROP(idx, color_high);                                                             \
    static const uint32_t animation_battery_status_##idx##_color_middle =                          \
        DT_INST_PROP(idx, color_middle);                                                           \
    static const uint32_t animation_battery_status_##idx##_color_low =                             \
        DT_INST_PROP(idx, color_low);                                                              \
                                                                                                   \
    static const struct animation_battery_status_config animation_battery_status_##idx##_config =  \
        {                                                                                          \
            .pixel_map = animation_battery_status_##idx##_pixel_map,                               \
            .pixel_map_size = DT_INST_PROP_LEN(idx, pixels),                                       \
            .animation_duration_frames =                                                           \
                DT_INST_PROP(idx, animation_duration_seconds) * CONFIG_ZMK_ANIMATION_FPS,          \
            .color_high =                                                                          \
                (const struct zmk_color_hsl *)&animation_battery_status_##idx##_color_high,        \
            .color_middle =                                                                        \
                (const struct zmk_color_hsl *)&animation_battery_status_##idx##_color_middle,      \
            .color_low =                                                                           \
                (const struct zmk_color_hsl *)&animation_battery_status_##idx##_color_low,         \
            .low_alert_start_threshold = DT_INST_PROP(idx, low_alert_start_threshold),             \
            .low_alert_stop_threshold = DT_INST_PROP(idx, low_alert_stop_threshold),               \
            .low_alert_interval_ms = DT_INST_PROP(idx, low_alert_interval_seconds) * 1000,         \
            .low_alert_duration_ms = DT_INST_PROP(idx, low_alert_duration_ms),                     \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &animation_battery_status_init, NULL,                               \
                          &animation_battery_status_##idx##_data,                                  \
                          &animation_battery_status_##idx##_config, POST_KERNEL,                   \
                          CONFIG_APPLICATION_INIT_PRIORITY, &animation_battery_status_api);

DT_INST_FOREACH_STATUS_OKAY(ANIMATION_BATTERY_STATUS_DEVICE);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define DEVICE_ADDR(idx) DEVICE_DT_GET(DT_DRV_INST(idx)),
static const struct device *animation_battery_status_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(DEVICE_ADDR)};

ZMK_ANIMATION_DEFINE_LISTENER(animation_battery_status, animation_battery_status_devices,
                              ARRAY_SIZE(animation_battery_status_devices),
                              on_battery_state_changed, zmk_battery_state_changed);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
