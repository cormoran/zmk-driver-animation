/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <drivers/behavior.h>
#include <dt-bindings/zmk_driver_animation/animation_control.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk_driver_animation/drivers/animation_control.h>

#define DT_DRV_COMPAT zmk_behavior_animation_control

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    // log binding
    LOG_INF("binding: %d %d", binding->param1, binding->param2);
    switch (binding->param1) {
        case ANIMATION_CONTROL_CMD_ENABLE:
            animation_control_set_enabled0(binding->param2);
            return 0;
        case ANIMATION_CONTROL_CMD_SHIFT:
            animation_control_set_next_animation0(
                binding->param2, ANIMATION_CONTROL_POWER_SOURCE_CURRENT);
            return 0;
        case ANIMATION_CONTROL_CMD_SELECT:
            animation_control_set_animation0(
                binding->param2, ANIMATION_CONTROL_POWER_SOURCE_CURRENT);
            return 0;
        case ANIMATION_CONTROL_CMD_BRIGHT:
            animation_control_change_brightness0(
                binding->param2, ANIMATION_CONTROL_POWER_SOURCE_CURRENT);
            return 0;
        default:
            LOG_ERR("Unknown command: %d", binding->param1);
    }
    return -ENOTSUP;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_animation_init(const struct device *dev) { return 0; }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata enable_param1_values[] = {
    {
        .display_name = "Enable/Disable animation",
        .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value        = ANIMATION_CONTROL_CMD_ENABLE,
    },
};

static const struct behavior_parameter_value_metadata enable_param2_values[] = {
    {
        .display_name = "Enable",
        .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value        = 1,
    },
    {
        .display_name = "Disable",
        .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value        = 0,
    },
};

static const struct behavior_parameter_metadata_set enable_metadata_set = {
    .param1_values     = enable_param1_values,
    .param1_values_len = ARRAY_SIZE(enable_param1_values),
    .param2_values     = enable_param2_values,
    .param2_values_len = ARRAY_SIZE(enable_param2_values),
};

static const struct behavior_parameter_value_metadata inc_dec_param1_values[] =
    {
        {
            .display_name = "Change background animation",
            .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
            .value        = ANIMATION_CONTROL_CMD_SHIFT,
        },
        {
            .display_name = "Change brightness",
            .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
            .value        = ANIMATION_CONTROL_CMD_BRIGHT,
        },
};

static const struct behavior_parameter_value_metadata inc_dec_param2_values[] =
    {
        {
            .display_name = "Increment",
            .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
            .value        = 1,
        },
        {
            .display_name = "Decrement",
            .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
            .value        = -1,
        },
};

static const struct behavior_parameter_metadata_set inc_dec_metadata_set = {
    .param1_values     = inc_dec_param1_values,
    .param1_values_len = ARRAY_SIZE(inc_dec_param1_values),
    .param2_values     = inc_dec_param2_values,
    .param2_values_len = ARRAY_SIZE(inc_dec_param2_values),
};

static const struct behavior_parameter_value_metadata select_param1_values[] = {
    {
        .display_name = "Select background animation",
        .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value        = ANIMATION_CONTROL_CMD_SELECT,
    },
};

static const struct behavior_parameter_value_metadata select_param2_values[] = {
    {
        .display_name = "Animation index",
        .type         = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
                .min = 0,
                .max = MAX(DT_PROP_LEN(DT_CHOSEN(zmk_animation_control),
                                       powered_animations) -
                               1,
                           DT_PROP_LEN(DT_CHOSEN(zmk_animation_control),
                                       battery_animations) -
                               1),
            },
    },
};

static const struct behavior_parameter_metadata_set select_metadata_set = {
    .param1_values     = select_param1_values,
    .param1_values_len = ARRAY_SIZE(select_param1_values),
    .param2_values     = select_param2_values,
    .param2_values_len = ARRAY_SIZE(select_param2_values),
};

static const struct behavior_parameter_metadata_set metadata_sets[] = {
    enable_metadata_set, inc_dec_metadata_set, select_metadata_set};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(metadata_sets),
    .sets     = metadata_sets,
};
#endif

static const struct behavior_driver_api behavior_animation_driver_api = {
    .binding_pressed  = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality         = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif  // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_animation_init, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_animation_driver_api);
