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
 * No island-eligible functions in the initial implementation.
 * All processing functions are in osrc_pc_process.c (non-island).
 */