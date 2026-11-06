/*==============================================================================
 @file osrc_pc_process_island.c
 @brief OSResampler-based PCM processing library — island process stub.

 In the initial implementation, the OSResampler process path is NOT placed in
 island memory.  The convolution loop in resampleProcess() is too large for
 island, and the portability macros redirect malloc/free to posal_memory_malloc
 which is not island-safe.

 Island refactoring is deferred to a follow-up (see capi_osrc_mfc_design.md
 §8.3 "Island Classification of resampler.c — RESOLVED").

 Impact:
   The resampler convolution loop will exit island on each process() call.
   This is acceptable for the initial implementation and can be optimized in
   a follow-up by:
     1. Splitting resampler.c into init (non-island) and process (island) files.
     2. Replacing the portability macros with island-safe memory functions.
     3. Moving osrc_pc_resampler_process() to this file.

 This file is intentionally minimal.  It exists so that the SCons build script
 can use env.IslandFilter() on the source list without errors.

================================================================================
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
All rights reserved.
Confidential and Proprietary - Qualcomm Technologies, Inc.
==============================================================================*/

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