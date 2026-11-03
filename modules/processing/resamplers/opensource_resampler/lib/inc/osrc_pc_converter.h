#ifndef OSRC_PC_CONVERTER_H
#define OSRC_PC_CONVERTER_H

/*==============================================================================
 @file osrc_pc_converter.h
 @brief OSResampler-based PCM processing library API.

 This library is a thin wrapper around the existing pcm_mf_cnv_lib (pc_osrc_lib_t).
 It reuses all non-resampler pipeline stages (endianness, byte conversion,
 interleaving, channel mixing, float conversion) unchanged.  The only
 difference is that the resampler stages (RESAMPLER_PRE / RESAMPLER_POST)
 are handled by osrc_pc_resampler_process() which drives the open-source
 windowed-sinc resampler (dbry/audio-resampler) instead of the Qualcomm
 osrc_rs_lib_wrapper.

================================================================================
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
All rights reserved.
Confidential and Proprietary - Qualcomm Technologies, Inc.
==============================================================================*/

/*
 * NOTE: pc_osrc_converter.h defines the OSRC-flavoured pc_osrc_lib_t with
 * osrc_rs_lib_ptr.  The field is freed immediately after pc_osrc_init() returns
 * and replaced by rs_cxt (the direct Resample* context).
 * pc_osrc_deinit() NULL-checks osrc_rs_lib_ptr, so the tail call in
 * osrc_pc_deinit() is always a safe no-op on that field.
 */
#include "pc_osrc_converter.h"
#include "resampler.h"

#ifdef __cplusplus
extern "C" {
#endif

//#define OSRC_DEBUG

/*------------------------------------------------------------------------------
   Default OSResampler quality preset (Preset 0: 48 taps, 48 filters)
   Suitable for embedded DSP with tight memory constraints (~16 KB stereo RAM).
------------------------------------------------------------------------------*/
#define OSRC_RS_DEFAULT_NUM_TAPS    48
#define OSRC_RS_DEFAULT_MAX_FILTERS 48

/**
 * Default flags for resampleFixedRatioInit():
 *   SUBSAMPLE_INTERPOLATE — interpolate between adjacent sinc filters (quality)
 *   BLACKMAN_HARRIS       — 4-term Blackman-Harris window (better stopband)
 * INCLUDE_LOWPASS is OR-ed in automatically for downsampling in osrc_pc_init().
 */
#define OSRC_RS_DEFAULT_FLAGS  (SUBSAMPLE_INTERPOLATE | BLACKMAN_HARRIS)

/*------------------------------------------------------------------------------
   osrc_pc_lib_t — OSResampler-based processing library instance
------------------------------------------------------------------------------*/

/**
 * @brief OSResampler-based processing library instance.
 *
 * The first member MUST be pc_osrc_lib_t so that (osrc_pc_lib_t *) can be safely
 * cast to (pc_osrc_lib_t *) and vice-versa.  pc_osrc_process() is called directly on
 * the embedded base; the resampler stage function pointers in
 * base.core_lib.pc_proc_info[RESAMPLER_PRE/POST].process are replaced with
 * osrc_pc_resampler_process() after pc_osrc_init() returns.
 *
 * NOTE: base.osrc_rs_lib_ptr is always NULL in this library after osrc_pc_init()
 * completes — it is freed immediately after pc_osrc_init() and replaced by rs_cxt.
 */
typedef struct osrc_pc_lib_t
{
   /*
    * MUST be first — base pc_osrc_lib_t.
    * All non-resampler stages (endianness, byte conversion, interleaving,
    * channel mixing, float conversion) use the function pointers set by
    * pc_osrc_init() and are called through pc_osrc_process() unchanged.
    */
   pc_osrc_lib_t base;

   /* ---- OSResampler context ---- */

   /** OSResampler instance (replaces base.osrc_rs_lib_ptr). */
   Resample *rs_cxt;

   /** Number of sinc filter taps (quality knob). Default: OSRC_RS_DEFAULT_NUM_TAPS. */
   uint32_t num_taps;

   /** Maximum number of sinc filters (quality knob). Default: OSRC_RS_DEFAULT_MAX_FILTERS. */
   uint32_t max_filters;

   /**
    * Flags passed to resampleFixedRatioInit().
    * Default: OSRC_RS_DEFAULT_FLAGS.
    * INCLUDE_LOWPASS is OR-ed in automatically for downsampling.
    */
   int rs_flags;

   /* ---- Float scratch buffers (allocated lazily on first process() call) ---- */

   /**
    * Array of per-channel float input pointers (size: float_buf_channels).
    * float_input_ptrs[ch] points into float_input_buf at offset
    * ch * float_buf_samples.
    */
   float **float_input_ptrs;

   /**
    * Array of per-channel float output pointers (size: float_buf_channels).
    * float_output_ptrs[ch] points into float_output_buf at offset
    * ch * float_buf_samples.
    */
   float **float_output_ptrs;

   /** Contiguous float input buffer: float_buf_channels * float_buf_samples floats. */
   float *float_input_buf;

   /** Contiguous float output buffer: float_buf_channels * float_buf_samples floats. */
   float *float_output_buf;

   /** Capacity of each per-channel float buffer in samples. */
   uint32_t float_buf_samples;

   /** Number of channels for which float buffers are currently allocated. */
   uint32_t float_buf_channels;

} osrc_pc_lib_t;

/*------------------------------------------------------------------------------
   Public API — non-island (init / deinit / scratch allocation)
------------------------------------------------------------------------------*/

/**
 * @brief Initialize the OSResampler-based processing library.
 *
 * Mirrors pc_osrc_init() but replaces the resampler backend with the OSResampler.
 * Internally:
 *   1. Calls pc_osrc_init() to set up the full 11-stage pipeline.
 *   2. Frees osrc_rs_lib_ptr allocated by pc_osrc_init().
 *   3. Replaces RESAMPLER_PRE / RESAMPLER_POST stage function pointers with
 *      osrc_pc_resampler_process().
 *   4. Calls resampleFixedRatioInit() to create the OSResampler context.
 *
 * @param pc_ptr               Library instance (caller-allocated, zeroed).
 * @param input_media_fmt_ptr  Input media format.
 * @param output_media_fmt_ptr Output media format.
 * @param coef_set_ptr         Channel mixer coefficient set (NULL = default).
 * @param heap_id              SPF heap ID for all memory allocations.
 * @param lib_enable           [out] TRUE if any pipeline stage is active.
 * @param miid                 Module instance ID for logging.
 * @param type                 Module type (must be MFC = 3).
 *
 * @return AR_EOK on success, AR_EFAILED / AR_ENOMEMORY on failure.
 *         On failure, lib_enable is set to FALSE and the caller must NOT
 *         call osrc_pc_deinit() (pc_osrc_deinit() is called internally on error).
 *         pc_osrc_deinit() will free osrc_rs_lib_ptr if non-NULL.
 */
ar_result_t osrc_pc_init(osrc_pc_lib_t  *pc_ptr,
                          pc_media_fmt_t *input_media_fmt_ptr,
                          pc_media_fmt_t *output_media_fmt_ptr,
                          void           *coef_set_ptr,
                          uint32_t        heap_id,
                          bool_t         *lib_enable,
                          uint32_t        miid,
                          uint8_t         type);

/**
 * @brief Deinitialize the OSResampler-based processing library.
 *
 * Frees the OSResampler context (rs_cxt), float scratch buffers, and calls
 * pc_osrc_deinit() to free the channel mixer, osrc_rs_lib_ptr and pipeline memory.
 *
 * @param pc_ptr  Library instance.
 */
void osrc_pc_deinit(osrc_pc_lib_t *pc_ptr);

/**
 * @brief Allocate (or reallocate) the per-channel float scratch buffers.
 *
 * Called lazily from osrc_pc_resampler_process() when the buffer size or
 * channel count changes.  Frees any existing buffers before allocating.
 *
 * @param pc_ptr            Library instance.
 * @param num_channels      Number of channels to allocate for.
 * @param max_samples_per_ch Maximum samples per channel per process() call.
 * @param heap_id           SPF heap ID.
 *
 * @return AR_EOK on success, AR_ENOMEMORY on failure.
 */
ar_result_t osrc_pc_alloc_float_scratch(osrc_pc_lib_t *pc_ptr,
                                         uint32_t       num_channels,
                                         uint32_t       max_samples_per_ch,
                                         uint32_t       heap_id);

/**
 * @brief Free the per-channel float scratch buffers.
 *
 * @param pc_ptr  Library instance.
 */
void osrc_pc_free_float_scratch(osrc_pc_lib_t *pc_ptr);

/*------------------------------------------------------------------------------
   Public API — island-eligible (process / DM queries)
------------------------------------------------------------------------------*/

/**
 * @brief OSResampler stage process function.
 *
 * This function is installed as the process function pointer for both
 * RESAMPLER_PRE (stage 4) and RESAMPLER_POST (stage 6) in the pc_proc_info
 * vtable.  It is called by pc_osrc_process() through the standard vtable dispatch.
 *
 * Data path:
 *   fixed-point input -> int-to-float conversion -> resampleProcess() ->
 *   float-to-int conversion -> fixed-point output
 *
 * The float scratch buffers (float_input_ptrs / float_output_ptrs) are
 * allocated lazily on the first call.
 *
 * EOS handling:
 *   When input_buf_ptr[0].actual_data_len == 0 and the caller has set the
 *   EOS flag, pass numInputFrames = -1 to resampleProcess() to flush the
 *   filter delay line.
 *
 * NOTE: This function is NOT island-safe in the initial implementation.
 *       Island refactoring is deferred (see capi_osrc_mfc_design.md §8.3).
 *
 * @param me_ptr               Pointer to osrc_pc_lib_t (cast from pc_osrc_lib_t*).
 * @param input_buf_ptr        Per-channel input capi_buf_t array.
 * @param output_buf_ptr       Per-channel output capi_buf_t array.
 * @param input_media_fmt_ptr  Input media format at this pipeline stage.
 * @param output_media_fmt_ptr Output media format at this pipeline stage.
 *
 * @return AR_EOK on success, AR_EFAILED / AR_ENOMEMORY on failure.
 */
ar_result_t osrc_pc_resampler_process(void           *me_ptr,
                                       capi_buf_t     *input_buf_ptr,
                                       capi_buf_t     *output_buf_ptr,
                                       pc_media_fmt_t *input_media_fmt_ptr,
                                       pc_media_fmt_t *output_media_fmt_ptr);

/**
 * @brief Get required input samples to produce a fixed number of output samples.
 *
 * Used for DM fixed-output mode.  Wraps resampleGetRequiredSamples().
 *
 * Caveat: resampleGetRequiredSamples() may return one fewer input sample than
 * needed for ratios just over 1.0.  Always restrict output with numOutputFrames
 * in resampleProcess() to prevent over-generation.
 *
 * @param pc_ptr          Library instance.
 * @param req_out_samples Requested output samples per channel.
 *
 * @return Expected input samples per channel, or 0 if rs_cxt is NULL.
 */
uint32_t osrc_pc_get_fixed_out_samples(osrc_pc_lib_t *pc_ptr, uint32_t req_out_samples);

/**
 * @brief Get expected output samples for a fixed number of input samples.
 *
 * Used for DM fixed-input mode.  Wraps resampleGetExpectedOutput().
 *
 * @param pc_ptr         Library instance.
 * @param req_in_samples Available input samples per channel.
 *
 * @return Expected output samples per channel, or 0 if rs_cxt is NULL.
 */
uint32_t osrc_pc_get_expected_output(osrc_pc_lib_t *pc_ptr, uint32_t req_in_samples);

/**
 * @brief Get the algorithmic delay introduced by the OSResampler in microseconds.
 *
 * Delay = (num_taps / 2) * 1,000,000 / source_sample_rate
 *
 * @param pc_ptr    Library instance.
 * @param source_sr Source sample rate in Hz.
 *
 * @return Algorithmic delay in microseconds, or 0 if no resampling is active.
 */
uint32_t osrc_pc_get_algo_delay_us(osrc_pc_lib_t *pc_ptr, uint32_t source_sr);

#ifdef __cplusplus
}
#endif

#endif /* OSRC_PC_CONVERTER_H */