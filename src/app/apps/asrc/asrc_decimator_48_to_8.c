#include "asrc_decimator_48_to_8.h"

#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_ASRC
#  error "asrc_decimator_48_to_8.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#if APP_ASRC_48K_TO_8_DECIMATOR || APP_ASRC_RUNTIME_48K_TO_8

#include <string.h>

#include "asrc_decimator_48_to_8_coeffs.inc"
#include "asrc_decimator_48_to_16_coeffs.inc"
#include "asrc_decimator_48_to_24_coeffs.inc"
#include "asrc_decimator_48_to_12_coeffs.inc"
#include "asrc_decimator_48_to_32_coeffs.inc"
#if ASRC_DECIMATOR_HAS_96_TO_48   /* pre-stage coefficients: only where leg A can be 96 kHz */
#include "asrc_decimator_96_to_48_coeffs.inc"
#endif /* ASRC_DECIMATOR_HAS_96_TO_48 */

_Static_assert(ASRC_DECIMATOR_STAGE1_TAPS == ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS,
               "stage 1 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_STAGE2_TAPS == ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS,
               "stage 2 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_COEFF_CRC32 == ASRC_DECIMATOR_48_TO_8_COEFF_CRC32,
               "coefficient CRC metadata mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_16_COEFF_TAPS == ASRC_DECIMATOR_48_TO_16_TAPS,
               "48-to-16 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_16_GENERATED_CRC32 == ASRC_DECIMATOR_48_TO_16_COEFF_CRC32,
               "48-to-16 coefficient CRC metadata mismatch");
_Static_assert((ASRC_DECIMATOR_48_TO_16_TAPS & 1u) != 0u,
               "symmetric FIR helper requires an odd tap count");
_Static_assert(ASRC_DECIMATOR_48_TO_24_COEFF_TAPS == ASRC_DECIMATOR_48_TO_24_TAPS,
               "48-to-24 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_24_GENERATED_CRC32 == ASRC_DECIMATOR_48_TO_24_COEFF_CRC32,
               "48-to-24 coefficient CRC metadata mismatch");
_Static_assert((ASRC_DECIMATOR_48_TO_24_TAPS & 1u) != 0u,
               "symmetric FIR helper requires an odd tap count");
/* The 22.05 kHz variant shares the tap count above -- that sharing is the whole reason it needs
 * no second history layout, no second union member and no second code path, so pin it here
 * rather than leaving it as an unstated assumption of process_48_to_24(). */
_Static_assert(sizeof(s_48_to_24_out22k_coeff) == sizeof(s_48_to_24_coeff),
               "both /2 variants must share the tap count");
_Static_assert(ASRC_DECIMATOR_48_TO_24_OUT22K_GENERATED_CRC32 ==
                   ASRC_DECIMATOR_48_TO_24_OUT22K_COEFF_CRC32,
               "48-to-24 22.05kHz-variant coefficient CRC metadata mismatch");
/* The /6 chain folds too (both stages are odd-length and their generated coefficient
 * arrays are bit-exactly symmetric -- checked against the design script output). */
_Static_assert((ASRC_DECIMATOR_STAGE1_TAPS & 1u) != 0u,
               "stage 1 folding requires an odd tap count");
_Static_assert((ASRC_DECIMATOR_STAGE2_TAPS & 1u) != 0u,
               "stage 2 folding requires an odd tap count");
_Static_assert(ASRC_DECIMATOR_48_TO_12_STAGE1_COEFF_TAPS == ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS,
               "48-to-12 stage1 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_12_STAGE2_COEFF_TAPS == ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS,
               "48-to-12 stage2 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_12_GENERATED_CRC32 == ASRC_DECIMATOR_48_TO_12_COEFF_CRC32,
               "48-to-12 coefficient CRC metadata mismatch");
_Static_assert((ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS & 1u) != 0u,
               "48-to-12 stage 1 folding requires an odd tap count");
_Static_assert((ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS & 1u) != 0u,
               "48-to-12 stage 2 folding requires an odd tap count");
/* The 12 kHz variant shares the tap counts above -- that sharing is the whole reason it needs
 * no second history layout, no second union member and no second code path, so pin it here
 * rather than leaving it as an unstated assumption of process_48_to_12(). */
_Static_assert(sizeof(s_48_to_12_out12k_stage1_coeff) == sizeof(s_48_to_12_stage1_coeff),
               "both /4 variants must share stage-1 tap count");
_Static_assert(sizeof(s_48_to_12_out12k_stage2_coeff) == sizeof(s_48_to_12_stage2_coeff),
               "both /4 variants must share stage-2 tap count");
_Static_assert(ASRC_DECIMATOR_48_TO_12_OUT12K_GENERATED_CRC32 ==
                   ASRC_DECIMATOR_48_TO_12_OUT12K_COEFF_CRC32,
               "48-to-12 12kHz-variant coefficient CRC metadata mismatch");
/* 48 -> 32 kHz AUDIO MODE (L=2/M=3, N=97).  The two rows are GENERATED, and the CRC32 below is
 * what makes a hand edit of the include a build failure rather than a quiet change of filter --
 * the same pinning every chain above gets.  The tap split is pinned too: 97 = 49 + 48 is not a
 * free choice, it is the even/odd decomposition of the prototype, and a row count that drifts
 * from it would still compile and would still produce audio, just not this filter's audio. */
_Static_assert(ASRC_DECIMATOR_48_TO_32_PROTO_COEFF_TAPS == ASRC_DECIMATOR_48_TO_32_PROTO_TAPS,
               "48-to-32 prototype tap count mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_32_PHASE0_COEFF_TAPS == ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS,
               "48-to-32 phase-0 tap count mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_32_PHASE1_COEFF_TAPS == ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS,
               "48-to-32 phase-1 tap count mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_32_GENERATED_CRC32 == ASRC_DECIMATOR_48_TO_32_COEFF_CRC32,
               "48-to-32 coefficient CRC metadata mismatch");
_Static_assert((ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS + ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS) ==
                   ASRC_DECIMATOR_48_TO_32_PROTO_TAPS,
               "the two L=2 rows must together be the whole prototype");
_Static_assert(sizeof(s_48_to_32_phase0_coeff) / sizeof(s_48_to_32_phase0_coeff[0]) ==
                   ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS,
               "48-to-32 phase-0 array length mismatch");
_Static_assert(sizeof(s_48_to_32_phase1_coeff) / sizeof(s_48_to_32_phase1_coeff[0]) ==
                   ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS,
               "48-to-32 phase-1 array length mismatch");
_Static_assert(ASRC_DECIMATOR_48_TO_32_L == 2u, "the rational front end is L=2");
_Static_assert(ASRC_DECIMATOR_48_TO_32_M == 3u, "the rational front end is M=3");
#if ASRC_DECIMATOR_HAS_96_TO_48   /* pre-stage generated-vs-header pinning */
_Static_assert(ASRC_DECIMATOR_96_TO_48_COEFF_TAPS == ASRC_DECIMATOR_96_TO_48_TAPS,
               "96-to-48 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_96_TO_48_GENERATED_CRC32 == ASRC_DECIMATOR_96_TO_48_COEFF_CRC32,
               "96-to-48 coefficient CRC metadata mismatch");
_Static_assert((ASRC_DECIMATOR_96_TO_48_TAPS & 1u) != 0u,
               "symmetric FIR helper requires an odd tap count");
/* The two wide variants.  Pinned here even though the FLOAT implementation below never loads
 * them: this file is where generated-vs-header agreement is checked for every set in the .inc,
 * and a set whose CRC nobody pins is a set that can drift silently. */
_Static_assert(ASRC_DECIMATOR_96_TO_48_OUT44K1_COEFF_TAPS == ASRC_DECIMATOR_96_TO_48_OUT44K1_TAPS,
               "96-to-48 FOR_44100 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_96_TO_48_OUT44K1_GENERATED_CRC32 ==
                   ASRC_DECIMATOR_96_TO_48_OUT44K1_COEFF_CRC32,
               "96-to-48 FOR_44100 coefficient CRC metadata mismatch");
_Static_assert(ASRC_DECIMATOR_96_TO_48_OUT48K_COEFF_TAPS == ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS,
               "96-to-48 FOR_48000 coefficient count mismatch");
_Static_assert(ASRC_DECIMATOR_96_TO_48_OUT48K_GENERATED_CRC32 ==
                   ASRC_DECIMATOR_96_TO_48_OUT48K_COEFF_CRC32,
               "96-to-48 FOR_48000 coefficient CRC metadata mismatch");
_Static_assert(((ASRC_DECIMATOR_96_TO_48_OUT44K1_TAPS & 1u) != 0u) &&
                   ((ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS & 1u) != 0u),
               "every pre-stage variant must have an odd tap count");
_Static_assert(ASRC_DECIMATOR_96_TO_48_TAPS_MAX == ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS,
               "the widest pre-stage variant is the 48 kHz one; storage is sized by TAPS_MAX");
#endif /* ASRC_DECIMATOR_HAS_96_TO_48 */

/*
 * Odd-length linear-phase FIR over a MIRRORED history (see the header): the
 * window is contiguous, so the two symmetric walkers are plain post-increment /
 * post-decrement pointers -- no per-tap index compare, no wrap branch.
 *
 * `window` points at the OLDEST of the taps samples (= &history[write], the
 * mirror guarantees write+taps-1 is in range).  Pair symmetric samples so an
 * N-tap filter uses (N+1)/2 multiplies.
 *
 * The accumulation order is identical to the original modulo-ring loop
 * (ascending coefficient index, `sum += (newest + oldest) * c`, centre tap
 * last), so results are bit-exact against it -- asrc_decimator_selftest()
 * checks exactly that.
 */
#define DEC_STRIDE ASRC_DECIMATOR_FLOAT_MAX_CHANNELS   /* history frame stride */

static float fir_symmetric_win(const float* window,
                               const float* coeff,
                               uint16_t taps)
{
    const float* pn = window + ((size_t)(taps - 1u) * DEC_STRIDE); /* newest */
    const float* po = window;                                      /* oldest */
    float sum = 0.0f;
    uint16_t pairs = (uint16_t)(taps >> 1u);

    while (pairs-- != 0u)
    {
        sum += (*pn + *po) * *coeff++;
        pn -= DEC_STRIDE;
        po += DEC_STRIDE;
    }
    return sum + (*pn * *coeff);              /* centre tap */
}

/*
 * Two channels in one pass.  The coefficient load, the loop counter, the branch
 * AND the address arithmetic are all shared: the interleaved history puts both
 * channels of a frame side by side, so one ascending and one descending frame
 * pointer cover both, with the channel picked by a constant element offset.
 *
 * Bit-exact with two fir_symmetric_win() calls -- the per-channel sequence of
 * float operations is untouched, only independent channels are interleaved.
 */
static void fir_symmetric_win2(const float* window,
                               const float* coeff,
                               uint16_t taps,
                               float* out0,
                               float* out1)
{
    const float* po = window;                                      /* oldest frame */
    const float* pn = window + ((size_t)(taps - 1u) * DEC_STRIDE);  /* newest frame */
    float s0 = 0.0f;
    float s1 = 0.0f;
    uint16_t pairs = (uint16_t)(taps >> 1u);

    while (pairs-- != 0u)
    {
        const float c = *coeff++;
        s0 += (pn[0] + po[0]) * c;
        s1 += (pn[1] + po[1]) * c;
        po += DEC_STRIDE;
        pn -= DEC_STRIDE;
    }
    *out0 = s0 + (pn[0] * *coeff);            /* centre tap */
    *out1 = s1 + (pn[1] * *coeff);
}

static int32_t float_to_s24_left(float value)
{
    if (value > 8388607.0f)
    {
        value = 8388607.0f;
    }
    else if (value < -8388608.0f)
    {
        value = -8388608.0f;
    }
    return (int32_t)((uint32_t)(int32_t)value << 8);
}

bool asrc_decimator_48_to_8_init(asrc_decimator_48_to_8_t* state,
                                 uint8_t channels)
{
    if ((state == NULL) || (channels == 0u) ||
        (channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS))
    {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->channels = channels;
    return true;
}

size_t asrc_decimator_48_to_8_output_frames(
    const asrc_decimator_48_to_8_t* state,
    size_t input_frames)
{
    if ((state == NULL) || (state->channels == 0u))
    {
        return 0u;
    }

    const size_t stage1_frames =
        ((size_t)state->stage1_phase + input_frames) / 3u;
    return ((size_t)state->stage2_phase + stage1_frames) / 2u;
}

/*
 * Sample format of the caller's interleaved buffers.  Passed as a value, not as
 * reader/writer function pointers: the old indirection cost one indirect call
 * per sample per channel inside leg A's RX ISR.
 */
typedef enum
{
    FMT_F32      = 0,
    FMT_S24_LEFT = 1
} sample_fmt_t;

static inline float read_sample(const void* input, size_t index, sample_fmt_t fmt)
{
    return (fmt == FMT_S24_LEFT) ? (float)(((const int32_t*)input)[index] >> 8)
                                 : ((const float*)input)[index];
}

static inline void write_sample(void* output, size_t index, float value,
                                sample_fmt_t fmt)
{
    if (fmt == FMT_S24_LEFT)
    {
        ((int32_t*)output)[index] = float_to_s24_left(value);
    }
    else
    {
        ((float*)output)[index] = value;
    }
}

/*
 * Mirrored-ring push of one channel of one frame: store twice, so the newest
 * `taps` frames stay contiguous.  `hist` is the interleaved history base,
 * `write` the frame index in [0, taps).
 */
static inline void hist_push(float* hist, uint16_t taps, uint16_t write,
                             uint8_t channel, float x)
{
    const size_t i = ((size_t)write * DEC_STRIDE) + channel;
    hist[i] = x;
    hist[i + ((size_t)taps * DEC_STRIDE)] = x;
}

/* Base of the taps-frame window that starts at frame index `write`. */
static inline float* hist_window(float* hist, uint16_t write)
{
    return hist + ((size_t)write * DEC_STRIDE);
}

static bool process(asrc_decimator_48_to_8_t* state,
                    const void* input,
                    size_t input_frames,
                    size_t input_stride,
                    void* output,
                    size_t output_capacity_frames,
                    size_t output_stride,
                    size_t* output_frames,
                    sample_fmt_t fmt)
{
    if (output_frames != NULL)
    {
        *output_frames = 0u;
    }
    if ((state == NULL) || (state->channels == 0u) ||
        (state->channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS) ||
        (output_frames == NULL) ||
        ((input_frames != 0u) && (input == NULL)) ||
        (input_stride < state->channels) ||
        (output_stride < state->channels))
    {
        return false;
    }

    const size_t required =
        asrc_decimator_48_to_8_output_frames(state, input_frames);
    if ((required > output_capacity_frames) ||
        ((required != 0u) && (output == NULL)))
    {
        return false;
    }

    size_t produced = 0u;
    const uint8_t nch = state->channels;
    for (size_t frame = 0u; frame < input_frames; ++frame)
    {
        const size_t in_base = frame * input_stride;
        for (uint8_t channel = 0u; channel < nch; ++channel)
        {
            hist_push(state->stage1_history,
                      ASRC_DECIMATOR_STAGE1_TAPS,
                      state->stage1_write, channel,
                      read_sample(input, in_base + channel, fmt));
        }
        ++state->stage1_write;
        if (state->stage1_write == ASRC_DECIMATOR_STAGE1_TAPS)
        {
            state->stage1_write = 0u;
        }

        ++state->stage1_phase;
        if (state->stage1_phase != 3u)
        {
            continue;
        }
        state->stage1_phase = 0u;

        /* Window base = the (already wrapped) write index; the mirror makes the
         * taps samples from there contiguous and in time order. */
        const float* const w1 = hist_window(state->stage1_history,
                                            state->stage1_write);
        if (nch == 2u)
        {
            float y0;
            float y1;
            fir_symmetric_win2(w1, s_stage1_coeff,
                               ASRC_DECIMATOR_STAGE1_TAPS, &y0, &y1);
            hist_push(state->stage2_history, ASRC_DECIMATOR_STAGE2_TAPS,
                      state->stage2_write, 0u, y0);
            hist_push(state->stage2_history, ASRC_DECIMATOR_STAGE2_TAPS,
                      state->stage2_write, 1u, y1);
        }
        else
        {
            for (uint8_t channel = 0u; channel < nch; ++channel)
            {
                hist_push(state->stage2_history,
                          ASRC_DECIMATOR_STAGE2_TAPS,
                          state->stage2_write, channel,
                          fir_symmetric_win(w1 + channel, s_stage1_coeff,
                                            ASRC_DECIMATOR_STAGE1_TAPS));
            }
        }
        ++state->stage2_write;
        if (state->stage2_write == ASRC_DECIMATOR_STAGE2_TAPS)
        {
            state->stage2_write = 0u;
        }

        ++state->stage2_phase;
        if (state->stage2_phase != 2u)
        {
            continue;
        }
        state->stage2_phase = 0u;

        const size_t out_base = produced * output_stride;
        const float* const w2 = hist_window(state->stage2_history,
                                            state->stage2_write);
        if (nch == 2u)
        {
            float y0;
            float y1;
            fir_symmetric_win2(w2, s_stage2_coeff,
                               ASRC_DECIMATOR_STAGE2_TAPS, &y0, &y1);
            write_sample(output, out_base + 0u, y0, fmt);
            write_sample(output, out_base + 1u, y1, fmt);
        }
        else
        {
            for (uint8_t channel = 0u; channel < nch; ++channel)
            {
                write_sample(output, out_base + channel,
                             fir_symmetric_win(w2 + channel, s_stage2_coeff,
                                               ASRC_DECIMATOR_STAGE2_TAPS),
                             fmt);
            }
        }
        ++produced;
    }

    state->input_frames += input_frames;
    state->output_frames += produced;
    *output_frames = produced;
    return true;
}

bool asrc_decimator_48_to_8_process_f32(
    asrc_decimator_48_to_8_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process(state, input, input_frames, input_stride, output,
                   output_capacity_frames, output_stride, output_frames,
                   FMT_F32);
}

bool asrc_decimator_48_to_8_process_s24_left(
    asrc_decimator_48_to_8_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process(state, input, input_frames, input_stride, output,
                   output_capacity_frames, output_stride, output_frames,
                   FMT_S24_LEFT);
}

bool asrc_decimator_48_to_16_init(asrc_decimator_48_to_16_t* state,
                                  uint8_t channels)
{
    if ((state == NULL) || (channels == 0u) ||
        (channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS))
    {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->channels = channels;
    return true;
}

size_t asrc_decimator_48_to_16_output_frames(
    const asrc_decimator_48_to_16_t* state,
    size_t input_frames)
{
    if ((state == NULL) || (state->channels == 0u))
    {
        return 0u;
    }
    return ((size_t)state->phase + input_frames) / 3u;
}

static bool process_48_to_16(asrc_decimator_48_to_16_t* state,
                             const void* input,
                             size_t input_frames,
                             size_t input_stride,
                             void* output,
                             size_t output_capacity_frames,
                             size_t output_stride,
                             size_t* output_frames,
                             sample_fmt_t fmt)
{
    if (output_frames != NULL)
    {
        *output_frames = 0u;
    }
    if ((state == NULL) || (state->channels == 0u) ||
        (state->channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS) ||
        (output_frames == NULL) ||
        ((input_frames != 0u) && (input == NULL)) ||
        (input_stride < state->channels) ||
        (output_stride < state->channels))
    {
        return false;
    }

    const size_t required =
        asrc_decimator_48_to_16_output_frames(state, input_frames);
    if ((required > output_capacity_frames) ||
        ((required != 0u) && (output == NULL)))
    {
        return false;
    }

    size_t produced = 0u;
    const uint8_t nch = state->channels;
    for (size_t frame = 0u; frame < input_frames; ++frame)
    {
        const size_t in_base = frame * input_stride;
        for (uint8_t channel = 0u; channel < nch; ++channel)
        {
            hist_push(state->history,
                      ASRC_DECIMATOR_48_TO_16_TAPS,
                      state->write, channel,
                      read_sample(input, in_base + channel, fmt));
        }
        ++state->write;
        if (state->write == ASRC_DECIMATOR_48_TO_16_TAPS)
        {
            state->write = 0u;
        }

        ++state->phase;
        if (state->phase != 3u)
        {
            continue;
        }
        state->phase = 0u;

        const size_t out_base = produced * output_stride;
        const float* const win = hist_window(state->history, state->write);
        /* ONE stage, and that is forced.  This 3:1 decimation folds at multiples of 16 kHz and
         * the 16 kHz output's Nyquist IS the intermediate's, so every input above 8000 Hz lands
         * inside the final band during this very decimation -- a stage placed after it cannot
         * separate what has already been summed into one bin.  (Measured: relaxing this filter
         * and adding a 75-tap "repair" stage at 16 kHz moves the worst alias -14.8 -> -21.2 dB.
         * Report section 12.)  Hence ASRC_DECIMATOR_48_TO_16_TAPS taps here (161), with the
         * stopband pinned on 8000 Hz.  157 was the first candidate; see the study document's
         * "why 161 taps, not 157" section for the pitfall that forced the extra 4. */
        if (nch == 2u)
        {
            float y0;
            float y1;
            fir_symmetric_win2(win, s_48_to_16_coeff,
                               ASRC_DECIMATOR_48_TO_16_TAPS, &y0, &y1);
            write_sample(output, out_base + 0u, y0, fmt);
            write_sample(output, out_base + 1u, y1, fmt);
        }
        else
        {
            for (uint8_t channel = 0u; channel < nch; ++channel)
            {
                write_sample(output, out_base + channel,
                             fir_symmetric_win(win + channel, s_48_to_16_coeff,
                                               ASRC_DECIMATOR_48_TO_16_TAPS),
                             fmt);
            }
        }
        ++produced;
    }

    state->input_frames += input_frames;
    state->output_frames += produced;
    *output_frames = produced;
    return true;
}

bool asrc_decimator_48_to_16_process_f32(
    asrc_decimator_48_to_16_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_48_to_16(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_F32);
}

bool asrc_decimator_48_to_16_process_s24_left(
    asrc_decimator_48_to_16_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_48_to_16(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_S24_LEFT);
}

#if ASRC_DECIMATOR_HAS_96_TO_48   /* pre-stage implementation */
bool asrc_decimator_96_to_48_init(asrc_decimator_96_to_48_t* state,
                                  uint8_t channels)
{
    if ((state == NULL) || (channels == 0u) ||
        (channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS))
    {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->channels = channels;
    return true;
}

size_t asrc_decimator_96_to_48_output_frames(
    const asrc_decimator_96_to_48_t* state,
    size_t input_frames)
{
    if ((state == NULL) || (state->channels == 0u))
    {
        return 0u;
    }
    return ((size_t)state->phase + input_frames) / 2u;
}

/* Its own function rather than a divisor argument added to process_48_to_16(), for the reason
 * spelled out above process_48_to_12(): the decimation factor and tap count are compile-time
 * literals in the pointer walk, and turning them into arguments would change codegen for the
 * shipping paths that tools/asrc/hotpath_invariance.py holds byte-identical. */
static bool process_96_to_48(asrc_decimator_96_to_48_t* state,
                             const void* input,
                             size_t input_frames,
                             size_t input_stride,
                             void* output,
                             size_t output_capacity_frames,
                             size_t output_stride,
                             size_t* output_frames,
                             sample_fmt_t fmt)
{
    if (output_frames != NULL)
    {
        *output_frames = 0u;
    }
    if ((state == NULL) || (state->channels == 0u) ||
        (state->channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS) ||
        (output_frames == NULL) ||
        ((input_frames != 0u) && (input == NULL)) ||
        (input_stride < state->channels) ||
        (output_stride < state->channels))
    {
        return false;
    }

    const size_t required =
        asrc_decimator_96_to_48_output_frames(state, input_frames);
    if ((required > output_capacity_frames) ||
        ((required != 0u) && (output == NULL)))
    {
        return false;
    }

    size_t produced = 0u;
    const uint8_t nch = state->channels;
    for (size_t frame = 0u; frame < input_frames; ++frame)
    {
        const size_t in_base = frame * input_stride;
        for (uint8_t channel = 0u; channel < nch; ++channel)
        {
            hist_push(state->history,
                      ASRC_DECIMATOR_96_TO_48_TAPS,
                      state->write, channel,
                      read_sample(input, in_base + channel, fmt));
        }
        ++state->write;
        if (state->write == ASRC_DECIMATOR_96_TO_48_TAPS)
        {
            state->write = 0u;
        }

        ++state->phase;
        if (state->phase != 2u)
        {
            continue;
        }
        state->phase = 0u;

        const size_t out_base = produced * output_stride;
        const float* const win = hist_window(state->history, state->write);
        /* Only 21 taps, and that is NOT the /2 shape used at 48 kHz input (107 taps).  This stage
         * is the FIRST of a composed chain, so like the /4 chain's 27-tap first stage it only has
         * to suppress what would fold into the FINAL band after the remaining decimation -- from
         * 48000 - final_Nyquist upward, an enormous transition width.  One shared coefficient set
         * covers all four served rates, hence a direct array reference here rather than the
         * coefficient pointer the /2 and /4 stages need.  See the header's tap-count block. */
        if (nch == 2u)
        {
            float y0;
            float y1;
            fir_symmetric_win2(win, s_96_to_48_coeff,
                               ASRC_DECIMATOR_96_TO_48_TAPS, &y0, &y1);
            write_sample(output, out_base + 0u, y0, fmt);
            write_sample(output, out_base + 1u, y1, fmt);
        }
        else
        {
            for (uint8_t channel = 0u; channel < nch; ++channel)
            {
                write_sample(output, out_base + channel,
                             fir_symmetric_win(win + channel, s_96_to_48_coeff,
                                               ASRC_DECIMATOR_96_TO_48_TAPS),
                             fmt);
            }
        }
        ++produced;
    }

    state->input_frames += input_frames;
    state->output_frames += produced;
    *output_frames = produced;
    return true;
}

bool asrc_decimator_96_to_48_process_f32(
    asrc_decimator_96_to_48_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_96_to_48(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_F32);
}

bool asrc_decimator_96_to_48_process_s24_left(
    asrc_decimator_96_to_48_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_96_to_48(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_S24_LEFT);
}
#endif /* ASRC_DECIMATOR_HAS_96_TO_48 */

bool asrc_decimator_48_to_24_init(asrc_decimator_48_to_24_t* state,
                                  uint8_t channels,
                                  asrc_decimator_48_to_24_variant_t variant)
{
    if ((state == NULL) || (channels == 0u) ||
        (channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS))
    {
        return false;
    }
    /* Reject an unknown variant rather than defaulting to one, exactly as the /4 init does: a
     * future rate added to the routing gate without its own coefficient set must fail loudly at
     * init (the caller leaves `ready` clear and the block ISR drops) rather than filtering with
     * band edges designed for a different Nyquist. */
    const float* coeff;
    switch (variant)
    {
    case ASRC_DECIMATOR_48_TO_24_FOR_24000:
        coeff = s_48_to_24_coeff;
        break;
    case ASRC_DECIMATOR_48_TO_24_FOR_22050:
        coeff = s_48_to_24_out22k_coeff;
        break;
    default:
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->channels = channels;
    state->coeff = coeff;
    return true;
}

size_t asrc_decimator_48_to_24_output_frames(
    const asrc_decimator_48_to_24_t* state,
    size_t input_frames)
{
    if ((state == NULL) || (state->channels == 0u))
    {
        return 0u;
    }
    return ((size_t)state->phase + input_frames) / 2u;
}

static bool process_48_to_24(asrc_decimator_48_to_24_t* state,
                             const void* input,
                             size_t input_frames,
                             size_t input_stride,
                             void* output,
                             size_t output_capacity_frames,
                             size_t output_stride,
                             size_t* output_frames,
                             sample_fmt_t fmt)
{
    if (output_frames != NULL)
    {
        *output_frames = 0u;
    }
    if ((state == NULL) || (state->channels == 0u) ||
        (state->channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS) ||
        (state->coeff == NULL) ||
        (output_frames == NULL) ||
        ((input_frames != 0u) && (input == NULL)) ||
        (input_stride < state->channels) ||
        (output_stride < state->channels))
    {
        return false;
    }

    const size_t required =
        asrc_decimator_48_to_24_output_frames(state, input_frames);
    if ((required > output_capacity_frames) ||
        ((required != 0u) && (output == NULL)))
    {
        return false;
    }

    size_t produced = 0u;
    const uint8_t nch = state->channels;
    /* Hoisted OUT of the frame loop, like the /4 chain's c1/c2: the variant is fixed for the
     * lifetime of the instance, so selecting a coefficient set costs one load per call and the
     * tap loop below keeps its compile-time literal tap count. */
    const float* const c = state->coeff;
    for (size_t frame = 0u; frame < input_frames; ++frame)
    {
        const size_t in_base = frame * input_stride;
        for (uint8_t channel = 0u; channel < nch; ++channel)
        {
            hist_push(state->history,
                      ASRC_DECIMATOR_48_TO_24_TAPS,
                      state->write, channel,
                      read_sample(input, in_base + channel, fmt));
        }
        ++state->write;
        if (state->write == ASRC_DECIMATOR_48_TO_24_TAPS)
        {
            state->write = 0u;
        }

        ++state->phase;
        if (state->phase != 2u)
        {
            continue;
        }
        state->phase = 0u;

        const size_t out_base = produced * output_stride;
        const float* const win = hist_window(state->history, state->write);
        /* ONE stage, and NOT a half-band, both forced.  This 2:1 decimation folds at multiples
         * of 24 kHz and the 24 kHz output's Nyquist IS the intermediate's, so every input above
         * 12000 Hz lands inside the final band during this very decimation -- a stage placed
         * after it cannot separate what has already been summed into one bin (report section
         * 12).  A half-band would need its stopband ABOVE 12000 Hz with the resampler covering
         * the gap, and measured that costs over 401 taps instead of saving half.  Hence 107
         * full taps with the stopband pinned on the OUTPUT Nyquist -- 12000 Hz for a 24 kHz
         * output, 11025 Hz for a 22.05 kHz one, which is the whole difference between the two
         * variants `c` selects between. */
        if (nch == 2u)
        {
            float y0;
            float y1;
            fir_symmetric_win2(win, c,
                               ASRC_DECIMATOR_48_TO_24_TAPS, &y0, &y1);
            write_sample(output, out_base + 0u, y0, fmt);
            write_sample(output, out_base + 1u, y1, fmt);
        }
        else
        {
            for (uint8_t channel = 0u; channel < nch; ++channel)
            {
                write_sample(output, out_base + channel,
                             fir_symmetric_win(win + channel, c,
                                               ASRC_DECIMATOR_48_TO_24_TAPS),
                             fmt);
            }
        }
        ++produced;
    }

    state->input_frames += input_frames;
    state->output_frames += produced;
    *output_frames = produced;
    return true;
}

bool asrc_decimator_48_to_24_process_f32(
    asrc_decimator_48_to_24_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_48_to_24(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_F32);
}

bool asrc_decimator_48_to_24_process_s24_left(
    asrc_decimator_48_to_24_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_48_to_24(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_S24_LEFT);
}

bool asrc_decimator_48_to_12_init(asrc_decimator_48_to_12_t* state,
                                  uint8_t channels,
                                  asrc_decimator_48_to_12_variant_t variant)
{
    if ((state == NULL) || (channels == 0u) ||
        (channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS))
    {
        return false;
    }
    /* Reject an unknown variant rather than defaulting to one: a future rate added to the
     * routing gate without its own coefficient set must fail loudly at init (the caller
     * leaves `ready` clear and the block ISR drops rather than filtering with band edges
     * designed for a different Nyquist). */
    const float* stage1_coeff;
    const float* stage2_coeff;
    switch (variant)
    {
    case ASRC_DECIMATOR_48_TO_12_FOR_11025:
        stage1_coeff = s_48_to_12_stage1_coeff;
        stage2_coeff = s_48_to_12_stage2_coeff;
        break;
    case ASRC_DECIMATOR_48_TO_12_FOR_12000:
        stage1_coeff = s_48_to_12_out12k_stage1_coeff;
        stage2_coeff = s_48_to_12_out12k_stage2_coeff;
        break;
    default:
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->channels = channels;
    state->stage1_coeff = stage1_coeff;
    state->stage2_coeff = stage2_coeff;
    return true;
}

size_t asrc_decimator_48_to_12_output_frames(
    const asrc_decimator_48_to_12_t* state,
    size_t input_frames)
{
    if ((state == NULL) || (state->channels == 0u))
    {
        return 0u;
    }

    const size_t stage1_frames =
        ((size_t)state->stage1_phase + input_frames) / 2u;
    return ((size_t)state->stage2_phase + stage1_frames) / 2u;
}

/*
 * Structurally identical to process() above -- two rate-changing stages over
 * mirrored histories -- with both decimation factors 2 instead of 3 and 2, and
 * the 48->12 coefficient sets.  Kept as a separate function rather than
 * parameterizing process(): the decimation factors and tap counts are compile-time
 * literals there, and turning them into arguments would change the code generated
 * for the shipping /6 path, which tools/asrc/hotpath_invariance.py requires to
 * stay byte-identical.  process_48_to_16() is a separate function for the same
 * reason.
 *
 * The COEFFICIENTS, unlike the tap counts, do come from the state (2026-07-29): one function
 * serves both the 11.025 kHz and the 12 kHz output rates.  The two base pointers are hoisted
 * into locals below, so the per-tap loop is unchanged and the only added work is one pointer
 * load per stage invocation -- outside the tap loop, in a function that already dereferences
 * `state` a dozen times.  Making the tap counts dynamic too would have cost real time (the
 * mirrored-window walk and the fold-point arithmetic both fold to constants today), which is
 * why the two variants were designed to SHARE tap counts instead.
 */
static bool process_48_to_12(asrc_decimator_48_to_12_t* state,
                             const void* input,
                             size_t input_frames,
                             size_t input_stride,
                             void* output,
                             size_t output_capacity_frames,
                             size_t output_stride,
                             size_t* output_frames,
                             sample_fmt_t fmt)
{
    if (output_frames != NULL)
    {
        *output_frames = 0u;
    }
    if ((state == NULL) || (state->channels == 0u) ||
        (state->channels > ASRC_DECIMATOR_FLOAT_MAX_CHANNELS) ||
        (state->stage1_coeff == NULL) || (state->stage2_coeff == NULL) ||
        (output_frames == NULL) ||
        ((input_frames != 0u) && (input == NULL)) ||
        (input_stride < state->channels) ||
        (output_stride < state->channels))
    {
        return false;
    }

    const size_t required =
        asrc_decimator_48_to_12_output_frames(state, input_frames);
    if ((required > output_capacity_frames) ||
        ((required != 0u) && (output == NULL)))
    {
        return false;
    }

    size_t produced = 0u;
    const uint8_t nch = state->channels;
    /* Hoisted out of the frame loop: which variant this instance runs is fixed at init. */
    const float* const c1 = state->stage1_coeff;
    const float* const c2 = state->stage2_coeff;
    for (size_t frame = 0u; frame < input_frames; ++frame)
    {
        const size_t in_base = frame * input_stride;
        for (uint8_t channel = 0u; channel < nch; ++channel)
        {
            hist_push(state->stage1_history,
                      ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS,
                      state->stage1_write, channel,
                      read_sample(input, in_base + channel, fmt));
        }
        ++state->stage1_write;
        if (state->stage1_write == ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS)
        {
            state->stage1_write = 0u;
        }

        ++state->stage1_phase;
        if (state->stage1_phase != 2u)
        {
            continue;
        }
        state->stage1_phase = 0u;

        const float* const w1 = hist_window(state->stage1_history,
                                            state->stage1_write);
        if (nch == 2u)
        {
            float y0;
            float y1;
            fir_symmetric_win2(w1, c1,
                               ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS, &y0, &y1);
            hist_push(state->stage2_history, ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS,
                      state->stage2_write, 0u, y0);
            hist_push(state->stage2_history, ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS,
                      state->stage2_write, 1u, y1);
        }
        else
        {
            for (uint8_t channel = 0u; channel < nch; ++channel)
            {
                hist_push(state->stage2_history,
                          ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS,
                          state->stage2_write, channel,
                          fir_symmetric_win(w1 + channel, c1,
                                            ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS));
            }
        }
        ++state->stage2_write;
        if (state->stage2_write == ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS)
        {
            state->stage2_write = 0u;
        }

        ++state->stage2_phase;
        if (state->stage2_phase != 2u)
        {
            continue;
        }
        state->stage2_phase = 0u;

        const size_t out_base = produced * output_stride;
        const float* const w2 = hist_window(state->stage2_history,
                                            state->stage2_write);
        if (nch == 2u)
        {
            float y0;
            float y1;
            fir_symmetric_win2(w2, c2,
                               ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS, &y0, &y1);
            write_sample(output, out_base + 0u, y0, fmt);
            write_sample(output, out_base + 1u, y1, fmt);
        }
        else
        {
            for (uint8_t channel = 0u; channel < nch; ++channel)
            {
                write_sample(output, out_base + channel,
                             fir_symmetric_win(w2 + channel, c2,
                                               ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS),
                             fmt);
            }
        }
        ++produced;
    }

    state->input_frames += input_frames;
    state->output_frames += produced;
    *output_frames = produced;
    return true;
}

bool asrc_decimator_48_to_12_process_f32(
    asrc_decimator_48_to_12_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_48_to_12(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_F32);
}

bool asrc_decimator_48_to_12_process_s24_left(
    asrc_decimator_48_to_12_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames)
{
    return process_48_to_12(state, input, input_frames, input_stride, output,
                            output_capacity_frames, output_stride, output_frames,
                            FMT_S24_LEFT);
}

#if APP_ASRC_FRONTEND_SELFTEST

/*===========================================================================
 * Boot equivalence check: mirrored-window front end vs. the original algorithm
 *
 * The shipped front end below (ref_*) is the ORIGINAL per-channel modulo-ring
 * implementation, kept only as a reference oracle.  It is never on the audio
 * path.  The optimized path must reproduce its s24 output BIT-EXACTLY: the
 * mirrored ring only changes WHERE a sample lives, and the two-channel tap loop
 * only changes the order in which independent channels are accumulated, so the
 * per-channel sequence of float operations is identical -- there is no
 * "close enough" tolerance to argue about.
 *===========================================================================*/

#define ASRC_DEC_ST_FRAMES   (16u)   /* input frames per simulated block      */
#define ASRC_DEC_ST_BLOCKS   (40u)   /* 640 frames: wraps stage 2 (>441) too  */
/* The float family's OWN width, on purpose: this oracle checks that THIS implementation is
 * correct at the width it implements (ASRC_DECIMATOR_FLOAT_MAX_CHANNELS).  It is a module unit
 * test and deliberately NOT parameterised by ASRC_CH -- "test width --X--> shipping width" is a
 * forbidden arrow in both directions, and widening it here would hide where this legacy family's
 * boundary actually is.
 *
 * It therefore proves nothing about the product's logical width.  That is a different layer and
 * has its own check: the ASRC_CH-wide channel-isolation selftest in asrc_audio_path.c, which walks
 * ch 0..ASRC_CH-1 through the live front end and fails if any channel is not independently
 * carried.  Keep the two separate; a build can pass this one and still be narrowing the path. */
#define ASRC_DEC_ST_CH       (ASRC_DECIMATOR_FLOAT_MAX_CHANNELS)

typedef struct
{
    float    s1[ASRC_DEC_ST_CH][ASRC_DECIMATOR_STAGE1_TAPS];
    float    s2[ASRC_DEC_ST_CH][ASRC_DECIMATOR_STAGE2_TAPS];
    uint16_t w1;
    uint16_t w2;
    uint8_t  p1;
    uint8_t  p2;
} dec_ref_6_t;

typedef struct
{
    float    h[ASRC_DEC_ST_CH][ASRC_DECIMATOR_48_TO_16_TAPS];
    uint16_t w;
    uint8_t  p;
} dec_ref_3_t;

typedef struct
{
    float    h[ASRC_DEC_ST_CH][ASRC_DECIMATOR_48_TO_24_TAPS];
    uint16_t w;
    uint8_t  p;
} dec_ref_2_t;

#if ASRC_DECIMATOR_HAS_96_TO_48   /* pre-stage selftest oracle state */
/* 96 -> 48 kHz pre-stage: same single-stage shape as dec_ref_3_t / dec_ref_2_t, 21 taps. */
typedef struct
{
    float    h[ASRC_DEC_ST_CH][ASRC_DECIMATOR_96_TO_48_TAPS];
    uint16_t w;
    uint8_t  p;
} dec_ref_pre_t;
#endif /* ASRC_DECIMATOR_HAS_96_TO_48 */

/* /4 chain: two rate-changing stages, so a phase counter per stage (same shape
 * as dec_ref_6_t, both factors 2). */
typedef struct
{
    float    s1[ASRC_DEC_ST_CH][ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS];
    float    s2[ASRC_DEC_ST_CH][ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS];
    uint16_t w1;
    uint16_t w2;
    uint8_t  p1;
    uint8_t  p2;
} dec_ref_4_t;

/* Original modulo-ring symmetric FIR, verbatim. */
static float ref_fir(const float* history,
                     uint16_t write,
                     const float* coeff,
                     uint16_t taps)
{
    float sum = 0.0f;
    uint16_t newest = (write == 0u) ? (uint16_t)(taps - 1u)
                                    : (uint16_t)(write - 1u);
    uint16_t oldest = write;
    const uint16_t pairs = taps / 2u;

    for (uint16_t tap = 0u; tap < pairs; ++tap)
    {
        sum += (history[newest] + history[oldest]) * coeff[tap];
        newest = (newest == 0u) ? (uint16_t)(taps - 1u)
                                : (uint16_t)(newest - 1u);
        ++oldest;
        if (oldest == taps) { oldest = 0u; }
    }
    sum += history[newest] * coeff[pairs];
    return sum;
}

static size_t ref_process_6(dec_ref_6_t* st,
                            const int32_t* in,
                            size_t frames,
                            size_t istride,
                            int32_t* out,
                            size_t ostride)
{
    size_t produced = 0u;
    for (size_t frame = 0u; frame < frames; ++frame)
    {
        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            st->s1[ch][st->w1] = (float)(in[frame * istride + ch] >> 8);
        }
        if (++st->w1 == ASRC_DECIMATOR_STAGE1_TAPS) { st->w1 = 0u; }
        if (++st->p1 != 3u) { continue; }
        st->p1 = 0u;

        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            st->s2[ch][st->w2] = ref_fir(st->s1[ch], st->w1, s_stage1_coeff,
                                         ASRC_DECIMATOR_STAGE1_TAPS);
        }
        if (++st->w2 == ASRC_DECIMATOR_STAGE2_TAPS) { st->w2 = 0u; }
        if (++st->p2 != 2u) { continue; }
        st->p2 = 0u;

        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            out[produced * ostride + ch] = float_to_s24_left(
                ref_fir(st->s2[ch], st->w2, s_stage2_coeff,
                        ASRC_DECIMATOR_STAGE2_TAPS));
        }
        ++produced;
    }
    return produced;
}

static size_t ref_process_3(dec_ref_3_t* st,
                            const int32_t* in,
                            size_t frames,
                            size_t istride,
                            int32_t* out,
                            size_t ostride)
{
    size_t produced = 0u;
    for (size_t frame = 0u; frame < frames; ++frame)
    {
        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            st->h[ch][st->w] = (float)(in[frame * istride + ch] >> 8);
        }
        if (++st->w == ASRC_DECIMATOR_48_TO_16_TAPS) { st->w = 0u; }
        if (++st->p != 3u) { continue; }
        st->p = 0u;

        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            out[produced * ostride + ch] = float_to_s24_left(
                ref_fir(st->h[ch], st->w, s_48_to_16_coeff,
                        ASRC_DECIMATOR_48_TO_16_TAPS));
        }
        ++produced;
    }
    return produced;
}

#if ASRC_DECIMATOR_HAS_96_TO_48   /* pre-stage selftest oracle */
/* No coefficient argument, unlike ref_process_2/_4 below: the pre-stage has a single shared set. */
static size_t ref_process_pre(dec_ref_pre_t* st,
                              const int32_t* in,
                              size_t frames,
                              size_t istride,
                              int32_t* out,
                              size_t ostride)
{
    size_t produced = 0u;
    for (size_t frame = 0u; frame < frames; ++frame)
    {
        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            st->h[ch][st->w] = (float)(in[frame * istride + ch] >> 8);
        }
        if (++st->w == ASRC_DECIMATOR_96_TO_48_TAPS) { st->w = 0u; }
        if (++st->p != 2u) { continue; }
        st->p = 0u;

        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            out[produced * ostride + ch] = float_to_s24_left(
                ref_fir(st->h[ch], st->w, s_96_to_48_coeff,
                        ASRC_DECIMATOR_96_TO_48_TAPS));
        }
        ++produced;
    }
    return produced;
}
#endif /* ASRC_DECIMATOR_HAS_96_TO_48 */

/* Takes the coefficient set so the SAME oracle checks both /2 variants -- the reference
 * algorithm does not change between them, only the numbers it multiplies by. */
static size_t ref_process_2(dec_ref_2_t* st,
                            const int32_t* in,
                            size_t frames,
                            size_t istride,
                            int32_t* out,
                            size_t ostride,
                            const float* c)
{
    size_t produced = 0u;
    for (size_t frame = 0u; frame < frames; ++frame)
    {
        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            st->h[ch][st->w] = (float)(in[frame * istride + ch] >> 8);
        }
        if (++st->w == ASRC_DECIMATOR_48_TO_24_TAPS) { st->w = 0u; }
        if (++st->p != 2u) { continue; }
        st->p = 0u;

        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            out[produced * ostride + ch] = float_to_s24_left(
                ref_fir(st->h[ch], st->w, c,
                        ASRC_DECIMATOR_48_TO_24_TAPS));
        }
        ++produced;
    }
    return produced;
}

/* Takes the coefficient sets so the SAME oracle checks both /4 variants -- the reference
 * algorithm does not change between them, only the numbers it multiplies by. */
static size_t ref_process_4(dec_ref_4_t* st,
                            const int32_t* in,
                            size_t frames,
                            size_t istride,
                            int32_t* out,
                            size_t ostride,
                            const float* c1,
                            const float* c2)
{
    size_t produced = 0u;
    for (size_t frame = 0u; frame < frames; ++frame)
    {
        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            st->s1[ch][st->w1] = (float)(in[frame * istride + ch] >> 8);
        }
        if (++st->w1 == ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS) { st->w1 = 0u; }
        if (++st->p1 != 2u) { continue; }
        st->p1 = 0u;

        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            st->s2[ch][st->w2] = ref_fir(st->s1[ch], st->w1,
                                         c1,
                                         ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS);
        }
        if (++st->w2 == ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS) { st->w2 = 0u; }
        if (++st->p2 != 2u) { continue; }
        st->p2 = 0u;

        for (uint8_t ch = 0u; ch < ASRC_DEC_ST_CH; ++ch)
        {
            out[produced * ostride + ch] = float_to_s24_left(
                ref_fir(st->s2[ch], st->w2, c2,
                        ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS));
        }
        ++produced;
    }
    return produced;
}

/* Deterministic full-scale s24-left stimulus (also drives the output clamp). */
static void dec_st_fill(int32_t* buf, size_t n, uint32_t* lcg)
{
    for (size_t i = 0u; i < n; ++i)
    {
        *lcg = (*lcg * 1664525u) + 1013904223u;
        buf[i] = (int32_t)(*lcg & 0xFFFFFF00u);
    }
}

/* Separate functions so the two ~2 KB local frames do not overlap. */
static bool dec_selftest_6(void)
{
    asrc_decimator_48_to_8_t opt;
    dec_ref_6_t              ref;
    int32_t in[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t got[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t exp[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    uint32_t lcg = 0xC0FFEE11u;

    if (!asrc_decimator_48_to_8_init(&opt, ASRC_DEC_ST_CH)) { return false; }
    memset(&ref, 0, sizeof(ref));

    for (uint32_t blk = 0u; blk < ASRC_DEC_ST_BLOCKS; ++blk)
    {
        size_t n_opt = 0u;
        dec_st_fill(in, ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH, &lcg);
        memset(got, 0x5A, sizeof(got));
        memset(exp, 0xA5, sizeof(exp));

        if (!asrc_decimator_48_to_8_process_s24_left(
                &opt, in, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH,
                got, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH, &n_opt))
        {
            return false;
        }
        const size_t n_ref = ref_process_6(&ref, in, ASRC_DEC_ST_FRAMES,
                                           ASRC_DEC_ST_CH, exp, ASRC_DEC_ST_CH);
        if (n_opt != n_ref) { return false; }
        for (size_t i = 0u; i < (n_ref * ASRC_DEC_ST_CH); ++i)
        {
            if (got[i] != exp[i]) { return false; }
        }
    }
    return true;
}

static bool dec_selftest_3(void)
{
    asrc_decimator_48_to_16_t opt;
    dec_ref_3_t               ref;
    int32_t in[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t got[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t exp[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    uint32_t lcg = 0x1BADB002u;

    if (!asrc_decimator_48_to_16_init(&opt, ASRC_DEC_ST_CH)) { return false; }
    memset(&ref, 0, sizeof(ref));

    for (uint32_t blk = 0u; blk < ASRC_DEC_ST_BLOCKS; ++blk)
    {
        size_t n_opt = 0u;
        dec_st_fill(in, ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH, &lcg);
        memset(got, 0x5A, sizeof(got));
        memset(exp, 0xA5, sizeof(exp));

        if (!asrc_decimator_48_to_16_process_s24_left(
                &opt, in, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH,
                got, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH, &n_opt))
        {
            return false;
        }
        const size_t n_ref = ref_process_3(&ref, in, ASRC_DEC_ST_FRAMES,
                                           ASRC_DEC_ST_CH, exp, ASRC_DEC_ST_CH);
        if (n_opt != n_ref) { return false; }
        for (size_t i = 0u; i < (n_ref * ASRC_DEC_ST_CH); ++i)
        {
            if (got[i] != exp[i]) { return false; }
        }
    }
    return true;
}

#if ASRC_DECIMATOR_HAS_96_TO_48   /* pre-stage bit-exactness selftest */
/* The 96 -> 48 kHz pre-stage.  A 21-tap ring wraps every 21 frames, so ASRC_DEC_ST_BLOCKS (40
 * blocks of 16 frames) covers it many times over -- the count was chosen for the /6 chain's
 * 147-tap stage 2 and is more than sufficient here. */
static bool dec_selftest_prestage(void)
{
    asrc_decimator_96_to_48_t opt;
    dec_ref_pre_t             ref;
    int32_t in[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t got[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t exp[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    uint32_t lcg = 0x96C0FFEEu;

    if (!asrc_decimator_96_to_48_init(&opt, ASRC_DEC_ST_CH)) { return false; }
    memset(&ref, 0, sizeof(ref));

    for (uint32_t blk = 0u; blk < ASRC_DEC_ST_BLOCKS; ++blk)
    {
        size_t n_opt = 0u;
        dec_st_fill(in, ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH, &lcg);
        memset(got, 0x5A, sizeof(got));
        memset(exp, 0xA5, sizeof(exp));

        if (!asrc_decimator_96_to_48_process_s24_left(
                &opt, in, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH,
                got, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH, &n_opt))
        {
            return false;
        }
        const size_t n_ref = ref_process_pre(&ref, in, ASRC_DEC_ST_FRAMES,
                                             ASRC_DEC_ST_CH, exp, ASRC_DEC_ST_CH);
        if (n_opt != n_ref) { return false; }
        for (size_t i = 0u; i < (n_ref * ASRC_DEC_ST_CH); ++i)
        {
            if (got[i] != exp[i]) { return false; }
        }
    }
    return true;
}
#endif /* ASRC_DECIMATOR_HAS_96_TO_48 */

/* Runs one /2 coefficient variant.  Called once per variant (see asrc_decimator_selftest), for
 * the same reason as dec_selftest_4: both are on the audio path at runtime, selected by output
 * rate, so a claim that covered only one would leave the other unchecked in the shipped image. */
static bool dec_selftest_2(asrc_decimator_48_to_24_variant_t variant,
                           const float* c,
                           uint32_t seed)
{
    asrc_decimator_48_to_24_t opt;
    dec_ref_2_t               ref;
    int32_t in[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t got[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t exp[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    uint32_t lcg = seed;

    if (!asrc_decimator_48_to_24_init(&opt, ASRC_DEC_ST_CH, variant)) { return false; }
    /* The variant must actually reach the state: a silent fallback to the other set would still
     * pass the bit-exactness loop below, because the oracle is handed the pointer the caller
     * intended rather than the one init resolved. */
    if (opt.coeff != c) { return false; }
    memset(&ref, 0, sizeof(ref));

    for (uint32_t blk = 0u; blk < ASRC_DEC_ST_BLOCKS; ++blk)
    {
        size_t n_opt = 0u;
        dec_st_fill(in, ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH, &lcg);
        memset(got, 0x5A, sizeof(got));
        memset(exp, 0xA5, sizeof(exp));

        if (!asrc_decimator_48_to_24_process_s24_left(
                &opt, in, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH,
                got, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH, &n_opt))
        {
            return false;
        }
        const size_t n_ref = ref_process_2(&ref, in, ASRC_DEC_ST_FRAMES,
                                           ASRC_DEC_ST_CH, exp, ASRC_DEC_ST_CH, c);
        if (n_opt != n_ref) { return false; }
        for (size_t i = 0u; i < (n_ref * ASRC_DEC_ST_CH); ++i)
        {
            if (got[i] != exp[i]) { return false; }
        }
    }
    return true;
}

/* Runs one /4 coefficient variant.  Called once per variant (see asrc_decimator_selftest):
 * both are on the audio path at runtime, selected by output rate, so a bit-exactness claim
 * that covered only one of them would leave the other unchecked in the shipped image. */
static bool dec_selftest_4(asrc_decimator_48_to_12_variant_t variant,
                           const float* c1,
                           const float* c2,
                           uint32_t seed)
{
    asrc_decimator_48_to_12_t opt;
    dec_ref_4_t               ref;
    int32_t in[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t got[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    int32_t exp[ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH];
    uint32_t lcg = seed;

    if (!asrc_decimator_48_to_12_init(&opt, ASRC_DEC_ST_CH, variant)) { return false; }
    /* The variant must actually reach the state: a silent fallback to the other set would
     * still pass the bit-exactness loop below, because the oracle is handed the same
     * pointers the caller intended rather than the ones init resolved. */
    if ((opt.stage1_coeff != c1) || (opt.stage2_coeff != c2)) { return false; }
    memset(&ref, 0, sizeof(ref));

    for (uint32_t blk = 0u; blk < ASRC_DEC_ST_BLOCKS; ++blk)
    {
        size_t n_opt = 0u;
        dec_st_fill(in, ASRC_DEC_ST_FRAMES * ASRC_DEC_ST_CH, &lcg);
        memset(got, 0x5A, sizeof(got));
        memset(exp, 0xA5, sizeof(exp));

        if (!asrc_decimator_48_to_12_process_s24_left(
                &opt, in, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH,
                got, ASRC_DEC_ST_FRAMES, ASRC_DEC_ST_CH, &n_opt))
        {
            return false;
        }
        const size_t n_ref = ref_process_4(&ref, in, ASRC_DEC_ST_FRAMES,
                                           ASRC_DEC_ST_CH, exp, ASRC_DEC_ST_CH,
                                           c1, c2);
        if (n_opt != n_ref) { return false; }
        for (size_t i = 0u; i < (n_ref * ASRC_DEC_ST_CH); ++i)
        {
            if (got[i] != exp[i]) { return false; }
        }
    }
    return true;
}

bool asrc_decimator_selftest(void)
{
    if (!dec_selftest_6()) { return false; }
    if (!dec_selftest_3()) { return false; }
#if ASRC_DECIMATOR_HAS_96_TO_48
    if (!dec_selftest_prestage()) { return false; }
#endif
    /* Distinct seeds so the two /2 runs are not the same stimulus twice -- a stuck
     * coefficient pointer would otherwise be invisible to the second run. */
    if (!dec_selftest_2(ASRC_DECIMATOR_48_TO_24_FOR_24000,
                        s_48_to_24_coeff, 0x2C0DE24Au))
    {
        return false;
    }
    if (!dec_selftest_2(ASRC_DECIMATOR_48_TO_24_FOR_22050,
                        s_48_to_24_out22k_coeff, 0x22050A5Cu))
    {
        return false;
    }
    /* Distinct seeds so the two /4 runs are not the same stimulus twice -- a stuck
     * coefficient pointer would otherwise be invisible to the second run. */
    if (!dec_selftest_4(ASRC_DECIMATOR_48_TO_12_FOR_11025,
                        s_48_to_12_stage1_coeff, s_48_to_12_stage2_coeff,
                        0x5EEDC0DEu))
    {
        return false;
    }
    return dec_selftest_4(ASRC_DECIMATOR_48_TO_12_FOR_12000,
                          s_48_to_12_out12k_stage1_coeff,
                          s_48_to_12_out12k_stage2_coeff,
                          0x0DDBA11Du);
}

#endif /* APP_ASRC_FRONTEND_SELFTEST -- the oracle and its drivers above */

/* The Q31 front end.  Included rather than compiled separately so it shares the
 * coefficient tables above -- see its file header for why that is the point. */
#include "asrc_decimator_q31.inc"

#endif /* APP_ASRC_48K_TO_8_DECIMATOR || APP_ASRC_RUNTIME_48K_TO_8 */
