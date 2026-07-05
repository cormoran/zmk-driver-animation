/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file animation_settings.c
 *
 * @brief Registers the animation-control state (DESIGN.md #3.4) as custom
 * settings (subsystem "cormoran__animation"), and keeps control state and
 * the custom-settings registry in sync in both directions:
 *
 *  - At boot: zmk_animation_settings_apply_boot() is called by control.c
 *    from its existing init-animation delayed-work item (see
 *    animation_settings.h and control.c's init_animation_work_handler())
 *    rather than from a SYS_INIT/device init hook, to avoid a boot-ordering
 *    race against zmk-feature-custom-settings' own settings_load() (called
 *    from ZMK's main(), which runs after every SYS_INIT/DEVICE_DT_INST_DEFINE
 *    init level - see pmw3610_settings.c for the same trick applied there).
 *  - At runtime: a zmk_custom_setting_changed listener re-applies all 5
 *    settings' current effective values into control state (VALUE_UPDATED /
 *    SAVED / DISCARDED / RESET all just mean "re-read and push", matching
 *    pmw3610_settings.c's "wasteful but correct" choice - this module has
 *    only one settings-owning device, so there is nothing to iterate here,
 *    just one re-apply).
 *  - Control's public mutation API (control.c's zmk_animation_set_enabled()
 *    etc.) writes through to the custom-settings registry (MEMORY mode) on
 *    every mutation via the zmk_animation_settings_write_*() helpers below,
 *    so the generic custom-settings Studio RPC/web UI reflects
 *    keymap/RPC-driven changes live. Persisting to flash is left to the
 *    custom-settings module's own generic "save" RPC/UI action
 *    (zmk_custom_setting_save() / zmk_custom_settings_save_scope()) rather
 *    than being triggered automatically from here - see this file's
 *    "save-on-mutate design" comment below for the rationale.
 *
 * The generic get/set/save/discard/reset/export RPC surface for these
 * settings is provided by zmk-feature-custom-settings' own Studio RPC
 * subsystem - this file only *defines* the settings and reacts to changes;
 * it does not implement any RPC itself (Phase D wires up this module's own
 * animation RPC subsystem, unrelated to custom-settings' RPC).
 *
 * --- save-on-mutate design (DESIGN.md #3.5, scope item 4) ---
 *
 * Two designs were considered:
 *  (a) Write-through to MEMORY on every mutation, persist only through an
 *      explicit user/Studio-triggered save action.
 *  (b) Write-through straight to PERSIST (flash) on every mutation.
 *
 * (a) is what this file implements. Rationale: zmk_custom_setting_write()
 * with WRITE_MODE_PERSIST calls into the underlying Zephyr settings backend
 * (flash) synchronously on every call (see zmk-feature-custom-settings'
 * save_setting_locked()); animation brightness/selection changes happen from
 * a behavior binding (keymap press) or RPC request, both of which are latency
 * - and flash-wear-sensitive hot-ish paths (a user tapping a brightness-shift
 * key repeatedly, or an RPC brightness slider being dragged, could otherwise
 * hit flash on every single step). zmk-feature-custom-settings' own generic
 * Studio RPC surface already exposes an explicit save/discard/reset action
 * (zmk_custom_settings_save_scope()) that the web UI (or a future save-on-
 * idle debounce inside custom-settings itself) is expected to drive - this
 * matches the workspace convention demonstrated by
 * zmk-feature-custom-settings' own tests (test_scalar_lifecycle():
 * WRITE_MODE_MEMORY writes, explicit zmk_custom_setting_save() call) and by
 * pmw3610_settings.c (also MEMORY-only write-through; no PERSIST/save call
 * anywhere in that file). Mutating animation state without a subsequent
 * explicit save therefore does not persist across reboot until the user (or
 * the generic Studio UI) saves - documented in README.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cormoran/animation/control.h>
#include <cormoran/zmk/custom_settings.h>

#include <zmk/event_manager.h>

#include "animation_settings.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_animation_control

#define ANIMATION_SETTINGS_SUBSYSTEM_ID "cormoran__animation"

#define ANIMATION_SETTINGS_KEY_ENABLED "enabled"
#define ANIMATION_SETTINGS_KEY_BRIGHTNESS_POWERED "brightness_powered"
#define ANIMATION_SETTINGS_KEY_BRIGHTNESS_BATTERY "brightness_battery"
#define ANIMATION_SETTINGS_KEY_ANIMATION_POWERED "animation_powered"
#define ANIMATION_SETTINGS_KEY_ANIMATION_BATTERY "animation_battery"

/*
 * Range constraint bounds. DESIGN.md #6 Phase C scope note: the constraint
 * struct below is static/file-scope (built once at compile time), while
 * `brightness-steps`/list sizes are devicetree properties of the (at most
 * one, per BUILD_ASSERT in control.c) `zmk,animation-control` instance. When
 * that instance exists, its compile-time-known DT values are used directly
 * (DT_INST_PROP*() below resolves at preprocessing/compile time, same as
 * control.c's own ANIMATION_CONTROL_DEVICE() macro does) - not threaded
 * through as a runtime parameter, since STRUCT_SECTION_ITERABLE definitions
 * must be fully constant initializers. When no instance exists (e.g.
 * native_sim RPC/settings-only build with zero animation devices,
 * mirroring api_stub.c's "0 devices" pattern elsewhere in this module), a
 * fixed reasonable fallback range is used - the constraint is then unused in
 * practice (nothing will ever write these keys through a real animation
 * selection UI) but must still be a valid, non-degenerate range for the
 * settings registry itself to initialize cleanly.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#define ANIMATION_SETTINGS_BRIGHTNESS_MAX DT_INST_PROP(0, brightness_steps)
#define ANIMATION_SETTINGS_POWERED_MAX (DT_INST_PROP_LEN(0, powered_animations) - 1)
#define ANIMATION_SETTINGS_BATTERY_MAX (DT_INST_PROP_LEN(0, battery_animations) - 1)
#define ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_POWERED DT_INST_PROP(0, default_powered_brightness)
#define ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_BATTERY DT_INST_PROP(0, default_battery_brightness)
#else
#define ANIMATION_SETTINGS_BRIGHTNESS_MAX 10
#define ANIMATION_SETTINGS_POWERED_MAX 0
#define ANIMATION_SETTINGS_BATTERY_MAX 0
#define ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_POWERED 1
#define ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_BATTERY 1
#endif

/* NOTE: constraints and the zmk_custom_setting entries below are built with
 * plain designated-initializer syntax (no ZMK_CUSTOM_SETTING_DEFINE() +
 * ZMK_CUSTOM_SETTING_RANGE_INT32()/_NO_CONSTRAINT compound-literal macros),
 * matching pmw3610_settings.c and zmk-feature-custom-settings' own
 * src/test/zmk_config_sample_settings.c. This is *not* just a style
 * preference: ZMK_CUSTOM_SETTING_DEFINE()'s constraint argument ends up
 * inside another static array initializer
 * (`_name##_constraints[] = {__VA_ARGS__}`), and
 * ZMK_CUSTOM_SETTING_RANGE_INT32() itself expands to a *nested* compound
 * literal (a compound-literal .range field whose .min/.max members are
 * themselves compound literals via ZMK_CUSTOM_SETTING_VALUE_INT32()). Nested
 * compound literals are not permitted inside a static/file-scope initializer
 * per C11 6.6p9, and arm-zephyr-eabi-gcc enforces this strictly, failing
 * with "initializer element is not constant" - confirmed empirically while
 * building this file for the xiao_ble target (see report). Plain nested
 * brace-initializers (no cast) don't have this restriction. */

static const struct zmk_custom_setting_constraint animation_settings_no_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE},
};

static const struct zmk_custom_setting_constraint animation_settings_brightness_powered_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = ANIMATION_SETTINGS_BRIGHTNESS_MAX}}},
};

static const struct zmk_custom_setting_constraint animation_settings_brightness_battery_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = ANIMATION_SETTINGS_BRIGHTNESS_MAX}}},
};

static const struct zmk_custom_setting_constraint animation_settings_animation_powered_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = ANIMATION_SETTINGS_POWERED_MAX}}},
};

static const struct zmk_custom_setting_constraint animation_settings_animation_battery_constraint[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                       .int32_value = ANIMATION_SETTINGS_BATTERY_MAX}}},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, animation_setting_enabled) = {
    .custom_subsystem_id = ANIMATION_SETTINGS_SUBSYSTEM_ID,
    .key = ANIMATION_SETTINGS_KEY_ENABLED,
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = animation_settings_no_constraint,
    .constraints_count = ARRAY_SIZE(animation_settings_no_constraint),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = true},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, animation_setting_brightness_powered) = {
    .custom_subsystem_id = ANIMATION_SETTINGS_SUBSYSTEM_ID,
    .key = ANIMATION_SETTINGS_KEY_BRIGHTNESS_POWERED,
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = animation_settings_brightness_powered_constraint,
    .constraints_count = ARRAY_SIZE(animation_settings_brightness_powered_constraint),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                      .int32_value = ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_POWERED},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, animation_setting_brightness_battery) = {
    .custom_subsystem_id = ANIMATION_SETTINGS_SUBSYSTEM_ID,
    .key = ANIMATION_SETTINGS_KEY_BRIGHTNESS_BATTERY,
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = animation_settings_brightness_battery_constraint,
    .constraints_count = ARRAY_SIZE(animation_settings_brightness_battery_constraint),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                      .int32_value = ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_BATTERY},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, animation_setting_animation_powered) = {
    .custom_subsystem_id = ANIMATION_SETTINGS_SUBSYSTEM_ID,
    .key = ANIMATION_SETTINGS_KEY_ANIMATION_POWERED,
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = animation_settings_animation_powered_constraint,
    .constraints_count = ARRAY_SIZE(animation_settings_animation_powered_constraint),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, animation_setting_animation_battery) = {
    .custom_subsystem_id = ANIMATION_SETTINGS_SUBSYSTEM_ID,
    .key = ANIMATION_SETTINGS_KEY_ANIMATION_BATTERY,
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = animation_settings_animation_battery_constraint,
    .constraints_count = ARRAY_SIZE(animation_settings_animation_battery_constraint),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
};

/* --- read helpers -------------------------------------------------------- */

static bool read_bool(const char *key, bool fallback) {
    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(ANIMATION_SETTINGS_SUBSYSTEM_ID, key, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        return fallback;
    }
    return value.bool_value;
}

static int32_t read_int32(const char *key, int32_t fallback) {
    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(ANIMATION_SETTINGS_SUBSYSTEM_ID, key, &value) != 0 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        return fallback;
    }
    return value.int32_value;
}

/* --- apply settings -> control state -------------------------------------- */

/*
 * Guards against re-entering the write-through-to-settings helpers
 * (zmk_animation_settings_write_*(), called unconditionally by control.c's
 * public zmk_animation_set_*()/select() API on every mutation) while
 * apply_all() itself is driving those same setters. Without this guard,
 * apply_all() -> zmk_animation_select() -> write-through ->
 * zmk_custom_setting_write_by_key() -> zmk_custom_setting_changed event ->
 * animation_settings_changed_listener() -> zmk_animation_settings_apply_runtime()
 * -> apply_all() would recurse forever (confirmed empirically: an early
 * version without this guard produced an infinite "animation settings:
 * apply ..." log loop / stack overflow in tests/settings_apply). Not an
 * atomic/interrupt-safe flag - custom-settings' own event dispatch and this
 * module's control API all run on the same cooperative system workqueue
 * context (the init-animation delayed work, or the event manager's listener
 * dispatch), never from an ISR or a second thread.
 */
static bool applying;

/*
 * Shared by both the boot-time apply and the runtime changed-listener:
 * reads all 5 settings' current effective values and pushes them into
 * control state through control.c's public API. Using the public API here
 * (rather than poking animation_control_data fields directly) means the
 * usual clamping/ready-checks/refresh_base_animation() logic in
 * zmk_animation_set_enabled()/_set_brightness()/_select() is reused as-is,
 * so this file has no duplicate copy of that logic to keep in sync.
 *
 * The `applying` guard above suppresses control.c's write-through calls for
 * the duration of this function: re-entering that write-through here would
 * immediately re-write the same values we just read back into the registry,
 * re-raising zmk_custom_setting_changed and re-triggering this very
 * function. The two directions (settings -> control, control -> settings)
 * are intentionally asymmetric: this function is the only settings ->
 * control path, and control.c's setters (outside of this guarded window)
 * are the only control -> settings path.
 */
static void apply_all(void) {
    applying = true;

    bool enabled = read_bool(ANIMATION_SETTINGS_KEY_ENABLED, true);
    int32_t brightness_powered =
        read_int32(ANIMATION_SETTINGS_KEY_BRIGHTNESS_POWERED,
                  ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_POWERED);
    int32_t brightness_battery =
        read_int32(ANIMATION_SETTINGS_KEY_BRIGHTNESS_BATTERY,
                  ANIMATION_SETTINGS_DEFAULT_BRIGHTNESS_BATTERY);
    int32_t animation_powered = read_int32(ANIMATION_SETTINGS_KEY_ANIMATION_POWERED, 0);
    int32_t animation_battery = read_int32(ANIMATION_SETTINGS_KEY_ANIMATION_BATTERY, 0);

    LOG_INF("animation settings: apply enabled=%d brightness=%d/%d animation=%d/%d", enabled,
            brightness_powered, brightness_battery, animation_powered, animation_battery);

    /* Selection first, then brightness, then enabled - matches control.c's
     * own field order (DESIGN.md #3.4) and ensures refresh_base_animation()
     * (called by select()) sees the final brightness/enabled values already
     * in place when it starts the base animation. */
    zmk_animation_select((uint8_t)animation_powered, ZMK_ANIMATION_POWER_SOURCE_POWERED);
    zmk_animation_select((uint8_t)animation_battery, ZMK_ANIMATION_POWER_SOURCE_BATTERY);
    zmk_animation_set_brightness((uint8_t)brightness_powered, ZMK_ANIMATION_POWER_SOURCE_POWERED);
    zmk_animation_set_brightness((uint8_t)brightness_battery, ZMK_ANIMATION_POWER_SOURCE_BATTERY);
    zmk_animation_set_enabled(enabled);

    applying = false;
}

void zmk_animation_settings_apply_boot(void) { apply_all(); }

void zmk_animation_settings_apply_runtime(void) { apply_all(); }

/* --- write-through: control state -> settings ----------------------------- */
/* Each helper below no-ops while `applying` is true (see apply_all()'s doc
 * comment) - i.e. only writes through when called as a result of a "real"
 * mutation from a behavior/RPC, never while apply_all() itself is driving
 * control.c's setters from a settings read. */

void zmk_animation_settings_write_enabled(bool enabled) {
    if (applying) {
        return;
    }
    zmk_custom_setting_write_by_key(ANIMATION_SETTINGS_SUBSYSTEM_ID, ANIMATION_SETTINGS_KEY_ENABLED,
                                    &ZMK_CUSTOM_SETTING_VALUE_BOOL(enabled),
                                    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

void zmk_animation_settings_write_brightness_powered(uint8_t step) {
    if (applying) {
        return;
    }
    zmk_custom_setting_write_by_key(ANIMATION_SETTINGS_SUBSYSTEM_ID,
                                    ANIMATION_SETTINGS_KEY_BRIGHTNESS_POWERED,
                                    &ZMK_CUSTOM_SETTING_VALUE_INT32(step),
                                    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

void zmk_animation_settings_write_brightness_battery(uint8_t step) {
    if (applying) {
        return;
    }
    zmk_custom_setting_write_by_key(ANIMATION_SETTINGS_SUBSYSTEM_ID,
                                    ANIMATION_SETTINGS_KEY_BRIGHTNESS_BATTERY,
                                    &ZMK_CUSTOM_SETTING_VALUE_INT32(step),
                                    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

void zmk_animation_settings_write_animation_powered(uint8_t index) {
    if (applying) {
        return;
    }
    zmk_custom_setting_write_by_key(ANIMATION_SETTINGS_SUBSYSTEM_ID,
                                    ANIMATION_SETTINGS_KEY_ANIMATION_POWERED,
                                    &ZMK_CUSTOM_SETTING_VALUE_INT32(index),
                                    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

void zmk_animation_settings_write_animation_battery(uint8_t index) {
    if (applying) {
        return;
    }
    zmk_custom_setting_write_by_key(ANIMATION_SETTINGS_SUBSYSTEM_ID,
                                    ANIMATION_SETTINGS_KEY_ANIMATION_BATTERY,
                                    &ZMK_CUSTOM_SETTING_VALUE_INT32(index),
                                    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

/* --- runtime changed listener --------------------------------------------- */

static int animation_settings_changed_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *ev = as_zmk_custom_setting_changed(eh);
    if (!ev || !ev->setting) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (strncmp(ev->setting->custom_subsystem_id, ANIMATION_SETTINGS_SUBSYSTEM_ID,
                CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* All changed kinds (VALUE_UPDATED / SAVED / DISCARDED / RESET) mean the
     * same thing to us: re-read the effective value of every key and push
     * it into control state (pmw3610_settings.c's "wasteful but correct"
     * choice - only one settings-owning singleton here, so "every key" is
     * just these 5). */
    zmk_animation_settings_apply_runtime();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(animation_settings, animation_settings_changed_listener);
ZMK_SUBSCRIPTION(animation_settings, zmk_custom_setting_changed);
