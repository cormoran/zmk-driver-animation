/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Shared "static array of matching devices + event_listener +
 * ZMK_LISTENER/ZMK_SUBSCRIPTION" pattern (DESIGN.md #4 item 7). v1
 * copy-pasted this exact boilerplate 4 times (animation_control.c,
 * animation_battery_level.c, animation_endpoint.c, animation_layer_status.c
 * - each with its own `event_listener()` function name, device array, and
 * `ARRAY_SIZE`-by-hand computation). Every user of this header writes it
 * once via `ZMK_ANIMATION_DEFINE_LISTENER`.
 *
 * Usage (one event type):
 * @code
 * static void on_foo_changed(const struct device *dev) { ... }
 *
 * static const struct device *foo_devices[] = { ... };
 *
 * ZMK_ANIMATION_DEFINE_LISTENER(animation_foo, foo_devices,
 *                               ARRAY_SIZE(foo_devices), on_foo_changed,
 *                               zmk_foo_state_changed);
 * @endcode
 *
 * For a callback that also needs the event payload, or a listener that must
 * subscribe to more than one event type (e.g. control.c: USB conn state +
 * activity state), use `ZMK_ANIMATION_DEFINE_LISTENER_EX` and provide the
 * dispatch body yourself; it still generates the ZMK_LISTENER +
 * ZMK_SUBSCRIPTION boilerplate for an arbitrary list of event types.
 */

#include <zephyr/kernel.h>

#include <zmk/event_manager.h>

/**
 * @brief Defines a listener named `name` that, on any `event_type` event,
 * calls `per_device_cb(dev)` for every device in `devices[0..count)`.
 *
 * @param name Unique C identifier for this listener (becomes
 * `zmk_listener_##name`).
 * @param devices Array of `const struct device *`.
 * @param count Number of entries in `devices`.
 * @param per_device_cb `void (*)(const struct device *dev)`.
 * @param event_type A single ZMK event type name (e.g.
 * `zmk_battery_state_changed`), matching `as_##event_type()`.
 */
#define ZMK_ANIMATION_DEFINE_LISTENER(name, devices, count, per_device_cb, event_type)             \
    static int name##_event_listener(const zmk_event_t *event) {                                  \
        if (as_##event_type(event) != NULL) {                                                     \
            for (size_t _i = 0; _i < (count); ++_i) {                                             \
                per_device_cb((devices)[_i]);                                                      \
            }                                                                                     \
        }                                                                                          \
        return ZMK_EV_EVENT_BUBBLE;                                                                \
    }                                                                                              \
    ZMK_LISTENER(name, name##_event_listener);                                                     \
    ZMK_SUBSCRIPTION(name, event_type)

/**
 * @brief Like `ZMK_ANIMATION_DEFINE_LISTENER`, but fires `per_device_cb` on
 * either of two event types (e.g. endpoint.c's central variant: BLE active
 * profile changed OR endpoint changed). Remember to also add the second
 * `ZMK_SUBSCRIPTION(name, event_type_b)` after this macro - it only defines
 * the listener + the first subscription, matching the asymmetry every
 * other `ZMK_SUBSCRIPTION` call site already has.
 */
#define ZMK_ANIMATION_DEFINE_LISTENER2(name, devices, count, per_device_cb, event_type_a,          \
                                       event_type_b)                                                \
    static int name##_event_listener(const zmk_event_t *event) {                                  \
        if (as_##event_type_a(event) != NULL || as_##event_type_b(event) != NULL) {                \
            for (size_t _i = 0; _i < (count); ++_i) {                                             \
                per_device_cb((devices)[_i]);                                                      \
            }                                                                                     \
        }                                                                                          \
        return ZMK_EV_EVENT_BUBBLE;                                                                \
    }                                                                                              \
    ZMK_LISTENER(name, name##_event_listener);                                                     \
    ZMK_SUBSCRIPTION(name, event_type_a)

/**
 * @brief Like `ZMK_ANIMATION_DEFINE_LISTENER`, but the caller supplies the
 * dispatch function body (`dispatch_fn`, matching
 * `int (*)(const zmk_event_t *event)`) instead of a single per-device
 * callback, and may subscribe to any number of event types by repeating
 * `ZMK_SUBSCRIPTION(name, event_type)` after this macro. Use when a
 * listener needs the event payload or must handle more than one event
 * type (e.g. control.c).
 */
#define ZMK_ANIMATION_DEFINE_LISTENER_EX(name, dispatch_fn)                                        \
    ZMK_LISTENER(name, dispatch_fn)
