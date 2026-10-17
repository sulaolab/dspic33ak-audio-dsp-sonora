#ifndef SONORA_CLASSIC_DEMO_BUILD_CONFIG_H
#define SONORA_CLASSIC_DEMO_BUILD_CONFIG_H

/* Classic Audio Demo variation expansion.  No ASRC route is selected here. */
#ifndef APP_ASRC_CLOCK_OWNER
  #define APP_ASRC_CLOCK_OWNER  APP_ASRC_CLOCK_OWNER_CODEC /* N/A for Classic */
#endif

/* Neutral 0 stub: the shared config header reads APP_REQ_SPI2_INDEPENDENT_MASTER (defined for
 * real in the ASRC build-config) to derive the SPI2 bridge symbols.  Classic never runs an
 * independent SPI2 master, so it is always 0 here. */
#ifndef APP_REQ_SPI2_INDEPENDENT_MASTER
  #define APP_REQ_SPI2_INDEPENDENT_MASTER (0)
#endif

/* Neutral 0 stub: the resolved transport adapter (resolved_transport_config.h) references
 * APP_USE_CCP_FS_DETECT inside _Static_assert C-expressions -- not just #if -- so the symbol
 * must be a defined macro in EVERY build (an undefined macro is a bare undeclared identifier in
 * a C expression, unlike #if where it folds to 0).  Its real value lives in the ASRC app config;
 * Classic has no CCP FS detector, and APP_B_INDEP_DOMAIN==0 gates it to the NONE branch anyway,
 * so 0 is value-correct here. */
#ifndef APP_USE_CCP_FS_DETECT
  #define APP_USE_CCP_FS_DETECT (0)
#endif

/* Sonora AK512 Classic 1: keep MikroBUS B available for a second codec.
 * These defaults do not apply to AK128, ASRC, USB or 96 kHz presets. */
#if (APP_BUILD == APP_BUILD_STD_DEMO_1) && defined(__dsPIC33AK512MPS512__)
  #ifndef APP_PWM_OUTPUT
    #define APP_PWM_OUTPUT (1)
  #endif
  #ifndef MPQ6612A_ENABLE_MIKROB
    #if defined(MPQ6612A_STEREO) && MPQ6612A_STEREO
      #define MPQ6612A_ENABLE_MIKROB (1) /* compatibility with the stereo recipe */
    #else
      #define MPQ6612A_ENABLE_MIKROB (0)
    #endif
  #endif
  #ifndef MPQ6612A_ENABLE_XPRO1
    #define MPQ6612A_ENABLE_XPRO1 (APP_PWM_OUTPUT == 1)
  #endif
  #ifndef APP_PWM_AUDIO_HAL
    #define APP_PWM_AUDIO_HAL (APP_PWM_OUTPUT == 1)
  #endif
#endif

#if (APP_BUILD == APP_BUILD_STD_DEMO_2)
  #ifndef APP_USE_SPI_TDM_CLK_MASTER
    #define APP_USE_SPI_TDM_CLK_MASTER  (1)
  #endif
#endif

#if (APP_BUILD == APP_BUILD_DRC_DEMO)
  #ifndef ENA_DRC_DF2T_CASCADE
    #define ENA_DRC_DF2T_CASCADE  (1)
  #endif
#endif

#if (APP_BUILD == APP_BUILD_USB_48) || (APP_BUILD == APP_BUILD_USB_96)
  #ifndef ENA_USB_AUDIO_IN
    #define ENA_USB_AUDIO_IN
  #endif
#endif

#if (APP_BUILD == APP_BUILD_USB_96) || (APP_BUILD == APP_BUILD_DEMO_96K)
  #ifndef ENA_96K_RATE
    #define ENA_96K_RATE
  #endif
#endif

#endif /* SONORA_CLASSIC_DEMO_BUILD_CONFIG_H */
