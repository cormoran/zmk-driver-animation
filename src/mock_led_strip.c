/*
 * Copyright (c) 2025 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_mock_led_strip

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int mock_led_strip_update_rgb(const struct device *dev,
                                     struct led_rgb *pixels,
                                     size_t num_pixels) {
    LOG_DBG("update %u pixels", num_pixels);
    return 0;
}

static size_t mock_led_strip_length(const struct device *dev) {
    return DT_INST_PROP(0, chain_length);
}

static const struct led_strip_driver_api mock_api = {
    .update_rgb   = mock_led_strip_update_rgb,
    .length       = mock_led_strip_length,
};

#define MOCK_DEVICE(idx) \
    DEVICE_DT_INST_DEFINE(idx, NULL, NULL, NULL, NULL, \
                          POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY, &mock_api);

DT_INST_FOREACH_STATUS_OKAY(MOCK_DEVICE)
