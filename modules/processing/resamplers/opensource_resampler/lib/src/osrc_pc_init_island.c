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

/*==============================================================================
   osrc_pc_get_fixed_out_samples
   (island-eligible — wraps resampleGetRequiredSamples())
==============================================================================*/
uint32_t osrc_pc_get_fixed_out_samples(osrc_pc_lib_t *pc_ptr, uint32_t req_out_samples)
{
   if (NULL == pc_ptr || NULL == pc_ptr->rs_cxt || 0 == req_out_samples)
   {
      return 0;
   }

   /*
    * resampleGetRequiredSamples() performs a "dry run" of the resampler to
    * determine how many input samples are needed to produce req_out_samples
    * output samples.
    *
    * IMPORTANT CAVEAT (from capi_osrc_mfc_design.md §4.5):
    *   For ratios just over 1.0, this function may return one fewer input
    *   sample than actually needed.  Always restrict the output with
    *   numOutputFrames in resampleProcess() to prevent over-generation.
    *
    * The ratio parameter is ignored for fixed-ratio resamplers (the
    * fixedRatio stored in the context is used instead).
    */
   return (uint32_t)resampleGetRequiredSamples(pc_ptr->rs_cxt,
                                               (int)req_out_samples,
                                               0.0 /* ignored for fixed-ratio */);
}

/*==============================================================================
   osrc_pc_get_expected_output
   (island-eligible — wraps resampleGetExpectedOutput())
==============================================================================*/
uint32_t osrc_pc_get_expected_output(osrc_pc_lib_t *pc_ptr, uint32_t req_in_samples)
{
   if (NULL == pc_ptr || NULL == pc_ptr->rs_cxt || 0 == req_in_samples)
   {
      return 0;
   }

   /*
    * resampleGetExpectedOutput() performs a "dry run" of the resampler to
    * determine how many output samples would be generated from req_in_samples
    * input samples.
    *
    * The ratio parameter is ignored for fixed-ratio resamplers.
    */
   return (uint32_t)resampleGetExpectedOutput(pc_ptr->rs_cxt,
                                              (int)req_in_samples,
                                              0.0 /* ignored for fixed-ratio */);
}