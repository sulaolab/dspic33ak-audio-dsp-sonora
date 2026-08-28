#ifndef _MAIN_H
#define	_MAIN_H

#include <stdbool.h>
#include "app_specific_config_defs.h"

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================



//===========================================================
// Variables
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

// P5: audio restart moved to audio_transport.h as audio_transport_restart().
// P5: UsrOperate_* button/touch control moved to apps/classic/classic_controls.h.
// P5: boot-time input selection (the ADC34 audio-in boot-select) and its
//     sonora_app_configure_boot_input() hook were retired -- the feature was
//     compile-dead (APP_USE_ADC_INOUT was hard-0) and left only a misleading
//     button-hold prompt. The boot button is now read directly in main.c for
//     the boot-banner visibility hold (boot_banner_hold_if_requested()).



#endif	//!_MAIN_H

