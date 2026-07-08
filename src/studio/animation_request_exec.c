/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <cormoran/animation/animation_request_exec.h>
#include <cormoran/animation/control.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Maps cormoran.animation.PowerSource (POWERED=0/BATTERY=1) directly onto
 * `enum zmk_animation_power_source`'s first two values - proto has no
 * CURRENT: a web UI shows separate powered/battery controls and always
 * targets one explicitly (DESIGN.md #3.6 deviation, documented in the
 * Phase D report). */
static enum zmk_animation_power_source to_control_power_source(cormoran_animation_PowerSource ps) {
    return ps == cormoran_animation_PowerSource_POWER_SOURCE_BATTERY
               ? ZMK_ANIMATION_POWER_SOURCE_BATTERY
               : ZMK_ANIMATION_POWER_SOURCE_POWERED;
}

static void fill_state_response(cormoran_animation_StateResponse *out) {
    struct zmk_animation_state state;
    zmk_animation_get_state(&state);

    *out = (cormoran_animation_StateResponse)cormoran_animation_StateResponse_init_zero;
    out->enabled = state.enabled;
    out->brightness_powered = state.brightness_powered;
    out->brightness_battery = state.brightness_battery;
    out->selected_powered = state.selected_powered;
    out->selected_battery = state.selected_battery;
    out->is_powered = state.is_powered;
    out->overlay_active = state.overlay_active;
    out->has_overlay_index = state.has_overlay_index;
    out->overlay_index = state.overlay_index;
}

/* Sets resp to the StateResponse variant, i.e. the idempotent read-back
 * every Set/Select/Trigger/StopOverlay request returns on success
 * (DESIGN.md #3.6). */
static void respond_with_state(cormoran_animation_Response *resp) {
    resp->which_response_type = cormoran_animation_Response_state_tag;
    fill_state_response(&resp->response_type.state);
}

static void set_error(cormoran_animation_Response *resp, const char *message) {
    cormoran_animation_ErrorResponse err = cormoran_animation_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_animation_Response_error_tag;
    resp->response_type.error = err;
}

static void copy_info_list(const struct zmk_animation_info_entry *entries, size_t count,
                           cormoran_animation_AnimationInfo *out, pb_size_t *out_count,
                           size_t max_count) {
    size_t n = MIN(count, max_count);
    for (size_t i = 0; i < n; i++) {
        out[i] = (cormoran_animation_AnimationInfo)cormoran_animation_AnimationInfo_init_zero;
        out[i].index = (uint32_t)i;
        snprintf(out[i].name, sizeof(out[i].name), "%s", entries[i].name ? entries[i].name : "");
    }
    *out_count = (pb_size_t)n;
}

static void handle_get_info(cormoran_animation_Response *resp) {
    struct zmk_animation_info info;
    zmk_animation_get_info(&info);

    cormoran_animation_GetInfoResponse out = cormoran_animation_GetInfoResponse_init_zero;
    out.num_pixels = (uint32_t)info.num_pixels;
    out.fps = info.fps;
    out.brightness_steps = info.brightness_steps;

    copy_info_list(info.powered_animations, info.powered_animations_size, out.powered_animations,
                   &out.powered_animations_count, ARRAY_SIZE(out.powered_animations));
    copy_info_list(info.battery_animations, info.battery_animations_size, out.battery_animations,
                   &out.battery_animations_count, ARRAY_SIZE(out.battery_animations));
    copy_info_list(info.behavior_animations, info.behavior_animations_size, out.behavior_animations,
                   &out.behavior_animations_count, ARRAY_SIZE(out.behavior_animations));

    resp->which_response_type = cormoran_animation_Response_info_tag;
    resp->response_type.info = out;
}

static void handle_set_enabled(const cormoran_animation_SetEnabledRequest *req,
                               cormoran_animation_Response *resp) {
    zmk_animation_set_enabled(req->enabled);
    respond_with_state(resp);
}

static void handle_set_brightness(const cormoran_animation_SetBrightnessRequest *req,
                                  cormoran_animation_Response *resp) {
    zmk_animation_set_brightness((uint8_t)req->step, to_control_power_source(req->power_source));
    respond_with_state(resp);
}

static void handle_select_animation(const cormoran_animation_SelectAnimationRequest *req,
                                    cormoran_animation_Response *resp) {
    struct zmk_animation_info info;
    zmk_animation_get_info(&info);

    size_t size = req->power_source == cormoran_animation_PowerSource_POWER_SOURCE_BATTERY
                      ? info.battery_animations_size
                      : info.powered_animations_size;

    /*
     * control.c's zmk_animation_select() itself clamps via `% size`
     * (Phase B), but request_exec.c validates and returns an explicit error
     * for an out-of-range index instead of silently clamping: better RPC UX
     * (DESIGN.md #3.6 does not mandate either way) - a UI-driven "select
     * index 7 of a 3-entry list" bug should surface as an error, not
     * silently select index 1.
     */
    if (size == 0 || req->index >= size) {
        set_error(resp, "index out of range");
        return;
    }

    zmk_animation_select((uint8_t)req->index, to_control_power_source(req->power_source));
    respond_with_state(resp);
}

static void handle_trigger(const cormoran_animation_TriggerAnimationRequest *req,
                           cormoran_animation_Response *resp) {
    struct zmk_animation_info info;
    zmk_animation_get_info(&info);

    if (req->index >= info.behavior_animations_size) {
        set_error(resp, "index out of range");
        return;
    }

    enum zmk_animation_trigger_mode mode =
        req->mode == cormoran_animation_TriggerMode_TRIGGER_MODE_PLAY_NOW
            ? ZMK_ANIMATION_TRIGGER_PLAY_NOW
            : ZMK_ANIMATION_TRIGGER_ENQUEUE;

    int rc = zmk_animation_trigger((uint8_t)req->index, req->duration_ms, mode, req->cancelable);
    if (rc != 0) {
        set_error(resp, "trigger failed");
        return;
    }
    respond_with_state(resp);
}

static void handle_stop_overlay(const cormoran_animation_StopOverlayRequest *req,
                                cormoran_animation_Response *resp) {
    ARG_UNUSED(req);

    struct zmk_animation_state state;
    zmk_animation_get_state(&state);
    if (state.overlay_active && state.has_overlay_index) {
        zmk_animation_trigger_stop(state.overlay_index);
    }
    respond_with_state(resp);
}

bool zmk_animation_request_exec_handle(const cormoran_animation_Request *req,
                                       cormoran_animation_Response *resp) {
    /*
     * Suppress animation state-changed notifications for the whole RPC
     * request dispatch below (hardware-confirmed root cause: docs/design/
     * hardware-validation.md). A mutating request's Response already carries
     * a full StateResponse read-back, so any animation notification raised
     * while handling this request (directly, or re-entrantly via
     * animation_settings.c's apply_all() write-through) is redundant for the
     * requesting client - and, confirmed on hardware, actively harmful:
     * even one such notification concurrent with the Response starves that
     * Response on the shared Studio transport, so the client times out
     * despite the mutation having applied. ZMK Studio RPC is single-
     * connection (core selects one transport), so there is no other
     * observer that needs the notification here. Uses the nesting-safe
     * depth counter (control.h/control.c) so this outer bracket composes
     * correctly with apply_all()'s own inner bracket.
     */
    bool handled = true;
    zmk_animation_control_set_notify_suppressed(true);
    switch (req->which_request_type) {
    case cormoran_animation_Request_get_info_tag:
        handle_get_info(resp);
        break;
    case cormoran_animation_Request_get_state_tag:
        respond_with_state(resp);
        break;
    case cormoran_animation_Request_set_enabled_tag:
        handle_set_enabled(&req->request_type.set_enabled, resp);
        break;
    case cormoran_animation_Request_set_brightness_tag:
        handle_set_brightness(&req->request_type.set_brightness, resp);
        break;
    case cormoran_animation_Request_select_animation_tag:
        handle_select_animation(&req->request_type.select_animation, resp);
        break;
    case cormoran_animation_Request_trigger_tag:
        handle_trigger(&req->request_type.trigger, resp);
        break;
    case cormoran_animation_Request_stop_overlay_tag:
        handle_stop_overlay(&req->request_type.stop_overlay, resp);
        break;
    default:
        LOG_WRN("Unsupported animation request type: %d", req->which_request_type);
        handled = false;
        break;
    }
    zmk_animation_control_set_notify_suppressed(false);
    return handled;
}
