/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Private interface between control.c and settings/animation_settings.c
 * (DESIGN.md #3.5). Not part of the public API in include/cormoran/animation/.
 *
 * Only compiled/linked when CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS=y; every
 * call site in control.c must be guarded with
 * `#if IS_ENABLED(CONFIG_ZMK_ANIMATION_CUSTOM_SETTINGS)`.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reads all 5 registered settings (enabled, brightness_powered,
 * brightness_battery, animation_powered, animation_battery) from the
 * zmk-feature-custom-settings registry and applies them directly into the
 * animation-control singleton's in-memory state, refreshing the rendered
 * base animation if the effective selection/enabled state changed.
 *
 * Must only be called once the control singleton is ready (i.e. from or
 * after animation_control_init()'s delayed init-animation work item -
 * DESIGN.md #3.5 boot-ordering note); calling earlier is safe (control.c's
 * public setters no-op/log-warn if the device isn't ready yet) but pointless
 * since settings_load() (main(), after all SYS_INIT/device init levels) will
 * not have populated persisted values yet.
 *
 * This bypasses control.c's public zmk_animation_set_*()/select() write-
 * through-to-settings behavior (animation_settings_write_back_all()) by
 * design: re-entering that write-through here would immediately re-raise
 * zmk_custom_setting_changed for values we just read from that very
 * registry, which would in turn re-invoke the runtime change listener -
 * harmless in effect (idempotent) but wasteful and confusing in logs, so
 * boot-apply writes control state fields directly instead.
 */
void zmk_animation_settings_apply_boot(void);

/**
 * @brief Re-reads all 5 settings and applies them into control state, same
 * as zmk_animation_settings_apply_boot() minus the "is this actually boot
 * time" framing. Called by the zmk_custom_setting_changed listener whenever
 * any cormoran__animation setting changes (e.g. edited via the generic
 * custom-settings Studio RPC/web UI) so control state stays in sync.
 */
void zmk_animation_settings_apply_runtime(void);

/**
 * @brief Write-through helpers called from control.c's public mutation API
 * (zmk_animation_set_enabled()/_set_brightness()/_select(), and their shift
 * variants) so the custom-settings registry's in-memory value tracks
 * keymap/RPC-driven state changes live (DESIGN.md #3.5). Write mode is
 * always MEMORY - see animation_settings.c's top-of-file comment for why
 * PERSIST-on-every-mutation is not used.
 */
void zmk_animation_settings_write_enabled(bool enabled);
void zmk_animation_settings_write_brightness_powered(uint8_t step);
void zmk_animation_settings_write_brightness_battery(uint8_t step);
void zmk_animation_settings_write_animation_powered(uint8_t index);
void zmk_animation_settings_write_animation_battery(uint8_t index);

#ifdef __cplusplus
}
#endif
