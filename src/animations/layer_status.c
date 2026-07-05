/*
 * Copyright (c) 2025 cormoran
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation_layer_status

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include <cormoran/animation/animation.h>
#include <cormoran/animation/color.h>
#include <dt-bindings/zmk_driver_animation/animation_layer_status.h>

#include "../core/event_dispatch.h"
#include "layer_status.h"

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#define IS_SPLIT_PERIPHERAL                                                                        \
    (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Exactly one `zmk,animation-layer-status` instance is supported (DESIGN.md
 * #4 item 11, same BUILD_ASSERT pattern as engine.c/control.c). v1
 * hardcoded DT instance 0 directly with no such guard.
 */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "Only one enabled `zmk,animation-layer-status` instance is supported");

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * QUARANTINED HACK (DESIGN.md #3.7, #3.8): on a split keyboard, the central
 * computes the layer bitmask and needs it mirrored to the peripheral for
 * rendering, but the RPC/relay subsystem is central-only (no split-peripheral
 * transport of its own). v1 worked around this by invoking the `animls`
 * *behavior* as an ad-hoc cross-role RPC: ZMK already relays behavior
 * invocations from central to peripheral, so "invoke a behavior whose only
 * job is to receive a uint32_t layer bitmask" repurposes that relay as a
 * transport. This is accepted as-is for v2 (not redesigned - see DESIGN.md
 * #3.7); the fix in this phase is only for v1 defect #12, the *compile-time
 * check* of the string naming the peripheral-side behavior device below.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/*
 * v1 wrote `.behavior_dev = "animls"` as a bare string literal, matched
 * against the DT node labeled `animls:` purely by convention - nothing
 * caught the two drifting apart (DESIGN.md #4 item 12). DEVICE_DT_NAME()
 * derives the same string from the DT node itself (its `label`, or the
 * full node name if unlabeled), so if the node referenced here is ever
 * renamed/removed, this fails to *compile* (unknown node), not just fails
 * silently at runtime when the relayed behavior can't be found by name.
 */
#define ANIMLS_BEHAVIOR_DEV_NAME DEVICE_DT_NAME(DT_NODELABEL(animls))

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

struct animation_layer_status_config {
    const size_t *pixel_map;
    size_t pixel_map_size;
    const struct zmk_color_hsl *default_color;
    uint8_t layer_offset;
    uint32_t extend_duration_frames;
    const struct zmk_color_hsl *colors;
    uint8_t colors_size;
};

struct animation_layer_status_data {
    uint32_t counter;
    uint32_t layer_status;
    int64_t last_set;
};

static const struct device *layer_status_dev;

#if IS_CENTRAL
static void refresh_layer_status_central(const struct device *dev) {
    const struct animation_layer_status_config *config = dev->config;
    struct animation_layer_status_data *data = dev->data;
    uint32_t previous = data->layer_status;

    /* zmk_keymap_layer_state() alone omits the default layer in some ZMK
     * versions when nothing else is active; OR it in explicitly (kept from
     * v1, marked there as a workaround for that behavior). */
    zmk_keymap_layer_id_t default_layer = zmk_keymap_layer_default();
    data->layer_status = zmk_keymap_layer_state() | BIT(default_layer);

    if (previous != data->layer_status && data->counter > 0) {
        if (data->counter < config->extend_duration_frames) {
            data->counter = config->extend_duration_frames;
            zmk_animation_request_frames_cap(data->counter);
        }
    }
}
#endif

#if IS_SPLIT_PERIPHERAL
static void refresh_layer_status_peripheral(const struct device *dev, uint32_t layer_status) {
    const struct animation_layer_status_config *config = dev->config;
    struct animation_layer_status_data *data = dev->data;
    uint32_t previous = data->layer_status;

    data->layer_status = layer_status;
    data->last_set = k_uptime_get();

    if (previous != 0 && previous != data->layer_status && data->counter > 0) {
        if (data->counter < config->extend_duration_frames) {
            data->counter = config->extend_duration_frames;
            zmk_animation_request_frames_cap(data->counter);
        }
    }
}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void sync_layer_status_to_peripheral(const struct device *dev) {
    struct animation_layer_status_data *data = dev->data;

    struct zmk_behavior_binding binding = {
        .behavior_dev = ANIMLS_BEHAVIOR_DEV_NAME,
        .param1 = ANIMATION_LAYER_STATUS_CMD_FOR_PERIPHERAL,
        .param2 = data->layer_status,
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = 0,
#endif
    };
    zmk_behavior_invoke_binding(&binding, event, false);
}
#endif

static void animation_layer_status_render_frame(const struct device *dev,
                                                struct zmk_animation_pixel *pixels,
                                                size_t num_pixels) {
    ARG_UNUSED(num_pixels);
    const struct animation_layer_status_config *config = dev->config;
    struct animation_layer_status_data *data = dev->data;

    uint32_t counter = data->counter;
    if (counter == 0) {
        return;
    }

    struct zmk_color_rgb black = {0};
    struct zmk_color_rgb default_rgb;
    zmk_hsl_to_rgb(config->default_color, &default_rgb);

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        uint8_t idx = i + config->layer_offset;
        /*
         * data->layer_status is a uint32_t bitmask, so BIT(idx) is
         * undefined behavior once idx >= 32 (shifting by >= the width of
         * the shifted type) - unlike the colors[idx] access below, which
         * was already correctly guarded by idx < config->colors_size.
         * Layer indices >= 32 can't be represented in the bitmask at all,
         * so treat them the same as "bit not set" (pixel renders as
         * inactive/black) without evaluating BIT(idx).
         */
        if (idx < 32 && (data->layer_status & BIT(idx))) {
            if (idx < config->colors_size &&
                (config->colors[idx].h != 0 || config->colors[idx].s != 0 ||
                 config->colors[idx].l != 0)) {
                struct zmk_color_rgb rgb;
                zmk_hsl_to_rgb(&config->colors[idx], &rgb);
                pixels[config->pixel_map[i]].value = rgb;
            } else {
                pixels[config->pixel_map[i]].value = default_rgb;
            }
        } else {
            pixels[config->pixel_map[i]].value = black;
        }
    }

    counter -= 1;
    data->counter = counter;
    zmk_animation_request_frames_cap(counter);
}

static void animation_layer_status_start(const struct device *dev, uint32_t request_duration_ms) {
    struct animation_layer_status_data *data = dev->data;

    LOG_INF("Start animation layer status");

#if IS_CENTRAL
    /* Refresh before setting counter, so a status change picked up here
     * doesn't also trigger the (redundant) duration-extension path. */
    refresh_layer_status_central(dev);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    sync_layer_status_to_peripheral(dev);
#elif IS_SPLIT_PERIPHERAL
    /* Status is synced via the animls behavior relay. If it was set very
     * recently, keep it (a start() racing a fresh sync shouldn't reset to
     * 0 and cause a visible flicker back to "no layer"). */
    if (k_uptime_get() - data->last_set > 1000) {
        data->layer_status = 0;
    }
#endif

    data->counter =
        zmk_animation_duration_to_frames(request_duration_ms, ZMK_ANIMATION_DURATION_FOREVER);
    zmk_animation_request_frames_cap(data->counter);
}

static void animation_layer_status_stop(const struct device *dev) {
    struct animation_layer_status_data *data = dev->data;
    LOG_INF("Stop animation layer status");
    data->layer_status = 0;
    data->counter = 0;
}

static bool animation_layer_status_is_finished(const struct device *dev) {
    struct animation_layer_status_data *data = dev->data;
    return data->counter == 0;
}

/**
 * @brief Peripheral-side entry point invoked by the animls behavior
 * (src/behaviors/animation_layer_status.c) when the central relays a
 * layer-status update. Declared in layer_status.h (private core header,
 * not the public animation.h/control.h surface - this is transport-hack
 * plumbing, not something other animations should call).
 */
void zmk_animation_layer_status_set_status(uint32_t layer_status) {
#if IS_SPLIT_PERIPHERAL
    if (layer_status_dev != NULL) {
        refresh_layer_status_peripheral(layer_status_dev, layer_status);
    }
#else
    ARG_UNUSED(layer_status);
#endif
}

static int animation_layer_status_init(const struct device *dev) {
    /* BUILD_ASSERT above guarantees at most one instance. */
    layer_status_dev = dev;
    return 0;
}

static const struct zmk_animation_api animation_layer_status_api = {
    .start = animation_layer_status_start,
    .stop = animation_layer_status_stop,
    .render_frame = animation_layer_status_render_frame,
    .is_finished = animation_layer_status_is_finished,
};

#define ANIMATION_LAYER_STATUS_DEVICE(idx)                                                         \
                                                                                                   \
    static struct animation_layer_status_data animation_layer_status_##idx##_data;                 \
                                                                                                   \
    static const size_t animation_layer_status_##idx##_pixel_map[] = DT_INST_PROP(idx, pixels);    \
                                                                                                   \
    static const uint32_t animation_layer_status_##idx##_default_color =                           \
        DT_INST_PROP(idx, default_color);                                                          \
    static const uint32_t animation_layer_status_##idx##_colors[] = DT_INST_PROP(idx, colors);     \
                                                                                                   \
    static const struct animation_layer_status_config animation_layer_status_##idx##_config = {    \
        .pixel_map = animation_layer_status_##idx##_pixel_map,                                     \
        .pixel_map_size = DT_INST_PROP_LEN(idx, pixels),                                           \
        .default_color =                                                                           \
            (const struct zmk_color_hsl *)&animation_layer_status_##idx##_default_color,           \
        .colors = (const struct zmk_color_hsl *)animation_layer_status_##idx##_colors,             \
        .colors_size = DT_INST_PROP_LEN(idx, colors),                                              \
        .layer_offset = DT_INST_PROP(idx, layer_offset),                                           \
        .extend_duration_frames =                                                                  \
            DT_INST_PROP(idx, extend_duration_seconds) * CONFIG_ZMK_ANIMATION_FPS,                 \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &animation_layer_status_init, NULL,                                 \
                          &animation_layer_status_##idx##_data,                                    \
                          &animation_layer_status_##idx##_config, POST_KERNEL,                     \
                          CONFIG_APPLICATION_INIT_PRIORITY, &animation_layer_status_api);

DT_INST_FOREACH_STATUS_OKAY(ANIMATION_LAYER_STATUS_DEVICE);

#if IS_CENTRAL
static void on_layer_status_change(const struct device *dev) {
    refresh_layer_status_central(dev);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    sync_layer_status_to_peripheral(dev);
#endif
}

static const struct device *const animation_layer_status_devices[] = {
    DEVICE_DT_GET(DT_DRV_INST(0)),
};

ZMK_ANIMATION_DEFINE_LISTENER(animation_layer_status, animation_layer_status_devices,
                              ARRAY_SIZE(animation_layer_status_devices), on_layer_status_change,
                              zmk_layer_state_changed);
#endif /* IS_CENTRAL */

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
