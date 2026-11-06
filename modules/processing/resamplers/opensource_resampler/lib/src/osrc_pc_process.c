/*==============================================================================
 @file osrc_pc_process.c
 @brief OSResampler-based PCM processing library — resampler stage process.

 Implements osrc_pc_resampler_process(), the process function installed at
 RESAMPLER_PRE (stage 4) and RESAMPLER_POST (stage 6) in the pc_proc_info
 vtable.

 Data path:
   [SPF fixed-point input] -> [int-to-float] -> [resampleProcess()] ->
   [float-to-int] -> [SPF fixed-point output]

 Q-factor scaling:
   Q15  (16-bit): scale_in = 1/32768.0f,       scale_out = 32768.0f
   Q27  (32-bit): scale_in = 1/134217728.0f,   scale_out = 134217728.0f
   Q31  (32-bit): scale_in = 1/2147483648.0f,  scale_out = 2147483648.0f

 Float scratch buffers:
   Allocated lazily on the first call (or when size/channel count changes).
   Stored in osrc_pc_lib_t.float_input_buf / float_output_buf.

 EOS / flush handling:
   When the input has been exhausted (actual_data_len == 0), pass
   numInputFrames = -1 to resampleProcess() to flush the filter delay line
   (equivalent to feeding num_taps/2 zeros).  This is the standard OSResampler
   flush mechanism described in resampler.h.

 Buffer length contract (DEINTERLEAVED_UNPACKED_V2):
   The PC library uses DEINTERLEAVED_UNPACKED_V2 format throughout.
   Only input_buf_ptr[0].actual_data_len and output_buf_ptr[0].actual_data_len
   are updated; all other channel lengths are assumed equal by the framework.

 NOTE: This function is NOT island-safe in the initial implementation.
       Island refactoring is deferred (see capi_osrc_mfc_design.md §8.3).

================================================================================
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
All rights reserved.
Confidential and Proprietary - Qualcomm Technologies, Inc.
==============================================================================*/

#include "osrc_pc_converter.h"

/*------------------------------------------------------------------------------
   Internal helper: convert fixed-point deinterleaved buffer to float.
   Handles Q15 (16-bit words) and Q27/Q31 (32-bit words).
   in_buf->data_ptr points to the per-channel data for one channel.
------------------------------------------------------------------------------*/
static void osrc_fixed_to_float_ch(const capi_buf_t *in_buf,
                                    float            *out_float,
                                    uint32_t          num_samples,
                                    uint16_t          word_size,
                                    uint16_t          q_factor)
{
   float scale;

   /* float = fixed / (1 << q_factor) */
   if (15 == q_factor)
   {
      scale = 1.0f / 32768.0f;
   }
   else if (27 == q_factor)
   {
      scale = 1.0f / 134217728.0f;
   }
   else /* q_factor == 31 */
   {
      scale = 1.0f / 2147483648.0f;
   }

   if (16 == word_size)
   {
      const int16_t *src = (const int16_t *)in_buf->data_ptr;
      uint32_t       s;
      for (s = 0; s < num_samples; s++)
      {
         out_float[s] = (float)src[s] * scale;
      }
   }
   else /* 32-bit word */
   {
      const int32_t *src = (const int32_t *)in_buf->data_ptr;
      uint32_t       s;
      for (s = 0; s < num_samples; s++)
      {
         out_float[s] = (float)src[s] * scale;
      }
   }
}

/*------------------------------------------------------------------------------
   Internal helper: convert float buffer to fixed-point with saturation.
   Handles Q15 (16-bit words) and Q27/Q31 (32-bit words).
   out_buf->data_ptr points to the per-channel destination for one channel.
------------------------------------------------------------------------------*/
static void osrc_float_to_fixed_ch(const float *in_float,
                                    capi_buf_t  *out_buf,
                                    uint32_t     num_samples,
                                    uint16_t     word_size,
                                    uint16_t     q_factor)
{
   float scale;
   float max_val;
   float min_val;

   /* fixed = float * (1 << q_factor), saturated */
   if (15 == q_factor)
   {
      scale   = 32768.0f;
      max_val = 32767.0f;
      min_val = -32768.0f;
   }
   else if (27 == q_factor)
   {
      scale   = 134217728.0f;
      max_val = 134217727.0f;
      min_val = -134217728.0f;
   }
   else /* q_factor == 31 */
   {
      scale   = 2147483648.0f;
      max_val = 2147483647.0f;
      min_val = -2147483648.0f;
   }

   if (16 == word_size)
   {
      int16_t *dst = (int16_t *)out_buf->data_ptr;
      uint32_t s;
      for (s = 0; s < num_samples; s++)
      {
         float val = in_float[s] * scale;
         if (val > max_val) val = max_val;
         if (val < min_val) val = min_val;
         dst[s] = (int16_t)(int32_t)val;
      }
   }
   else /* 32-bit word */
   {
      int32_t *dst = (int32_t *)out_buf->data_ptr;
      uint32_t s;
      for (s = 0; s < num_samples; s++)
      {
         float val = in_float[s] * scale;
         if (val > max_val) val = max_val;
         if (val < min_val) val = min_val;
         dst[s] = (int32_t)val;
      }
   }
}

/*==============================================================================
   osrc_pc_resampler_process

   Called by pc_osrc_process() through the pc_proc_info vtable for stages
   RESAMPLER_PRE (4) and RESAMPLER_POST (6).

   me_ptr is cast from pc_osrc_lib_t* to osrc_pc_lib_t* — safe because
   osrc_pc_lib_t has pc_osrc_lib_t as its first member.
==============================================================================*/
ar_result_t osrc_pc_resampler_process(void           *me_ptr,
                                       capi_buf_t     *input_buf_ptr,
                                       capi_buf_t     *output_buf_ptr,
                                       pc_media_fmt_t *input_media_fmt_ptr,
                                       pc_media_fmt_t *output_media_fmt_ptr)
{
   osrc_pc_lib_t *pc_ptr = (osrc_pc_lib_t *)me_ptr;

   if (NULL == pc_ptr || NULL == input_buf_ptr || NULL == output_buf_ptr ||
       NULL == input_media_fmt_ptr || NULL == output_media_fmt_ptr)
   {
      AR_MSG(DBG_ERROR_PRIO, "OSRC_PC_PROC: NULL pointer in resampler_process");
      return AR_EBADPARAM;
   }

   if (NULL == pc_ptr->rs_cxt)
   {
      AR_MSG(DBG_ERROR_PRIO, "OSRC_PC_PROC: OSResampler context is NULL");
      return AR_EFAILED;
   }

   uint32_t num_channels         = input_media_fmt_ptr->num_channels;
   uint32_t bytes_per_sample_in  = (uint32_t)(input_media_fmt_ptr->word_size >> 3);
   uint32_t bytes_per_sample_out = (uint32_t)(output_media_fmt_ptr->word_size >> 3);

   /*
    * DEINTERLEAVED_UNPACKED_V2: only [0].actual_data_len is valid.
    * All channels have the same number of samples.
    */
   uint32_t num_input_samples  = input_buf_ptr[0].actual_data_len / bytes_per_sample_in;
   uint32_t max_output_samples = output_buf_ptr[0].max_data_len / bytes_per_sample_out;

   #ifdef OSRC_DEBUG
   AR_MSG(DBG_LOW_PRIO,
          "OSRC_PC_PROC: ENTRY in_actual=%lu in_max=%lu out_max=%lu"
          " in_samp=%lu out_max_samp=%lu ch=%lu bps_in=%lu bps_out=%lu",
          input_buf_ptr[0].actual_data_len,
          input_buf_ptr[0].max_data_len,
          output_buf_ptr[0].max_data_len,
          num_input_samples,
          max_output_samples,
          num_channels,
          bytes_per_sample_in,
          bytes_per_sample_out);
   AR_MSG(DBG_LOW_PRIO,
          "OSRC_PC_PROC: ENTRY in_sr=%lu out_sr=%lu",
          input_media_fmt_ptr->sampling_rate,
          output_media_fmt_ptr->sampling_rate);
   #endif

   if (0 == max_output_samples)
   {
      AR_MSG(DBG_ERROR_PRIO,
             "OSRC_PC_PROC: output buffer max_data_len is 0 — no room to write, in_actual=%lu",
             input_buf_ptr[0].actual_data_len);
      output_buf_ptr[0].actual_data_len = 0;
      return AR_EOK;
   }

   /* -----------------------------------------------------------------------
    * Lazy float scratch buffer allocation.
    *
    * Reallocate if:
    *   - Not yet allocated (float_buf_samples == 0)
    *   - Channel count increased
    *   - Buffer capacity is insufficient for the current frame
    *
    * DEV_TODO: Verify sizing covers DM mode edge cases where req_out_samples
    *           may exceed the nominal frame size.
    * ----------------------------------------------------------------------- */
   uint32_t max_samples = (num_input_samples > max_output_samples)
                             ? num_input_samples
                             : max_output_samples;

   /* Guard against zero max_samples (flush-only call) */
   if (0 == max_samples)
   {
      max_samples = (uint32_t)(pc_ptr->num_taps / 2) + 1;
      AR_MSG(DBG_LOW_PRIO,
             "OSRC_PC_PROC: zero max_samples (flush-only call), using taps/2+1=%lu",
             max_samples);
   }

   if (max_samples > pc_ptr->float_buf_samples ||
       num_channels > pc_ptr->float_buf_channels)
   {
      AR_MSG(DBG_LOW_PRIO,
             "OSRC_PC_PROC: scratch realloc — old_samp=%lu new_samp=%lu old_ch=%lu new_ch=%lu",
             pc_ptr->float_buf_samples, max_samples,
             pc_ptr->float_buf_channels, num_channels);

      ar_result_t alloc_result =
         osrc_pc_alloc_float_scratch(pc_ptr, num_channels, max_samples,
                                     pc_ptr->base.heap_id);
      if (AR_EOK != alloc_result)
      {
         AR_MSG(DBG_ERROR_PRIO,
                "OSRC_PC_PROC: Float scratch alloc failed (%lu ch x %lu samples) — DATA LOSS",
                num_channels, max_samples);
         output_buf_ptr[0].actual_data_len = 0;
         return alloc_result;
      }
      AR_MSG(DBG_LOW_PRIO,
             "OSRC_PC_PROC: scratch alloc ok — %lu ch x %lu samples",
             num_channels, max_samples);
   }

   /* -----------------------------------------------------------------------
    * Determine flush vs. normal mode.
    *
    * Flush mode (numInputFrames = -1):
    *   Triggered when num_input_samples == 0.  The OSResampler drains its
    *   internal delay line (num_taps/2 samples) and generates the remaining
    *   output.  The input pointer array is passed as NULL.
    *
    * Normal mode:
    *   Convert fixed-point input to float, then call resampleProcess().
    * ----------------------------------------------------------------------- */
   int          num_input_frames = (int)num_input_samples;
   const float *const *input_ptrs;

   if (0 == num_input_samples)
   {
      /*
       * EOS flush: numInputFrames = -1 tells the OSResampler to fill its
       * history with zeros and generate the remaining delayed output.
       * See resampler.h for the flush contract.
       */
      AR_MSG(DBG_LOW_PRIO,
             "OSRC_PC_PROC: FLUSH mode (num_input_samples==0) — draining delay line,"
             " num_taps=%lu, max_out_samp=%lu",
             pc_ptr->num_taps, max_output_samples);
      num_input_frames = -1;
      input_ptrs       = NULL;
   }
   else
   {
      /* Convert fixed-point input to float for each channel */
      uint32_t ch;
      for (ch = 0; ch < num_channels; ch++)
      {
         osrc_fixed_to_float_ch(&input_buf_ptr[ch],
                                 pc_ptr->float_input_ptrs[ch],
                                 num_input_samples,
                                 input_media_fmt_ptr->word_size,
                                 input_media_fmt_ptr->q_factor);
      }
      input_ptrs = (const float *const *)pc_ptr->float_input_ptrs;
   }

   AR_MSG(DBG_LOW_PRIO,
          "OSRC_PC_PROC: calling resampleProcess num_input_frames=%d max_out_samp=%lu",
          num_input_frames, max_output_samples);

   /* -----------------------------------------------------------------------
    * Call OSResampler.
    *
    * For fixed-ratio resamplers (created with resampleFixedRatioInit()),
    * the ratio parameter is ignored — the fixedRatio stored in the context
    * is used instead.
    *
    * resampleProcess() returns:
    *   result.input_used        — actual input samples consumed per channel
    *   result.output_generated  — actual output samples produced per channel
    * ----------------------------------------------------------------------- */
   ResampleResult rs_result = resampleProcess(
      pc_ptr->rs_cxt,
      input_ptrs,
      num_input_frames,
      (float *const *)pc_ptr->float_output_ptrs,
      (int)max_output_samples,
      0.0 /* ratio: ignored for fixed-ratio */);

   #ifdef OSRC_DEBUG
   AR_MSG(DBG_LOW_PRIO,
          "OSRC_PC_PROC: resampleProcess done input_used=%d output_generated=%d"
          " (in_samp_avail=%lu max_out_samp=%lu)",
          rs_result.input_used,
          rs_result.output_generated,
          num_input_samples,
          max_output_samples);
   #endif

   if (rs_result.input_used == 0 && num_input_frames > 0)
   {
      AR_MSG(DBG_LOW_PRIO,
             "OSRC_PC_PROC: WARNING resampleProcess consumed 0 of %lu input samples"
             " — possible stall or startup delay-line fill",
             num_input_samples);
   }

   if (rs_result.output_generated == 0 && num_input_frames != 0)
   {
      AR_MSG(DBG_LOW_PRIO,
             "OSRC_PC_PROC: WARNING resampleProcess produced 0 output samples"
             " from %d input frames — startup latency or filter fill in progress",
             num_input_frames);
   }

   /* -----------------------------------------------------------------------
    * Convert float output to fixed-point for each channel.
    * ----------------------------------------------------------------------- */
   if (rs_result.output_generated > 0)
   {
      uint32_t ch;
      for (ch = 0; ch < num_channels; ch++)
      {
         osrc_float_to_fixed_ch(pc_ptr->float_output_ptrs[ch],
                                 &output_buf_ptr[ch],
                                 rs_result.output_generated,
                                 output_media_fmt_ptr->word_size,
                                 output_media_fmt_ptr->q_factor);
      }
   }

   /* -----------------------------------------------------------------------
    * Update buffer lengths.
    *
    * DEINTERLEAVED_UNPACKED_V2: only update [0].actual_data_len.
    * The framework assumes all channels have the same length.
    * ----------------------------------------------------------------------- */
   output_buf_ptr[0].actual_data_len =
      rs_result.output_generated * bytes_per_sample_out;

   /*
    * Update consumed input length.
    * In flush mode (num_input_frames == -1), input_used is 0 — the input
    * buffer was already empty, so no update needed.
    */
   if (num_input_frames >= 0)
   {
      input_buf_ptr[0].actual_data_len =
         rs_result.input_used * bytes_per_sample_in;
   }
   else
   {
      /* Flush mode: input was already 0 */
      input_buf_ptr[0].actual_data_len = 0;
   }

   #ifdef OSRC_DEBUG
   AR_MSG(DBG_LOW_PRIO,
          "OSRC_PC_PROC: EXIT in_consumed=%lu out_produced=%lu"
          " (in_actual_after=%lu out_actual=%lu)",
          (num_input_frames >= 0)
             ? (uint32_t)(rs_result.input_used * bytes_per_sample_in)
             : 0u,
          output_buf_ptr[0].actual_data_len,
          input_buf_ptr[0].actual_data_len,
          output_buf_ptr[0].actual_data_len);
   #endif

   return AR_EOK;
}