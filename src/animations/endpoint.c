/*
 * Copyright (c) 2025 cormoran
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation_endpoint

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <cormoran/animation/animation.h>
#include <cormoran/animation/color.h>
#include <cormoran/animation/control.h>

#include "../core/event_dispatch.h"

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#define IS_SPLIT_PERIPHERAL                                                                        \
    (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * `zmk,animation-endpoint`: BLE/USB endpoint status, with distinct
 * central/peripheral rendering (central shows per-profile status + USB;
 * peripheral shows central-connection status only), selected at compile
 * time via IS_CENTRAL/IS_SPLIT_PERIPHERAL exactly as v1 did.
 */

enum ble_connection_status {
    BLE_STATUS_OPEN,
    BLE_STATUS_DISCONNECTED,
    BLE_STATUS_CONNECTED,
};

struct animation_endpoint_config {
    const size_t *pixel_map;
    size_t pixel_map_size;
    uint32_t duration_frames_on_endpoint_change;
    uint32_t not_connected_duration_frames;
    uint32_t blink_duration_frames;
    uint32_t extend_duration_frames;
    uint32_t event_handling_start_ms;
    const struct zmk_color_hsl *color_open;
    const struct zmk_color_hsl *color_disconnected;
    const struct zmk_color_hsl *color_connected;
    const struct zmk_color_hsl *color_usb;
};

struct animation_endpoint_data {
    uint32_t counter;
    uint32_t blink_counter;
#if IS_CENTRAL
    int active_index;
    enum ble_connection_status active_profile_status;
#elif IS_SPLIT_PERIPHERAL
    enum ble_connection_status central_status;
#endif
};

static void refresh_connection_status(const struct device *dev) {
    const struct animation_endpoint_config *config = dev->config;
    struct animation_endpoint_data *data = dev->data;
    bool is_connected = false;

#if IS_CENTRAL
    data->active_index = zmk_ble_active_profile_index();
    if (zmk_ble_active_profile_is_open()) {
        data->active_profile_status = BLE_STATUS_OPEN;
    } else if (zmk_ble_active_profile_is_connected()) {
        data->active_profile_status = BLE_STATUS_CONNECTED;
        is_connected = true;
    } else {
        data->active_profile_status = BLE_STATUS_DISCONNECTED;
    }
#elif IS_SPLIT_PERIPHERAL
    if (!zmk_split_bt_peripheral_is_bonded()) {
        data->central_status = BLE_STATUS_OPEN;
    } else if (zmk_split_bt_peripheral_is_connected()) {
        data->central_status = BLE_STATUS_CONNECTED;
        is_connected = true;
    } else {
        data->central_status = BLE_STATUS_DISCONNECTED;
    }
#endif

    if (!is_connected && data->counter < config->not_connected_duration_frames) {
        data->counter = config->not_connected_duration_frames;
        zmk_animation_request_frames_cap(data->counter);
    }
    if (data->counter < config->extend_duration_frames) {
        data->counter = config->extend_duration_frames;
        zmk_animation_request_frames_cap(data->counter);
    }
}

#if IS_CENTRAL
static void render_central(const struct device *dev, struct zmk_animation_pixel *pixels,
                           size_t num_pixels) {
    ARG_UNUSED(num_pixels);
    const struct animation_endpoint_config *config = dev->config;
    struct animation_endpoint_data *data = dev->data;

    bool is_usb_selected = zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        struct zmk_color_hsl color;
        struct zmk_color_rgb rgb = {0};

        if ((int)i != data->active_index || i >= ZMK_BLE_PROFILE_COUNT) {
            if (is_usb_selected) {
                color = *config->color_usb;
                zmk_hsl_to_rgb(&color, &rgb);
            }
            pixels[config->pixel_map[i]].value = rgb;
            continue;
        }

        bool blink = true;
        switch (data->active_profile_status) {
        case BLE_STATUS_OPEN:
            color = *config->color_open;
            break;
        case BLE_STATUS_CONNECTED:
            color = *config->color_connected;
            blink = false;
            break;
        case BLE_STATUS_DISCONNECTED:
        default:
            color = *config->color_disconnected;
            break;
        }
        if (blink) {
            uint32_t highest_point = config->blink_duration_frames / 2;
            uint32_t point = data->blink_counter % config->blink_duration_frames;
            float ratio =
                (float)(point < highest_point ? point : config->blink_duration_frames - point) /
                highest_point;
            color.l = (uint8_t)(ratio * color.l);
        }
        zmk_hsl_to_rgb(&color, &rgb);
        pixels[config->pixel_map[i]].value = rgb;
    }
}
#elif IS_SPLIT_PERIPHERAL
static void render_peripheral(const struct device *dev, struct zmk_animation_pixel *pixels,
                              size_t num_pixels) {
    ARG_UNUSED(num_pixels);
    const struct animation_endpoint_config *config = dev->config;
    struct animation_endpoint_data *data = dev->data;

    struct zmk_color_hsl color;
    bool animate = true;
    switch (data->central_status) {
    case BLE_STATUS_OPEN:
        color = *config->color_open;
        break;
    case BLE_STATUS_CONNECTED:
        color = *config->color_connected;
        animate = false;
        break;
    case BLE_STATUS_DISCONNECTED:
    default:
        color = *config->color_disconnected;
        break;
    }

    uint32_t highest_point = data->blink_counter % (config->blink_duration_frames * 2 - 1);
    if (highest_point >= config->blink_duration_frames) {
        highest_point = config->blink_duration_frames * 2 - highest_point - 1;
    }
    uint32_t unit = config->pixel_map_size == 1
                        ? 1
                        : (config->blink_duration_frames / (config->pixel_map_size - 1));

    for (size_t i = 0; i < config->pixel_map_size; ++i) {
        struct zmk_color_hsl pixel_color = {0};
        if (!animate) {
            pixel_color = color;
        } else {
            uint32_t point = i * unit;
            uint32_t gap = point < highest_point ? highest_point - point : point - highest_point;
            float ratio = gap > unit ? 0 : (1.0f - (float)gap / unit);
            pixel_color = color;
            pixel_color.l = (uint8_t)(ratio * pixel_color.l);
        }
        struct zmk_color_rgb rgb;
        zmk_hsl_to_rgb(&pixel_color, &rgb);
        pixels[config->pixel_map[i]].value = rgb;
    }
}
#endif

static void animation_endpoint_render_frame(const struct device *dev,
                                            struct zmk_animation_pixel *pixels, size_t num_pixels) {
    struct animation_endpoint_data *data = dev->data;
    uint32_t counter = data->counter;
    if (counter == 0) {
        return;
    }

#if IS_CENTRAL
    render_central(dev, pixels, num_pixels);
#elif IS_SPLIT_PERIPHERAL
    render_peripheral(dev, pixels, num_pixels);
#endif

    data->blink_counter++;
    counter -= 1;
    data->counter = counter;
    zmk_animation_request_frames_cap(counter);
}

static void animation_endpoint_start(const struct device *dev, uint32_t request_duration_ms) {
    const struct animation_endpoint_config *config = dev->config;
    struct animation_endpoint_data *data = dev->data;

    data->counter = zmk_animation_duration_to_frames(request_duration_ms,
                                                     config->duration_frames_on_endpoint_change);
    refresh_connection_status(dev);
    zmk_animation_request_frames_cap(data->counter);
    LOG_INF("Start animation endpoint status");
}

static void animation_endpoint_stop(const struct device *dev) {
    struct animation_endpoint_data *data = dev->data;
    data->counter = 0;
    LOG_INF("Stop animation endpoint status");
}

static bool animation_endpoint_is_finished(const struct device *dev) {
    struct animation_endpoint_data *data = dev->data;
    return data->counter == 0;
}

static void on_endpoint_status_change(const struct device *dev) {
    const struct animation_endpoint_config *config = dev->config;
    struct animation_endpoint_data *data = dev->data;

    if (data->counter == 0 && config->duration_frames_on_endpoint_change > 0) {
        if ((uint32_t)k_uptime_get() > config->event_handling_start_ms) {
            zmk_animation_enqueue(dev, false,
                                  config->duration_frames_on_endpoint_change * 1000 /
                                      CONFIG_ZMK_ANIMATION_FPS);
        }
        return;
    }
    refresh_connection_status(dev);
}

static int animation_endpoint_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

static const struct zmk_animation_api animation_endpoint_api = {
    .start = animation_endpoint_start,
    .stop = animation_endpoint_stop,
    .render_frame = animation_endpoint_render_frame,
    .is_finished = animation_endpoint_is_finished,
};

#define ANIMATION_ENDPOINT_DEVICE(idx)                                                             \
                                                                                                   \
    static struct animation_endpoint_data animation_endpoint_##idx##_data;                         \
                                                                                                   \
    static const size_t animation_endpoint_##idx##_pixel_map[] = DT_INST_PROP(idx, pixels);        \
                                                                                                   \
    static const uint32_t animation_endpoint_##idx##_color_open = DT_INST_PROP(idx, color_open);   \
    static const uint32_t animation_endpoint_##idx##_color_disconnected =                          \
        DT_INST_PROP(idx, color_disconnected);                                                     \
    static const uint32_t animation_endpoint_##idx##_color_connected =                             \
        DT_INST_PROP(idx, color_connected);                                                        \
    static const uint32_t animation_endpoint_##idx##_color_usb = DT_INST_PROP(idx, color_usb);     \
                                                                                                   \
    static const struct animation_endpoint_config animation_endpoint_##idx##_config = {            \
        .pixel_map = animation_endpoint_##idx##_pixel_map,                                         \
        .pixel_map_size = DT_INST_PROP_LEN(idx, pixels),                                           \
        .duration_frames_on_endpoint_change =                                                      \
            DT_INST_PROP(idx, duration_seconds_on_endpoint_change) * CONFIG_ZMK_ANIMATION_FPS,     \
        .not_connected_duration_frames =                                                           \
            DT_INST_PROP(idx, not_connected_duration_seconds) * CONFIG_ZMK_ANIMATION_FPS,          \
        .blink_duration_frames =                                                                   \
            DT_INST_PROP(idx, blink_duration_seconds) * CONFIG_ZMK_ANIMATION_FPS,                  \
        .extend_duration_frames =                                                                  \
            DT_INST_PROP(idx, extend_duration_seconds) * CONFIG_ZMK_ANIMATION_FPS,                 \
        .event_handling_start_ms = DT_INST_PROP(idx, event_handling_start_seconds) * 1000,         \
        .color_open = (const struct zmk_color_hsl *)&animation_endpoint_##idx##_color_open,        \
        .color_disconnected =                                                                      \
            (const struct zmk_color_hsl *)&animation_endpoint_##idx##_color_disconnected,          \
        .color_connected =                                                                         \
            (const struct zmk_color_hsl *)&animation_endpoint_##idx##_color_connected,             \
        .color_usb = (const struct zmk_color_hsl *)&animation_endpoint_##idx##_color_usb,          \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &animation_endpoint_init, NULL, &animation_endpoint_##idx##_data,   \
                          &animation_endpoint_##idx##_config, POST_KERNEL,                         \
                          CONFIG_APPLICATION_INIT_PRIORITY, &animation_endpoint_api);

DT_INST_FOREACH_STATUS_OKAY(ANIMATION_ENDPOINT_DEVICE);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define DEVICE_ADDR(idx) DEVICE_DT_GET(DT_DRV_INST(idx)),
static const struct device *animation_endpoint_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(DEVICE_ADDR)};

#if IS_CENTRAL
ZMK_ANIMATION_DEFINE_LISTENER2(animation_endpoint, animation_endpoint_devices,
                               ARRAY_SIZE(animation_endpoint_devices), on_endpoint_status_change,
                               zmk_ble_active_profile_changed, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(animation_endpoint, zmk_endpoint_changed);
#elif IS_SPLIT_PERIPHERAL
ZMK_ANIMATION_DEFINE_LISTENER(animation_endpoint, animation_endpoint_devices,
                              ARRAY_SIZE(animation_endpoint_devices), on_endpoint_status_change,
                              zmk_split_peripheral_status_changed);
#endif

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
