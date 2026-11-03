
/*========================================================================

Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
All rights reserved.
Confidential and Proprietary - Qualcomm Technologies, Inc.
======================================================================*/

/*========================================================================
Edit History


when       who     what, where, why
--------   ---     -------------------------------------------------------
7/15/26   vasharma    Added file for opensource resampler based MFC

========================================================================== */

/* =======================================================================
INCLUDE FILES FOR MODULE
========================================================================== */

#include "mfc_osrc_converter.h"

ar_result_t mfc_osrc_float_to_fixed_conv_process(void *          me_ptr,
                                           capi_buf_t *    input_buf_ptr,
                                           capi_buf_t *    output_buf_ptr,
                                           mfc_media_fmt_t *input_media_fmt_ptr,
                                           mfc_media_fmt_t *output_media_fmt_ptr)
{
   ar_result_t result = AR_EUNSUPPORTED;
   return result;
}

ar_result_t mfc_osrc_fixed_to_float_conv_process(void *          me_ptr,
                                           capi_buf_t *    input_buf_ptr,
                                           capi_buf_t *    output_buf_ptr,
                                           mfc_media_fmt_t *input_media_fmt_ptr,
                                           mfc_media_fmt_t *output_media_fmt_ptr)

{
   ar_result_t result = AR_EUNSUPPORTED;
   return result;
}

bool_t mfc_osrc_is_floating_point_data_format_supported()
{
   return FALSE;
}
