#if defined(ENA_ANC_MONITOR)

#ifndef _ANC_MONITOR_H
#define _ANC_MONITOR_H

//===========================================================
// anc_monitor: purpose
//  - Update metrics in audio ISR (per block)
//  - Print snapshot in main loop (e.g., every 200ms)
//===========================================================


//===========================================================
// INCLUDES
//===========================================================

//===========================================================
// Definition
//===========================================================

#define ANCMON_CORR_LEN   512
#define ANCMON_MAX_LAG    64


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    // config
    float fs;
    int   ch;           // number of channels in float buffer (e.g., STAGE_1_PROC_CH)
    int   frames;       // block size (e.g., APP_BLOCK_FRAMES)

    // which channels are "ref" and "err" for delay estimation (0-based)
    // if invalid (<0) delay estimation is disabled.
    int ref_ch;
    int err_ch;

    // delay estimation control (run correlation once per N blocks)
    int delay_est_decim_blocks;

    // ring buffer for correlation (stores recent samples)
    int   corr_len;     // correlation window length (samples)
    int   corr_max_lag; // +/- lag range (samples)

    // internal
    int   _blk_count;
    int   _rb_w;        // ring write index
//    float* _rb_ref;
//    float* _rb_err;
    float _rb_ref[ANCMON_CORR_LEN];
    float _rb_err[ANCMON_CORR_LEN];

    // snapshot updated by ISR (write) and read by main (read)
    volatile uint32_t seq;         // incremented after each update

    volatile float micL_rms;
    volatile float micR_rms;
    volatile float micL_pk;
    volatile float micR_pk;

    volatile float outL_rms;
    volatile float outR_rms;
    volatile float outL_pk;
    volatile float outR_pk;

    volatile uint32_t clip_count;  // counts peak > clip_th (per-sample basis)
    volatile float    clip_th;     // threshold in linear (e.g., 0.98)

    volatile int   est_delay_samp; // err relative to ref (samples). sign convention described in .c
    volatile float est_corr;       // normalized correlation peak 0..1 (rough)
} ancmon_t;






//===========================================================
// Variables
//===========================================================

//===========================================================
// Function Prototype
//===========================================================

//==================== API ====================
extern void  ancmon_init(ancmon_t* m,
                         float fs,
                         int ch,
                         int frames,
                         int ref_ch,
                         int err_ch,
                         int delay_est_decim_blocks);

extern void  ancmon_set_clip_threshold(ancmon_t* m, float th);

// Call from audio ISR per block (expects interleaved float buffer: [frame0_ch0, frame0_ch1, ...])
// If you have separate in/out buffers, call twice or pass both as needed.
extern void  ancmon_update_in(ancmon_t* m, const float* in_interleaved);
extern void  ancmon_update_out(ancmon_t* m, const float* out_interleaved);

// Called from main loop (200ms etc.)
extern void  ancmon_dbg_prt(const ancmon_t* m);

// Optional: header lines like your bassenh debug
extern void  ancmon_dbg_hdr(void);
extern void  ancmon_est_delay_main(ancmon_t* m);





//===========================================================
// API
//===========================================================
extern void  app_ancmon_init(void);
extern void  app_ancmon_process_in(const float* in_interleaved);
extern void  app_ancmon_process_out(const float* out_interleaved);
extern void  app_ancmon_dbg_prt(void);




#endif // _ANC_MONITOR_H
#endif //defined(ENA_ANC_MONITOR)

