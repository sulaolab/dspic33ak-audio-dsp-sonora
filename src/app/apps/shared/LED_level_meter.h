#ifndef _LED_LEVEL_METER_H
#define	_LED_LEVEL_METER_H

//===========================================================
// INCLUDES
//===========================================================




//===========================================================
// Definition
//===========================================================

#define LEVEL_METER_DEFAULT_UPDATE_MS      (0.8f)
#define LEVEL_METER_DEFAULT_ATTACK_MS      (0.5f)
#define LEVEL_METER_DEFAULT_RELEASE_MS     (0.3f)


//===========================================================
// Enum & Struct typedef
//===========================================================




//===========================================================
// Variables
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

/*
 * Initialize the LED level meter.
 *
 * sample_rate_hz:
 *   Audio sample rate in Hz. Pass SAMPLE_RATE or a runtime sample rate.
 *   If 0 is passed, SAMPLE_RATE is used.
 *
 * update_period_ms:
 *   Target visible LED update period in milliseconds.
 *   Fractional values are supported, for example 0.8f.
 *   The actual update period is rounded up to the nearest DMA audio block.
 *
 * attack_ms / release_ms:
 *   First-order smoothing time constants in milliseconds.
 *   Fractional values are supported, for example 0.5f.
 *   If 0 or negative is passed, default values are used.
 *
 * Recommended exaggerated display test:
 *   level_meter_init(SAMPLE_RATE, 0.8f, 0.5f, 0.3f);
 *
 */
extern void level_meter_init(uint32_t sample_rate_hz,
                             float    update_period_ms,
                             float    attack_ms,
                             float    release_ms);

extern void level_meter_process(const float* input);

// Submit one interleaved int32 codec/DMA buffer to the LED meter, described by its SHAPE (not by
// any "which source" tag): `slots_per_frame` slots per frame, `frames` frames; the metered stereo
// pair is slots 0/1 (L/R). Uses the SAME int->float scale as convert_codec_int_to_float
// (Q31_SCALE_FLOAT * Pre_Gain_CODEC, 24-bit mask, clip) -- no float scratch / convert pass.
//
// The meter holds the MAX per-block level across an update window, so submitting MORE THAN ONE
// buffer per block (e.g. the ASRC BIDIR route feeds both the A output and the B output) makes the
// 8-LED bar show the LOUDER of them -- max is order-independent, so no per-source state is needed.
// A one-way route simply submits its single output buffer once per block.
extern void level_meter_process_i32(const int32_t* buf, uint16_t slots_per_frame, uint16_t frames);

// Sparse integer-only sampling variant for ISR headroom experiments. It samples
// one rotating phase of frame_stride-spaced frames per call (avoids coherent
// tone blind spots), holds a Q23 peak, and smooths only the final 0..8 LED count.
// The ISR path has no float conversion or float arithmetic. Keep one frame_phase
// state per independently submitted stream.
extern void level_meter_process_i32_sparse(const int32_t* buf,
                                           uint16_t slots_per_frame,
                                           uint16_t frames,
                                           uint16_t frame_stride,
                                           uint16_t* frame_phase);



#endif	//!_LED_LEVEL_METER_H

