#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "gain_ctrl.h"


#include "ch_expand_2to4.h"


#if ENA_DRC_DF2T_CASCADE
//===========================================================
// Definition
//===========================================================

// Input  : 2ch sample-major interleaved
//          L, R, L, R, ...
//
// Output : 4ch sample-major interleaved
//          L1, R1, L2, R2, L1, R1, L2, R2, ...
//
// Current behavior:
//          L1 = L
//          R1 = R
//          L2 = L
//          R2 = R
//
// Future expansion:
//          Per-output gain can be applied here.


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================


//===========================================================
// Variables
//===========================================================


//===========================================================
// Global Function
//===========================================================

/**
 * @brief Initialize a 2ch-to-4ch channel expansion instance.
 *
 * Behavior:
 *   - Clears all structure fields.
 *   - Sets all output gains to unity.
 *
 * @param pexp Pointer to @c ch_expand_2to4_t instance.
 */
void ch_expand_2to4_init( ch_expand_2to4_t* pexp )
{
    if (pexp == NULL)
    {
        return;
    }

    memset( pexp, 0x00, sizeof(ch_expand_2to4_t) );

    pexp->gain_l1 = 1.0f;
    pexp->gain_r1 = 1.0f;
    pexp->gain_l2 = 1.0f;
    pexp->gain_r2 = 1.0f;
}


/**
 * @brief Reset a 2ch-to-4ch channel expansion instance.
 *
 * This module currently has no delay state. The reset API is kept so that
 * the application can reset all audio blocks with a consistent pattern.
 *
 * @param pexp Pointer to @c ch_expand_2to4_t instance.
 */
void ch_expand_2to4_reset( ch_expand_2to4_t* pexp )
{
    if (pexp == NULL)
    {
        return;
    }

    // No internal filter state in this module.
    // Keep gain settings unchanged.
}


/**
 * @brief Expand 2ch float input to 4ch float output.
 *
 * Input and output are channel-major buffers.
 *
 * Input layout:
 *   in[0][n] = L
 *   in[1][n] = R
 *
 * Output layout:
 *   out[0][n] = L1
 *   out[1][n] = R1
 *   out[2][n] = L2
 *   out[3][n] = R2
 *
 * Note:
 *   The arguments are flat pointers, but are indexed as [ch][sample].
 *   Pass &buffer[0][0] when the actual buffer is declared as
 *   float buffer[channels][samples].
 *
 * @param pexp    Pointer to @c ch_expand_2to4_t instance.
 * @param in  Channel-major 2ch input buffer [2][samples].
 * @param out Channel-major 4ch output buffer [4][samples].
 * @param samples Number of samples per channel.
 */
void ch_expand_2to4_process(       ch_expand_2to4_t* pexp,
                                 const float*            in,
                                       float*            out,
                                       int               samples )
{
//fast    if ((pexp == NULL) || (in == NULL) || (out == NULL) || (samples <= 0))
//fast    {
//fast        return;
//fast    }

    float gain_l1 = pexp->gain_l1;
    float gain_r1 = pexp->gain_r1;
    float gain_l2 = pexp->gain_l2;
    float gain_r2 = pexp->gain_r2;

    const float* in_l  = &in[0 * samples];
    const float* in_r  = &in[1 * samples];

    float* out_l1 = &out[0 * samples];
    float* out_r1 = &out[1 * samples];
    float* out_l2 = &out[2 * samples];
    float* out_r2 = &out[3 * samples];

    for (int sample_idx = 0; sample_idx < samples; sample_idx++)
    {
        float l = in_l[sample_idx];
        float r = in_r[sample_idx];

        out_l1[sample_idx] = l * gain_l1;     // L1
        out_r1[sample_idx] = r * gain_r1;     // R1
        out_l2[sample_idx] = l * gain_l2;     // L2
        out_r2[sample_idx] = r * gain_r2;     // R2
    }
}






//===========================================================
// Local Function
//===========================================================


//===========================================================
// API
//===========================================================

/*static*/ ch_expand_2to4_t My_ChExpand2to4;

void app_ch_expand_2to4_init(void)
{
    ch_expand_2to4_init(&My_ChExpand2to4);
}


void app_ch_expand_2to4_reset(void)
{
    ch_expand_2to4_reset(&My_ChExpand2to4);
}


bool app_ch_expand_2to4_set_gain( uint8_t ch, float gain )
{
    bool result = true;

    switch( ch )
    {
    case 0:
        My_ChExpand2to4.gain_l1 = gain;
        break;
    case 1:
        My_ChExpand2to4.gain_r1 = gain;
        break;
    case 2:
        My_ChExpand2to4.gain_l2 = gain;
        break;
    case 3:
        My_ChExpand2to4.gain_r2 = gain;
        break;
    default:
        printf("app_ch_expand_2to4_set_gain: invalid ch=%d\n", ch);
        result = false;
        break;
    }

    return result;
}


bool app_ch_expand_2to4_get_gain( uint8_t ch, float* pgain )
{
    bool result = true;

    if( !pgain )
    {
        return false;
    }

    switch( ch )
    {
    case 0:
        *pgain = My_ChExpand2to4.gain_l1;
        break;
    case 1:
        *pgain = My_ChExpand2to4.gain_r1;
        break;
    case 2:
        *pgain = My_ChExpand2to4.gain_l2;
        break;
    case 3:
        *pgain = My_ChExpand2to4.gain_r2;
        break;
    default:
        printf("app_ch_expand_2to4_get_gain: invalid ch=%d\n", ch);
        *pgain = 0.0f;
        result = false;
        break;
    }
    return result;
}


void app_ch_expand_2to4_print_status( void )
{
    float gain_l1;
    float gain_r1;
    float gain_l2;
    float gain_r2;

    float db_l1;
    float db_r1;
    float db_l2;
    float db_r2;

    gain_l1 = My_ChExpand2to4.gain_l1;
    gain_r1 = My_ChExpand2to4.gain_r1;
    gain_l2 = My_ChExpand2to4.gain_l2;
    gain_r2 = My_ChExpand2to4.gain_r2;

    db_l1 = (gain_l1 > 0.0f || gain_l1 < 0.0f) ? (20.0f * log10f(fabsf(gain_l1))) : -999.0f;
    db_r1 = (gain_r1 > 0.0f || gain_r1 < 0.0f) ? (20.0f * log10f(fabsf(gain_r1))) : -999.0f;
    db_l2 = (gain_l2 > 0.0f || gain_l2 < 0.0f) ? (20.0f * log10f(fabsf(gain_l2))) : -999.0f;
    db_r2 = (gain_r2 > 0.0f || gain_r2 < 0.0f) ? (20.0f * log10f(fabsf(gain_r2))) : -999.0f;

    printf("----------------------------------------\n");
    printf(" EXPAND 2to4 GAIN STATUS\n");
    printf(" ch\n");
    printf(" 0 L1: gain=% .6f  % .2fdB\n", gain_l1, db_l1);
    printf(" 1 R1: gain=% .6f  % .2fdB\n", gain_r1, db_r1);
    printf(" 2 L2: gain=% .6f  % .2fdB\n", gain_l2, db_l2);
    printf(" 3 R2: gain=% .6f  % .2fdB\n", gain_r2, db_r2);
    printf("----------------------------------------\n");
}


void app_ch_expand_2to4_process( const float* in, float* out )
{
    ch_expand_2to4_process(&My_ChExpand2to4, in, out, APP_BLOCK_FRAMES);
}


#endif //ENA_DRC_DF2T_CASCADE
