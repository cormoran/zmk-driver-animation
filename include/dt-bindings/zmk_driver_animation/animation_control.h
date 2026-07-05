/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * Maps HSL color settings into a single uint32_t value that can be cast to
 * `struct zmk_color_hsl`.
 */
#ifdef CONFIG_BIG_ENDIAN
#define HSL(h, s, l) ((h << 16) + (s << 8) + l)
#else
#define HSL(h, s, l) (h + (s << 16) + (l << 24))
#endif

/**
 * Animation control commands (`animctl` behavior's param1).
 *
 * v1 defined `ANIMATION_CONTROL_CMD_BRIGHT` twice in this file (harmless
 * since both defines had the same value, but a copy-paste hazard - DESIGN.md
 * #4 item 5); defined once here.
 */
#define ANIMATION_CONTROL_CMD_ENABLE 0
#define ANIMATION_CONTROL_CMD_SHIFT 1
#define ANIMATION_CONTROL_CMD_SELECT 3
#define ANIMATION_CONTROL_CMD_BRIGHT 4

// clang-format off
#define ANM_EN ANIMATION_CONTROL_CMD_ENABLE 1
#define ANM_DS ANIMATION_CONTROL_CMD_ENABLE 0
#define ANM_INC ANIMATION_CONTROL_CMD_SHIFT 1
#define ANM_DEC ANIMATION_CONTROL_CMD_SHIFT (-1)
#define ANM_SEL ANIMATION_CONTROL_CMD_SELECT
#define ANM_BRI ANIMATION_CONTROL_CMD_BRIGHT 1
#define ANM_BRD ANIMATION_CONTROL_CMD_BRIGHT (-1)
// clang-format on
