#ifndef CLICKCLACK_H
#define CLICKCLACK_H


#if defined(ENA_CLICK_CLACK)
//===========================================================
// clickclack : relay-like turn-signal click generator/inserter
// - Designed as a 48 kHz mono source for fx_domain_48k.
// - Current implementation uses A/B wavetable ticks extracted from
//   original_recon_residual_noise_period_0p40s_30s.wav.
// - The wavetable source is 48 kHz.
// - System sample-rate conversion is handled by fx_domain_48k.
//===========================================================


//===========================================================
// INCLUDES
//===========================================================

#include <stdint.h>
#include <stdbool.h>


//===========================================================
// Definition
//===========================================================

#define CLICKCLACK_INTERNAL_SAMPLE_RATE_HZ  (48000u)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    bool     enable;

    // Period control (e.g., 0.40s -> 19200 samples @48k)
    uint32_t period_samples;
    uint32_t phase_samples;

    // A/B alternation
    uint8_t  ab;                   // 0=A, 1=B

    // Tick playback
    uint32_t tick_pos;             // current 48 kHz wavetable source position
    float    tick_gain;            // overall gain

    // "Future-proof" tone knobs (kept now as scalars; used more in B plan)
    float metal;   // high-mode emphasis (placeholder)
    float ring;    // decay emphasis (placeholder)
    float mech;    // mechanical noise amount (placeholder)
    float attack;  // excitation hardness (placeholder)

} clickclack_t;




//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

void  clickclack_init_48k(clickclack_t* p);
void  clickclack_set_enable(clickclack_t* p, bool en);
void  clickclack_set_period_ms(clickclack_t* p, float period_ms);
void  clickclack_set_gain(clickclack_t* p, float gain);
void  clickclack_set_params(clickclack_t* p, float metal, float ring, float mech, float attack);
float clickclack_process_sample_48k(clickclack_t* p);






//===========================================================
// API
//===========================================================

void  app_clickclack_init_48k(void);
float app_clickclack_process_sample_48k(void);
void  app_clickclack_set_enable(bool en);
void  app_clickclack_set_period_ms(float period_ms);
void  app_clickclack_set_gain(float gain);
void  app_clickclack_set_params(float metal, float ring, float mech, float attack);


#endif //defined(ENA_CLICK_CLACK)
#endif //CLICKCLACK_H
