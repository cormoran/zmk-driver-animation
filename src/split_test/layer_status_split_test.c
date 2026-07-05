/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>

#include <dt-bindings/zmk_driver_animation/animation_layer_status.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * native_sim-only self-test (see tests/layer_status_peripheral and
 * DESIGN.md #3.7/#3.8's "quarantined transport hack" comment in
 * animations/layer_status.c): a split-peripheral role build forwards raw
 * kscan events toward the central rather than resolving keymap bindings
 * locally (native_sim confirms this - a keymap binding on a peripheral-role
 * test logs "No active transport that supports reporting events!" and
 * never reaches behavior invocation at all), so a keymap binding alone
 * cannot exercise the `animls` behavior's press/release handlers in
 * native_sim. This bypasses kscan/keymap and invokes the binding directly,
 * exactly the way ZMK's split relay invokes it on a real peripheral when
 * the central relays a triggered binding.
 */

#define ANIMLS_BEHAVIOR_DEV_NAME DEVICE_DT_NAME(DT_NODELABEL(animls))

static int layer_status_split_test_init(void) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = ANIMLS_BEHAVIOR_DEV_NAME,
        .param1 = ANIMATION_LAYER_STATUS_CMD_FOR_PERIPHERAL,
        .param2 = 1, /* BIT(0): mark layer 0 active. */
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = 0,
#endif
    };

    int rc = zmk_behavior_invoke_binding(&binding, event, true);
    if (rc != 0) {
        LOG_ERR("layer_status_split_test: press failed: %d", rc);
        return rc;
    }
    rc = zmk_behavior_invoke_binding(&binding, event, false);
    if (rc != 0) {
        LOG_ERR("layer_status_split_test: release failed: %d", rc);
        return rc;
    }

    printk("PASS: layer_status_split_test invoked animls binding\n");
    return 0;
}

SYS_INIT(layer_status_split_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
