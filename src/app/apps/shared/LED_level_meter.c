

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>

#include "board/devices/button_led.h"
#include "apps/shared/float_conversion.h"


#include "apps/shared/LED_level_meter.h"





//===========================================================
// Definition
//===========================================================

#define LED_COUNT                          (8u)

/*
 * Optional display effect:
 *
 * When enabled, the LED meter aims below the real level only while the level is
 * falling. This intentionally creates a stronger bouncing / flickering display.
 *
 * Disable:
 *   Comment out ENA_LEVEL_METER_RELEASE_UNDERSHOOT.
 *
 * Suggested first values:
 *   -3 dB : mild bounce
 *   -6 dB : stronger bounce
 *   -12dB : very exaggerated
 */
#define ENA_LEVEL_METER_RELEASE_UNDERSHOOT
#define LEVEL_METER_RELEASE_UNDERSHOOT_DB  (-6.0f)

/*
 * Default fallback values.
 *
 * These keep the previous behavior if level_meter_init() is not called.
 * New code should call level_meter_init() before nora_spi_i2s_tdm_start().
 */
#define LEVEL_METER_FALLBACK_UPDATE_BLOCKS (2u)
#define LEVEL_METER_FALLBACK_SAMPLE_WINDOW ((uint32_t)APP_BLOCK_FRAMES * (uint32_t)LEVEL_METER_FALLBACK_UPDATE_BLOCKS)
#define LEVEL_METER_FALLBACK_MEAN_SCALE    (0.5f / (float)LEVEL_METER_FALLBACK_SAMPLE_WINDOW)

#define LEVEL_METER_MIN_UPDATE_BLOCKS      (1u)

// Coarse sparse meter: one segment falls after this many LED update windows.
// The exact time follows the configured sample rate and rounded update period.
#define LEVEL_METER_COARSE_RELEASE_HOLD     (16u)
#define LEVEL_METER_Q23_FULL_SCALE_F        (8388608.0f)



//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    uint32_t sample_rate_hz;
    float    update_period_ms;

    uint32_t update_blocks;
    uint32_t sample_window;
    float    mean_scale;

    float    attack_ms;
    float    release_ms;
    float    attack_coeff;
    float    release_coeff;

    float    smoothed_peak;
    float    level_accum;     // float meter (level_meter_process): running SUM, averaged at commit
    uint32_t block_count;
    uint8_t  last_led_mask;

    // int32 shape-based meter (level_meter_i32_submit): MAX per-block level held across the update
    // window. Submitting several buffers per block makes the LED show the loudest (BIDIR uses this
    // to display max(A output, B output) without any per-source tag). Zero-init (designated init).
    float    meter_peak;

    // Sparse ASRC path. The ISR-facing sample scan and display state are fully
    // integer; float is used only once in level_meter_init() to derive these
    // eight Q23 thresholds from the shared dB table and Pre_Gain_CODEC.
    uint32_t threshold_q23[LED_COUNT];
    uint32_t meter_peak_q23;
    uint32_t coarse_block_count;
    uint8_t  coarse_led_count;
    uint8_t  coarse_release_hold;

} level_meter_state_t;



//===========================================================
// Function Prototype
//===========================================================

static inline float   level_meter_absf(float x);
static inline uint8_t level_meter_make_mask(float level);
static inline uint8_t level_meter_make_count_q23(uint32_t level_q23);
static inline uint8_t level_meter_count_to_mask(uint8_t count);

static uint32_t       level_meter_calc_update_blocks(uint32_t sample_rate_hz,
                                                     float    update_period_ms);
static float          level_meter_calc_smoothing_coeff(float actual_update_ms,
                                                       float time_constant_ms);
static void           level_meter_apply_smoothing(float* target, float mean_peak);
static void           level_meter_reset_runtime_state(void);
static void           level_meter_commit_block(void);
static void           level_meter_commit_i32_level(float block_level);
static void           level_meter_commit_i32_q23(uint32_t block_peak_q23);



//===========================================================
// Variables
//===========================================================

static level_meter_state_t s_level_meter =
{
    .sample_rate_hz   = SAMPLE_RATE,
    .update_period_ms = 0.0f,

    .update_blocks    = LEVEL_METER_FALLBACK_UPDATE_BLOCKS,
    .sample_window    = LEVEL_METER_FALLBACK_SAMPLE_WINDOW,
    .mean_scale       = LEVEL_METER_FALLBACK_MEAN_SCALE,

    .attack_ms        = 0.0f,
    .release_ms       = 0.0f,
    .attack_coeff     = 3.5f,     // previous behavior fallback
    .release_coeff    = 0.08f,    // previous behavior fallback

    .smoothed_peak    = 0.0f,
    .level_accum      = 0.0f,
    .block_count      = 0u,
    .last_led_mask    = 0xFFu
};


//=== LED threshold levels (log scale) ===
// K_VAL shifts the whole threshold table lower for the current LED visibility.
//#define K_VAL      (22.0f)
#define K_VAL      (28.0f)

static const float thresholds[LED_COUNT] =
{
    db_to_lin(-40.0f - K_VAL),   // LED1
    db_to_lin(-34.0f - K_VAL),   // LED2
    db_to_lin(-28.0f - K_VAL),   // LED3
    db_to_lin(-22.0f - K_VAL),   // LED4
    db_to_lin(-16.0f - K_VAL),   // LED5
    db_to_lin(-10.0f - K_VAL),   // LED6
    db_to_lin(-4.0f  - K_VAL),   // LED7
    db_to_lin(-0.9f  - K_VAL)    // LED8
};




//===========================================================
// Global Function
//===========================================================

void level_meter_init(uint32_t sample_rate_hz,
                      float    update_period_ms,
                      float    attack_ms,
                      float    release_ms)
{
    uint32_t update_blocks;
    uint32_t sample_window;
    float    actual_update_ms;

    if (sample_rate_hz == 0u)
    {
        sample_rate_hz = SAMPLE_RATE;
    }

    if (update_period_ms <= 0.0f)
    {
        update_period_ms = LEVEL_METER_DEFAULT_UPDATE_MS;
    }

    if (attack_ms <= 0.0f)
    {
        attack_ms = LEVEL_METER_DEFAULT_ATTACK_MS;
    }

    if (release_ms <= 0.0f)
    {
        release_ms = LEVEL_METER_DEFAULT_RELEASE_MS;
    }

    update_blocks = level_meter_calc_update_blocks(sample_rate_hz, update_period_ms);

    sample_window = update_blocks * (uint32_t)APP_BLOCK_FRAMES;
    if (sample_window == 0u)
    {
        sample_window = (uint32_t)APP_BLOCK_FRAMES;
    }

    /*
     * Use the actual update period after block rounding.
     * This keeps attack/release timing stable even when update_period_ms is not
     * an exact multiple of the DMA audio block period.
     */
    actual_update_ms = ((float)update_blocks * (float)APP_BLOCK_FRAMES * 1000.0f) /
                       (float)sample_rate_hz;

    s_level_meter.sample_rate_hz   = sample_rate_hz;
    s_level_meter.update_period_ms = update_period_ms;

    s_level_meter.update_blocks    = update_blocks;
    s_level_meter.sample_window    = sample_window;
    s_level_meter.mean_scale       = 0.5f / (float)sample_window;

    s_level_meter.attack_ms        = attack_ms;
    s_level_meter.release_ms       = release_ms;
    s_level_meter.attack_coeff     = level_meter_calc_smoothing_coeff(actual_update_ms, attack_ms);
    s_level_meter.release_coeff    = level_meter_calc_smoothing_coeff(actual_update_ms, release_ms);

    // Build raw signed-24-bit thresholds once in foreground. Sparse processing
    // can then avoid every float conversion, multiply, divide, and comparison
    // in the audio callbacks. Pre_Gain_CODEC is initialized before this call.
    const float gain = (Pre_Gain_CODEC > 0.0f) ? Pre_Gain_CODEC : 1.0f;
    for (uint8_t i = 0u; i < LED_COUNT; i++)
    {
        float q23 = thresholds[i] * LEVEL_METER_Q23_FULL_SCALE_F / gain;
        if (q23 < 1.0f) { q23 = 1.0f; }
        if (q23 > 8388607.0f) { q23 = 8388607.0f; }
        s_level_meter.threshold_q23[i] = (uint32_t)q23;
    }

    level_meter_reset_runtime_state();

    printf(" LED meter: fs=%luHz target_update=%.3fms actual_update=%.3fms blocks=%lu window=%lu attack=%.3fms coeff=%.5f release=%.3fms coeff=%.5f\n",
           (unsigned long)s_level_meter.sample_rate_hz,
           (double)s_level_meter.update_period_ms,
           (double)actual_update_ms,
           (unsigned long)s_level_meter.update_blocks,
           (unsigned long)s_level_meter.sample_window,
           (double)s_level_meter.attack_ms,
           (double)s_level_meter.attack_coeff,
           (double)s_level_meter.release_ms,
           (double)s_level_meter.release_coeff);

#if defined(APP_ASRC_LED_FRAME_STRIDE) && (APP_ASRC_LED_FRAME_STRIDE > 1u)
    printf(" LED meter: sparse integer peak, coarse release hold=%u windows\n",
           (unsigned)LEVEL_METER_COARSE_RELEASE_HOLD);
#elif defined(ENA_LEVEL_METER_RELEASE_UNDERSHOOT)
    printf(" LED meter: release undershoot enabled %.2fdB\n",
           (double)LEVEL_METER_RELEASE_UNDERSHOOT_DB);
#else
    printf(" LED meter: release undershoot disabled\n");
#endif
}


/**
 * @brief Process channel-major audio level and update an 8-segment LED meter.
 *
 * This is the channel-major version.
 *
 * Input layout:
 *   input[0 * APP_BLOCK_FRAMES + n] = Left channel
 *   input[1 * APP_BLOCK_FRAMES + n] = Right channel
 *
 * Only the first two channels are used for the meter.
 *
 * @param input     Pointer to channel-major float audio buffer [ch][sample]
 */
void level_meter_process(const float* input)
{
    if ( input == NULL )
    {
        return;
    }

    const float* p_l = &input[0u * APP_BLOCK_FRAMES];
    const float* p_r = &input[1u * APP_BLOCK_FRAMES];

    for (size_t i = 0; i < APP_BLOCK_FRAMES; ++i)
    {
        const float l = *p_l++;
        const float r = *p_r++;

        s_level_meter.level_accum += level_meter_absf(l) +
                                     level_meter_absf(r);
    }

    level_meter_commit_block();
}


/**
 * @brief Submit one interleaved int32 codec buffer to the LED meter, described by its SHAPE.
 *
 * Shape (not a "which source" tag): `slots_per_frame` slots per frame, `frames` frames; the
 * metered stereo pair is slots 0/1 (L/R). Same int->float scaling as convert_codec_int_to_float
 * (Q31_SCALE_FLOAT * Pre_Gain_CODEC, 24-bit mask, clip) -- no float scratch / convert pass.
 *
 * The meter holds the MAX per-block level across an update window. Submitting more than one buffer
 * per block therefore makes the LED show the LOUDER of them: the ASRC BIDIR route submits both the
 * A output and the B output, and the bar tracks max(A, B) -- max is order-independent, so no
 * per-source state is needed. A one-way route submits its single output once per block. The two
 * ASRC RX-block ISRs share PRIO_TDM_DMA (never preempt), so the shared meter state stays serialized.
 *
 * @param buf             interleaved Q31 int32 codec buffer [frame][slot], slots 0/1 = L/R
 * @param slots_per_frame slots per frame (buffer stride): I2S=2, TDM8=8
 * @param frames          frames in this block
 */
void level_meter_process_i32(const int32_t* buf, uint16_t slots_per_frame, uint16_t frames)
{
    if ( ( buf == NULL ) || ( frames == 0u ) )
    {
        return;
    }

    const float scale = Q31_SCALE_FLOAT * Pre_Gain_CODEC;   // match convert_codec_int_to_float
    float       sum   = 0.0f;

    for (uint16_t n = 0; n < frames; ++n)
    {
        const int32_t* frame = &buf[(size_t)n * (size_t)slots_per_frame];

        // 24-bit mask + scale + clip, exactly as convert_codec_int_to_float does per sample.
        int32_t l_raw = frame[0] & 0xFFFFFF00UL;
        int32_t r_raw = frame[1] & 0xFFFFFF00UL;

        float l = (float)l_raw * scale;
        float r = (float)r_raw * scale;

        if (l < -1.0f) l = -1.0f; else if (l > 0.99999994f) l = 0.99999994f;
        if (r < -1.0f) r = -1.0f; else if (r > 0.99999994f) r = 0.99999994f;

        sum += level_meter_absf(l) + level_meter_absf(r);
    }

    // Per-block mean magnitude (same units as the LED thresholds), then hold the window MAX so
    // multiple submits per block resolve to the loudest.
    const float block_level = sum * ( 0.5f / (float)frames );
    level_meter_commit_i32_level(block_level);
}


void level_meter_process_i32_sparse(const int32_t* buf,
                                    uint16_t slots_per_frame,
                                    uint16_t frames,
                                    uint16_t frame_stride,
                                    uint16_t* frame_phase)
{
    if ( ( buf == NULL ) || ( frames == 0u ) ||
         ( slots_per_frame < 2u ) || ( frame_stride == 0u ) ||
         ( frame_phase == NULL ) )
    {
        return;
    }

    const uint16_t stride = (frame_stride < frames) ? frame_stride : frames;
    const uint16_t first = (*frame_phase < stride) ? *frame_phase : 0u;
    *frame_phase = (uint16_t)(first + 1u);
    if (*frame_phase >= stride) { *frame_phase = 0u; }

    uint32_t peak24 = 0u;
    for (uint16_t n = first; n < frames; n = (uint16_t)(n + stride))
    {
        const int32_t* frame = &buf[(size_t)n * (size_t)slots_per_frame];
        const int32_t l = frame[0] >> 8;
        const int32_t r = frame[1] >> 8;
        const uint32_t al = (uint32_t)((l < 0) ? -l : l);
        const uint32_t ar = (uint32_t)((r < 0) ? -r : r);
        const uint32_t stereo_mean = (al + ar + 1u) >> 1u;
        if (stereo_mean > peak24) { peak24 = stereo_mean; }
    }

    // Deliberately coarse: the display has only eight states of interest. Hold
    // the loudest sampled stereo frame until the next integer LED update.
    level_meter_commit_i32_q23(peak24);
}


/*
 * Sparse ASRC meter commit. This path intentionally uses LED segment counts as
 * its smoothing domain: attack follows the current peak immediately, while
 * release drops one of the eight segments after a fixed number of windows.
 * Consequently, the audio callbacks need no floating-point work at all.
 */
static void level_meter_commit_i32_q23(uint32_t block_peak_q23)
{
    if (block_peak_q23 > s_level_meter.meter_peak_q23)
    {
        s_level_meter.meter_peak_q23 = block_peak_q23;
    }

    if (++s_level_meter.coarse_block_count < s_level_meter.update_blocks)
    {
        return;
    }

    const uint8_t target_count =
        level_meter_make_count_q23(s_level_meter.meter_peak_q23);

    if (target_count >= s_level_meter.coarse_led_count)
    {
        s_level_meter.coarse_led_count = target_count;
        s_level_meter.coarse_release_hold = 0u;
    }
    else if (++s_level_meter.coarse_release_hold >= LEVEL_METER_COARSE_RELEASE_HOLD)
    {
        s_level_meter.coarse_led_count--;
        s_level_meter.coarse_release_hold = 0u;
    }

    const uint8_t mask = level_meter_count_to_mask(s_level_meter.coarse_led_count);
    if (mask != s_level_meter.last_led_mask)
    {
        LED_Set_Mask(mask);
        s_level_meter.last_led_mask = mask;
    }

    s_level_meter.meter_peak_q23 = 0u;
    s_level_meter.coarse_block_count = 0u;
}


static void level_meter_commit_i32_level(float block_level)
{
    if (block_level > s_level_meter.meter_peak) { s_level_meter.meter_peak = block_level; }

    if (++s_level_meter.block_count >= s_level_meter.update_blocks)
    {
        level_meter_apply_smoothing(&s_level_meter.smoothed_peak, s_level_meter.meter_peak);

        const uint8_t mask = level_meter_make_mask(s_level_meter.smoothed_peak);

        if (mask != s_level_meter.last_led_mask)
        {
            LED_Set_Mask(mask);
            s_level_meter.last_led_mask = mask;
        }

        s_level_meter.meter_peak  = 0.0f;
        s_level_meter.block_count = 0u;
    }
}


/*
 * Shared block-commit tail for level_meter_process / level_meter_process_i32: after a block's
 * |L|+|R| has been accumulated, count the block and -- once update_blocks have elapsed -- turn
 * the mean level into the smoothed peak and push the LED mask (only on change).
 */
static void level_meter_commit_block(void)
{
    s_level_meter.block_count++;

    if (s_level_meter.block_count >= s_level_meter.update_blocks)
    {
        const float mean_peak = s_level_meter.level_accum * s_level_meter.mean_scale;

        level_meter_apply_smoothing(&s_level_meter.smoothed_peak, mean_peak);

        const uint8_t mask = level_meter_make_mask(s_level_meter.smoothed_peak);

        if (mask != s_level_meter.last_led_mask)
        {
            LED_Set_Mask(mask);
            s_level_meter.last_led_mask = mask;
        }

        s_level_meter.level_accum = 0.0f;
        s_level_meter.block_count = 0u;
    }
}




//===========================================================
// Local Function
//===========================================================

static inline float level_meter_absf(float x)
{
    return (x < 0.0f) ? -x : x;
}


static inline uint8_t level_meter_make_mask(float level)
{
    if (level >= thresholds[7]) return 0xFFu;
    if (level >= thresholds[6]) return 0xFEu;
    if (level >= thresholds[5]) return 0xFCu;
    if (level >= thresholds[4]) return 0xF8u;
    if (level >= thresholds[3]) return 0xF0u;
    if (level >= thresholds[2]) return 0xE0u;
    if (level >= thresholds[1]) return 0xC0u;
    if (level >= thresholds[0]) return 0x80u;

    return 0x00u;
}


static inline uint8_t level_meter_make_count_q23(uint32_t level_q23)
{
    if (level_q23 >= s_level_meter.threshold_q23[7]) return 8u;
    if (level_q23 >= s_level_meter.threshold_q23[6]) return 7u;
    if (level_q23 >= s_level_meter.threshold_q23[5]) return 6u;
    if (level_q23 >= s_level_meter.threshold_q23[4]) return 5u;
    if (level_q23 >= s_level_meter.threshold_q23[3]) return 4u;
    if (level_q23 >= s_level_meter.threshold_q23[2]) return 3u;
    if (level_q23 >= s_level_meter.threshold_q23[1]) return 2u;
    if (level_q23 >= s_level_meter.threshold_q23[0]) return 1u;

    return 0u;
}


static inline uint8_t level_meter_count_to_mask(uint8_t count)
{
    if (count == 0u) { return 0x00u; }
    if (count >= LED_COUNT) { return 0xFFu; }

    return (uint8_t)(0xFFu << (LED_COUNT - count));
}


static uint32_t level_meter_calc_update_blocks(uint32_t sample_rate_hz,
                                               float    update_period_ms)
{
    float    target_blocks_f;
    uint32_t blocks;

    if (sample_rate_hz == 0u)
    {
        sample_rate_hz = SAMPLE_RATE;
    }

    if (update_period_ms <= 0.0f)
    {
        update_period_ms = LEVEL_METER_DEFAULT_UPDATE_MS;
    }

    /*
     * Required blocks:
     *   ceil((sample_rate_hz * update_period_ms) / (APP_BLOCK_FRAMES * 1000))
     *
     * Example:
     *   48 kHz, APP_BLOCK_FRAMES=32, 0.5ms -> 1 block  = 0.667ms actual
     *   96 kHz, APP_BLOCK_FRAMES=32, 0.5ms -> 2 blocks = 0.667ms actual
     */
    target_blocks_f = ((float)sample_rate_hz * update_period_ms) /
                      ((float)APP_BLOCK_FRAMES * 1000.0f);

    blocks = (uint32_t)target_blocks_f;

    if ((float)blocks < target_blocks_f)
    {
        blocks++;
    }

    if (blocks < LEVEL_METER_MIN_UPDATE_BLOCKS)
    {
        blocks = LEVEL_METER_MIN_UPDATE_BLOCKS;
    }

    return blocks;
}


static float level_meter_calc_smoothing_coeff(float actual_update_ms,
                                              float time_constant_ms)
{
    float coeff;

    if (actual_update_ms <= 0.0f)
    {
        actual_update_ms = LEVEL_METER_DEFAULT_UPDATE_MS;
    }

    if (time_constant_ms <= 0.0f)
    {
        return 1.0f;
    }

    /*
     * First-order smoothing coefficient:
     *   y += coeff * (x - y)
     *   coeff = 1 - exp(-dt / tau)
     *
     * The smoothing update happens only when the visible LED level is updated,
     * so dt must be the actual LED update period, not the audio block period.
     */
    coeff = 1.0f - expf(-actual_update_ms / time_constant_ms);

    if (coeff < 0.0f)
    {
        coeff = 0.0f;
    }
    else if (coeff > 1.0f)
    {
        coeff = 1.0f;
    }

    return coeff;
}


// Attack/release smoothing into *target (the single meter's smoothed_peak, or a per-side one
// for the BIDIR 2-source MAX). Uses the shared attack/release coefficients.
static void level_meter_apply_smoothing(float* target, float mean_peak)
{
    if (mean_peak > *target)
    {
        /*
         * Rising:
         * Follow the real measured level normally.
         */
        *target += s_level_meter.attack_coeff * (mean_peak - *target);
    }
    else
    {
        float release_target = mean_peak;

#if defined(ENA_LEVEL_METER_RELEASE_UNDERSHOOT)
        /*
         * Falling:
         * Aim below the real measured level to create a stronger animated bounce.
         *
         * Example:
         *   mean_peak = 0.10
         *   undershoot = -6 dB -> gain ~= 0.501
         *   release_target ~= 0.05
         *
         * Once smoothed_peak falls below mean_peak, the next update uses the
         * normal attack path again. This creates intentional up/down movement.
         */
        release_target = mean_peak * db_to_lin(LEVEL_METER_RELEASE_UNDERSHOOT_DB);
#endif //defined(ENA_LEVEL_METER_RELEASE_UNDERSHOOT)

        *target += s_level_meter.release_coeff * (release_target - *target);

        if (*target < 0.0f)
        {
            *target = 0.0f;
        }
    }
}


static void level_meter_reset_runtime_state(void)
{
    s_level_meter.smoothed_peak = 0.0f;
    s_level_meter.level_accum   = 0.0f;
    s_level_meter.block_count   = 0u;
    s_level_meter.last_led_mask = 0xFFu;   // force first LED update

    s_level_meter.meter_peak          = 0.0f; // int32 shape-based meter window max-hold
    s_level_meter.meter_peak_q23      = 0u;
    s_level_meter.coarse_block_count  = 0u;
    s_level_meter.coarse_led_count    = 0u;
    s_level_meter.coarse_release_hold = 0u;
}
