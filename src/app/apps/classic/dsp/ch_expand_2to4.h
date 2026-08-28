#include "app_specific_config_defs.h"   // ENA_DRC_DF2T_CASCADE (self-contained: do not rely on include order)
#if ENA_DRC_DF2T_CASCADE

#ifndef _CH_EXPAND_2TO4_H
#define _CH_EXPAND_2TO4_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

#define CH_EXPAND_2TO4_IN_CH        (2)
#define CH_EXPAND_2TO4_OUT_CH       (4)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    float gain_l1;
    float gain_r1;
    float gain_l2;
    float gain_r2;
} ch_expand_2to4_t;


//===========================================================
// Variables
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

extern void ch_expand_2to4_init( ch_expand_2to4_t* pexp );
extern void ch_expand_2to4_reset( ch_expand_2to4_t* pexp );
extern void ch_expand_2to4_process(       ch_expand_2to4_t* pexp,
                                        const float*            in,
                                              float*            out,
                                              int               samples );


//===========================================================
// API
//===========================================================

extern void  app_ch_expand_2to4_init(void);
extern void  app_ch_expand_2to4_reset(void);
extern bool  app_ch_expand_2to4_set_gain( uint8_t ch, float gain );
extern bool  app_ch_expand_2to4_get_gain( uint8_t ch, float* pgain );
extern void  app_ch_expand_2to4_print_status( void );
extern void  app_ch_expand_2to4_process( const float* in, float* out );


#endif //!_CH_EXPAND_2TO4_H
#endif //ENA_DRC_DF2T_CASCADE
