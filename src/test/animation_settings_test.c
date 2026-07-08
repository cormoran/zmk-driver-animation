/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file animation_settings_test.c
 *
 * @brief native_sim-only self-test (CONFIG_ZMK_ANIMATION_SETTINGS_TEST, see
 * tests/settings_apply) proving the boot-ordering fix described in
 * DESIGN.md #3.5: seeds a persisted, non-default value for
 * "cormoran__animation"/"brightness_powered" directly into a fake settings
 * backend *before* ZMK's main() calls settings_load(), then relies on
 * control.c's init-animation delayed work (which calls
 * zmk_animation_settings_apply_boot()) to log the applied brightness once it
 * fires - the test asserts on that log line (events.patterns), the same
 * "observe via LOG_INF" mechanism every other native_sim test in this repo
 * uses (there is no RPC layer yet to query state through - Phase D).
 *
 * Modeled directly on zmk-feature-custom-settings' own
 * src/test/custom_settings_test.c fake settings_store (test_settings_store),
 * simplified to only ever need one pre-seeded record: this test does not
 * need to exercise custom-settings' write/save/discard lifecycle (that is
 * already covered by custom-settings' own tests), only that *something*
 * persisted before boot is visible to animation-control's settings-apply
 * step by the time it runs.
 *
 * Also registers a zmk_custom_setting_changed listener that logs the
 * effective in-registry value of any "cormoran__animation" setting whenever
 * it changes (tests/settings_write_through): this is how that test proves
 * control.c's public mutation API (e.g. the animctl behavior's BRIGHT
 * command) write-through-to-settings actually updates the custom-settings
 * registry itself, readable back via zmk_custom_setting_read_by_key() -
 * not just control.c's own in-memory struct animation_control_data (already
 * covered by tests/control).
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <cormoran/animation/control.h>
#include <cormoran/zmk/custom_settings.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Persisted settings key format, matching custom_settings.c's
 * setting_storage_name(): "custom_settings/<subsystem_id>/<key>". */
#define ANIMATION_SETTINGS_TEST_SEEDED_NAME "custom_settings/cormoran__animation/brightness_powered"
/* Any value different from the animation_settings.c default
 * (DT_INST_PROP(0, default_powered_brightness), which is 2 in
 * tests/settings_apply/native_sim.keymap's animation_control0 node) and
 * within the brightness-steps range constraint (0..5 there). */
#define ANIMATION_SETTINGS_TEST_SEEDED_VALUE 4

static const int32_t seeded_value = ANIMATION_SETTINGS_TEST_SEEDED_VALUE;

static ssize_t animation_settings_test_read_cb(void *cb_arg, void *data, size_t len) {
    ARG_UNUSED(cb_arg);
    size_t read_len = MIN(sizeof(seeded_value), len);
    memcpy(data, &seeded_value, read_len);
    return read_len;
}

static int animation_settings_test_load(struct settings_store *cs,
                                        const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);
    return settings_call_set_handler(ANIMATION_SETTINGS_TEST_SEEDED_NAME, sizeof(seeded_value),
                                     animation_settings_test_read_cb, NULL, arg);
}

static int animation_settings_test_save(struct settings_store *cs, const char *name,
                                        const char *value, size_t val_len) {
    /* Not exercised by this test (no write/save lifecycle here - see
     * top-of-file comment); present only because settings_store_itf
     * requires both callbacks. */
    ARG_UNUSED(cs);
    ARG_UNUSED(name);
    ARG_UNUSED(value);
    ARG_UNUSED(val_len);
    return 0;
}

static const struct settings_store_itf animation_settings_test_itf = {
    .csi_load = animation_settings_test_load,
    .csi_save = animation_settings_test_save,
};

static struct settings_store animation_settings_test_store = {
    .cs_itf = &animation_settings_test_itf,
};

/*
 * Regression test for the notification-storm bug (hardware-confirmed root
 * cause, see docs/design/hardware-validation.md and control.c's
 * notify_suppressed comment): a single mutation that writes through to
 * custom-settings re-enters animation_settings.c's apply_all() (via the
 * zmk_custom_setting_changed listener below), which used to re-apply all 5
 * settings through control.c's own setters - each calling
 * notify_state_changed() again on top of this call's own, turning one
 * mutation into 6 notifications and (on hardware) starving that mutation's
 * own RPC Response frame. This runs early (from animation_settings_test_init()
 * below, APPLICATION priority 99 - after custom_settings_init() at the
 * default APPLICATION priority 90 has reset the registry to its defaults,
 * but before ZMK's main() calls settings_load()) so the write-through ->
 * changed-event -> apply_all() re-entrant chain is already fully wired and
 * exercised, same as a real runtime RPC/behavior mutation would be.
 *
 * Any brightness value different from default-powered-brightness (2 in
 * every native_sim.keymap under tests/settings_apply*, tests/
 * settings_write_through) would prove the same point; 4 is arbitrary
 * (matches ANIMATION_SETTINGS_TEST_SEEDED_VALUE above, purely for a memorable
 * distinct number - the two are otherwise unrelated).
 */
/*
 * Gated to tests/settings_write_through only: this hook performs a real
 * brightness mutation, which would otherwise perturb the boot-apply
 * snapshots in tests/settings_apply* that share this same test file.
 */
#if IS_ENABLED(CONFIG_ZMK_ANIMATION_NOTIFY_AMPLIFICATION_TEST)
#define ANIMATION_SETTINGS_TEST_AMPLIFICATION_BRIGHTNESS 4

static int animation_settings_test_notify_count;

static void animation_settings_test_on_state_changed(void) {
    animation_settings_test_notify_count++;
}

static void animation_settings_test_amplification(void) {
    zmk_animation_set_state_changed_callback(animation_settings_test_on_state_changed);

    int before = animation_settings_test_notify_count;
    zmk_animation_set_brightness(ANIMATION_SETTINGS_TEST_AMPLIFICATION_BRIGHTNESS,
                                 ZMK_ANIMATION_POWER_SOURCE_POWERED);
    int fired = animation_settings_test_notify_count - before;

    LOG_INF("animation settings test: amplification test: brightness change fired=%d", fired);
}
#endif /* CONFIG_ZMK_ANIMATION_NOTIFY_AMPLIFICATION_TEST */

/*
 * Registers the fake backend at APPLICATION priority 99 - after
 * custom_settings_init() (APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY,
 * which resets every registered zmk_custom_setting to its default and does
 * NOT itself touch the Zephyr settings subsys) but, crucially, before ZMK's
 * main() calls settings_subsys_init()/settings_load().
 *
 * IMPORTANT: this calls settings_subsys_init() itself, *before*
 * settings_src_register() - not just for symmetry with main()'s own call,
 * but because it must be the *first* call in the whole program.
 * settings_subsys_init() is guarded by a `settings_subsys_initialized`
 * latch: the first call runs settings_init(), which calls
 * settings_store_init(), which does `sys_slist_init(&settings_load_srcs)` -
 * i.e. it discards any stores already registered via settings_src_register().
 * If main()'s settings_subsys_init() ran first (guard still false), it would
 * wipe the registration this SYS_INIT is about to make. Calling it here
 * first flips the latch, so main()'s later call becomes a no-op and our
 * registered store survives into settings_load(). Matches
 * zmk-feature-custom-settings' own custom_settings_test.c
 * (test_settings_backend_init() calls settings_subsys_init() before
 * settings_src_register(), same ordering).
 */
static int animation_settings_test_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    settings_src_register(&animation_settings_test_store);
    LOG_INF("animation settings test: seeded brightness_powered=%d before settings_load",
            ANIMATION_SETTINGS_TEST_SEEDED_VALUE);
#if IS_ENABLED(CONFIG_ZMK_ANIMATION_NOTIFY_AMPLIFICATION_TEST)
    animation_settings_test_amplification();
#endif
    return 0;
}

SYS_INIT(animation_settings_test_init, APPLICATION, 99);

/*
 * tests/settings_write_through support: logs the effective in-registry
 * value of the setting that just changed, reading it back through the same
 * public zmk_custom_setting_read_by_key() API an RPC handler or another
 * consumer would use - i.e. this proves the registry itself was updated by
 * control.c's write-through, not merely that control.c's own struct
 * animation_control_data field changed (that path is already covered by
 * tests/control's existing animctl assertions).
 */
static int animation_settings_test_changed_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *ev = as_zmk_custom_setting_changed(eh);
    if (!ev || !ev->setting) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (strcmp(ev->setting->custom_subsystem_id, "cormoran__animation") != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read(ev->setting, &value) != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (value.type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        LOG_INF("animation settings test: registry %s=%d", ev->setting->key, value.bool_value);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        LOG_INF("animation settings test: registry %s=%d", ev->setting->key, value.int32_value);
        break;
    default:
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(animation_settings_test, animation_settings_test_changed_listener);
ZMK_SUBSCRIPTION(animation_settings_test, zmk_custom_setting_changed);
