// asrc_full_iir_48_to_32.c
//
// The TRIAL Full-IIR 48 -> 32 kHz anti-alias stage.  See the header for the topology
// and for why it is not a drop-in replacement for a resampling front end.

#include "app_specific_config_defs.h"

#include "asrc_full_iir_48_to_32.h"

#if !SONORA_APP_IS_ASRC
#  error "asrc_full_iir_48_to_32.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#if !APP_ASRC_FULL_IIR_48_TO_32
typedef int asrc_full_iir_48_to_32_unavailable_t;   // keep the translation unit non-empty (C11 6.9)
#else

#include <stdio.h>
#include <string.h>

#include "arm_math.h"
#include "audio_app_asrc.h"
#include "asrc_full_iir_48_to_32_coeffs.h"

/* The project-owned DF2T kernel, NOT the vendor primitive.
 *
 * Two facts decided this, both measured, both in
 * [internal] precheck_ak512_48_to_32_full_iir_2026-09-05.md:
 *   - the vendor primitive (mchp_biquad_cascade_df2T_f32) BUS ERRORs on this board in
 *     a run that leaves the IPL4 streaming kernels enabled, and opt v2 faults in
 *     validation.  Only opt v1 survives.
 *   - ordinary local float C costs 395 us/block for six SOS, which alone exceeds the
 *     333.33 us block deadline.  There is no fallback: this kernel is load-bearing.
 * It is already called from an audio block ISR, in place, by the Classic DRC path
 * (src/app/apps/classic/dsp/biquad_cascade_4ch.c), which is the precedent for both.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(
    const mchp_biquad_cascade_df2T_instance_f32* S,
    const float32_t* pSrc, float32_t* pDst, uint32_t blockSize );

#define FI_CH            (ASRC_CH)
#define FI_SOS           (ASRC_FULL_IIR_48_TO_32_SOS)
#define FI_FRAMES        ((uint32_t)APP_BLOCK_FRAMES)
#define FI_FULL_SCALE    (8388607.0f)   /* == ASRC_SAMP_MAX: the ring's float domain is 24-bit counts */

_Static_assert( FI_SOS == 6u, "the qualified candidate is six sections" );
_Static_assert( ASRC_FULL_IIR_48_TO_32_UNITY_CRC32 == 0x67AE5F41u,
                "coefficient table is not derived from the measured Phase-4 candidate" );
/* The kernel's own precondition: DTB would wrap on 0 and run for ~4 billion iterations. */
_Static_assert( ( FI_FRAMES >= 1u ) && ( FI_FRAMES <= 512u ),
                "biquad_cascade_df2T_f32_dspic33ak_opt_v1 requires blockSize in 1..512" );
/* The ring writer below is the CH_MAJOR one.  TILE8 stores lanes interleaved and would
 * need a different scatter; fail the build rather than write the wrong layout. */
#if (ASRC_HISTORY_LAYOUT != ASRC_HISTORY_CH_MAJOR)
#  error "the Full-IIR ring writer assumes the CH_MAJOR history layout"
#endif
/* Float-only: the stage IS a float32 DF2T cascade, and there is no Q31 counterpart of
 * it (nor of the host qualification).  Refuse rather than silently push float values
 * into an integer ring -- the failure mode that cost 48 dB once already
 * (asrc_push_frames_selftest). */
#if ASRC_SAMPLE_Q31
#  error "APP_ASRC_FULL_IIR_48_TO_32 requires the float32 ASRC sample path (ASRC_SAMPLE_Q31 == 0)"
#endif

/* Y space on purpose.  X holds s_asrc (about 20 KB) and the coefficient tables, and
 * the precheck could not even add a 1.15 KB probe there.  The kernel reads state and
 * the work block with ordinary pointers, so neither has a space requirement -- this
 * is purely about which region has the room. */
static float s_fi_work[ FI_CH ][ (uint32_t)APP_BLOCK_FRAMES ] __attribute__((space(ymemory)));
static float s_fi_state[ FI_CH * FI_SOS * 2u ]                __attribute__((space(ymemory)));
static mchp_biquad_cascade_df2T_instance_f32 s_fi_inst[ FI_CH ];

/*
 * MEASUREMENT-ONLY BOOT DEFAULT for the mode (build condition, default unchanged).
 *
 * The mode is normally chosen by console command, and that stays the shipping
 * behaviour: N97 at boot, Full-IIR only when someone asks for it.  But a CPU load
 * study of a width that saturates the core cannot use the console -- it lives in
 * the foreground, so at ~100 % load "*aj01" is never dispatched and the image can
 * only ever be measured in the mode it booted in.  Two images that differ ONLY in
 * this default are then the way to get the method delta.  Nothing about the stage
 * moves: same coefficients, same six SOS in the same order, same headroom, same
 * ASRC.  "*aj" still overrides it at runtime whenever the console does answer.
 */
#if defined(APP_ASRC_FULL_IIR_BOOT_MODE) && (APP_ASRC_FULL_IIR_BOOT_MODE == 1)
static volatile uint8_t  s_fi_mode  = (uint8_t)ASRC_FULL_IIR_MODE_FULL_IIR;
#else
static volatile uint8_t  s_fi_mode  = (uint8_t)ASRC_FULL_IIR_MODE_N97;
#endif
static volatile uint8_t  s_fi_armed;
static volatile uint8_t  s_fi_ready;

#if APP_ASRC_FULL_IIR_OBSERVE
static volatile uint32_t s_fi_out_peak_milli;
static volatile uint32_t s_fi_state_peak_milli;
static volatile uint32_t s_fi_over_fs;
static volatile uint32_t s_fi_nonfinite;
static volatile uint32_t s_fi_blocks;
#endif

void asrc_full_iir_48_to_32_set_mode( asrc_full_iir_mode_t mode )
{
    s_fi_mode = ( mode == ASRC_FULL_IIR_MODE_FULL_IIR )
                    ? (uint8_t)ASRC_FULL_IIR_MODE_FULL_IIR
                    : (uint8_t)ASRC_FULL_IIR_MODE_N97;
}

asrc_full_iir_mode_t asrc_full_iir_48_to_32_mode( void )
{
    return ( s_fi_mode != 0u ) ? ASRC_FULL_IIR_MODE_FULL_IIR : ASRC_FULL_IIR_MODE_N97;
}

bool asrc_full_iir_48_to_32_armed( void )
{
    return ( s_fi_armed != 0u ) && ( s_fi_ready != 0u );
}

/* Retire before re-arming, in that order, for the same reason the front end does it:
 * the block ISR gates on `ready`, so between the two writes it takes the ordinary
 * direct push instead of running against half-initialised state. */
void asrc_full_iir_48_to_32_arm( bool on )
{
    if( !on )
    {
        s_fi_ready = 0u;
        s_fi_armed = 0u;
        return;
    }
    s_fi_armed = 1u;
}

void asrc_full_iir_48_to_32_identity( uint32_t* crc32, uint32_t* unity_crc32,
                                      int32_t* headroom_mdb, uint32_t* sos )
{
    if( crc32 )        { *crc32        = (uint32_t)ASRC_FULL_IIR_48_TO_32_CRC32; }
    if( unity_crc32 )  { *unity_crc32  = (uint32_t)ASRC_FULL_IIR_48_TO_32_UNITY_CRC32; }
    if( headroom_mdb ) { *headroom_mdb = (int32_t)ASRC_FULL_IIR_48_TO_32_HEADROOM_MDB; }
    if( sos )          { *sos          = (uint32_t)FI_SOS; }
}

void asrc_full_iir_48_to_32_reset( void )
{
    s_fi_ready = 0u;
    memset( (void*)s_fi_state, 0, sizeof(s_fi_state) );
    memset( (void*)s_fi_work,  0, sizeof(s_fi_work) );
    for( uint32_t ch = 0u; ch < FI_CH; ch++ )
    {
        /* THROUGH THE LIBRARY INITIALIZER, never by field assignment.  The struct's
         * field names and the assembly's offsets disagree: dspcommon.inc puts
         * iirCasPState_f32 at +4 and iirCasPCoeffs_f32 at +8, while the init routine
         * stores its pCoeffs argument at +4 and pState at +8 -- and every reader
         * (vendor and opt v1) reads +4 as the coefficients.  It is self-consistent
         * only through this call.  Assigning .pCoeffs / .pState directly is what
         * produced the Phase-2 BUS ERROR. */
        mchp_biquad_cascade_df2T_init_f32(
            &s_fi_inst[ch], (uint8_t)FI_SOS,
            asrc_full_iir_48_to_32_df2t_sos,
            &s_fi_state[ ch * FI_SOS * 2u ] );
    }
#if APP_ASRC_FULL_IIR_OBSERVE
    asrc_full_iir_48_to_32_stats_clear();
#endif
    s_fi_ready = 1u;
}

#if APP_ASRC_FULL_IIR_OBSERVE
/* Observe channels 0 and 1 ONLY, and that is exact rather than a sample.  Every ASRC
 * channel reads source slot (c & 1) -- see asrc_push() -- so the sixteen channels
 * carry exactly two distinct signals, and each channel's filter has identical
 * coefficients and an identical state trajectory from the same reset.  Channels 0/1
 * therefore hold the peak of every channel, bit for bit.  Observing all sixteen would
 * cost 8x for no additional information.
 *
 * The finite test is on the exponent bits, not isfinite(): a NaN never updates a peak
 * by comparison, so a peak-only observer cannot see one. */
static void fi_observe( uint32_t frames )
{
    union { float f; uint32_t u; } t;
    float    out_peak   = 0.0f;
    float    state_peak = 0.0f;
    uint32_t over_fs    = 0u;
    uint32_t nonfinite  = 0u;

    for( uint32_t ch = 0u; ch < 2u; ch++ )
    {
        for( uint32_t n = 0u; n < frames; n++ )
        {
            const float v = s_fi_work[ch][n];
            t.f = v;
            if( ( t.u & 0x7F800000u ) == 0x7F800000u ) { nonfinite++; continue; }
            const float a = ( v < 0.0f ) ? -v : v;
            if( a > out_peak ) { out_peak = a; }
            if( a >= FI_FULL_SCALE ) { over_fs++; }
        }
        for( uint32_t k = 0u; k < ( FI_SOS * 2u ); k++ )
        {
            const float v = s_fi_state[ ( ch * FI_SOS * 2u ) + k ];
            t.f = v;
            if( ( t.u & 0x7F800000u ) == 0x7F800000u ) { nonfinite++; continue; }
            const float a = ( v < 0.0f ) ? -v : v;
            if( a > state_peak ) { state_peak = a; }
        }
    }

    {
        const uint32_t out_milli   = (uint32_t)( ( out_peak   / FI_FULL_SCALE ) * 1000.0f );
        const uint32_t state_milli = (uint32_t)( ( state_peak / FI_FULL_SCALE ) * 1000.0f );
        if( out_milli   > s_fi_out_peak_milli )   { s_fi_out_peak_milli   = out_milli; }
        if( state_milli > s_fi_state_peak_milli ) { s_fi_state_peak_milli = state_milli; }
    }
    s_fi_over_fs   += over_fs;
    s_fi_nonfinite += nonfinite;
    s_fi_blocks++;
}

void asrc_full_iir_48_to_32_stats( uint32_t* out_peak_milli, uint32_t* state_peak_milli,
                                   uint32_t* over_fs, uint32_t* nonfinite, uint32_t* blocks )
{
    if( out_peak_milli )   { *out_peak_milli   = s_fi_out_peak_milli; }
    if( state_peak_milli ) { *state_peak_milli = s_fi_state_peak_milli; }
    if( over_fs )          { *over_fs          = s_fi_over_fs; }
    if( nonfinite )        { *nonfinite        = s_fi_nonfinite; }
    if( blocks )           { *blocks           = s_fi_blocks; }
}

void asrc_full_iir_48_to_32_stats_clear( void )
{
    s_fi_out_peak_milli   = 0u;
    s_fi_state_peak_milli = 0u;
    s_fi_over_fs          = 0u;
    s_fi_nonfinite        = 0u;
    s_fi_blocks           = 0u;
}
#else
void asrc_full_iir_48_to_32_stats( uint32_t* out_peak_milli, uint32_t* state_peak_milli,
                                   uint32_t* over_fs, uint32_t* nonfinite, uint32_t* blocks )
{
    if( out_peak_milli )   { *out_peak_milli   = 0u; }
    if( state_peak_milli ) { *state_peak_milli = 0u; }
    if( over_fs )          { *over_fs          = 0u; }
    if( nonfinite )        { *nonfinite        = 0u; }
    if( blocks )           { *blocks           = 0u; }
}
void asrc_full_iir_48_to_32_stats_clear( void ) { }
#endif /* APP_ASRC_FULL_IIR_OBSERVE */

/* Shared body of both entry points.  `stride` is the word pitch between frames of the
 * source (APP_SLOTS_PER_FS for a raw TDM block, ASRC_CH for the pre-stage's output) and
 * `frames` how many frames it carries.  Nothing else varies between the two: same
 * channel mapping, same coefficients, same 6 SOS, same headroom, all FI_CH filtered. */
static void fi_process_push_ab( const int32_t* src, uint32_t frames, uint32_t stride,
                               uint8_t from_frontend )
{
    if( ( src == NULL ) || ( s_fi_ready == 0u ) ) { return; }
    if( ( frames == 0u ) || ( frames > FI_FRAMES ) ) { return; }

    /* Gather: TDM frame-major s24-left -> channel-major float, in the SAME amplitude
     * representation asrc_push() writes into the ring ((float)(word >> 8), i.e. 24-bit
     * counts).  Channel c takes source slot (c & 1), which is the mapping the ring
     * writer has always used, so the stage neither widens nor narrows the path.
     *
     * All sixteen channels are filtered.  They carry two distinct signals, but the
     * CPU cost of an anti-alias stage at ASRC_CH is the quantity under test, and a
     * width reduced to fit measures nothing.  Nothing is discarded either: the ring
     * writer below stores every channel's own output. */
    for( uint32_t ch = 0u; ch < FI_CH; ch++ )
    {
        const int32_t* p = &src[ ch & 1u ];
        float*         w = s_fi_work[ch];
        for( uint32_t n = 0u; n < frames; n++ )
        {
            w[n] = (float)( p[ n * stride ] >> 8 );
        }
    }

    /* IN PLACE.  The kernel reads x then writes y for each sample and, from stage 1
     * on, deliberately aliases its own input and output; pSrc == pDst is what the
     * Classic DRC path already does.  The headroom gain is in the coefficients, so
     * there is no scaling pass here. */
    for( uint32_t ch = 0u; ch < FI_CH; ch++ )
    {
        biquad_cascade_df2T_f32_dspic33ak_opt_v1(
            &s_fi_inst[ch], s_fi_work[ch], s_fi_work[ch], frames );
    }

#if APP_ASRC_FULL_IIR_OBSERVE
    fi_observe( frames );
#endif

    /* Straight into the float ring: no fixed-point round trip, so the first
     * conversion that can clip stays where it already is, in asrc_pull(). */
    audio_app_asrc_push_ab_block_f32( &s_fi_work[0][0], (uint32_t)APP_BLOCK_FRAMES,
                                      frames, from_frontend );
}

void asrc_full_iir_48_to_32_process_push_ab( const int32_t* src )
{
    fi_process_push_ab( src, FI_FRAMES, (uint32_t)APP_SLOTS_PER_FS, 0u );
}

void asrc_full_iir_48_to_32_process_push_ab_frames( const int32_t* src, uint32_t frames,
                                                   uint32_t stride )
{
    fi_process_push_ab( src, frames, stride, 1u );
}

void asrc_full_iir_48_to_32_dbg_print( void )
{
    if( s_fi_armed == 0u ) { return; }
#if APP_ASRC_FULL_IIR_OBSERVE
    printf( "[full-iir x%uch]AB %u SOS gain=%ld.%03lddB out=%lu.%03lu state=%lu.%03lu FS"
            " over_fs=%lu nan=%lu blk=%lu\n",
            (unsigned)FI_CH, (unsigned)FI_SOS,
            (long)( ASRC_FULL_IIR_48_TO_32_HEADROOM_MDB / 1000 ),
            (long)( ( ASRC_FULL_IIR_48_TO_32_HEADROOM_MDB < 0 )
                        ? -( ASRC_FULL_IIR_48_TO_32_HEADROOM_MDB % 1000 )
                        :  ( ASRC_FULL_IIR_48_TO_32_HEADROOM_MDB % 1000 ) ),
            (unsigned long)( s_fi_out_peak_milli / 1000u ),
            (unsigned long)( s_fi_out_peak_milli % 1000u ),
            (unsigned long)( s_fi_state_peak_milli / 1000u ),
            (unsigned long)( s_fi_state_peak_milli % 1000u ),
            (unsigned long)s_fi_over_fs, (unsigned long)s_fi_nonfinite,
            (unsigned long)s_fi_blocks );
#else
    printf( "[full-iir x%uch]AB %u SOS crc=%08lX observe=off\n",
            (unsigned)FI_CH, (unsigned)FI_SOS,
            (unsigned long)ASRC_FULL_IIR_48_TO_32_CRC32 );
#endif
}

#endif /* APP_ASRC_FULL_IIR_48_TO_32 */
