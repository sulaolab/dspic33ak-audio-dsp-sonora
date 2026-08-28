#ifndef TEST_FAKE_XC_H
#define TEST_FAKE_XC_H

#include <stdint.h>

#include "nora_fake_clock.h"

/*
 * The SFR names src/app/hal_clock/nora_clock_dspic33ak_reg.c uses, mapped onto the
 * host model in nora_fake_clock.c.  Nothing else in the Clock HAL includes
 * <xc.h>, so this file is the complete boundary between the code under test and
 * the silicon.
 *
 * The `bits` names expand to a dereferenced accessor rather than to a variable,
 * because that is what runs the model's sequencer on each access -- see the
 * header for why the sequencer has to exist and what it costs (no read/write
 * distinction).  The plain names are read-only word forms: they appear only in
 * the raw-capture functions, which read and never write.
 *
 * The generator numbering is not dense (1, 5, 6, 8, 9, 10, 12, 13) and this file
 * declares exactly the ones the reg layer names.  A generator added to the
 * backend without being added here shows up as a compile error, which is the
 * outcome to want: the alternative is a host build that silently omits it.
 */

#define CLK1CONbits  (*nora_fake_clkcon(1u))
#define CLK1DIVbits  (*nora_fake_clkdiv(1u))
#define CLK5CONbits  (*nora_fake_clkcon(5u))
#define CLK5DIVbits  (*nora_fake_clkdiv(5u))
#define CLK6CONbits  (*nora_fake_clkcon(6u))
#define CLK6DIVbits  (*nora_fake_clkdiv(6u))
#define CLK8CONbits  (*nora_fake_clkcon(8u))
#define CLK8DIVbits  (*nora_fake_clkdiv(8u))
#define CLK9CONbits  (*nora_fake_clkcon(9u))
#define CLK9DIVbits  (*nora_fake_clkdiv(9u))
#define CLK10CONbits (*nora_fake_clkcon(10u))
#define CLK10DIVbits (*nora_fake_clkdiv(10u))
#define CLK12CONbits (*nora_fake_clkcon(12u))
#define CLK12DIVbits (*nora_fake_clkdiv(12u))
#define CLK13CONbits (*nora_fake_clkcon(13u))
#define CLK13DIVbits (*nora_fake_clkdiv(13u))

#define CLK1CON  nora_fake_clkcon_word(1u)
#define CLK1DIV  nora_fake_clkdiv_word(1u)
#define CLK5CON  nora_fake_clkcon_word(5u)
#define CLK5DIV  nora_fake_clkdiv_word(5u)
#define CLK6CON  nora_fake_clkcon_word(6u)
#define CLK6DIV  nora_fake_clkdiv_word(6u)
#define CLK8CON  nora_fake_clkcon_word(8u)
#define CLK8DIV  nora_fake_clkdiv_word(8u)
#define CLK9CON  nora_fake_clkcon_word(9u)
#define CLK9DIV  nora_fake_clkdiv_word(9u)
#define CLK10CON nora_fake_clkcon_word(10u)
#define CLK10DIV nora_fake_clkdiv_word(10u)
#define CLK12CON nora_fake_clkcon_word(12u)
#define CLK12DIV nora_fake_clkdiv_word(12u)
#define CLK13CON nora_fake_clkcon_word(13u)
#define CLK13DIV nora_fake_clkdiv_word(13u)

#define PLL1CONbits (*nora_fake_pllcon(1u))
#define PLL1DIVbits (*nora_fake_plldiv(1u))
#define PLL2CONbits (*nora_fake_pllcon(2u))
#define PLL2DIVbits (*nora_fake_plldiv(2u))

#define PLL1CON nora_fake_pllcon_word(1u)
#define PLL1DIV nora_fake_plldiv_word(1u)
#define PLL2CON nora_fake_pllcon_word(2u)
#define PLL2DIV nora_fake_plldiv_word(2u)

#define OSCCTRLbits (*nora_fake_oscctrl())
#define OSCCTRL     nora_fake_oscctrl_word()
#define CLKFAIL     nora_fake_clkfail_word()

#endif /* TEST_FAKE_XC_H */
