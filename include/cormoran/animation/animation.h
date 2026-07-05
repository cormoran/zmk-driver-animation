/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Public animation vtable + frame-request API.
 *
 * Animations are Zephyr devices instantiated from devicetree. Every
 * animation implements `struct zmk_animation_api` and cooperates with the
 * engine's frame budget (see `zmk_animation_request_frames()` below) to
 * drive the shared tick timer only while something actually needs to
 * render.
 */

#include <zephyr/device.h>
#include <zephyr/types.h>

#include <cormoran/animation/color.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sentinel meaning "run until stopped", as opposed to a fixed duration. */
#define ZMK_ANIMATION_DURATION_FOREVER UINT32_MAX

/**
 * One pixel slot in the shared frame buffer. Animations write into
 * `value`; position is fixed at boot from the `zmk,animation` node's
 * `pixels` property and is read-only to animations.
 */
struct zmk_animation_pixel {
    const uint8_t position_x;
    const uint8_t position_y;

    struct zmk_color_rgb value;
};

/**
 * @brief Callback API for starting an animation.
 *
 * @param request_duration_ms Hint for how long the animation is expected to
 * play, in milliseconds, or `ZMK_ANIMATION_DURATION_FOREVER` to run until
 * `stop()` is called. Animations should honor this via
 * `zmk_animation_duration_to_frames()` rather than always running forever.
 */
typedef void (*zmk_animation_api_start_t)(const struct device *dev, uint32_t request_duration_ms);

/** @brief Callback API for stopping an animation. */
typedef void (*zmk_animation_api_stop_t)(const struct device *dev);

/**
 * @brief Callback API for rendering one frame.
 *
 * Called once per tick while the engine's frame budget is non-zero. The
 * buffer has already been reset to black for this tick; implementations
 * only need to write the pixels they own.
 */
typedef void (*zmk_animation_api_render_frame_t)(const struct device *dev,
                                                 struct zmk_animation_pixel *pixels,
                                                 size_t num_pixels);

/**
 * @brief Callback API to check whether a started animation has completed.
 *
 * Used by the engine only to log a warning if the frame budget expires
 * while the animation still reports itself unfinished (a sign the
 * animation forgot to request more frames).
 */
typedef bool (*zmk_animation_api_is_finished_t)(const struct device *dev);

struct zmk_animation_api {
    zmk_animation_api_start_t start;
    zmk_animation_api_stop_t stop;
    zmk_animation_api_render_frame_t render_frame;
    zmk_animation_api_is_finished_t is_finished;
};

static inline void zmk_animation_call_start(const struct device *dev,
                                            uint32_t request_duration_ms) {
    const struct zmk_animation_api *api = (const struct zmk_animation_api *)dev->api;

    api->start(dev, request_duration_ms);
}

static inline void zmk_animation_call_stop(const struct device *dev) {
    const struct zmk_animation_api *api = (const struct zmk_animation_api *)dev->api;

    api->stop(dev);
}

static inline void zmk_animation_call_render_frame(const struct device *dev,
                                                   struct zmk_animation_pixel *pixels,
                                                   size_t num_pixels) {
    const struct zmk_animation_api *api = (const struct zmk_animation_api *)dev->api;

    api->render_frame(dev, pixels, num_pixels);
}

static inline bool zmk_animation_call_is_finished(const struct device *dev) {
    const struct zmk_animation_api *api = (const struct zmk_animation_api *)dev->api;

    return api->is_finished(dev);
}

/**
 * @brief Convert a requested play duration into a frame count.
 *
 * Shared helper so every animation honors `request_duration_ms` the same
 * way instead of re-implementing the same ternary (v1 had this copy-pasted
 * 4 times and left it as a TODO in several animations).
 *
 * @param request_duration_ms Duration requested by the caller, in
 * milliseconds, or `ZMK_ANIMATION_DURATION_FOREVER`.
 * @param default_frames Frame count to use when `request_duration_ms` is 0
 * (i.e. "no preference specified").
 * @return `ZMK_ANIMATION_DURATION_FOREVER` if `request_duration_ms` is
 * `ZMK_ANIMATION_DURATION_FOREVER`; otherwise `default_frames` if
 * `request_duration_ms` is 0; otherwise `request_duration_ms` converted to
 * frames at `CONFIG_ZMK_ANIMATION_FPS`.
 */
static inline uint32_t zmk_animation_duration_to_frames(uint32_t request_duration_ms,
                                                        uint32_t default_frames) {
    if (request_duration_ms == ZMK_ANIMATION_DURATION_FOREVER) {
        return ZMK_ANIMATION_DURATION_FOREVER;
    }
    if (request_duration_ms == 0) {
        return default_frames;
    }
    return request_duration_ms * CONFIG_ZMK_ANIMATION_FPS / 1000;
}

/**
 * @brief Request that the engine render at least `frames` more ticks.
 *
 * Cooperative budget model: the engine keeps a single countdown and only
 * runs the shared tick timer while it is non-zero. Calling this with a
 * value not larger than the current remaining budget is a no-op; calling
 * it with a larger value extends the budget (and starts the timer if it
 * was stopped). Animations must call this (directly or via
 * `zmk_animation_request_frames_cap()`) every frame they still need to
 * render, or the engine will stop ticking them.
 */
void zmk_animation_request_frames(uint32_t frames);

/**
 * @brief Like `zmk_animation_request_frames()`, but caps the request to at
 * most one second worth of frames.
 *
 * Useful when `decremental_counter` is a large (or
 * `ZMK_ANIMATION_DURATION_FOREVER`-sized) remaining-frame counter: it avoids
 * requesting a huge frame budget up front that would keep ticking well past
 * the point the animation is cancelled.
 */
void zmk_animation_request_frames_cap(uint32_t decremental_counter);

#ifdef __cplusplus
}
#endif
