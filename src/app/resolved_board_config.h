#ifndef SONORA_RESOLVED_BOARD_CONFIG_H
#define SONORA_RESOLVED_BOARD_CONFIG_H

/*
 * Transitional compile-time adapter for board/product integration facts.
 *
 * Board consumers use these neutral names instead of interpreting application
 * presets.  Existing APP_* macros remain authoritative until the final build
 * composition phase removes the transitional resolver.
 */
#include "app_specific_config_defs.h"

#define RESOLVED_BOARD_TARGET_AK512_VALUE  (1u)
#define RESOLVED_BOARD_TARGET_AK128_VALUE  (2u)

#if APP_TARGET == APP_TARGET_AK512
#define RESOLVED_BOARD_TARGET  RESOLVED_BOARD_TARGET_AK512_VALUE
#elif APP_TARGET == APP_TARGET_AK128
#define RESOLVED_BOARD_TARGET  RESOLVED_BOARD_TARGET_AK128_VALUE
#else
#error "Unsupported target in resolved board configuration."
#endif

#define RESOLVED_BOARD_AUDIO_INPUT_IS_USB  (APP_USE_USB_AUDIO_IN)
#define RESOLVED_BOARD_AK128_J3_TDM_B      (APP_AK128_J3_TDM_B)

/* Board clock-tree policy and hardware-route facts.
 *
 * PLL1 <- FRC and PLL2 <- REFI1 are fixed for every build, so there is no clock
 * "case" to resolve any more -- only whether this build takes SPI2's transport
 * clock from PLL2. See [internal] clock_design_contract.md. */
#define RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2     (APP_CLK_SPI_ON_PLL2)
#define RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2  (APP_CCP_TIMEBASE_FROM_PLL2)
#define RESOLVED_BOARD_COHERENT_OFFSET_CLOCK   (APP_Q27B_COHERENT_OFFSET)
#if defined(WM8904_PCB_REV4)
#define RESOLVED_BOARD_CODEC_REFI1_ON_RP16     (1)
#else
#define RESOLVED_BOARD_CODEC_REFI1_ON_RP16     (0)
#endif

/* Capability gate for the optional CCP capture time base.
 *
 * APP_CCP_TIMEBASE_FROM_PLL2's entire claim -- that the rate measurement's frequency
 * bias is structurally zero -- rests on REFI1 carrying codec-A's XTALout: a FIXED
 * 12.288 MHz that does not follow fs. Only the Rev.4 PCB route (RP16) is that signal,
 * and only it has been measured (a 48k -> 16k codec-A rate change moved BCLK_DIV while
 * the reading stayed exact). The other REFI1 routes are different signals: RP26 is the
 * USB source's MCLK, AK128 routes RP33, and the legacy RP75 route is documented as
 * BCLK -- which, if it really is BCLK, moves with BCLK_DIV and would silently
 * invalidate PLL2's fixed input_hz on every rate change.
 *
 * The flag's own consumer routes RP16 explicitly rather than sharing the PWM path's
 * board-aware router, so these refusals and that route must agree. Refuse rather than
 * measure a crystal-clocked rate against an unknown ruler. */
#if RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2
  #if RESOLVED_BOARD_TARGET != RESOLVED_BOARD_TARGET_AK512_VALUE
    #error "APP_CCP_TIMEBASE_FROM_PLL2 is AK512-only: AK128 routes REFI1 from RP33 (OSCO/CLKO), not the codec XTALout."
  #endif
  #if RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    #error "APP_CCP_TIMEBASE_FROM_PLL2 needs codec-A's XTALout on REFI1, but a USB-audio build routes REFI1 from the USB source's MCLK (RP26)."
  #endif
  #if !RESOLVED_BOARD_CODEC_REFI1_ON_RP16
    #error "APP_CCP_TIMEBASE_FROM_PLL2 requires the Rev.4 PCB route (define WM8904_PCB_REV4): only RP16 carries the fs-independent 12.288 MHz XTALout. The legacy RP75 route is documented as BCLK and is NOT verified fs-independent, so the fixed PLL2 input_hz would be unsound there."
  #endif
#endif

/* Board-device integration selections. */
#if defined(ENA_CMSIS_I2C)
#define RESOLVED_BOARD_USE_CMSIS_I2C           (1)
#else
#define RESOLVED_BOARD_USE_CMSIS_I2C           (0)
#endif
#if defined(ENA_REGULAR_ADC)
#define RESOLVED_BOARD_USE_REGULAR_ADC_API     (1)
#else
#define RESOLVED_BOARD_USE_REGULAR_ADC_API     (0)
#endif
#define RESOLVED_BOARD_USE_SST26                (APP_USE_SST26)
#if defined(ENA_RED_IN_JACK)
#define RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK  (1)
#else
#define RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK  (0)
#endif
#if defined(ENA_MIC_IN)
#define RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED   (1)
#else
#define RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED   (0)
#endif

/* Product policy consumed only by the CMSIS-SAI compatibility hook. */
#define RESOLVED_BOARD_SAI_RATE_IS_SUPPORTED(hz) \
    APP_SAMPLE_RATE_IS_SUPPORTED(hz)

_Static_assert(
    (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE) ==
        (APP_TARGET == APP_TARGET_AK512),
    "resolved AK512 target must match APP_TARGET" );
_Static_assert(
    (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE) ==
        (APP_TARGET == APP_TARGET_AK128),
    "resolved AK128 target must match APP_TARGET" );
_Static_assert( RESOLVED_BOARD_AUDIO_INPUT_IS_USB == APP_USE_USB_AUDIO_IN,
                "resolved board input must match APP_USE_USB_AUDIO_IN" );
_Static_assert( !RESOLVED_BOARD_AK128_J3_TDM_B ||
                    (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE),
                "AK128 J3 TDM-B capability must select the AK128 board" );
_Static_assert( !RESOLVED_BOARD_AK128_J3_TDM_B || APP_USE_SPI2_AUDIO,
                "AK128 J3 TDM-B capability must enable the second audio leg" );
_Static_assert( RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2 == APP_CLK_SPI_ON_PLL2,
                "resolved SPI clock source must match APP_CLK_SPI_ON_PLL2" );
_Static_assert( RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2 == APP_CCP_TIMEBASE_FROM_PLL2,
                "resolved CCP time base must match APP_CCP_TIMEBASE_FROM_PLL2" );
_Static_assert( RESOLVED_BOARD_COHERENT_OFFSET_CLOCK == APP_Q27B_COHERENT_OFFSET,
                "resolved coherent clock must match APP_Q27B_COHERENT_OFFSET" );
_Static_assert( RESOLVED_BOARD_USE_SST26 == APP_USE_SST26,
                "resolved SST26 presence must match APP_USE_SST26" );

#define RESOLVED_BOARD_CONFIG_READY  (1)

#endif /* SONORA_RESOLVED_BOARD_CONFIG_H */
