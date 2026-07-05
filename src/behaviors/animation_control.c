/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_animation_control

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/animation/control.h>
#include <dt-bindings/zmk_driver_animation/animation_control.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * `zmk,behavior-animation-control` (`animctl`): thin adapter over
 * control.h, per DESIGN.md #3.7. No local state; every command maps
 * directly onto one control.h call.
 *
 * Guarded on DT_HAS_COMPAT_STATUS_OKAY like the animation devices
 * (DESIGN.md #4 item 11 pattern): BEHAVIOR_DT_INST_DEFINE(0, ...) with no
 * matching DT node is not simply a no-op, it fails the build (a bogus
 * generated device name) - so boards that don't reference `&animctl`
 * anywhere still need this behavior compiled conditionally.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    switch (binding->param1) {
    case ANIMATION_CONTROL_CMD_ENABLE:
        zmk_animation_set_enabled(binding->param2);
        return 0;
    case ANIMATION_CONTROL_CMD_SHIFT:
        zmk_animation_select_shift((int)binding->param2, ZMK_ANIMATION_POWER_SOURCE_CURRENT);
        return 0;
    case ANIMATION_CONTROL_CMD_SELECT:
        zmk_animation_select(binding->param2, ZMK_ANIMATION_POWER_SOURCE_CURRENT);
        return 0;
    case ANIMATION_CONTROL_CMD_BRIGHT:
        zmk_animation_set_brightness_shift((int)binding->param2, ZMK_ANIMATION_POWER_SOURCE_CURRENT);
        return 0;
    default:
        LOG_ERR("Unknown animctl command: %d", binding->param1);
        return -ENOTSUP;
    }
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_animation_control_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata enable_param1_values[] = {
    {
        .display_name = "Enable/Disable animation",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = ANIMATION_CONTROL_CMD_ENABLE,
    },
};

static const struct behavior_parameter_value_metadata enable_param2_values[] = {
    {.display_name = "Enable", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = 1},
    {.display_name = "Disable", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = 0},
};

static const struct behavior_parameter_metadata_set enable_metadata_set = {
    .param1_values = enable_param1_values,
    .param1_values_len = ARRAY_SIZE(enable_param1_values),
    .param2_values = enable_param2_values,
    .param2_values_len = ARRAY_SIZE(enable_param2_values),
};

static const struct behavior_parameter_value_metadata inc_dec_param1_values[] = {
    {
        .display_name = "Change background animation",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = ANIMATION_CONTROL_CMD_SHIFT,
    },
    {
        .display_name = "Change brightness",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = ANIMATION_CONTROL_CMD_BRIGHT,
    },
};

static const struct behavior_parameter_value_metadata inc_dec_param2_values[] = {
    {.display_name = "Increment", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = 1},
    {.display_name = "Decrement", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = -1},
};

static const struct behavior_parameter_metadata_set inc_dec_metadata_set = {
    .param1_values = inc_dec_param1_values,
    .param1_values_len = ARRAY_SIZE(inc_dec_param1_values),
    .param2_values = inc_dec_param2_values,
    .param2_values_len = ARRAY_SIZE(inc_dec_param2_values),
};

static const struct behavior_parameter_value_metadata select_param1_values[] = {
    {
        .display_name = "Select background animation",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = ANIMATION_CONTROL_CMD_SELECT,
    },
};

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_animation_control)
static const struct behavior_parameter_value_metadata select_param2_values[] = {
    {
        .display_name = "Animation index",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
                .min = 0,
                .max = MAX(DT_PROP_LEN_OR(DT_CHOSEN(zmk_animation_control), powered_animations, 1) - 1,
                          DT_PROP_LEN_OR(DT_CHOSEN(zmk_animation_control), battery_animations, 1) - 1),
            },
    },
};

static const struct behavior_parameter_metadata_set select_metadata_set = {
    .param1_values = select_param1_values,
    .param1_values_len = ARRAY_SIZE(select_param1_values),
    .param2_values = select_param2_values,
    .param2_values_len = ARRAY_SIZE(select_param2_values),
};

static const struct behavior_parameter_metadata_set metadata_sets[] = {
    enable_metadata_set, inc_dec_metadata_set, select_metadata_set};
#else
static const struct behavior_parameter_metadata_set metadata_sets[] = {enable_metadata_set,
                                                                        inc_dec_metadata_set};
#endif

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(metadata_sets),
    .sets = metadata_sets,
};
#endif /* IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA) */

static const struct behavior_driver_api behavior_animation_control_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_animation_control_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_animation_control_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
