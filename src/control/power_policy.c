/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

#include <drivers/ext_power.h>

#include "power_policy.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

bool zmk_animation_power_policy_is_powered(void) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    switch (zmk_usb_get_conn_state()) {
    case ZMK_USB_CONN_HID:
    case ZMK_USB_CONN_POWERED:
        return true;
    default:
        return false;
    }
#else
    return false;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
}

void zmk_animation_power_policy_set_power(const struct device *ext_power, bool enable) {
    if (ext_power == NULL || !device_is_ready(ext_power)) {
        return;
    }

    int rc = enable ? ext_power_enable(ext_power) : ext_power_disable(ext_power);
    if (rc != 0) {
        LOG_ERR("Unable to set ext-power %s: %d", enable ? "on" : "off", rc);
        return;
    }
    LOG_INF("Animation ext-power %s", enable ? "ON" : "OFF");
}
