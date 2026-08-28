#if defined(ENA_AVAS_SYNTH)

#ifndef _APP_AVAS_H
#define	_APP_AVAS_H


//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

#define AVAS_SLOTS_PER_FS            (STAGE_1_PROC_CH)

#define AVAS_LOW_MAX_PARTIALS        (8u)
#define AVAS_RESONANT_MAX_PARTIALS   (24u)
#define AVAS_LFO_NUM                 (3u)

#define AVAS_ALERT_MAX_PARTIALS      (16u)
#define AVAS_ALERT_LFO_NUM           (2u)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct avas_synth_s avas_synth_t;

struct avas_synth_s
{
    float fs;

    struct {
        float phase;
        float step;
        float gain;
        float mod_depth;
        uint8_t mod_id;
    } low_partial[AVAS_LOW_MAX_PARTIALS];

    struct {
        float phase;
        float step;
        float gain;
    } resonant_partial[AVAS_RESONANT_MAX_PARTIALS];

    struct {
        float phase;
        float step;
        float gain;
    } alert_partial[AVAS_ALERT_MAX_PARTIALS];

    struct {
        float phase;
        float step;
        float depth;
    } lfo[AVAS_LFO_NUM];

    struct {
        float phase;
        float step;
        float depth;
    } alert_lfo[AVAS_ALERT_LFO_NUM];

    uint8_t low_num;
    uint8_t resonant_num;
    uint8_t alert_num;

    float low_gain;
    float resonant_gain;
    float alert_gain;
    float master_gain;

    float resonant_env_base;
    float resonant_env_depth;
    float alert_env_base;
    float alert_env_depth;

    float gate;
    float gate_target;
    float gate_attack_alpha;
    float gate_release_alpha;

    float low_dc_x1;
    float low_dc_y1;
    float resonant_dc_x1;
    float resonant_dc_y1;
    float alert_dc_x1;
    float alert_dc_y1;
};




//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

extern void avas_synth_init(avas_synth_t *s, float fs);
extern void avas_synth_reset(avas_synth_t *s);
extern void avas_synth_process(avas_synth_t *s, float *in, float *out, uint16_t samples, uint16_t num_proc_ch);


extern void avas_synth_set_low_gain_db(avas_synth_t *s, float db);
extern void avas_synth_set_resonant_gain_db(avas_synth_t *s, float db);
extern void avas_synth_set_alert_gain_db(avas_synth_t *s, float db);
extern void avas_synth_set_master_gain_db(avas_synth_t *s, float db);

extern void avas_synth_gate_on(avas_synth_t *s);
extern void avas_synth_gate_off(avas_synth_t *s);


//===========================================================
// API
//===========================================================

extern void app_avas_init(void);
extern void app_avas_process(float *in, float *out);



#endif //!_APP_AVAS_H
#endif //defined(ENA_AVAS_SYNTH)
