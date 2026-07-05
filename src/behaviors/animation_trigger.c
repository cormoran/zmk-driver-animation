/*
 * Copyright (c) 2025 cormoran
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_animation_trigger

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/animation/control.h>
#include <dt-bindings/zmk_driver_animation/animation_trigger.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * `zmk,behavior-animation-trigger` (`animtrig`): press/hold/release
 * semantics with min/max duration + hold-extension. Keeps v1's fixed-size
 * array + delayed-work bookkeeping (DESIGN.md #3.7), but routes the actual
 * play/stop through control.h's zmk_animation_trigger()/_stop() instead of
 * reaching into overlay_queue.c directly, so RPC and keymap triggers share
 * one dispatch path.
 */

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct trigger_status {
    bool triggered;
    uint8_t index;
    uint8_t num_pressed;
    uint32_t remaining_duration_ms;
};

static struct trigger_status trigger_statuses[CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM];
static struct k_mutex mutex;
static struct k_work_delayable animation_stop_work;
static int64_t last_work_time;

static uint8_t find_trigger_status_index(uint8_t index) {
    for (int i = 0; i < CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM; i++) {
        if (trigger_statuses[i].triggered && trigger_statuses[i].index == index) {
            return i;
        }
    }
    return CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM;
}

static uint8_t find_empty_index(void) {
    for (int i = 0; i < CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM; i++) {
        if (!trigger_statuses[i].triggered) {
            return i;
        }
    }
    return CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM;
}

static void animation_stop_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    uint32_t elapsed = (uint32_t)(now - last_work_time);
    uint32_t min_remaining = 0;

    for (int i = 0; i < CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM; i++) {
        if (!trigger_statuses[i].triggered) {
            continue;
        }
        k_mutex_lock(&mutex, K_FOREVER);
        struct trigger_status s = trigger_statuses[i];
        if (!s.triggered) {
            k_mutex_unlock(&mutex);
            continue;
        }
        if (s.remaining_duration_ms > elapsed) {
            s.remaining_duration_ms -= elapsed;
            if (min_remaining < s.remaining_duration_ms) {
                min_remaining = s.remaining_duration_ms;
            }
            trigger_statuses[i] = s;
            k_mutex_unlock(&mutex);
            continue;
        }
        if (s.num_pressed > 0) {
            LOG_INF("animtrig %d: still held, extending", s.index);
            s.remaining_duration_ms = CONFIG_ZMK_ANIMATION_TRIGGER_EXTEND_MS_ON_HOLD;
            if (min_remaining < s.remaining_duration_ms) {
                min_remaining = s.remaining_duration_ms;
            }
            trigger_statuses[i] = s;
            k_mutex_unlock(&mutex);
            continue;
        }
        LOG_INF("animtrig %d: stop (duration elapsed)", s.index);
        zmk_animation_trigger_stop(s.index);
        trigger_statuses[i] = (struct trigger_status){0};
        k_mutex_unlock(&mutex);
    }

    last_work_time = now;
    if (min_remaining > 0) {
        k_work_schedule(&animation_stop_work, K_MSEC(min_remaining));
    }
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    if (binding->param1 != ANIMATION_TRIGGER_CMD_TRIGGER) {
        LOG_ERR("Unknown animtrig command: %d", binding->param1);
        return -ENOTSUP;
    }

    uint8_t animation_index = binding->param2;

    k_mutex_lock(&mutex, K_FOREVER);
    uint8_t index = find_trigger_status_index(animation_index);
    if (index < CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM) {
        trigger_statuses[index].num_pressed++;
        LOG_INF("animtrig %d: additional press (num_pressed=%d)", animation_index,
                trigger_statuses[index].num_pressed);
        k_mutex_unlock(&mutex);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    index = find_empty_index();
    if (index >= CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM) {
        LOG_ERR("No empty trigger slot for animation %d", animation_index);
        k_mutex_unlock(&mutex);
        return -ENOTSUP;
    }
    trigger_statuses[index] = (struct trigger_status){
        .triggered = true,
        .index = animation_index,
        .num_pressed = 1,
        .remaining_duration_ms = CONFIG_ZMK_ANIMATION_TRIGGER_MIN_DURATION_MS,
    };
    k_mutex_unlock(&mutex);

    LOG_INF("animtrig %d: pressed", animation_index);
    int rc = zmk_animation_trigger(animation_index, CONFIG_ZMK_ANIMATION_TRIGGER_MAX_DURATION_MS,
                                   ZMK_ANIMATION_TRIGGER_PLAY_NOW, true);
    if (rc != 0) {
        LOG_ERR("Failed to play animation %d: %d", animation_index, rc);
        return rc;
    }

    last_work_time = k_uptime_get();
    k_work_schedule(&animation_stop_work, K_MSEC(CONFIG_ZMK_ANIMATION_TRIGGER_MIN_DURATION_MS));
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    if (binding->param1 != ANIMATION_TRIGGER_CMD_TRIGGER) {
        LOG_ERR("Unknown animtrig command: %d", binding->param1);
        return -ENOTSUP;
    }

    uint8_t animation_index = binding->param2;
    LOG_INF("animtrig %d: released", animation_index);
    k_mutex_lock(&mutex, K_FOREVER);
    uint8_t index = find_trigger_status_index(animation_index);
    if (index < CONFIG_ZMK_ANIMATION_TRIGGER_MAX_PARALLELISM) {
        uint8_t pressed = trigger_statuses[index].num_pressed;
        trigger_statuses[index].num_pressed = pressed > 0 ? pressed - 1 : 0;
    }
    k_mutex_unlock(&mutex);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_animation_trigger_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_mutex_init(&mutex);
    k_work_init_delayable(&animation_stop_work, animation_stop_work_handler);
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata trigger_param1_values[] = {
    {
        .display_name = "Trigger animation",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = ANIMATION_TRIGGER_CMD_TRIGGER,
    },
};

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_animation_control)
static const struct behavior_parameter_value_metadata trigger_param2_values[] = {
    {
        .display_name = "Animation index",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
                .min = 0,
                .max = DT_PROP_LEN_OR(DT_CHOSEN(zmk_animation_control), behavior_animations, 1) - 1,
            },
    },
};

static const struct behavior_parameter_metadata_set trigger_metadata_set = {
    .param1_values = trigger_param1_values,
    .param1_values_len = ARRAY_SIZE(trigger_param1_values),
    .param2_values = trigger_param2_values,
    .param2_values_len = ARRAY_SIZE(trigger_param2_values),
};

static const struct behavior_parameter_metadata_set metadata_sets[] = {trigger_metadata_set};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(metadata_sets),
    .sets = metadata_sets,
};
#endif
#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

static const struct behavior_driver_api behavior_animation_trigger_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) && DT_HAS_COMPAT_STATUS_OKAY(zmk_animation_control)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_animation_trigger_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_animation_trigger_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
