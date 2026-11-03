/*==============================================================================
 @file osrc_pc_converter.c
 @brief OSResampler-based PCM processing library — non-island orchestration.

 Contains:
   - g_osrc_rs_heap_id global definition
   - osrc_pc_alloc_float_scratch() / osrc_pc_free_float_scratch()
   - osrc_pc_get_algo_delay_us()

 The init and deinit functions are in osrc_pc_init.c.
 The process and DM query functions are in osrc_pc_process.c.

================================================================================
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
All rights reserved.
Confidential and Proprietary - Qualcomm Technologies, Inc.
==============================================================================*/

#include "osrc_pc_converter.h"

/*------------------------------------------------------------------------------
   Global heap ID for OSResampler memory allocation
   Set by osrc_rs_set_heap_id() immediately before resampleFixedRatioInit().
   Safe under the single-threaded DSP execution model.
------------------------------------------------------------------------------*/
uint32_t g_osrc_rs_heap_id = 0;

/*==============================================================================
   osrc_pc_free_float_scratch
==============================================================================*/
void osrc_pc_free_float_scratch(osrc_pc_lib_t *pc_ptr)
{
   if (NULL == pc_ptr)
   {
      return;
   }

   if (pc_ptr->float_input_buf)
   {
      posal_memory_free(pc_ptr->float_input_buf);
      pc_ptr->float_input_buf = NULL;
   }

   if (pc_ptr->float_output_buf)
   {
      posal_memory_free(pc_ptr->float_output_buf);
      pc_ptr->float_output_buf = NULL;
   }

   if (pc_ptr->float_input_ptrs)
   {
      posal_memory_free(pc_ptr->float_input_ptrs);
      pc_ptr->float_input_ptrs = NULL;
   }

   if (pc_ptr->float_output_ptrs)
   {
      posal_memory_free(pc_ptr->float_output_ptrs);
      pc_ptr->float_output_ptrs = NULL;
   }

   pc_ptr->float_buf_samples  = 0;
   pc_ptr->float_buf_channels = 0;
}

/*==============================================================================
   osrc_pc_alloc_float_scratch
==============================================================================*/
ar_result_t osrc_pc_alloc_float_scratch(osrc_pc_lib_t *pc_ptr,
                                         uint32_t       num_channels,
                                         uint32_t       max_samples_per_ch,
                                         uint32_t       heap_id)
{
   if (NULL == pc_ptr || 0 == num_channels || 0 == max_samples_per_ch)
   {
      return AR_EBADPARAM;
   }

   /* Free any existing buffers first */
   osrc_pc_free_float_scratch(pc_ptr);

   uint32_t float_buf_bytes = num_channels * max_samples_per_ch * sizeof(float);
   uint32_t ptr_arr_bytes   = num_channels * sizeof(float *);

   /* Allocate contiguous float input buffer (all channels) */
   pc_ptr->float_input_buf =
      (float *)posal_memory_malloc(float_buf_bytes, (POSAL_HEAP_ID)heap_id);
   if (NULL == pc_ptr->float_input_buf)
   {
      AR_MSG(DBG_ERROR_PRIO,
             "OSRC_PC: Failed to alloc float_input_buf (%lu bytes)", float_buf_bytes);
      return AR_ENOMEMORY;
   }

   /* Allocate contiguous float output buffer (all channels) */
   pc_ptr->float_output_buf =
      (float *)posal_memory_malloc(float_buf_bytes, (POSAL_HEAP_ID)heap_id);
   if (NULL == pc_ptr->float_output_buf)
   {
      AR_MSG(DBG_ERROR_PRIO,
             "OSRC_PC: Failed to alloc float_output_buf (%lu bytes)", float_buf_bytes);
      posal_memory_free(pc_ptr->float_input_buf);
      pc_ptr->float_input_buf = NULL;
      return AR_ENOMEMORY;
   }

   /* Allocate per-channel input pointer array */
   pc_ptr->float_input_ptrs =
      (float **)posal_memory_malloc(ptr_arr_bytes, (POSAL_HEAP_ID)heap_id);
   if (NULL == pc_ptr->float_input_ptrs)
   {
      AR_MSG(DBG_ERROR_PRIO,
             "OSRC_PC: Failed to alloc float_input_ptrs (%lu bytes)", ptr_arr_bytes);
      posal_memory_free(pc_ptr->float_input_buf);
      posal_memory_free(pc_ptr->float_output_buf);
      pc_ptr->float_input_buf  = NULL;
      pc_ptr->float_output_buf = NULL;
      return AR_ENOMEMORY;
   }

   /* Allocate per-channel output pointer array */
   pc_ptr->float_output_ptrs =
      (float **)posal_memory_malloc(ptr_arr_bytes, (POSAL_HEAP_ID)heap_id);
   if (NULL == pc_ptr->float_output_ptrs)
   {
      AR_MSG(DBG_ERROR_PRIO,
             "OSRC_PC: Failed to alloc float_output_ptrs (%lu bytes)", ptr_arr_bytes);
      posal_memory_free(pc_ptr->float_input_buf);
      posal_memory_free(pc_ptr->float_output_buf);
      posal_memory_free(pc_ptr->float_input_ptrs);
      pc_ptr->float_input_buf  = NULL;
      pc_ptr->float_output_buf = NULL;
      pc_ptr->float_input_ptrs = NULL;
      return AR_ENOMEMORY;
   }

   /* Set up per-channel pointer arrays into the contiguous buffers */
   for (uint32_t ch = 0; ch < num_channels; ch++)
   {
      pc_ptr->float_input_ptrs[ch]  = pc_ptr->float_input_buf  + ch * max_samples_per_ch;
      pc_ptr->float_output_ptrs[ch] = pc_ptr->float_output_buf + ch * max_samples_per_ch;
   }

   pc_ptr->float_buf_samples  = max_samples_per_ch;
   pc_ptr->float_buf_channels = num_channels;

   AR_MSG(DBG_HIGH_PRIO,
          "OSRC_PC: Float scratch allocated: %lu ch x %lu samples (%lu bytes each buf)",
          num_channels, max_samples_per_ch, float_buf_bytes);

   return AR_EOK;
}

/*==============================================================================
   osrc_pc_get_algo_delay_us
==============================================================================*/
uint32_t osrc_pc_get_algo_delay_us(osrc_pc_lib_t *pc_ptr, uint32_t source_sr)
{
   if (NULL == pc_ptr || NULL == pc_ptr->rs_cxt || 0 == source_sr)
   {
      return 0;
   }

   if (!pc_ptr->base.core_lib.flags.RESAMPLER_PRE &&
       !pc_ptr->base.core_lib.flags.RESAMPLER_POST)
   {
      return 0;
   }

   /*
    * OSResampler algorithmic delay = numTaps / 2 input samples.
    * Delay in microseconds = (numTaps / 2) * 1,000,000 / source_sample_rate
    *
    * DEV_TODO: Validate this formula against actual measured delay on target.
    */
   uint32_t delay_samples = pc_ptr->num_taps / 2;
   uint32_t delay_us      = (uint32_t)(((uint64_t)delay_samples * 1000000ULL) / source_sr);

   return delay_us;
}