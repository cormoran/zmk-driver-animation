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

/** One entry in the capability lists returned by `zmk_animation_get_info()`. */
struct zmk_animation_info_entry {
    /** Display name: the DT `display-name` property if set, else the DT
     * node name (`device->name`). Points at static/rodata storage (either a
     * DT-property string literal or `device->name`), valid for the process
     * lifetime - safe to copy by pointer into a response without a static
     * buffer of its own. */
    const char *name;
};

/**
 * Static capability info (DESIGN.md #3.6 GetInfoResponse): the DT-fixed
 * powered/battery/behavior animation lists (as display-name entries, index
 * implied by array position) plus global capability numbers. All array
 * pointers are valid for the process lifetime (backed by control.c's
 * compile-time DT arrays) and NULL with size 0 when no control device is
 * registered.
 */
struct zmk_animation_info {
    const struct zmk_animation_info_entry *powered_animations;
    size_t powered_animations_size;
    const struct zmk_animation_info_entry *battery_animations;
    size_t battery_animations_size;
    const struct zmk_animation_info_entry *behavior_animations;
    size_t behavior_animations_size;
    size_t num_pixels;
    uint32_t fps;
    uint8_t brightness_steps;
};

/** Fills `out` with the current static capability info. Safe to call with
 * no control device registered (returns all-zero lists). */
void zmk_animation_get_info(struct zmk_animation_info *out);

/**
 * Full live control state (DESIGN.md #3.4), used for both GetStateRequest
 * and the idempotent read-back every Set/Select/Trigger/StopOverlay RPC
 * returns.
 */
struct zmk_animation_state {
    bool enabled;
    uint8_t brightness_powered;
    uint8_t brightness_battery;
    uint8_t selected_powered;
    uint8_t selected_battery;
    /** True if the board is currently USB-bus-powered. */
    bool is_powered;
    /** True while an ad-hoc overlay is the thing being rendered. */
    bool overlay_active;
    /** Whether `overlay_index` below is meaningful (the active overlay's
     * device matches one of `behavior-animations`; false for overlays
     * started by other means, e.g. init/activation animation or a low-
     * battery alert not itself in `behavior-animations`). */
    bool has_overlay_index;
    uint8_t overlay_index;
};

/** Fills `out` with the current live control state. Safe to call with no
 * control device registered (returns a disabled/idle default state). */
void zmk_animation_get_state(struct zmk_animation_state *out);

/**
 * Registers a callback invoked every time control state changes, for any
 * reason (RPC, keymap behavior, settings-restore boot-apply, USB plug/
 * unplug affecting the active power source, ...). Used by the Studio RPC
 * handler (Phase D) to raise a `Notification.state_changed` so a connected
 * web UI stays live without polling. At most one callback is supported
 * (there is exactly one in-module RPC consumer; see DESIGN.md #3.6's note on
 * why this is a plain callback rather than a full ZMK event type) - a
 * second call replaces the previous callback. Pass NULL to unregister.
 */
typedef void (*zmk_animation_state_changed_cb_t)(void);
void zmk_animation_set_state_changed_callback(zmk_animation_state_changed_cb_t callback);

/**
 * Suppresses (`true`)/re-enables (`false`) the state-changed callback
 * registered above, without touching the registration itself. Intended for
 * a bulk re-apply of several settings at once (animation_settings.c's
 * apply_all(), triggered either at boot or re-entrantly whenever one of
 * this module's own setters writes through to custom-settings) to emit at
 * most one notification for the whole batch instead of one per setter
 * call. Confirmed on hardware (docs/design/hardware-validation.md) that
 * without this, a single Studio RPC mutation re-enters apply_all() and
 * amplifies into a burst of state-changed notifications that floods the
 * shared transport and starves that same RPC call's own Response frame.
 * Not reentrant/nesting-aware - callers that bracket a scope with
 * `true`/`false` must not call this from within another such scope.
 */
void zmk_animation_control_set_notify_suppressed(bool suppressed);

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
