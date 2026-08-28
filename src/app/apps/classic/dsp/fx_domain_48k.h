#ifndef _FX_DOMAIN_48K_H
#define _FX_DOMAIN_48K_H

//===========================================================
// fx_domain_48k
//===========================================================
// 48 kHz internal domain for FX / SFX / synth sources.
//
// System side:
//   app_fx_domain_48k_process() is called at system sample rate.
//
// Internal side:
//   AVAS / pinger / future FX sources generate mono samples at 48 kHz.
//   Each source keeps its own control API, e.g. app_avas_type_ty_set_enable()
//   and app_pinger_set_enable(). This module owns source initialization, but not per-source
//   enable/disable state.
//   This module always calls all registered 48 kHz sources, mixes them into
//   one mono FX bus, and converts it to the system sample rate by Q16 linear SRC.
//===========================================================


//===========================================================
// Definition
//===========================================================

#define FX_DOMAIN_48K_SAMPLE_RATE_HZ    (48000u)


//===========================================================
// Function Prototype
//===========================================================

void app_fx_domain_48k_init(uint32_t sample_rate_Hz);
void app_fx_domain_48k_process(float *in, float *out);


#endif //!_FX_DOMAIN_48K_H
