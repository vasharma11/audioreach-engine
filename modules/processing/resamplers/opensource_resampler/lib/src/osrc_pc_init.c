/*==============================================================================
 @file osrc_pc_init.c
 @brief OSResampler-based PCM processing library — initialization and teardown.

 Implements:
   osrc_pc_init()   — Initialize the 11-stage pipeline with OSResampler backend.
   osrc_pc_deinit() — Free all resources.

 Design:
   osrc_pc_init() calls pc_osrc_init() to set up the full pipeline, then:
     1. Frees osrc_rs_lib_ptr allocated by pc_osrc_init().
     2. Replaces RESAMPLER_PRE / RESAMPLER_POST stage function pointers with
        osrc_pc_resampler_process().
     3. Calls resampleFixedRatioInit() to create the OSResampler context.

   This approach reuses all non-resampler stage logic (endianness, byte
   conversion, interleaving, channel mixing, float conversion) from
   pcm_mf_cnv_lib without modification.

================================================================================
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
All rights reserved.
Confidential and Proprietary - Qualcomm Technologies, Inc.
==============================================================================*/

#include "osrc_pc_converter.h"


#define OSRC_PC_MSG_PREFIX "OSRC_PC:[%lX] "
#define OSRC_PC_MSG(miid, ss, fmt, ...) \
   AR_MSG(ss, OSRC_PC_MSG_PREFIX fmt, (miid), ##__VA_ARGS__)

/*==============================================================================
   osrc_pc_init
==============================================================================*/
ar_result_t osrc_pc_init(osrc_pc_lib_t  *pc_ptr,
                          pc_media_fmt_t *input_media_fmt_ptr,
                          pc_media_fmt_t *output_media_fmt_ptr,
                          void           *coef_set_ptr,
                          uint32_t        heap_id,
                          bool_t         *lib_enable,
                          uint32_t        miid,
                          uint8_t         type)
{
   ar_result_t result = AR_EOK;

   if (NULL == pc_ptr || NULL == input_media_fmt_ptr || NULL == output_media_fmt_ptr ||
       NULL == lib_enable)
   {
      AR_MSG(DBG_ERROR_PRIO, "OSRC_PC: osrc_pc_init received NULL pointer");
      return AR_EBADPARAM;
   }

   /* Apply default quality settings if not already configured by the caller */
   if (0 == pc_ptr->num_taps)
   {
      pc_ptr->num_taps    = OSRC_RS_DEFAULT_NUM_TAPS;
      pc_ptr->max_filters = OSRC_RS_DEFAULT_MAX_FILTERS;
      pc_ptr->rs_flags    = OSRC_RS_DEFAULT_FLAGS;
   }

   /* -----------------------------------------------------------------------
    * Step 1: Call pc_osrc_init() to set up the full 11-stage pipeline.
    *
    * pc_osrc_init() will:
    *   - Identify required pipeline stages based on input/output media formats
    *   - Allocate channel mixer memory
    *   - Allocate osrc_rs_lib_ptr (OSRC resampler) if resampling is needed
    *   - Fill the pc_proc_info vtable with process function pointers
    *   - Initialize the channel mixer
    *
    * We will free osrc_rs_lib_ptr immediately after and replace it with rs_cxt.
    * ----------------------------------------------------------------------- */
   pc_ptr->base.heap_id = heap_id;

   result = pc_osrc_init((pc_osrc_lib_t *)pc_ptr,
                    input_media_fmt_ptr,
                    output_media_fmt_ptr,
                    coef_set_ptr,
                    heap_id,
                    lib_enable,
                    miid,
                    type);

   if (AR_EOK != result)
   {
      OSRC_PC_MSG(miid, DBG_ERROR_PRIO,
                  "pc_osrc_init() failed with result 0x%lx", result);
      return result;
   }

   if (FALSE == *lib_enable)
   {
      /* Formats are identical — passthrough mode, no resampler needed */
      OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
                  "lib_enable=FALSE (passthrough), no OSResampler needed");
      return AR_EOK;
   }

   /* -----------------------------------------------------------------------
    * Step 2: Free the OSRC resampler allocated by pc_osrc_init() and replace
    * with the OSResampler context (rs_cxt).
    * pc_osrc_deinit() NULL-checks osrc_rs_lib_ptr, so this is defensive cleanup.
    * ----------------------------------------------------------------------- */
   if (pc_ptr->base.osrc_rs_lib_ptr)
   {
      osrc_rs_lib_deinit(pc_ptr->base.osrc_rs_lib_ptr);
      posal_memory_free(pc_ptr->base.osrc_rs_lib_ptr);
      pc_ptr->base.osrc_rs_lib_ptr = NULL;
      OSRC_PC_MSG(miid, DBG_HIGH_PRIO, "Freed osrc_rs_lib_ptr (replaced by rs_cxt)");
   }

   /* -----------------------------------------------------------------------
    * Step 3: Replace resampler stage function pointers with
    *         osrc_pc_resampler_process().
    *
    * pc_osrc_init() set these to pc_osrc_resampler_process().
    * We override them with osrc_pc_resampler_process().
    * All other stage function pointers remain unchanged.
    * ----------------------------------------------------------------------- */
   bool_t resampler_active = FALSE;

   if (pc_ptr->base.core_lib.flags.RESAMPLER_PRE)
   {
      pc_ptr->base.core_lib.pc_proc_info[RESAMPLER_PRE].process =
         osrc_pc_resampler_process;
      resampler_active = TRUE;
      OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
                  "Installed osrc_pc_resampler_process at RESAMPLER_PRE stage");
   }

   if (pc_ptr->base.core_lib.flags.RESAMPLER_POST)
   {
      pc_ptr->base.core_lib.pc_proc_info[RESAMPLER_POST].process =
         osrc_pc_resampler_process;
      resampler_active = TRUE;
      OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
                  "Installed osrc_pc_resampler_process at RESAMPLER_POST stage");
   }

   if (!resampler_active)
   {
      /* No resampling needed — lib is enabled for other conversions only */
      OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
                  "No resampler stage active, OSResampler not needed");
      return AR_EOK;
   }

   /* -----------------------------------------------------------------------
    * Step 4: Initialize the OSResampler.
    *
    * Determine:
    *   - Number of channels at the resampler stage
    *   - Source and destination sample rates
    *   - Flags (add INCLUDE_LOWPASS for downsampling)
    * ----------------------------------------------------------------------- */

   /* Free existing OSResampler context if present (re-init case) */
   if (pc_ptr->rs_cxt)
   {
      resampleFree(pc_ptr->rs_cxt);
      pc_ptr->rs_cxt = NULL;
   }

   /* Free existing float scratch buffers (will be reallocated lazily) */
   osrc_pc_free_float_scratch(pc_ptr);

   uint32_t in_sr  = input_media_fmt_ptr->sampling_rate;
   uint32_t out_sr = output_media_fmt_ptr->sampling_rate;

   /*
    * Number of channels at the resampler stage:
    *   RESAMPLER_PRE  (stage 4): resamples BEFORE channel mixer
    *                             -> uses input channel count
    *   RESAMPLER_POST (stage 6): resamples AFTER channel mixer
    *                             -> uses output channel count
    *
    * Only one of RESAMPLER_PRE / RESAMPLER_POST can be active at a time.
    */
   uint32_t rs_num_channels;
   if (pc_ptr->base.core_lib.flags.RESAMPLER_PRE)
   {
      rs_num_channels = input_media_fmt_ptr->num_channels;
   }
   else /* RESAMPLER_POST */
   {
      rs_num_channels = output_media_fmt_ptr->num_channels;
   }

   /* Add INCLUDE_LOWPASS automatically for downsampling to prevent aliasing */
   int rs_flags = pc_ptr->rs_flags;
   if (out_sr < in_sr)
   {
      rs_flags |= INCLUDE_LOWPASS;
      OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
                  "Downsampling %lu->%lu Hz: auto-enabling INCLUDE_LOWPASS",
                  in_sr, out_sr);
   }

   #ifdef OSRC_DEBUG
   OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
               "OSResampler init: %lu ch, %lu taps, %lu max_filters, "
               "%lu Hz -> %lu Hz, flags=0x%x",
               rs_num_channels, pc_ptr->num_taps, pc_ptr->max_filters,
               in_sr, out_sr, rs_flags);
   #endif

   /* Set heap ID for OSResampler memory allocation */
   osrc_rs_set_heap_id(heap_id);

   /*
    * resampleFixedRatioInit() parameters:
    *   numChannels  = rs_num_channels
    *   numTaps      = pc_ptr->num_taps
    *   maxFilters   = pc_ptr->max_filters (may be reduced for integer ratios)
    *   sourceRate   = in_sr
    *   destinRate   = out_sr
    *   lowpassFreq  = 0 (auto-calculate when INCLUDE_LOWPASS is set)
    *   flags        = rs_flags
    *
    * For integer-ratio conversions (e.g., 44100->48000 = 160:147),
    * resampleFixedRatioInit() automatically reduces the filter count to the
    * exact number needed, eliminating interpolation and doubling performance.
    */
   pc_ptr->rs_cxt = resampleFixedRatioInit(
      (int)rs_num_channels,
      (int)pc_ptr->num_taps,
      (int)pc_ptr->max_filters,
      (double)in_sr,
      (double)out_sr,
      0,        /* lowpassFreq: 0 = auto-calculate */
      rs_flags);

   if (NULL == pc_ptr->rs_cxt)
   {
      OSRC_PC_MSG(miid, DBG_ERROR_PRIO,
                  "resampleFixedRatioInit() failed for %lu->%lu Hz, %lu ch",
                  in_sr, out_sr, rs_num_channels);
      *lib_enable = FALSE;
      pc_osrc_deinit((pc_osrc_lib_t *)pc_ptr); /* frees pc_mem_ptr and NULL osrc_rs_lib_ptr */
      return AR_EFAILED;
   }

   /* Pre-prime the OSResampler delay-line with numTaps/2 zero-valued input samples.
    *
    * hwsw_rs_lib achieves zero startup latency by zero-initialising its circular
    * delay-line and starting convolution from index 0 immediately. resampleProcess()
    * is a black-box API that does not expose the delay-line directly, so we achieve
    * the same effect by feeding (numTaps/2) zero-valued input samples before the
    * first real frame arrives. The output produced during pre-priming (the filter's
    * step-response ramp) is discarded. */
   {
      uint32_t prime_samples   = pc_ptr->num_taps / 2;
      uint32_t prime_out_count = (uint32_t)(((uint64_t)prime_samples * out_sr + in_sr - 1) / in_sr);

      uint32_t in_buf_size  = rs_num_channels * prime_samples  * sizeof(float);
      uint32_t out_buf_size = rs_num_channels * prime_out_count * sizeof(float);

      float *prime_in_buf  = (float *)posal_memory_malloc(in_buf_size,  (POSAL_HEAP_ID)heap_id);
      float *prime_out_buf = (float *)posal_memory_malloc(out_buf_size, (POSAL_HEAP_ID)heap_id);

      if (prime_in_buf && prime_out_buf)
      {
         memset(prime_in_buf,  0, in_buf_size);
         memset(prime_out_buf, 0, out_buf_size);

         const float *prime_in_ptrs[CAPI_MAX_CHANNELS_V2];
         float       *prime_out_ptrs[CAPI_MAX_CHANNELS_V2];
         uint32_t ch;
         for (ch = 0; ch < rs_num_channels; ch++)
         {
            prime_in_ptrs[ch]  = prime_in_buf  + ch * prime_samples;
            prime_out_ptrs[ch] = prime_out_buf + ch * prime_out_count;
         }

         resampleProcess(pc_ptr->rs_cxt,
                         prime_in_ptrs,
                         (int)prime_samples,
                         prime_out_ptrs,
                         (int)prime_out_count,
                         0.0);

         OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
                     "OSResampler pre-primed with %lu zero samples (%lu ch)",
                     prime_samples, rs_num_channels);
      }
      else
      {
         OSRC_PC_MSG(miid, DBG_ERROR_PRIO,
                     "OSResampler pre-prime skipped: malloc failed (in=%p out=%p)",
                     prime_in_buf, prime_out_buf);
      }

      if (prime_in_buf)  posal_memory_free(prime_in_buf);
      if (prime_out_buf) posal_memory_free(prime_out_buf);
   }

   OSRC_PC_MSG(miid, DBG_HIGH_PRIO,
               "OSResampler initialized: actual filters=%d, interpolation=%d, "
               "algo_delay=%lu us",
               resampleGetNumFilters(pc_ptr->rs_cxt),
               resampleInterpolationUsed(pc_ptr->rs_cxt),
               osrc_pc_get_algo_delay_us(pc_ptr, in_sr));

   /*
    * Reset max_data_len_per_ch in the base to 0 so that the CAPI layer
    * triggers scratch buffer reallocation on the next process() call.
    * (Same pattern as existing MFC — see capi_pcm_mf_cnv_lib_init().)
    */
   /* Note: max_data_len_per_ch is in the CAPI layer (capi_pcm_mf_cnv_t),
    * not in pc_osrc_lib_t.  The CAPI layer must reset it after calling osrc_pc_init().
    * The float scratch buffers in osrc_pc_lib_t are freed above and will be
    * reallocated lazily on the first osrc_pc_resampler_process() call. */

   return AR_EOK;
}

/*==============================================================================
   osrc_pc_deinit
==============================================================================*/
void osrc_pc_deinit(osrc_pc_lib_t *pc_ptr)
{
   if (NULL == pc_ptr)
   {
      return;
   }

   /* Free OSResampler context */
   if (pc_ptr->rs_cxt)
   {
      resampleFree(pc_ptr->rs_cxt);
      pc_ptr->rs_cxt = NULL;
   }

   /* Free float scratch buffers */
   osrc_pc_free_float_scratch(pc_ptr);

   /*
    * Call base pc_osrc_deinit() to free:
    *   - Channel mixer memory (pc_mem_ptr)
    *   - osrc_rs_lib_ptr (already NULL after Step 2, but pc_osrc_deinit() checks)
    */
   pc_osrc_deinit((pc_osrc_lib_t *)pc_ptr);
}