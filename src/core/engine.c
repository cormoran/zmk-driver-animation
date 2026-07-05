/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include <cormoran/animation/animation.h>

#include "engine.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Exactly one `zmk,animation` instance is supported. v1 silently hardcoded
 * DT instance 0 with no guard (DESIGN.md #4 item 11); a stray second
 * instance now fails the build instead of being silently ignored.
 */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "Only one enabled `zmk,animation` instance is supported");

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define PHANDLE_TO_DEVICE(node_id, prop, idx) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define PHANDLE_TO_PIXEL(node_id, prop, idx)                                                       \
    {                                                                                              \
        .position_x = DT_PHA_BY_IDX(node_id, prop, idx, position_x),                               \
        .position_y = DT_PHA_BY_IDX(node_id, prop, idx, position_y),                               \
    },

/** LED strip driver device pointers, one per `drivers` phandle. */
static const struct device *drivers[] = {DT_INST_FOREACH_PROP_ELEM(0, drivers, PHANDLE_TO_DEVICE)};

/** Number of entries in `drivers`. */
static const size_t drivers_size = DT_INST_PROP_LEN(0, drivers);

/** Number of LEDs handled by each entry in `drivers`. */
static const size_t pixels_per_driver[] = DT_INST_PROP(0, chain_lengths);

/** Shared pixel buffer, one entry per `pixels` phandle. */
static struct zmk_animation_pixel pixels[] = {
    DT_INST_FOREACH_PROP_ELEM(0, pixels, PHANDLE_TO_PIXEL)};

/** Number of entries in `pixels`. */
static const size_t pixels_size = DT_INST_PROP_LEN(0, pixels);

/** Output buffer, converted to `led_rgb` and handed to the drivers. */
static struct led_rgb px_buffer[DT_INST_PROP_LEN(0, pixels)];

/*
 * The `zmk,animation` chosen node. Per DESIGN.md #4 item 10, from Phase B
 * onward this must resolve to `animation-control`, not an arbitrary
 * animation node; the engine only knows how to render *some* animation
 * device. Phase A has no control layer yet, so the chosen node is rendered
 * directly.
 */
static const struct device *root_animation = DEVICE_DT_GET(DT_CHOSEN(zmk_animation));

/*
 * Frames requested but not yet rendered.
 *
 * Locking invariant: `frame_budget` is read/written from both the timer
 * ISR (`engine_tick_timer_handler`, invoked directly from the system clock
 * interrupt) and thread context (`zmk_animation_request_frames*()`, called
 * from `render_frame` on the system workqueue, and `engine_stop()`, called
 * from the activity listener). Every access to `frame_budget` itself must
 * happen while holding `frame_budget_lock`.
 *
 * `k_timer_start()`/`k_timer_stop()` are *not* called while holding this
 * lock (they take their own internal kernel spinlock and may reschedule on
 * exit; nesting that under ours is unnecessary risk for no benefit here).
 * Instead, the transition decision (did the budget go from zero to
 * non-zero -> start timer; did it just hit zero -> stop timer) is made
 * atomically *inside* the critical section by comparing old vs. new value,
 * and the corresponding k_timer_* call is issued right after releasing the
 * lock. This still closes the TOCTOU: the "is the budget zero" check and
 * the "set the new budget" write can no longer be interleaved with the
 * ISR's decrement-to-zero, because both sides serialize on the same lock
 * for the read-modify-write itself. A timer start/stop decided this way can
 * race another start/stop *call* (e.g. ISR decides "stop" the same instant
 * the thread decides "start" for the next request), but k_timer_start()/
 * k_timer_stop() are idempotent and safe to call in either order or
 * concurrently (they have their own lock), so the worst case is one extra
 * start-then-stop or stop-then-start pair, never a stopped timer with a
 * nonzero frame_budget or a running timer nobody accounts for.
 */
static uint32_t frame_budget;
static struct k_spinlock frame_budget_lock;

static void engine_tick(struct k_work *work) {
    zmk_animation_render(root_animation, pixels, pixels_size);

    for (size_t i = 0; i < pixels_size; ++i) {
        zmk_rgb_to_led_rgb(&pixels[i].value, &px_buffer[i]);
    }

    size_t pixels_sent = 0;
    for (size_t i = 0; i < drivers_size; ++i) {
        led_strip_update_rgb(drivers[i], &px_buffer[pixels_sent], pixels_per_driver[i]);
        pixels_sent += pixels_per_driver[i];
    }
}

K_WORK_DEFINE(engine_work, engine_tick);

static void engine_tick_timer_handler(struct k_timer *timer) {
    bool budget_expired = false;

    K_SPINLOCK(&frame_budget_lock) {
        if (frame_budget > 0 && --frame_budget == 0) {
            budget_expired = true;
        }
    }

    if (budget_expired) {
        /*
         * Decided outside the lock (see the locking-invariant comment on
         * `frame_budget`): a concurrent zmk_animation_request_frames() may
         * race this k_timer_stop() with its own k_timer_start(), but
         * k_timer_start()/k_timer_stop() are safe to call concurrently and
         * the frame_budget value itself is never inconsistent with whether
         * *this* call observed expiry, since that decision was made
         * atomically above.
         */
        k_timer_stop(timer);

        if (!zmk_animation_call_is_finished(root_animation)) {
            /*
             * v1 silently stalled mid-animation here: the timer stopped and
             * nothing rendered again until some unrelated event happened to
             * call zmk_animation_request_frames(). Log so a stuck animation
             * (one that doesn't call zmk_animation_request_frames_cap()
             * every frame it still needs) is visible instead of silent.
             */
            LOG_WRN("Animation frame budget expired while animation reports unfinished");
        }
    }

    k_work_submit(&engine_work);
}

K_TIMER_DEFINE(engine_tick_timer, engine_tick_timer_handler, NULL);

void zmk_animation_request_frames(uint32_t frames) {
    bool need_start = false;

    K_SPINLOCK(&frame_budget_lock) {
        if (frames <= frame_budget) {
            K_SPINLOCK_BREAK;
        }

        /*
         * Reading `frame_budget == 0` and writing the new value happen in
         * the same critical section, so this can no longer race the ISR's
         * decrement-to-zero the way the un-synchronized version did: either
         * the ISR's decrement (and its own budget_expired transition)
         * happens-before this section (and we correctly see 0 and restart
         * the timer), or it happens-after (and it will observe the
         * `frames` value written here, not stop a timer we just started
         * for a still-nonzero budget).
         */
        need_start = frame_budget == 0;
        frame_budget = frames;
    }

    if (need_start) {
        k_timer_start(&engine_tick_timer, K_MSEC(1000 / CONFIG_ZMK_ANIMATION_FPS),
                      K_MSEC(1000 / CONFIG_ZMK_ANIMATION_FPS));
    }
}

void zmk_animation_request_frames_cap(uint32_t decremental_counter) {
    zmk_animation_request_frames(decremental_counter > CONFIG_ZMK_ANIMATION_FPS
                                     ? CONFIG_ZMK_ANIMATION_FPS
                                     : decremental_counter);
}

static void engine_stop(void) {
    /*
     * Order matters here: clear the budget before stopping the timer. If a
     * racing zmk_animation_request_frames() lands between the two, the
     * worst case is it observes frame_budget == 0 (already cleared), sets a
     * new nonzero budget and calls k_timer_start() right after our
     * k_timer_stop() -- i.e. the animation keeps ticking, which is no worse
     * than the pre-existing (non-locking) behavior for this ordering.
     * Doing it in the other order (stop-then-clear) would let the ISR fire
     * once more between the two and decrement a budget that's about to be
     * zeroed anyway, which is harmless, but clearing first makes the
     * invariant "frame_budget != 0 implies the timer should be running"
     * hold at every observable point from other threads.
     */
    K_SPINLOCK(&frame_budget_lock) { frame_budget = 0; }
    k_timer_stop(&engine_tick_timer);
    zmk_animation_call_stop(root_animation);
}

static int engine_on_activity_state_changed(const zmk_event_t *event) {
    const struct zmk_activity_state_changed *activity_state_event =
        as_zmk_activity_state_changed(event);

    if (activity_state_event == NULL) {
        return -ENOTSUP;
    }

    switch (activity_state_event->state) {
    case ZMK_ACTIVITY_ACTIVE:
        zmk_animation_call_start(root_animation, ZMK_ANIMATION_DURATION_FOREVER);
        return 0;
    case ZMK_ACTIVITY_IDLE:
        if (IS_ENABLED(CONFIG_ZMK_ANIMATION_STOP_ON_IDLE)) {
            engine_stop();
        }
        return 0;
    case ZMK_ACTIVITY_SLEEP:
        engine_stop();
        return 0;
    default:
        return 0;
    }
}

ZMK_LISTENER(zmk_animation_engine, engine_on_activity_state_changed);
ZMK_SUBSCRIPTION(zmk_animation_engine, zmk_activity_state_changed);

static int engine_init(void) {
    LOG_INF("ZMK animation engine ready (%zu pixels, %zu drivers)", pixels_size, drivers_size);
    zmk_animation_call_start(root_animation, ZMK_ANIMATION_DURATION_FOREVER);

    return 0;
}

SYS_INIT(engine_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
