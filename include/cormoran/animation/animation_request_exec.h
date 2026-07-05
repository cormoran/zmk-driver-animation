/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @file
 * @brief Transport-independent execution of every `cormoran.animation` proto
 * request kind against `control.h`'s public API (DESIGN.md #3.6, the
 * pmw3610 handler/request_exec split lesson). `src/studio/animation_handler.c`
 * only decodes/encodes and owns the static response buffer; this file maps
 * a decoded request onto control.h and builds the response, with no
 * knowledge of the RPC transport - ready for a future split relay (DESIGN.md
 * #3.8) without needing to touch this file.
 */

#include <stdbool.h>

#include <cormoran/animation/animation.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Execute any supported request kind and fill `resp` with either the
 * successful response or an ErrorResponse.
 *
 * @return true if `req` was a supported kind (resp is always filled in that
 * case); false if `req` has no request_type set (resp untouched).
 */
bool zmk_animation_request_exec_handle(const cormoran_animation_Request *req,
                                       cormoran_animation_Response *resp);

#ifdef __cplusplus
}
#endif
