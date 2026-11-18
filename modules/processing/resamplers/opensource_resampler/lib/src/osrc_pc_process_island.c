/*
 * \copyright
 *  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *  SPDX-License-Identifier: BSD-3-Clause
 */

#include "osrc_pc_converter.h"

/* Use island AR_MSG override if building for island */
#ifdef AR_MSG_IN_ISLAND
#include "ar_msg_island_override.h"
#endif

/*
 * No island-eligible process functions in the initial implementation.
 * osrc_pc_resampler_process() is in osrc_pc_process.c (non-island).
 *
 * DEV_TODO: When island support is added:
 *   1. Move osrc_pc_resampler_process() here.
 *   2. Move osrc_fixed_to_float_ch() and osrc_float_to_fixed_ch() here.
 *   3. Ensure resampleProcess() is island-safe (no malloc/free during process).
 *   4. Tag this file with the appropriate island pragma in the SCons script.
 */