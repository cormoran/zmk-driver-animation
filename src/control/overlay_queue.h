/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief The single runtime animation-sequencing mechanism (DESIGN.md
 * #3.3): a small FIFO queue of ad-hoc overlay records, with two distinct
 * entry points.
 *
 * `zmk_overlay_enqueue()` appends to the queue; the record plays after the
 * current overlay (if any) finishes. `zmk_overlay_play_now()` preempts a
 * cancelable current overlay immediately. These are genuinely different
 * vtable-shaped operations in v1 (`animation_control_api.enqueue_animation`
 * vs `.play_now`), and v1's header had them accidentally dispatch to the
 * same underlying function (DESIGN.md #4 item 2) - see
 * tests/overlay_queue for a regression test of this exact class of bug.
 * This header has no vtable at all: the two entry points are ordinary
 * functions, so there is nothing for a copy-paste to mix up.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <cormoran/animation/animation.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One queued or currently-playing ad-hoc overlay. */
struct zmk_overlay_record {
    const struct device *animation;
    bool cancelable;
    uint32_t duration_ms;
};

/**
 * @brief Initializes the overlay queue with the given backing storage.
 * Called once from control.c's device init.
 */
void zmk_overlay_queue_init(struct k_msgq *msgq, char *buffer, size_t max_records);

/**
 * @brief Appends `record` to the queue. Played after the current overlay
 * (if any) finishes or is preempted by `zmk_overlay_play_now()`.
 *
 * @return 0 on success, -ENOMSG if the queue is full.
 */
int zmk_overlay_enqueue(struct k_msgq *msgq, const struct zmk_overlay_record *record);

/**
 * @brief Preempts the current overlay (if `cancelable`) and starts `record`
 * immediately. If a base animation is currently rendered (no overlay
 * active), it is preempted unconditionally - there is nothing cancelable
 * about "no overlay".
 *
 * Returns via `out_started` (may be NULL) whether the record actually
 * became the active overlay (false if the current overlay was not
 * cancelable, in which case `record` was appended to the queue instead,
 * matching v1's fallback behavior of not silently dropping the request).
 */
void zmk_overlay_play_now(struct k_msgq *msgq, const struct zmk_overlay_record *record);

/**
 * @brief Returns true if any record is queued (waiting to play).
 */
bool zmk_overlay_has_queued(struct k_msgq *msgq);

/**
 * @brief Returns the currently active/playing overlay record, or a
 * zeroed record (`.animation == NULL`) if none is active.
 */
struct zmk_overlay_record zmk_overlay_current(void);

/**
 * @brief True while an ad-hoc overlay (as opposed to the power-policy base
 * animation) is the thing being rendered.
 */
bool zmk_overlay_is_active(void);

/**
 * @brief Marks the given animation as no longer the active overlay if it
 * is currently active (used by `zmk_animation_trigger_stop()`); a no-op
 * otherwise.
 */
void zmk_overlay_stop_if_active(struct k_msgq *msgq, const struct device *animation);

/**
 * @brief Advances overlay dispatch for this tick. Two cases: an active
 * overlay finished/was canceled, so the next queued record (if any)
 * becomes active; or nothing is active but the queue is non-empty (a
 * record was enqueued while idle), so it is dequeued and started now.
 * Called once per tick from control.c's render step, after rendering
 * whatever is currently active.
 *
 * @param msgq The queue.
 * @param current_finished Whether the currently-playing overlay's
 * `is_finished()` reports true this tick (ignored if nothing is active).
 * @param force_cancel Whether an external event (behavior/RPC select
 * change) requested the current overlay be canceled even if not finished.
 */
void zmk_overlay_advance(struct k_msgq *msgq, bool current_finished, bool force_cancel);

/** Resets all overlay state (used when control stops/restarts). */
void zmk_overlay_reset(struct k_msgq *msgq);

#ifdef __cplusplus
}
#endif
