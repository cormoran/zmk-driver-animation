/*
 * Copyright (c) 2025 cormoran
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_animation_layer_status

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk_driver_animation/animation_layer_status.h>

#include "../animations/layer_status.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * `zmk,behavior-animation-layer-status` (`animls`): peripheral-side
 * receiver for the central->peripheral layer-status sync (DESIGN.md #3.7 -
 * quarantined transport hack; see animations/layer_status.c's
 * top-of-file comment and #4 item 12 for the string-contract fix applied
 * to the *sender* side).
 */

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    switch (binding->param1) {
    case ANIMATION_LAYER_STATUS_CMD_FOR_PERIPHERAL:
        zmk_animation_layer_status_set_status(binding->param2);
        return 0;
    default:
        LOG_ERR("Unknown animls command: %d", binding->param1);
        return -ENOTSUP;
    }
}

static int behavior_animation_layer_status_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_metadata metadata = {
    .sets_len = 0,
    .sets = NULL,
};
#endif

static const struct behavior_driver_api behavior_animation_layer_status_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_animation_layer_status_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_animation_layer_status_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
