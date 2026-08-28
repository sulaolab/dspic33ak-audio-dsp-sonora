//===========================================================
// app_silicon.c -- read the device identity out of the signature space.
//
// THE ADDRESSES ARE FROM THE DEVICE DESCRIPTION FILE, NOT FROM A FORUM POST. The DFP's
// ATDF for this part declares them as memory segments of type "signatures":
//     <memory-segment start="0x7C2000" size="0x4" name="devid"/>
//     <memory-segment start="0x7C2004" size="0x4" name="revid"/>
// and the compiler header agrees on the first (__DEVID_BASE 0x7c2000, __DEVID_LENGTH 0x4).
// Both were confirmed against the debugger, which reports the same pair on this board as
// "Device Id = 0xa77c" / "Device Revision Id = 0x1".
//
// NO PSV, NO TABLE READ. dsPIC33A has one unified address space, so a plain volatile load
// reaches program space -- the same thing nora_udid_dspic33ak.c does for the UDID words,
// and the reason this file needs no device-specific read machinery at all.
//===========================================================
#include "app_silicon.h"

#include <stdio.h>

/* Signature space, per the ATDF segments quoted above. */
#define APP_SILICON_DEVID_ADDRESS   (0x007C2000UL)
#define APP_SILICON_REVID_ADDRESS   (0x007C2004UL)

/* dsPIC33AK512MPS512. Checked before naming a revision: a DEVREV of 1 means "A1" only if
 * we are actually looking at the part whose revision numbering we know. */
#define APP_SILICON_DEVID_AK512MPS512  (0x0000A77CUL)

/* Errata DS80001162E numbers the revisions of this part 01h = A1, 02h = A2. Only the low
 * byte is the revision; the rest of the word is masked off rather than assumed zero. */
#define APP_SILICON_REVID_MASK      (0x000000FFUL)
#define APP_SILICON_REVID_A1        (0x01UL)
#define APP_SILICON_REVID_A2        (0x02UL)

static uint32_t app_silicon_read_word( uint32_t address )
{
    const volatile uint32_t *source;

    source = (const volatile uint32_t *)(uintptr_t)address;
    return *source;
}

uint32_t app_silicon_devid( void )
{
    return app_silicon_read_word( APP_SILICON_DEVID_ADDRESS );
}

uint32_t app_silicon_revid( void )
{
    return app_silicon_read_word( APP_SILICON_REVID_ADDRESS );
}

app_silicon_rev_t app_silicon_rev( void )
{
    uint32_t revid;

    /* A revision number only means what this file thinks it means on the part this file
     * knows. On anything else the honest answer is UNKNOWN, which never refuses. */
    if( app_silicon_devid() != APP_SILICON_DEVID_AK512MPS512 )
    {
        return APP_SILICON_REV_UNKNOWN;
    }

    revid = app_silicon_revid() & APP_SILICON_REVID_MASK;
    if( revid == APP_SILICON_REVID_A1 )
    {
        return APP_SILICON_REV_A1;
    }
    if( revid == APP_SILICON_REVID_A2 )
    {
        return APP_SILICON_REV_A2;
    }
    return APP_SILICON_REV_UNKNOWN;
}

const char *app_silicon_rev_str( app_silicon_rev_t rev )
{
    switch( rev )
    {
        case APP_SILICON_REV_A1: return "A1";
        case APP_SILICON_REV_A2: return "A2";
        default:                 return "unknown";
    }
}

const char *app_silicon_device_str( void )
{
    if( app_silicon_devid() == APP_SILICON_DEVID_AK512MPS512 )
    {
        return "dsPIC33AK512MPS512";
    }
    return "unknown device";
}

void app_silicon_print_identity( void )
{
    /* Unconditional, in every configuration, and in the banner rather than after it. The
     * part name and revision come first because that is what a reader is looking for; the
     * raw words follow in parentheses so a log from an unrecognised die -- one this file
     * can only call "unknown device" -- still identifies it exactly. */
    printf( " Silicon: %s rev %s (DEVID=%08lX DEVREV=%08lX)\n",
            app_silicon_device_str(),
            app_silicon_rev_str( app_silicon_rev() ),
            (unsigned long)app_silicon_devid(),
            (unsigned long)app_silicon_revid() );

#if ( APP_SILICON_EXPECTED_REV != 0 )
    /* An image that will refuse says so before it refuses. Otherwise the only evidence of
     * the requirement is the refusal message, which does not appear on the boards that
     * pass -- and then no log records that the check was armed at all. */
    printf( " Silicon: this image requires rev %s (APP_SILICON_EXPECTED_REV=%d)\n",
            app_silicon_rev_str( (app_silicon_rev_t)APP_SILICON_EXPECTED_REV ),
            (int)APP_SILICON_EXPECTED_REV );
#endif
}

bool app_silicon_check( void )
{
#if ( APP_SILICON_EXPECTED_REV != 0 )
    app_silicon_rev_t rev;

    rev = app_silicon_rev();

    if( rev == APP_SILICON_REV_UNKNOWN )
    {
        /* Say it out loud rather than pass quietly: the run is going ahead unverified, and
         * whoever reads the log later has to know the check did not happen. */
        printf( " Silicon: image expects %s but the die could not be identified --"
                " starting anyway, UNVERIFIED\n",
                app_silicon_rev_str( (app_silicon_rev_t)APP_SILICON_EXPECTED_REV ) );
        return true;
    }

    if( (int)rev != (int)APP_SILICON_EXPECTED_REV )
    {
        printf( "\n*** SILICON MISMATCH -- AUDIO ENGINE NOT STARTED ***\n" );
        printf( "    this image was built for %s, this board is %s\n",
                app_silicon_rev_str( (app_silicon_rev_t)APP_SILICON_EXPECTED_REV ),
                app_silicon_rev_str( rev ) );
        printf( "    Nothing in the signal path is running. Flash the image built for %s,\n",
                app_silicon_rev_str( rev ) );
        printf( "    or rebuild with APP_SILICON_EXPECTED_REV=%d (0 = announce only).\n",
                (int)rev );
        return false;
    }
#endif

    return true;
}
