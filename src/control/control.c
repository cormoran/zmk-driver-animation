/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation_control

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

#include <cormoran/animation/animation.h>
#include <cormoran/animation/control.h>

#include "../core/event_dispatch.h"
#include "control_internal.h"
#include "overlay_queue.h"
#include "power_policy.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Exactly one `zmk,animation-control` instance is supported (DESIGN.md #4
 * item 11, same pattern as engine.c's BUILD_ASSERT): a stray second
 * instance fails the build instead of being silently ignored the way v1's
 * hardcoded DT-instance-0 access would have.
 */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "Only one enabled `zmk,animation-control` instance is supported");

/*
 * The public control.h API and the control_internal.h accessors (below)
 * are defined unconditionally - not just when a `zmk,animation-control`
 * node exists - mirroring the RPC/settings "API stub returning 0 devices"
 * pattern used elsewhere in this template (skills/zmk-module-dev). This
 * lets render.c, behaviors, and (in Phase D) request_exec link against a
 * single symbol set regardless of whether animation is wired up as a bare
 * `zmk,animation` node (no control layer at all) or with a full
 * `zmk,animation-control`. Every function already checks `control_dev ==
 * NULL` first and degrades to a no-op / false / 1.0x-brightness default,
 * so behavior is unaffected by which branch actually compiled the device.
 */
static const struct device *control_dev;

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define PHANDLE_TO_DEVICE(node_id, prop, idx) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

struct animation_control_config {
    const struct device *const *powered_animations;
    size_t powered_animations_size;
    const struct device *const *battery_animations;
    size_t battery_animations_size;
    const struct device *const *behavior_animations;
    size_t behavior_animations_size;
    const struct device *init_animation;
    uint32_t init_animation_duration_ms;
    uint32_t init_animation_delay_ms;
    const struct device *activation_animation;
    uint32_t activation_animation_duration_ms;
    const struct device *ext_power;
    uint8_t brightness_steps;
    uint8_t max_brightness;
    struct k_msgq *overlay_msgq;
    char *overlay_msgq_buffer;
    size_t overlay_msgq_capacity;
};

struct animation_control_data {
    /* DESIGN.md #3.4 control state. Every mutation goes through the
     * zmk_animation_*() functions below; nothing else writes these. */
    bool enabled;
    uint8_t brightness_powered;
    uint8_t brightness_battery;
    uint8_t selected_powered;
    uint8_t selected_battery;

    /* True once start() has been called and stop() has not undone it;
     * gates rendering/overlay dispatch the same way v1's `running` did. */
    bool running;
    /* Cached power source as of the last render/change_base tick, so a mid-
     * overlay power-source flip doesn't change the base animation choice
     * out from under an active overlay until it next dispatches. */
    bool last_powered;
    /*
     * The animation device refresh_base_animation() last actually
     * started (NULL if none / not running). This is *not* recomputable
     * from {last_powered, selected_powered, selected_battery} alone: a
     * selection change (zmk_animation_select*()) mutates
     * selected_powered/battery *before* calling refresh_base_animation(),
     * so recomputing "previous" from current state inside that function
     * would see the already-updated selection and wrongly conclude
     * nothing changed. Tracking the actual running device sidesteps that
     * class of bug entirely.
     */
    const struct device *current_base;

    struct k_work_delayable init_animation_work;
};

static const struct device *current_base_animation(const struct device *dev) {
    const struct animation_control_config *config = dev->config;
    struct animation_control_data *data = dev->data;

    if (data->last_powered) {
        return config->powered_animations[data->selected_powered];
    }
    return config->battery_animations[data->selected_battery];
}

/**
 * @brief Switches the rendered base animation if the power source or
 * selection changed since the last call. Starts/stops the ext-power rail
 * to match "something is playing".
 */
static void refresh_base_animation(const struct device *dev) {
    struct animation_control_data *data = dev->data;

    data->last_powered = zmk_animation_power_policy_is_powered();

    const struct device *next = current_base_animation(dev);
    const struct device *previous = data->current_base;

    if (next == previous) {
        return;
    }

    if (previous != NULL) {
        zmk_animation_call_stop(previous);
    }
    if (next != NULL) {
        zmk_animation_call_start(next, ZMK_ANIMATION_DURATION_FOREVER);
    }
    data->current_base = next;
    zmk_animation_request_frames(1);
}

/* --- control_internal.h: queried by render.c every tick --- */

bool zmk_animation_control_is_running(void) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        return false;
    }
    struct animation_control_data *data = control_dev->data;
    return data->enabled && data->running;
}

const struct device *zmk_animation_control_current_base(void) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        return NULL;
    }
    struct animation_control_data *data = control_dev->data;
    return data->current_base;
}

float zmk_animation_control_current_brightness_multiplier(void) {
    if (control_dev == NULL) {
        return 1.0f;
    }
    const struct animation_control_config *config = control_dev->config;
    struct animation_control_data *data = control_dev->data;

    uint8_t step = data->last_powered ? data->brightness_powered : data->brightness_battery;
    if (step > config->brightness_steps) {
        step = config->brightness_steps;
    }

    float step_ratio = (float)step / (float)config->brightness_steps;
    float max_ratio = (float)config->max_brightness / 255.0f;
    return step_ratio * max_ratio;
}

void zmk_animation_overlay_render(struct zmk_animation_pixel *pixels, size_t num_pixels) {
    if (control_dev == NULL) {
        return;
    }
    const struct animation_control_config *config = control_dev->config;

    if (!zmk_overlay_is_active() && !zmk_overlay_has_queued(config->overlay_msgq)) {
        return;
    }

    struct zmk_overlay_record active = zmk_overlay_current();
    bool finished = true;
    if (active.animation != NULL) {
        zmk_animation_call_render_frame(active.animation, pixels, num_pixels);
        finished = zmk_animation_call_is_finished(active.animation);
    }
    /* zmk_overlay_advance() itself distinguishes "active overlay finished"
     * from "idle but something is queued" (see its comment); this call
     * covers both. */
    zmk_overlay_advance(config->overlay_msgq, finished, false);
}

/* --- struct zmk_animation_api: control behaves like an animation, so the
 * engine can start/stop/is_finished it uninvolved with what's inside. --- */

static void control_start(const struct device *dev, uint32_t request_duration_ms) {
    ARG_UNUSED(request_duration_ms);
    const struct animation_control_config *config = dev->config;
    struct animation_control_data *data = dev->data;

    if (!data->enabled || data->running) {
        return;
    }
    data->running = true;
    refresh_base_animation(dev);
    zmk_animation_power_policy_set_power(config->ext_power, true);
}

static void control_stop(const struct device *dev) {
    const struct animation_control_config *config = dev->config;
    struct animation_control_data *data = dev->data;

    if (!data->running) {
        return;
    }

    if (data->current_base != NULL) {
        zmk_animation_call_stop(data->current_base);
    }
    zmk_overlay_reset(config->overlay_msgq);
    zmk_animation_power_policy_set_power(config->ext_power, false);
    data->running = false;
    data->current_base = NULL;
}

static void control_render_frame(const struct device *dev, struct zmk_animation_pixel *pixels,
                                 size_t num_pixels) {
    /* Compositing lives in core/render.c (DESIGN.md #3.3 describes the
     * pipeline as owned by render.c); control's vtable render_frame is not
     * used by the engine (it drives rendering through the
     * control_internal.h accessors instead) but is implemented so control
     * is a well-formed `zmk_animation_api` device, e.g. if ever nested
     * inside a zmk,animation-compose in a future extension. */
    if (!zmk_animation_control_is_running()) {
        return;
    }
    struct animation_control_data *data = dev->data;
    if (data->current_base != NULL) {
        zmk_animation_call_render_frame(data->current_base, pixels, num_pixels);
    }
    zmk_animation_overlay_render(pixels, num_pixels);
}

static bool control_is_finished(const struct device *dev) {
    struct animation_control_data *data = dev->data;
    return !data->running;
}

static const struct zmk_animation_api control_api = {
    .start = control_start,
    .stop = control_stop,
    .render_frame = control_render_frame,
    .is_finished = control_is_finished,
};

/* --- public control.h API, used by behaviors / studio request_exec --- */

void zmk_animation_set_enabled(bool enabled) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        LOG_WRN("animation control not ready, ignoring set_enabled");
        return;
    }
    struct animation_control_data *data = control_dev->data;
    if (data->enabled == enabled) {
        return;
    }
    LOG_INF("animation control: set enabled %d", enabled);
    data->enabled = enabled;
    if (enabled) {
        control_start(control_dev, ZMK_ANIMATION_DURATION_FOREVER);
    } else {
        control_stop(control_dev);
    }
}

static uint8_t *brightness_ref(enum zmk_animation_power_source power_source) {
    struct animation_control_data *data = control_dev->data;
    bool powered = power_source == ZMK_ANIMATION_POWER_SOURCE_POWERED ||
                   (power_source == ZMK_ANIMATION_POWER_SOURCE_CURRENT && data->last_powered);
    return powered ? &data->brightness_powered : &data->brightness_battery;
}

void zmk_animation_set_brightness_shift(int steps, enum zmk_animation_power_source power_source) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        LOG_WRN("animation control not ready, ignoring set_brightness_shift");
        return;
    }
    const struct animation_control_config *config = control_dev->config;
    uint8_t *ref = brightness_ref(power_source);

    int current = *ref;
    int next = current + steps;
    if (next < 0) {
        next = 0;
    }
    if (next > config->brightness_steps) {
        next = config->brightness_steps;
    }
    if (next != current) {
        LOG_INF("animation control: change brightness %d->%d", current, next);
        *ref = (uint8_t)next;
    }
}

void zmk_animation_set_brightness(uint8_t step, enum zmk_animation_power_source power_source) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        LOG_WRN("animation control not ready, ignoring set_brightness");
        return;
    }
    const struct animation_control_config *config = control_dev->config;
    uint8_t *ref = brightness_ref(power_source);
    *ref = step > config->brightness_steps ? config->brightness_steps : step;
}

static uint8_t *selected_ref(enum zmk_animation_power_source power_source, size_t *out_size) {
    const struct animation_control_config *config = control_dev->config;
    struct animation_control_data *data = control_dev->data;
    bool powered = power_source == ZMK_ANIMATION_POWER_SOURCE_POWERED ||
                   (power_source == ZMK_ANIMATION_POWER_SOURCE_CURRENT && data->last_powered);

    if (powered) {
        *out_size = config->powered_animations_size;
        return &data->selected_powered;
    }
    *out_size = config->battery_animations_size;
    return &data->selected_battery;
}

void zmk_animation_select_shift(int index_offset, enum zmk_animation_power_source power_source) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        LOG_WRN("animation control not ready, ignoring select_shift");
        return;
    }
    if (index_offset == 0) {
        return;
    }
    size_t size;
    uint8_t *ref = selected_ref(power_source, &size);
    if (size == 0) {
        return;
    }

    int offset = index_offset % (int)size;
    int current = *ref;
    int next = (current + offset + (int)size) % (int)size;
    /* No spaces around "->": ZMK's native_sim test harness strips log
     * lines with `sed -e "s/.*> //"`, which is greedy and eats through any
     * "> " (greater-than followed by a space) in the message itself, not
     * just the <inf>/<dbg> level tag - "%d -> %d" gets truncated to just
     * the last number. "%d->%d" (control.c's brightness log already used
     * this form) has no such substring and survives intact. */
    LOG_INF("animation control: shift index %d->%d", current, next);
    *ref = (uint8_t)next;

    refresh_base_animation(control_dev);
}

void zmk_animation_select(uint8_t index, enum zmk_animation_power_source power_source) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        LOG_WRN("animation control not ready, ignoring select");
        return;
    }
    size_t size;
    uint8_t *ref = selected_ref(power_source, &size);
    if (size == 0) {
        return;
    }
    uint8_t next = index % size;
    if (next != *ref) {
        LOG_INF("animation control: select index %d->%d", *ref, next);
        *ref = next;
    }

    refresh_base_animation(control_dev);
}

int zmk_animation_trigger(uint8_t index, uint32_t duration_ms, enum zmk_animation_trigger_mode mode,
                          bool cancelable) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        LOG_WRN("animation control not ready, ignoring trigger");
        return -ENODEV;
    }
    const struct animation_control_config *config = control_dev->config;
    if (index >= config->behavior_animations_size) {
        LOG_ERR("animation trigger index %u out of range", index);
        return -EINVAL;
    }
    if (!zmk_animation_control_is_running()) {
        return 0;
    }

    const struct device *animation = config->behavior_animations[index];
    struct zmk_overlay_record record = {
        .animation = animation,
        .cancelable = cancelable,
        .duration_ms = duration_ms,
    };

    /*
     * v1's `animation_control_play_now()` accidentally dispatched to the
     * `enqueue_animation` vtable slot instead of `play_now` (DESIGN.md #4
     * item 2, include/zmk_driver_animation/drivers/animation_control.h
     * lines 140-147). There is no vtable here to mix up: the two modes call
     * two distinctly-named overlay_queue.h functions directly.
     * tests/overlay_queue exercises both to catch a regression.
     */
    switch (mode) {
    case ZMK_ANIMATION_TRIGGER_ENQUEUE:
        return zmk_overlay_enqueue(config->overlay_msgq, &record);
    case ZMK_ANIMATION_TRIGGER_PLAY_NOW:
        zmk_overlay_play_now(config->overlay_msgq, &record);
        return 0;
    default:
        return -EINVAL;
    }
}

int zmk_animation_trigger_stop(uint8_t index) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        return -ENODEV;
    }
    const struct animation_control_config *config = control_dev->config;
    if (index >= config->behavior_animations_size) {
        return -EINVAL;
    }
    zmk_overlay_stop_if_active(config->overlay_msgq, config->behavior_animations[index]);
    return 0;
}

int zmk_animation_enqueue(const struct device *animation, bool cancelable, uint32_t duration_ms) {
    if (control_dev == NULL || !device_is_ready(control_dev)) {
        LOG_WRN("animation control not ready, ignoring enqueue");
        return -ENODEV;
    }
    if (!zmk_animation_control_is_running()) {
        return 0;
    }
    const struct animation_control_config *config = control_dev->config;
    struct zmk_overlay_record record = {
        .animation = animation,
        .cancelable = cancelable,
        .duration_ms = duration_ms,
    };
    return zmk_overlay_enqueue(config->overlay_msgq, &record);
}

/* --- init + event listeners --- */

static void init_animation_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    const struct animation_control_config *config = control_dev->config;
    if (config->init_animation == NULL) {
        return;
    }
    uint32_t duration_ms = config->init_animation_duration_ms > 0
                               ? config->init_animation_duration_ms
                               : ZMK_ANIMATION_DURATION_FOREVER;
    zmk_animation_enqueue(config->init_animation, false, duration_ms);
}

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
static int
animation_control_on_usb_conn_state_changed(const struct device *dev,
                                            const struct zmk_usb_conn_state_changed *event) {
    ARG_UNUSED(event);
    struct animation_control_data *data = dev->data;
    if (data->running) {
        refresh_base_animation(dev);
    }
    return 0;
}
#endif

static int
animation_control_on_activity_state_changed(const struct device *dev,
                                            const struct zmk_activity_state_changed *event) {
    const struct animation_control_config *config = dev->config;
    struct animation_control_data *data = dev->data;

    if (event->state == ZMK_ACTIVITY_ACTIVE && data->running &&
        config->activation_animation != NULL) {
        uint32_t duration_ms = config->activation_animation_duration_ms > 0
                                   ? config->activation_animation_duration_ms
                                   : ZMK_ANIMATION_DURATION_FOREVER;
        zmk_animation_enqueue(config->activation_animation, false, duration_ms);
    }
    return 0;
}

static int control_event_listener(const zmk_event_t *event) {
    if (control_dev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    const struct zmk_usb_conn_state_changed *usb_event = as_zmk_usb_conn_state_changed(event);
    if (usb_event != NULL) {
        animation_control_on_usb_conn_state_changed(control_dev, usb_event);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif
    const struct zmk_activity_state_changed *activity_event = as_zmk_activity_state_changed(event);
    if (activity_event != NULL) {
        animation_control_on_activity_state_changed(control_dev, activity_event);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_ANIMATION_DEFINE_LISTENER_EX(zmk_animation_control, control_event_listener);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(zmk_animation_control, zmk_usb_conn_state_changed);
#endif
ZMK_SUBSCRIPTION(zmk_animation_control, zmk_activity_state_changed);

static int animation_control_init(const struct device *dev) {
    const struct animation_control_config *config = dev->config;
    struct animation_control_data *data = dev->data;

    if (config->ext_power != NULL && !device_is_ready(config->ext_power)) {
        LOG_ERR("ext-power device not ready");
        return -ENODEV;
    }

    /*
     * BUILD_ASSERT above guarantees at most one instance, so it is safe to
     * resolve the control.h/control_internal.h singleton pointer here, in
     * the device's own init function - no separate SYS_INIT priority to
     * get right relative to this one.
     */
    control_dev = dev;
    zmk_overlay_queue_init(config->overlay_msgq, config->overlay_msgq_buffer,
                           config->overlay_msgq_capacity);

    data->last_powered = zmk_animation_power_policy_is_powered();

    if (config->init_animation != NULL) {
        k_work_init_delayable(&data->init_animation_work, init_animation_work_handler);
        k_work_schedule(&data->init_animation_work, K_MSEC(config->init_animation_delay_ms));
    }

    LOG_INF("ZMK animation control ready (%zu powered, %zu battery, %zu behavior animations)",
            config->powered_animations_size, config->battery_animations_size,
            config->behavior_animations_size);
    return 0;
}

#define ANIMATION_CONTROL_DEVICE(idx)                                                              \
                                                                                                   \
    static const struct device *animation_control_##idx##_powered[] = {                            \
        DT_INST_FOREACH_PROP_ELEM(idx, powered_animations, PHANDLE_TO_DEVICE)};                    \
    static const struct device *animation_control_##idx##_battery[] = {                            \
        DT_INST_FOREACH_PROP_ELEM(idx, battery_animations, PHANDLE_TO_DEVICE)};                    \
    static const struct device *animation_control_##idx##_behavior[] = {                           \
        DT_INST_FOREACH_PROP_ELEM(idx, behavior_animations, PHANDLE_TO_DEVICE)};                   \
                                                                                                   \
    static char animation_control_##idx##_overlay_buffer[DT_INST_PROP(idx, queue_size) *           \
                                                         sizeof(struct zmk_overlay_record)];       \
    static struct k_msgq animation_control_##idx##_overlay_msgq;                                   \
                                                                                                   \
    static const struct animation_control_config animation_control_##idx##_config = {              \
        .powered_animations = animation_control_##idx##_powered,                                   \
        .powered_animations_size = DT_INST_PROP_LEN(idx, powered_animations),                      \
        .battery_animations = animation_control_##idx##_battery,                                   \
        .battery_animations_size = DT_INST_PROP_LEN(idx, battery_animations),                      \
        .behavior_animations = animation_control_##idx##_behavior,                                 \
        .behavior_animations_size = DT_INST_PROP_LEN(idx, behavior_animations),                    \
        .init_animation = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(idx, init_animation)),             \
        .init_animation_duration_ms = DT_INST_PROP(idx, init_animation_duration_ms),               \
        .init_animation_delay_ms = DT_INST_PROP(idx, init_animation_delay_ms),                     \
        .activation_animation = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(idx, activation_animation)), \
        .activation_animation_duration_ms = DT_INST_PROP(idx, activation_animation_duration_ms),   \
        .ext_power = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(idx, ext_power)),                       \
        .brightness_steps = DT_INST_PROP(idx, brightness_steps),                                   \
        .max_brightness = DT_INST_PROP(idx, max_brightness),                                       \
        .overlay_msgq = &animation_control_##idx##_overlay_msgq,                                   \
        .overlay_msgq_buffer = animation_control_##idx##_overlay_buffer,                           \
        .overlay_msgq_capacity = DT_INST_PROP(idx, queue_size),                                    \
    };                                                                                             \
                                                                                                   \
    static struct animation_control_data animation_control_##idx##_data = {                        \
        .enabled = true,                                                                           \
        .brightness_powered = DT_INST_PROP(idx, default_powered_brightness),                       \
        .brightness_battery = DT_INST_PROP(idx, default_battery_brightness),                       \
        .selected_powered = 0,                                                                     \
        .selected_battery = 0,                                                                     \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &animation_control_init, NULL, &animation_control_##idx##_data,     \
                          &animation_control_##idx##_config, POST_KERNEL,                          \
                          CONFIG_APPLICATION_INIT_PRIORITY, &control_api);

DT_INST_FOREACH_STATUS_OKAY(ANIMATION_CONTROL_DEVICE);

#else /* !DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

/*
 * No `zmk,animation-control` node in this build (e.g. a bare `zmk,animation`
 * node with no control layer, or RPC/settings-only native_sim builds with
 * zero animation devices at all). `control_dev` stays NULL, so every
 * function above that would need it degrades to its documented no-op /
 * false / 1.0x-brightness default - these stubs exist purely so render.c
 * and the behaviors link.
 */

bool zmk_animation_control_is_running(void) { return false; }
const struct device *zmk_animation_control_current_base(void) { return NULL; }
float zmk_animation_control_current_brightness_multiplier(void) { return 1.0f; }
void zmk_animation_overlay_render(struct zmk_animation_pixel *pixels, size_t num_pixels) {
    ARG_UNUSED(pixels);
    ARG_UNUSED(num_pixels);
}

void zmk_animation_set_enabled(bool enabled) {
    ARG_UNUSED(enabled);
    LOG_WRN("no animation control device, ignoring set_enabled");
}
void zmk_animation_set_brightness_shift(int steps, enum zmk_animation_power_source power_source) {
    ARG_UNUSED(steps);
    ARG_UNUSED(power_source);
    LOG_WRN("no animation control device, ignoring set_brightness_shift");
}
void zmk_animation_set_brightness(uint8_t step, enum zmk_animation_power_source power_source) {
    ARG_UNUSED(step);
    ARG_UNUSED(power_source);
    LOG_WRN("no animation control device, ignoring set_brightness");
}
void zmk_animation_select_shift(int index_offset, enum zmk_animation_power_source power_source) {
    ARG_UNUSED(index_offset);
    ARG_UNUSED(power_source);
    LOG_WRN("no animation control device, ignoring select_shift");
}
void zmk_animation_select(uint8_t index, enum zmk_animation_power_source power_source) {
    ARG_UNUSED(index);
    ARG_UNUSED(power_source);
    LOG_WRN("no animation control device, ignoring select");
}
int zmk_animation_trigger(uint8_t index, uint32_t duration_ms, enum zmk_animation_trigger_mode mode,
                          bool cancelable) {
    ARG_UNUSED(index);
    ARG_UNUSED(duration_ms);
    ARG_UNUSED(mode);
    ARG_UNUSED(cancelable);
    return -ENODEV;
}
int zmk_animation_trigger_stop(uint8_t index) {
    ARG_UNUSED(index);
    return -ENODEV;
}
int zmk_animation_enqueue(const struct device *animation, bool cancelable, uint32_t duration_ms) {
    ARG_UNUSED(animation);
    ARG_UNUSED(cancelable);
    ARG_UNUSED(duration_ms);
    return -ENODEV;
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
