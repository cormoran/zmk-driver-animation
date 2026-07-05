/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Public control API used by behaviors, RPC request_exec (Phase D),
 * and (indirectly) by animations that need to enqueue an ad-hoc overlay
 * (e.g. the low-battery alert).
 *
 * Control is a singleton Zephyr device (`zmk,animation-control`), resolved
 * internally by control.c. There is no `dev0` global or `_0`-suffixed shim
 * (DESIGN.md #4 item 4): every function below is safe to call from anywhere
 * (including before the control device's own `init()` has run, e.g. from a
 * behavior's init function) because they resolve the singleton lazily and
 * log + no-op if it is not registered or not yet ready. See control.c's
 * top-of-file comment for why v1's compile-time-avoidance rationale for
 * `dev0` does not actually require a global variable.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Target power source for a brightness/select/shift mutation. Mirrors v1's
 * `animation_control_power_source`.
 */
enum zmk_animation_power_source {
    ZMK_ANIMATION_POWER_SOURCE_POWERED,
    ZMK_ANIMATION_POWER_SOURCE_BATTERY,
    /** Resolve to whichever source `power_policy.c` currently reports active. */
    ZMK_ANIMATION_POWER_SOURCE_CURRENT,
};

/** Overlay dispatch mode for `zmk_animation_trigger()`. */
enum zmk_animation_trigger_mode {
    /** Play after the current overlay (if any) finishes. */
    ZMK_ANIMATION_TRIGGER_ENQUEUE,
    /** Preempt the current overlay immediately, if it is cancelable. */
    ZMK_ANIMATION_TRIGGER_PLAY_NOW,
};

/** Enable/disable animation rendering entirely (all power sources). */
void zmk_animation_set_enabled(bool enabled);

/**
 * Change brightness for `power_source` by `steps` (positive or negative),
 * clamped to `[0, brightness-steps]`.
 */
void zmk_animation_set_brightness_shift(int steps, enum zmk_animation_power_source power_source);

/** Set brightness for `power_source` to an absolute step value. */
void zmk_animation_set_brightness(uint8_t step, enum zmk_animation_power_source power_source);

/**
 * Change the selected base animation for `power_source` by `index_offset`
 * (wrapping), e.g. +1/-1 for next/previous.
 */
void zmk_animation_select_shift(int index_offset, enum zmk_animation_power_source power_source);

/** Select the base animation for `power_source` by absolute index (modulo). */
void zmk_animation_select(uint8_t index, enum zmk_animation_power_source power_source);

/**
 * Enqueue or play-now the `behavior-animations[index]` overlay.
 *
 * @param index Index into the `behavior-animations` DT list.
 * @param duration_ms Overlay duration, or `ZMK_ANIMATION_DURATION_FOREVER`
 * (0 is also treated as "until finished/canceled", matching v1).
 * @param mode `ZMK_ANIMATION_TRIGGER_ENQUEUE` or `..._PLAY_NOW`.
 * @param cancelable Whether a later overlay may preempt this one.
 * @return 0 on success, -EINVAL if index is out of range, -ENODEV if no
 * control device is registered/ready yet.
 */
int zmk_animation_trigger(uint8_t index, uint32_t duration_ms, enum zmk_animation_trigger_mode mode,
                          bool cancelable);

/**
 * Stop the `behavior-animations[index]` overlay immediately (used by
 * animation_trigger's release handling). No-op if it is not the active
 * overlay.
 */
int zmk_animation_trigger_stop(uint8_t index);

/**
 * Enqueue an arbitrary animation device as an ad-hoc overlay (used by
 * animations themselves, e.g. battery_status's low-battery alert). Prefer
 * `zmk_animation_trigger()` when the animation is reachable via
 * `behavior-animations`; this entry point is for animations that reference
 * another animation device directly via devicetree.
 */
int zmk_animation_enqueue(const struct device *animation, bool cancelable, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
