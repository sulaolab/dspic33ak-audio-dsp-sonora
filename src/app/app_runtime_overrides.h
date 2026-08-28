#ifndef APP_RUNTIME_OVERRIDES_H
#define APP_RUNTIME_OVERRIDES_H

//===========================================================
// app_runtime_overrides.h
//
// App-wide runtime implementation choices: utility helpers and
// memory-function replacements for ISR/DMA-safe operation.
//
// Include this from every APP-SIDE .c that uses memset/memcpy/memmove etc.
// or needs the app_utils utilities (COMPILEASSERT, ARRAY_SIZE, biquad_t ...).
//
// Do NOT include this from pure-config headers (app_specific_config_defs.h,
// nora_spi_i2s_tdm_conf.h) -- those are consumed by the HAL core too,
// and this file must NOT reach the HAL compilation path.
//===========================================================

#include "app_utils.h"


/*
 * Use app_memcpy/app_memset instead of standard library memcpy/memset
 * for buffers shared with ISR/DMA/UART coefficient update path.
 *
 * On dsPIC33AK/XC-DSC, standard library/builtin memcpy/memset may use
 * optimized access patterns that can disturb real-time ISR behavior or
 * memory access timing in this application.
 */
#if 1
#define memset   app_memset
#define memcpy   app_memcpy
#define memmove  app_memmove
#define memcmp   app_memcmp
#define memchr   app_memchr
#endif


#endif /* APP_RUNTIME_OVERRIDES_H */
