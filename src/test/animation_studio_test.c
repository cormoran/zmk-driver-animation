/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file animation_studio_test.c
 *
 * @brief native_sim-only self-test (CONFIG_ZMK_ANIMATION_STUDIO_RPC_TEST, see
 * tests/studio) exercising every `cormoran.animation` proto request kind
 * directly against zmk_animation_request_exec_handle() - the same
 * transport-agnostic entry point src/studio/animation_handler.c calls after
 * decoding a real RPC payload. native_sim has no serial/BLE transport, so
 * this bypasses RPC framing entirely and calls the decoded-request/response
 * structs directly, the same "observe via LOG_INF, assert via
 * events.patterns/keycode_events.snapshot" mechanism every other native_sim
 * test in this repo uses (see animation_settings_test.c's precedent).
 *
 * Also proves the Notification.state_changed hook (DESIGN.md #3.6) fires
 * from a non-RPC path: after exercising every RPC kind, it calls
 * zmk_animation_select() directly (the same control.h entry point the
 * animctl behavior uses) and logs whether the registered callback fired.
 *
 * Also proves the RPC-response-loss fix (docs/design/hardware-validation.md):
 * a mutating request dispatched through zmk_animation_request_exec_handle()
 * must fire *zero* animation state-changed notifications (the response
 * already carries the full state read-back; any concurrent animation
 * notification is confirmed on hardware to starve that response on the
 * shared Studio transport). test_rpc_mutation_suppresses_notification()
 * below asserts that delta directly; test_notification_from_behavior_path()
 * (a non-RPC, direct control.h call) is the contrasting case that must still
 * notify normally.
 */

#include <stdint.h>
#include <stdio.h>

#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cormoran/animation/animation_request_exec.h>
#include <cormoran/animation/control.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Buffer-sizing pitfall (skills/zmk-module-dev/SKILL.md): encoded responses
 * must fit CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE. GetInfoResponse's encoded size
 * is not compile-time-constant (it depends on display-name string lengths),
 * so a BUILD_ASSERT can't check it directly - this test instead measures the
 * real encoded size with pb_get_encoded_size() and logs it (asserted via
 * events.patterns/keycode_events.snapshot) against
 * CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE, using tests/studio/native_sim.keymap's
 * real (non-zero-device) animation-control config so the measurement
 * reflects actual entries, not an empty list.
 */
static void log_get_info_encoded_size(const cormoran_animation_Response *resp) {
    size_t encoded_size = 0;
    bool ok = pb_get_encoded_size(&encoded_size, cormoran_animation_Response_fields, resp);
    LOG_INF("animation studio test: get_info encoded_size=%u tx_buf_size=%u fits=%d",
            (unsigned int)(ok ? encoded_size : 0), CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
            ok && encoded_size <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);
}

/*
 * The keymap's real animation-control config (2 powered/2 battery/1
 * behavior, short names) only exercises a small GetInfoResponse. This
 * builds a synthetic worst-case response instead - every list filled to its
 * `.options` max_count (8, animation.options) with maximum-length names
 * (23 visible chars + NUL, animation.options' max_size:24) - so the
 * TX-buffer-fits regression check is independent of whatever a specific
 * keyboard's DT happens to configure, per the skill's documented
 * buffer-sizing pitfall.
 */
static void log_get_info_worst_case_encoded_size(void) {
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;
    resp.which_response_type = cormoran_animation_Response_info_tag;
    cormoran_animation_GetInfoResponse *info = &resp.response_type.info;

    cormoran_animation_AnimationInfo *lists[] = {info->powered_animations, info->battery_animations,
                                                 info->behavior_animations};
    pb_size_t *counts[] = {&info->powered_animations_count, &info->battery_animations_count,
                           &info->behavior_animations_count};
    for (size_t list = 0; list < ARRAY_SIZE(lists); list++) {
        size_t max_count = ARRAY_SIZE(info->powered_animations);
        for (size_t i = 0; i < max_count; i++) {
            lists[list][i].index = (uint32_t)i;
            snprintf(lists[list][i].name, sizeof(lists[list][i].name),
                     "%22zu-xxxxxxxxxxxxxxxxxxxxxxxx", i);
        }
        *counts[list] = (pb_size_t)max_count;
    }

    size_t encoded_size = 0;
    bool ok = pb_get_encoded_size(&encoded_size, cormoran_animation_Response_fields, &resp);
    LOG_INF("animation studio test: get_info_worst_case encoded_size=%u tx_buf_size=%u fits=%d",
            (unsigned int)(ok ? encoded_size : 0), CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
            ok && encoded_size <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);
}

static void log_state(const char *tag, const cormoran_animation_StateResponse *state) {
    LOG_INF("animation studio test: %s enabled=%d brightness=%u/%u selected=%u/%u is_powered=%d "
            "overlay_active=%d has_overlay_index=%d overlay_index=%u",
            tag, state->enabled, state->brightness_powered, state->brightness_battery,
            state->selected_powered, state->selected_battery, state->is_powered,
            state->overlay_active, state->has_overlay_index, state->overlay_index);
}

static void test_get_info(void) {
    cormoran_animation_Request req = cormoran_animation_Request_init_zero;
    req.which_request_type = cormoran_animation_Request_get_info_tag;
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;

    zmk_animation_request_exec_handle(&req, &resp);

    if (resp.which_response_type != cormoran_animation_Response_info_tag) {
        LOG_ERR("animation studio test: get_info did not return GetInfoResponse");
        return;
    }
    const cormoran_animation_GetInfoResponse *info = &resp.response_type.info;
    LOG_INF("animation studio test: get_info powered=%u battery=%u behavior=%u num_pixels=%u "
            "fps=%u brightness_steps=%u",
            (unsigned int)info->powered_animations_count,
            (unsigned int)info->battery_animations_count,
            (unsigned int)info->behavior_animations_count, info->num_pixels, info->fps,
            info->brightness_steps);
    for (pb_size_t i = 0; i < info->powered_animations_count; i++) {
        LOG_INF("animation studio test: get_info powered[%u]=%s", info->powered_animations[i].index,
                info->powered_animations[i].name);
    }
    for (pb_size_t i = 0; i < info->behavior_animations_count; i++) {
        LOG_INF("animation studio test: get_info behavior[%u]=%s",
                info->behavior_animations[i].index, info->behavior_animations[i].name);
    }
    log_get_info_encoded_size(&resp);
}

static void test_get_state(void) {
    cormoran_animation_Request req = cormoran_animation_Request_init_zero;
    req.which_request_type = cormoran_animation_Request_get_state_tag;
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;

    zmk_animation_request_exec_handle(&req, &resp);

    if (resp.which_response_type != cormoran_animation_Response_state_tag) {
        LOG_ERR("animation studio test: get_state did not return StateResponse");
        return;
    }
    log_state("get_state", &resp.response_type.state);
}

static void test_set_enabled(void) {
    cormoran_animation_Request req = cormoran_animation_Request_init_zero;
    req.which_request_type = cormoran_animation_Request_set_enabled_tag;
    req.request_type.set_enabled.enabled = false;
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;

    zmk_animation_request_exec_handle(&req, &resp);

    if (resp.which_response_type != cormoran_animation_Response_state_tag) {
        LOG_ERR("animation studio test: set_enabled did not return StateResponse");
        return;
    }
    log_state("set_enabled", &resp.response_type.state);

    /* Re-enable for the remaining requests below. */
    req.request_type.set_enabled.enabled = true;
    zmk_animation_request_exec_handle(&req, &resp);
}

static void test_set_brightness(void) {
    cormoran_animation_Request req = cormoran_animation_Request_init_zero;
    req.which_request_type = cormoran_animation_Request_set_brightness_tag;
    req.request_type.set_brightness.power_source =
        cormoran_animation_PowerSource_POWER_SOURCE_POWERED;
    req.request_type.set_brightness.step = 3;
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;

    zmk_animation_request_exec_handle(&req, &resp);

    if (resp.which_response_type != cormoran_animation_Response_state_tag) {
        LOG_ERR("animation studio test: set_brightness did not return StateResponse");
        return;
    }
    log_state("set_brightness", &resp.response_type.state);
}

static void test_select_animation(void) {
    cormoran_animation_Request req = cormoran_animation_Request_init_zero;
    req.which_request_type = cormoran_animation_Request_select_animation_tag;
    req.request_type.select_animation.power_source =
        cormoran_animation_PowerSource_POWER_SOURCE_POWERED;
    req.request_type.select_animation.index = 1;
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;

    zmk_animation_request_exec_handle(&req, &resp);

    if (resp.which_response_type != cormoran_animation_Response_state_tag) {
        LOG_ERR("animation studio test: select_animation did not return StateResponse");
        return;
    }
    log_state("select_animation", &resp.response_type.state);

    /* Out-of-range index must be rejected explicitly (DESIGN.md #3.6:
     * request_exec.c validates rather than relying on control.c's own
     * modulo-clamp) rather than silently wrapping. */
    req.request_type.select_animation.index = 99;
    zmk_animation_request_exec_handle(&req, &resp);
    LOG_INF("animation studio test: select_animation out-of-range which_response_type=%d",
            resp.which_response_type);
}

static void test_trigger_and_stop_overlay(void) {
    cormoran_animation_Request req = cormoran_animation_Request_init_zero;
    req.which_request_type = cormoran_animation_Request_trigger_tag;
    req.request_type.trigger.index = 0;
    req.request_type.trigger.duration_ms = 5000;
    req.request_type.trigger.mode = cormoran_animation_TriggerMode_TRIGGER_MODE_PLAY_NOW;
    req.request_type.trigger.cancelable = true;
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;

    zmk_animation_request_exec_handle(&req, &resp);

    if (resp.which_response_type != cormoran_animation_Response_state_tag) {
        LOG_ERR("animation studio test: trigger did not return StateResponse");
        return;
    }
    log_state("trigger", &resp.response_type.state);

    cormoran_animation_Request stop_req = cormoran_animation_Request_init_zero;
    stop_req.which_request_type = cormoran_animation_Request_stop_overlay_tag;
    cormoran_animation_Response stop_resp = cormoran_animation_Response_init_zero;
    zmk_animation_request_exec_handle(&stop_req, &stop_resp);
    if (stop_resp.which_response_type != cormoran_animation_Response_state_tag) {
        LOG_ERR("animation studio test: stop_overlay did not return StateResponse");
        return;
    }
    /* overlay_active is still true here: zmk_overlay_stop_if_active() only
     * calls the overlay animation's stop() vtable slot; overlay_queue.c's
     * own `current`/`overlay_active` bookkeeping is only cleared by
     * zmk_overlay_advance(), called from the engine's per-tick render step
     * (core/render.c). No engine tick runs between the two RPC calls in
     * this boot-time test (there is no k_sleep here), so this is expected -
     * on a real board the next ~33ms tick (30 FPS) settles it. */
    log_state("stop_overlay", &stop_resp.response_type.state);
}

/* --- Notification.state_changed suppression/delivery -------------------- */

static int notify_count;

static void on_state_changed(void) { notify_count++; }

/*
 * Regression test for the hardware-confirmed RPC-response-loss bug (see
 * docs/design/hardware-validation.md and src/studio/animation_request_exec.c's
 * zmk_animation_request_exec_handle()): a mutating RPC request must emit
 * *zero* animation state-changed notifications for its whole dispatch,
 * including any re-entrant apply_all() notifications from the
 * CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS write-through (tests/studio enables
 * custom-settings - see native_sim.conf - so this exercises the nested
 * suppression, not just the outer bracket alone). Uses a brightness step
 * genuinely different from the current value (set to 3 by test_set_brightness
 * above) so this is a real mutation, not a no-op that could pass for the
 * wrong reason.
 */
static void test_rpc_mutation_suppresses_notification(void) {
    int before = notify_count;

    cormoran_animation_Request req = cormoran_animation_Request_init_zero;
    req.which_request_type = cormoran_animation_Request_set_brightness_tag;
    req.request_type.set_brightness.power_source =
        cormoran_animation_PowerSource_POWER_SOURCE_POWERED;
    req.request_type.set_brightness.step = 4;
    cormoran_animation_Response resp = cormoran_animation_Response_init_zero;

    zmk_animation_request_exec_handle(&req, &resp);

    if (resp.which_response_type != cormoran_animation_Response_state_tag) {
        LOG_ERR("animation studio test: rpc_mutation_suppresses_notification did not return "
                "StateResponse");
        return;
    }
    log_state("rpc_mutation_suppresses_notification", &resp.response_type.state);
    LOG_INF("animation studio test: rpc_mutation_suppresses_notification notify_delta=%d",
            notify_count - before);
}

static void test_notification_from_behavior_path(void) {
    int before = notify_count;
    /* Same control.h entry point the animctl behavior calls - not an RPC
     * request at all; unlike test_rpc_mutation_suppresses_notification()
     * above, this must still notify. */
    zmk_animation_select(0, ZMK_ANIMATION_POWER_SOURCE_BATTERY);

    LOG_INF("animation studio test: notification_from_behavior_path fired=%d",
            notify_count > before);
}

static int animation_studio_test_init(void) {
    /* Registered before any request_exec/control.h call below so
     * test_rpc_mutation_suppresses_notification()'s delta assertion is
     * meaningful. */
    zmk_animation_set_state_changed_callback(on_state_changed);

    log_get_info_worst_case_encoded_size();
    test_get_info();
    test_get_state();
    test_set_enabled();
    test_set_brightness();
    test_select_animation();
    test_trigger_and_stop_overlay();
    test_rpc_mutation_suppresses_notification();
    test_notification_from_behavior_path();
    return 0;
}

/* APPLICATION priority, after animation_control_init() (POST_KERNEL) so the
 * control singleton is registered; before ZMK's main() runs the keymap
 * events in tests/studio/native_sim.keymap (there are none in this test -
 * everything is driven from this boot-time hook). */
SYS_INIT(animation_studio_test_init, APPLICATION, 98);
