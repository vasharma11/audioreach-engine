#ifndef OSRC_RESAMPLER_PORT_H
#define OSRC_RESAMPLER_PORT_H

/*==============================================================================
 @file osrc_resampler_port.h
 @brief SPF portability layer for the OSResampler (dbry/audio-resampler).

 This header redirects the standard C memory allocation and I/O functions used
 by resampler.c to their SPF equivalents:
   malloc  -> posal_memory_malloc
   calloc  -> posal_memory_malloc + memset
   free    -> posal_memory_free
   fprintf -> AR_MSG

================================================================================
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
All rights reserved.
Confidential and Proprietary - Qualcomm Technologies, Inc.
==============================================================================*/

#include <string.h>  
#include <stdint.h>
#include <stddef.h>   

#include "posal_memory.h"
#include "ar_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/*------------------------------------------------------------------------------
   Global heap ID — set by osrc_rs_set_heap_id() before every init call
------------------------------------------------------------------------------*/
extern uint32_t g_osrc_rs_heap_id;

/*------------------------------------------------------------------------------
   Inline helpers
------------------------------------------------------------------------------*/

/**
 * Set the heap ID that will be used for all subsequent OSResampler memory
 * allocations.  Call this immediately before resampleFixedRatioInit() or
 * resampleInit().
 */
static inline void osrc_rs_set_heap_id(uint32_t heap_id)
{
   g_osrc_rs_heap_id = heap_id;
}

/** malloc replacement — allocates from the current SPF heap. */
static inline void *osrc_rs_malloc(size_t size)
{
   return posal_memory_malloc((uint32_t)size, (POSAL_HEAP_ID)g_osrc_rs_heap_id);
}

/** calloc replacement — allocates from the current SPF heap and zeroes. */
static inline void *osrc_rs_calloc(size_t n, size_t size)
{
   uint32_t total = (uint32_t)(n * size);
   void    *ptr   = posal_memory_malloc(total, (POSAL_HEAP_ID)g_osrc_rs_heap_id);
   if (ptr)
   {
      memset(ptr, 0, total);
   }
   return ptr;
}

/** free replacement — returns memory to the SPF heap. */
static inline void osrc_rs_free(void *ptr)
{
   if (ptr)
   {
      posal_memory_free(ptr);
   }
}

/*------------------------------------------------------------------------------
   Macro overrides for resampler.c
   These must be defined BEFORE resampler.h / resampler.c is included.
------------------------------------------------------------------------------*/

/* Undefine any prior definitions to avoid redefinition warnings */
#ifdef malloc
#undef malloc
#endif
#ifdef calloc
#undef calloc
#endif
#ifdef free
#undef free
#endif
#ifdef fprintf
#undef fprintf
#endif

#define malloc(s)         osrc_rs_malloc(s)
#define calloc(n, s)      osrc_rs_calloc((n), (s))
#define free(p)           osrc_rs_free(p)

/*
 * fprintf(stderr, fmt, ...) -> AR_MSG(DBG_ERROR_PRIO, fmt, ...)
 * The variadic macro drops the first argument (the FILE* stream) and forwards
 * the format string and remaining arguments to AR_MSG.
 */
#define fprintf(stream, ...) AR_MSG(DBG_ERROR_PRIO, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* OSRC_RESAMPLER_PORT_H */