#ifndef _APP_PINGER_SYNTH_H
#define _APP_PINGER_SYNTH_H

#if defined(ENA_PINGER_SOUND)


//===========================================================
// This module is a 48 kHz mono source for fx_domain_48k.
// It does not directly process the system ch-major DMA buffer.


//===========================================================
// Definition
//===========================================================

#define PINGER_OSC_NUM           (2u)

//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct pinger_synth_s pinger_synth_t;

struct pinger_synth_s
{
    float fs;    // internal float sample rate used by oscillator/envelope math

    struct {
        float phase;
        float step;
        float gain;
    } osc[PINGER_OSC_NUM];

    float master_gain;

    /* envelope */
    float env;
    float ta_s;
    float td_s;
    float attack_coeff;
    float decay_coeff;
    float attack_state;
    float decay_state;

    /* pulse scheduler */
    uint32_t sample_count;
    uint32_t next_event_sample;
    uint32_t event_index;
    uint8_t  pulse_active;

    /* event timing */
    uint32_t event_period_samples;
    uint32_t event_start_offset_samples;

    /* optional finite pulse end */
    uint32_t event_elapsed_samples;
    uint32_t event_len_samples;

    /* output dc blocker */
    float dc_x1;
    float dc_y1;

    /* global enable gate */
    float gate;
    float gate_target;
    float gate_attack_alpha;
    float gate_release_alpha;
};

//===========================================================
// Function Prototype
//===========================================================

extern void  pinger_synth_init(pinger_synth_t *s, float fs);
extern float pinger_synth_process_sample(pinger_synth_t *s);

extern void pinger_synth_set_master_gain_db(pinger_synth_t *s, float db);
extern void pinger_synth_gate_on(pinger_synth_t *s);
extern void pinger_synth_gate_off(pinger_synth_t *s);

/* optional controls */
extern void pinger_synth_set_decay_ms(pinger_synth_t *s, float td_ms);
extern void pinger_synth_set_attack_ms(pinger_synth_t *s, float ta_ms);

//===========================================================
// API
//===========================================================

extern void  app_pinger_init_48k(void);
extern float app_pinger_process_sample_48k(void);
extern void  app_pinger_set_enable(bool enable);



#endif //defined(ENA_PINGER_SOUND)
#endif //_APP_PINGER_SYNTH_H
