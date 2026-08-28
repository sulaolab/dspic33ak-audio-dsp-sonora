#ifndef KINKON_H
#define KINKON_H

//===========================================================
// kinkon : speed-chime-like "kin-kon" generator/inserter
//
// Required behavior:
// Pattern 1:
//   app_kinkon_set_enable(true)  -> starts from KIN (A)
//   app_kinkon_request_stop()    -> keeps current cycle normal,
//                                   then outputs final pattern C
//                                   at the NEXT cycle head, then stops
//
// Pattern 2:
//   app_kinkon_set_enable(true)  -> starts from KIN (A)
//   app_kinkon_set_enable(false) -> stops immediately without pattern C
//
// Sequence while running normally:
//   A -> B -> A -> B -> A -> B ...
//
// Sequence after request_stop():
//   ... -> A -> B -> C -> stop
//
// Source wavetable:
//   The A/B/C tables are generated from 48 kHz source WAVs.
//   System sample-rate conversion is handled by fx_domain_48k.
//===========================================================


//===========================================================
// INCLUDES
//===========================================================

#include <stdint.h>
#include <stdbool.h>


//===========================================================
// Definition
//===========================================================

#define KINKON_INTERNAL_SAMPLE_RATE_HZ    (48000u)


typedef enum
{
    KINKON_EVT_A = 0u,   // regular KIN
    KINKON_EVT_B = 1u,   // regular KON
    KINKON_EVT_C = 2u    // final end-only KIN
} kinkon_event_t;

typedef struct
{
    bool     enable;
    bool     stop_pending;

    // cadence
    uint32_t period_samples;      // A -> next A
    uint32_t gap_samples;         // A -> B gap
    uint32_t cycle_phase_samples; // phase inside one cycle

    // current event playback
    uint8_t  seq_state;           // 0=start A, 1=wait B, 2=wait next head, 3=play final C, 4=idle/stopped
    uint8_t  current_event;       // kinkon_event_t
    uint32_t tick_pos;            // current 48 kHz wavetable source position

    float    tick_gain;

    // future-proof knobs
    float bright;
    float ring;
    float tail;
    float attack;

} kinkon_t;

//===========================================================
// Core API
//===========================================================

void  kinkon_init_48k(kinkon_t* p);
void  kinkon_set_enable(kinkon_t* p, bool en);
void  kinkon_request_stop(kinkon_t* p);
void  kinkon_set_period_ms(kinkon_t* p, float period_ms);
void  kinkon_set_gap_ms(kinkon_t* p, float gap_ms);
void  kinkon_set_gain(kinkon_t* p, float gain);
void  kinkon_set_params(kinkon_t* p, float bright, float ring, float tail, float attack);
float kinkon_process_sample_48k(kinkon_t* p);

//===========================================================
// app_ wrappers
//===========================================================

void  app_kinkon_init_48k(void);
float app_kinkon_process_sample_48k(void);
void  app_kinkon_set_enable(bool en);
void  app_kinkon_request_stop(void);
void  app_kinkon_set_period_ms(float period_ms);
void  app_kinkon_set_gap_ms(float gap_ms);
void  app_kinkon_set_gain(float gain);
void  app_kinkon_set_params(float bright, float ring, float tail, float attack);

#endif // KINKON_H
