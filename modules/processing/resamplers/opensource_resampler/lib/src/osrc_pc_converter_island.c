/*==============================================================================
 @file osrc_pc_converter_island.c
 @brief OSResampler-based PCM processing library — island stub.

 In the initial implementation, the OSResampler process path is NOT placed in
 island memory.  resampleProcess() calls malloc/free internally (via the
 portability macros) which are not island-safe, and the convolution loop is
 too large to fit in island.

 Island refactoring is deferred to a follow-up (see capi_osrc_mfc_design.md
 §8.3 "Island Classification of resampler.c — RESOLVED").

 This file is intentionally minimal.  It exists so that the SCons build script
 can use env.IslandFilter() on the source list without errors — the island
 filter will place this file in the island library and the non-island sources
 in the non-island library.

 DEV_TODO: When island support is added, move osrc_pc_resampler_process() and
           osrc_pc_get_fixed_out_samples() / osrc_pc_get_expected_output() to
           this file and annotate with the appropriate island pragmas.

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
 * No island-eligible functions in the initial implementation.
 * All processing functions are in osrc_pc_process.c (non-island).
 */