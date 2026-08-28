//===========================================================
// dsPIC33AK backend for nora_reset.h -- RCON snapshot + the family's cause table.
//
// The classification, the policy, and the API shape are the AK/CK common face and
// live in the header. What is genuinely per-family, and therefore lives here, is the
// CAUSE TABLE: dsPIC33A RCON has no TRAPR and no IOPUWR bit. It has CM (configuration
// mismatch) instead, so this table is six entries where CK's is seven, and CK's
// "TRAPR is the only evidence of a stack overflow" reasoning has no counterpart here.
//
// The low status bits (POR/BOR/IDLE/SLEEP/WDTO/SWR/EXTR/CM) are identical on the
// dsPIC33AK512MPS512 (AK512) and dsPIC33AK128MC106 (AK128), so this one backend
// builds for both AK targets without a part #if.
//===========================================================

#include "nora_reset.h"

#include <xc.h>

// Most-specific-first, for the nora_reset_cause_str() DIAGNOSTIC (see the header's
// two-precedence note -- this order deliberately differs from the classification
// below). The table exists so the ladder and the "is more than one candidate set"
// test cannot disagree about what the candidates are: the bug a second hand-written
// list invites.
typedef struct
{
    uint32_t    mask;
    const char *name;
} nora_reset_cause_desc_t;

static const nora_reset_cause_desc_t s_reset_causes[] = {
    { (uint32_t)(1uL << _RCON_CM_POSITION),   "CM(configuration mismatch)" },
    { (uint32_t)(1uL << _RCON_WDTO_POSITION), "WDTO(watchdog)" },
    { (uint32_t)(1uL << _RCON_SWR_POSITION),  "SWR(software reset)" },
    { (uint32_t)(1uL << _RCON_EXTR_POSITION), "EXTR(MCLR)" },
    { (uint32_t)(1uL << _RCON_POR_POSITION),  "POR(power-on)" },
    { (uint32_t)(1uL << _RCON_BOR_POSITION),  "BOR(brown-out)" },
};

#define NORA_RESET_CAUSE_COUNT (sizeof s_reset_causes / sizeof s_reset_causes[0])

// Snapshot state (module-private). Set once by nora_reset_snapshot_capture().
static bool               s_snapshot_captured;
static uint32_t           s_snapshot_raw;
static nora_reset_cause_t s_snapshot_cause;

/*
 * Power-event-wins CLASSIFICATION, only ever reached on the AND_CLEAR path.
 *
 * If POR is set it is a genuine power-up even when EXTR is also set (e.g. MCLR held
 * low through power-on): the rails moved, so the codecs are cold. BOR-without-POR is
 * a brown-out dip -- rails still moved, so also cold. Only when no power flag is set
 * do the dsPIC-only causes (MCLR / software / watchdog) indicate that the board rails,
 * and thus a codec, likely kept power.
 *
 * Classifying under PRESERVE would be the wrong thing: the bits may have accumulated
 * across several resets and no precedence can be justified. Hence the caller gate.
 */
static nora_reset_cause_t reset_snapshot_decode( uint32_t raw )
{
    if( ( raw & (uint32_t)( 1uL << _RCON_POR_POSITION ) ) != 0u )
    {
        return NORA_RESET_CAUSE_POWER_ON;
    }
    if( ( raw & (uint32_t)( 1uL << _RCON_BOR_POSITION ) ) != 0u )
    {
        return NORA_RESET_CAUSE_BROWNOUT;
    }
    if( ( raw & (uint32_t)( 1uL << _RCON_EXTR_POSITION ) ) != 0u )
    {
        return NORA_RESET_CAUSE_EXTERNAL;
    }
    if( ( raw & (uint32_t)( 1uL << _RCON_SWR_POSITION ) ) != 0u )
    {
        return NORA_RESET_CAUSE_SOFTWARE;
    }
    if( ( raw & (uint32_t)( 1uL << _RCON_WDTO_POSITION ) ) != 0u )
    {
        return NORA_RESET_CAUSE_WATCHDOG;
    }
    // e.g. a configuration-mismatch reset, or flags already cleared. Conservative.
    return NORA_RESET_CAUSE_OTHER;
}


bool nora_reset_snapshot_capture( nora_reset_latch_policy_t policy )
{
    if( s_snapshot_captured ||
        ( ( policy != NORA_RESET_LATCH_PRESERVE_RCON ) &&
          ( policy != NORA_RESET_LATCH_AND_CLEAR_RCON ) ) )
    {
        return false;
    }

    // Read RCON once, before any optional clear.
    s_snapshot_raw      = (uint32_t)RCON;
    s_snapshot_captured = true;
    s_snapshot_cause    = NORA_RESET_CAUSE_UNKNOWN;

    if( policy == NORA_RESET_LATCH_AND_CLEAR_RCON )
    {
        s_snapshot_cause = reset_snapshot_decode( s_snapshot_raw );
        nora_reset_cause_clear();
    }

    return true;
}


bool nora_reset_snapshot_capture_forwarded( uint32_t raw )
{
    if( s_snapshot_captured )
    {
        return false;
    }

    s_snapshot_raw      = raw;
    s_snapshot_captured = true;
    s_snapshot_cause    = reset_snapshot_decode( raw );

    // No nora_reset_cause_clear() here, deliberately. The stage that captured `raw` had to
    // clear the bits to keep its own next reset unambiguous, so RCON is already clean; a
    // clear would be a write with nothing to write about.
    return true;
}


bool nora_reset_snapshot_is_captured( void )
{
    return s_snapshot_captured;
}


nora_reset_cause_t nora_reset_snapshot_cause( void )
{
    return s_snapshot_cause;
}


uint32_t nora_reset_snapshot_raw( void )
{
    return s_snapshot_raw;
}


bool nora_reset_snapshot_is_power_on_class( void )
{
    return ( s_snapshot_cause == NORA_RESET_CAUSE_POWER_ON ) ||
           ( s_snapshot_cause == NORA_RESET_CAUSE_BROWNOUT );
}


bool nora_reset_snapshot_is_warm( void )
{
    // Everything that is NOT a power event: the dsPIC reset while the board (and a
    // codec) may have kept power. UNKNOWN/OTHER map here so an un-captured or novel
    // cause errs on the safe side (quiesce first).
    return !nora_reset_snapshot_is_power_on_class();
}


const char *nora_reset_snapshot_cause_str( void )
{
    switch( s_snapshot_cause )
    {
        case NORA_RESET_CAUSE_POWER_ON:  return "POR (power-on, cold)";
        case NORA_RESET_CAUSE_BROWNOUT:  return "BOR (brown-out, cold)";
        case NORA_RESET_CAUSE_EXTERNAL:  return "MCLR (external, warm)";
        case NORA_RESET_CAUSE_SOFTWARE:  return "SWR (software, warm)";
        case NORA_RESET_CAUSE_WATCHDOG:  return "WDT (watchdog, warm)";
        case NORA_RESET_CAUSE_OTHER:     return "OTHER (warm)";
        case NORA_RESET_CAUSE_UNKNOWN:
        default:                         return "UNKNOWN (warm)";
    }
}


const char *nora_reset_cause_str( uint32_t latched, bool is_latched )
{
    uint8_t i;
    uint8_t set_count = 0u;

    for( i = 0u; i < (uint8_t)NORA_RESET_CAUSE_COUNT; i++ )
    {
        if( ( latched & s_reset_causes[ i ].mask ) != 0u )
        {
            set_count++;
        }
    }

    if( set_count == 0u )
    {
        return "unknown";
    }

    /*
     * On a board that does not latch, two or more candidates means the bits have
     * accumulated across resets and the most specific one is NOT necessarily this
     * boot's cause. Say that instead of guessing; the raw word is still available to
     * the caller and is the honest evidence.
     *
     * A POR/BOR pair is the one case where two bits are normal even on a latching
     * board, which is why this test only applies when the caller admits it did not latch.
     */
    if( !is_latched && ( set_count > 1u ) )
    {
        return "(multiple bits set, not latched -- read the raw word)";
    }

    for( i = 0u; i < (uint8_t)NORA_RESET_CAUSE_COUNT; i++ )
    {
        if( ( latched & s_reset_causes[ i ].mask ) != 0u )
        {
            return s_reset_causes[ i ].name;
        }
    }

    return "unknown"; // unreachable: set_count > 0 guarantees a hit above
}


void nora_reset_cause_clear( void )
{
    // Only the cause bits are touched. IDLE/SLEEP report the wake source and
    // BUCKR/VREGnR the regulator state -- neither is a reset cause, so both are left
    // alone for a debugger to read.
    RCONbits.POR  = 0;
    RCONbits.BOR  = 0;
    RCONbits.EXTR = 0;
    RCONbits.SWR  = 0;
    RCONbits.WDTO = 0;
    RCONbits.CM   = 0;
}
