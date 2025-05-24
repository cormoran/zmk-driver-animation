/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>

void zmk_animation_request_frames(uint32_t frames);

/**
 * Request some frames depending on the given decremental counter value.
 * It internally requests small frames (e.g. at most 1sec) if the
 * decremental_counter value is large. It's useful to avoid requesting too big
 * frames if counter value is very large and animation will be cancelled in the
 * middle. This method should called in every animation frame to keep the
 * animation working.
 */
void zmk_animation_request_frames_cap(uint32_t decremental_counter);
