/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/animation/animation.h>

#include "overlay_queue.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * `current` is read from the engine tick (system workqueue) and written
 * from behavior/RPC contexts (zmk_overlay_enqueue / play_now, indirectly via
 * control.c). A spinlock (rather than a mutex) is enough: every critical
 * section here is a short, non-blocking read-modify-write of `current` plus,
 * in play_now's preemption path, a couple of animation_stop()/start() calls.
 * Those animation_* calls happen to be made *outside* the lock (see below)
 * so the lock is never held across a call into unrelated animation code.
 */
static struct zmk_overlay_record current;
static bool overlay_active;
static struct k_spinlock lock;

void zmk_overlay_queue_init(struct k_msgq *msgq, char *buffer, size_t max_records) {
    k_msgq_init(msgq, buffer, sizeof(struct zmk_overlay_record), max_records);
    K_SPINLOCK(&lock) {
        current = (struct zmk_overlay_record){0};
        overlay_active = false;
    }
}

int zmk_overlay_enqueue(struct k_msgq *msgq, const struct zmk_overlay_record *record) {
    int rc = k_msgq_put(msgq, record, K_NO_WAIT);
    if (rc != 0) {
        LOG_ERR("overlay queue full, dropping enqueue for %s",
                record->animation ? record->animation->name : "(null)");
        return -ENOMSG;
    }
    LOG_INF("overlay queue: enqueued %s", record->animation ? record->animation->name : "(null)");
    /* Give the engine a chance to notice the queue is non-empty even if it
     * is currently idle (no base/overlay animation requesting frames). */
    zmk_animation_request_frames(1);
    return 0;
}

void zmk_overlay_play_now(struct k_msgq *msgq, const struct zmk_overlay_record *record) {
    bool preempt = false;
    struct zmk_overlay_record previous = {0};

    K_SPINLOCK(&lock) {
        if (!overlay_active || current.cancelable) {
            preempt = true;
            previous = current;
            current = *record;
            overlay_active = true;
        }
    }

    if (!preempt) {
        /* Current overlay isn't cancelable: fall back to enqueueing so the
         * request is not silently dropped (matches v1's queue fallback). */
        LOG_INF("overlay queue: play_now %s deferred (current overlay not cancelable)",
                record->animation ? record->animation->name : "(null)");
        zmk_overlay_enqueue(msgq, record);
        return;
    }

    LOG_INF("overlay queue: play_now %s (preempted %s)",
            record->animation ? record->animation->name : "(null)",
            previous.animation ? previous.animation->name : "(none)");

    if (previous.animation != NULL) {
        zmk_animation_call_stop(previous.animation);
    }
    if (record->animation != NULL) {
        zmk_animation_call_start(record->animation, record->duration_ms);
    }
    zmk_animation_request_frames(1);
}

bool zmk_overlay_has_queued(struct k_msgq *msgq) { return k_msgq_num_used_get(msgq) > 0; }

struct zmk_overlay_record zmk_overlay_current(void) {
    struct zmk_overlay_record snapshot;
    K_SPINLOCK(&lock) { snapshot = current; }
    return snapshot;
}

bool zmk_overlay_is_active(void) {
    bool active = false;
    K_SPINLOCK(&lock) { active = overlay_active; }
    return active;
}

void zmk_overlay_stop_if_active(struct k_msgq *msgq, const struct device *animation) {
    ARG_UNUSED(msgq);
    bool stop = false;

    K_SPINLOCK(&lock) {
        if (overlay_active && current.animation == animation) {
            stop = true;
        }
    }

    if (stop) {
        zmk_animation_call_stop(animation);
        /* Let the next tick's zmk_overlay_advance() notice is_finished()
         * and move on to the next queued record or the base animation. */
        zmk_animation_request_frames(1);
    }
}

void zmk_overlay_advance(struct k_msgq *msgq, bool current_finished, bool force_cancel) {
    bool should_advance = false;

    K_SPINLOCK(&lock) {
        /*
         * Two cases both mean "figure out what should be playing now and
         * dispatch it": an active overlay just finished/was canceled, OR
         * nothing is active but the queue is non-empty (e.g. a record was
         * enqueued while idle - zmk_overlay_enqueue() only appends to the
         * msgq and requests a frame; the actual dequeue happens here on
         * the next tick). Gating this solely on `overlay_active` (as an
         * earlier version of this function did) silently dropped the
         * second case: an enqueue while idle would sit in the msgq forever
         * because nothing was ever "active" to finish.
         */
        if ((overlay_active && (current_finished || (current.cancelable && force_cancel))) ||
            (!overlay_active && k_msgq_num_used_get(msgq) > 0)) {
            should_advance = true;
        }
    }

    if (!should_advance) {
        return;
    }

    struct zmk_overlay_record previous;
    struct zmk_overlay_record next = {0};
    bool got_next = false;

    K_SPINLOCK(&lock) { previous = current; }

    if (previous.animation != NULL) {
        zmk_animation_call_stop(previous.animation);
    }

    if (k_msgq_num_used_get(msgq) > 0) {
        if (k_msgq_get(msgq, &next, K_NO_WAIT) == 0) {
            got_next = true;
        }
    }

    K_SPINLOCK(&lock) {
        if (got_next) {
            current = next;
            overlay_active = true;
        } else {
            current = (struct zmk_overlay_record){0};
            overlay_active = false;
        }
    }

    if (got_next && next.animation != NULL) {
        zmk_animation_call_start(next.animation, next.duration_ms);
    }
    zmk_animation_request_frames(1);
}

void zmk_overlay_reset(struct k_msgq *msgq) {
    struct zmk_overlay_record drained;
    while (k_msgq_get(msgq, &drained, K_NO_WAIT) == 0) {
        /* drop */
    }
    K_SPINLOCK(&lock) {
        current = (struct zmk_overlay_record){0};
        overlay_active = false;
    }
}
