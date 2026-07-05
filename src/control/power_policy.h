/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief USB-vs-battery source detection and ext-power rail gating
 * (DESIGN.md #3.3, control/power_policy.c). Ported from v1's
 * animation_control.c `is_powered()`/`set_power()`, now `static`-scoped
 * behind this header instead of unprefixed, collision-prone file-scope
 * globals (DESIGN.md #4 item 4).
 */

#include <stdbool.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns true if the board is currently USB-bus-powered.
 *
 * Gated on `CONFIG_USB_DEVICE_STACK`; always false when USB is not
 * compiled in (e.g. BLE-only boards, or native_sim without the USB stack).
 */
bool zmk_animation_power_policy_is_powered(void);

/**
 * @brief Enables or disables the ext-power rail for `ext_power` (may be
 * NULL, in which case this is a no-op - not every board wires one up).
 */
void zmk_animation_power_policy_set_power(const struct device *ext_power, bool enable);

#ifdef __cplusplus
}
#endif
