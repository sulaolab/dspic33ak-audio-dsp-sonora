/*
 * nora_gpio_dspic33ak.c
 * ----------------
 * Small, readable NORA GPIO backend for dsPIC33AK devices. See nora_gpio.h for the
 * public contract, pin addressing, and interrupt-safety policy.
 *
 * Implementation notes:
 *   - One device-neutral driver body drives any port through a table of plain
 *     32-bit register pointers (s_gpio_regs[], indexed by port code). Only the
 *     table is device-specific; a port row is emitted only when the device
 *     header defines that port's SFRs (#if defined(LATx)), so the table tracks
 *     the silicon automatically.
 *   - GPIO SFRs on dsPIC33AK are 32-bit in the DFP headers; the register
 *     pointers are uint32_t to match.
 *   - Accessors are plain read-modify-write and never disable interrupts.
 */

//===========================================================
// INCLUDES
//===========================================================
#include "nora_gpio.h"

#include <xc.h>

#include "nora_gpio_dspic33ak_reg.h"


//===========================================================
// Definition
//===========================================================

/* Per-port register pointers (uniform 32-bit SFRs). */
typedef struct
{
    volatile uint32_t *port;    /* read input level   */
    volatile uint32_t *lat;     /* output latch       */
    volatile uint32_t *tris;    /* 1 = input, 0 = output */
    volatile uint32_t *ansel;   /* 1 = analog, 0 = digital */
    volatile uint32_t *odc;     /* 1 = open-drain     */
    volatile uint32_t *cnpu;    /* 1 = pull-up        */
    volatile uint32_t *cnpd;    /* 1 = pull-down      */
} nora_gpio_regs_t;

#define NORA_GPIO_ARRAY_LEN(a)   (sizeof(a) / sizeof((a)[0]))

#define NORA_GPIO_PORT_ROW(L) \
    { &PORT##L, &LAT##L, &TRIS##L, &ANSEL##L, &ODC##L, &CNPU##L, &CNPD##L }
#define NORA_GPIO_PORT_NONE \
    { 0, 0, 0, 0, 0, 0, 0 }


//===========================================================
// Function Prototype
//===========================================================

static const nora_gpio_regs_t *nora_gpio_regs_for(nora_gpio_pin_t pin);
static uint32_t                      nora_gpio_mask(nora_gpio_pin_t pin);


//===========================================================
// Variables
//===========================================================

/*
 * Indexed by nora_gpio_port_t (A=0 .. H=7). Every slot is present so the
 * index always equals the port code; a port absent on the device expands to a
 * NULL row and is rejected by nora_gpio_regs_for().
 */
static const nora_gpio_regs_t s_gpio_regs[] =
{
#if defined(LATA)
    NORA_GPIO_PORT_ROW(A),     /* [0] PORTA */
#else
    NORA_GPIO_PORT_NONE,
#endif
#if defined(LATB)
    NORA_GPIO_PORT_ROW(B),     /* [1] PORTB */
#else
    NORA_GPIO_PORT_NONE,
#endif
#if defined(LATC)
    NORA_GPIO_PORT_ROW(C),     /* [2] PORTC */
#else
    NORA_GPIO_PORT_NONE,
#endif
#if defined(LATD)
    NORA_GPIO_PORT_ROW(D),     /* [3] PORTD */
#else
    NORA_GPIO_PORT_NONE,
#endif
#if defined(LATE)
    NORA_GPIO_PORT_ROW(E),     /* [4] PORTE */
#else
    NORA_GPIO_PORT_NONE,
#endif
#if defined(LATF)
    NORA_GPIO_PORT_ROW(F),     /* [5] PORTF */
#else
    NORA_GPIO_PORT_NONE,
#endif
#if defined(LATG)
    NORA_GPIO_PORT_ROW(G),     /* [6] PORTG */
#else
    NORA_GPIO_PORT_NONE,
#endif
#if defined(LATH)
    NORA_GPIO_PORT_ROW(H),     /* [7] PORTH */
#else
    NORA_GPIO_PORT_NONE,
#endif
};


//===========================================================
// Global Function
//===========================================================

bool nora_gpio_set_direction(nora_gpio_pin_t pin, nora_gpio_dir_t dir)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    uint32_t mask = nora_gpio_mask(pin);

    if (dir == NORA_GPIO_DIR_OUTPUT)
    {
        nora_gpio_reg_clear(r->tris, mask);   /* TRIS = 0 -> output */
    }
    else
    {
        nora_gpio_reg_set(r->tris, mask);     /* TRIS = 1 -> input  */
    }
    return true;
}

bool nora_gpio_set_pull(nora_gpio_pin_t pin, nora_gpio_pull_t pull)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    uint32_t mask = nora_gpio_mask(pin);

    switch (pull)
    {
    case NORA_GPIO_PULL_UP:
        nora_gpio_reg_clear(r->cnpd, mask);
        nora_gpio_reg_set(r->cnpu, mask);
        break;
    case NORA_GPIO_PULL_DOWN:
        nora_gpio_reg_clear(r->cnpu, mask);
        nora_gpio_reg_set(r->cnpd, mask);
        break;
    case NORA_GPIO_PULL_NONE:
    default:
        nora_gpio_reg_clear(r->cnpu, mask);
        nora_gpio_reg_clear(r->cnpd, mask);
        break;
    }
    return true;
}

bool nora_gpio_set_analog(nora_gpio_pin_t pin, bool analog)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    uint32_t mask = nora_gpio_mask(pin);

    if (analog)
    {
        nora_gpio_reg_set(r->ansel, mask);    /* ANSEL = 1 -> analog  */
    }
    else
    {
        nora_gpio_reg_clear(r->ansel, mask);  /* ANSEL = 0 -> digital */
    }
    return true;
}

bool nora_gpio_set_open_drain(nora_gpio_pin_t pin, bool enable)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    uint32_t mask = nora_gpio_mask(pin);

    if (enable)
    {
        nora_gpio_reg_set(r->odc, mask);
    }
    else
    {
        nora_gpio_reg_clear(r->odc, mask);
    }
    return true;
}

bool nora_gpio_config(nora_gpio_pin_t pin, const nora_gpio_config_t *config)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if ((r == 0) || (config == 0))
    {
        return false;
    }
    uint32_t mask = nora_gpio_mask(pin);

    /* When a live output is being changed to an input, stop driving it before
     * changing its electrical attributes.  This makes pull, analog and
     * open-drain reconfiguration safe for a runtime direction transition. */
    if (config->dir == NORA_GPIO_DIR_INPUT)
    {
        nora_gpio_reg_set(r->tris, mask);     /* input */
    }

    /* analog / digital */
    if (config->analog)
    {
        nora_gpio_reg_set(r->ansel, mask);
    }
    else
    {
        nora_gpio_reg_clear(r->ansel, mask);
    }

    /* pull */
    (void)nora_gpio_set_pull(pin, config->pull);

    /* open-drain */
    if (config->open_drain)
    {
        nora_gpio_reg_set(r->odc, mask);
    }
    else
    {
        nora_gpio_reg_clear(r->odc, mask);
    }

    /* For an output, set the initial level before enabling the driver so the
     * pin does not glitch to a stale latch value. */
    if (config->dir == NORA_GPIO_DIR_OUTPUT)
    {
        if (config->initial_high)
        {
            nora_gpio_reg_set(r->lat, mask);
        }
        else
        {
            nora_gpio_reg_clear(r->lat, mask);
        }
        nora_gpio_reg_clear(r->tris, mask);   /* output */
    }
    return true;
}

bool nora_gpio_write(nora_gpio_pin_t pin, bool high)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    uint32_t mask = nora_gpio_mask(pin);

    if (high)
    {
        nora_gpio_reg_set(r->lat, mask);
    }
    else
    {
        nora_gpio_reg_clear(r->lat, mask);
    }
    return true;
}

bool nora_gpio_set(nora_gpio_pin_t pin)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    nora_gpio_reg_set(r->lat, nora_gpio_mask(pin));
    return true;
}

bool nora_gpio_clear(nora_gpio_pin_t pin)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    nora_gpio_reg_clear(r->lat, nora_gpio_mask(pin));
    return true;
}

bool nora_gpio_toggle(nora_gpio_pin_t pin)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return false;
    }
    nora_gpio_reg_toggle(r->lat, nora_gpio_mask(pin));
    return true;
}

nora_gpio_level_t nora_gpio_read(nora_gpio_pin_t pin)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return NORA_GPIO_LEVEL_ERROR;
    }
    return nora_gpio_reg_is_set(r->port, nora_gpio_mask(pin))
           ? NORA_GPIO_LEVEL_HIGH : NORA_GPIO_LEVEL_LOW;
}

nora_gpio_level_t nora_gpio_read_output(nora_gpio_pin_t pin)
{
    const nora_gpio_regs_t *r = nora_gpio_regs_for(pin);
    if (r == 0)
    {
        return NORA_GPIO_LEVEL_ERROR;
    }
    return nora_gpio_reg_is_set(r->lat, nora_gpio_mask(pin))
           ? NORA_GPIO_LEVEL_HIGH : NORA_GPIO_LEVEL_LOW;
}

bool nora_gpio_config_digital_input(nora_gpio_pin_t pin)
{
    const nora_gpio_config_t cfg =
    {
        .dir = NORA_GPIO_DIR_INPUT,  .pull = NORA_GPIO_PULL_NONE,
        .analog = false, .open_drain = false, .initial_high = false,
    };
    return nora_gpio_config(pin, &cfg);
}

bool nora_gpio_config_digital_output(nora_gpio_pin_t pin, bool initial_high)
{
    const nora_gpio_config_t cfg =
    {
        .dir = NORA_GPIO_DIR_OUTPUT, .pull = NORA_GPIO_PULL_NONE,
        .analog = false, .open_drain = false, .initial_high = initial_high,
    };
    return nora_gpio_config(pin, &cfg);
}


//===========================================================
// Local Function
//===========================================================

/* Resolve a packed pin to its port register set, or NULL if the port is not
 * present on this device (no register row). */
static const nora_gpio_regs_t *nora_gpio_regs_for(nora_gpio_pin_t pin)
{
    unsigned port = (unsigned)(pin >> 4);

    if (port >= NORA_GPIO_ARRAY_LEN(s_gpio_regs))
    {
        return 0;
    }
    if (s_gpio_regs[port].lat == 0)
    {
        return 0;
    }
    return &s_gpio_regs[port];
}

/* Single-bit mask for the pin's bit position (0..15). */
static uint32_t nora_gpio_mask(nora_gpio_pin_t pin)
{
    return (uint32_t)1u << (pin & 0x0Fu);
}

//===========================================================
// Remappable-pin (RPn) <-> packed-pin conversion. See the RP-first section in
// nora_gpio.h. Flat encoding rule (no table): RPn = packed_pin + 1.
// Range-only validation (encoding); package bonding is not checked here.
//===========================================================
bool nora_gpio_pin_from_rp(nora_gpio_rp_t rp, nora_gpio_pin_t *pin)
{
    if (pin == 0 || rp == 0u || rp > NORA_GPIO_RP_MAX)
    {
        return false;
    }
    *pin = (nora_gpio_pin_t)((uint16_t)rp - 1u);
    return true;
}

bool nora_gpio_rp_from_pin(nora_gpio_pin_t pin, nora_gpio_rp_t *rp)
{
    if (rp == 0 || (uint16_t)pin >= NORA_GPIO_RP_MAX)
    {
        return false;
    }
    *rp = (uint8_t)((uint16_t)pin + 1u);
    return true;
}

//===========================================================
// RP adapter API. Thin wrappers: convert RPn -> packed pin (the validated
// nora_gpio_pin_from_rp), then call the packed-pin function. No GPIO
// register access and no copy of the RP->pin formula here.
//===========================================================
bool nora_gpio_rp_config(nora_gpio_rp_t rp, const nora_gpio_config_t *config)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_config(pin, config);
}

bool nora_gpio_rp_set_direction(nora_gpio_rp_t rp, nora_gpio_dir_t dir)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_set_direction(pin, dir);
}

bool nora_gpio_rp_set_pull(nora_gpio_rp_t rp, nora_gpio_pull_t pull)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_set_pull(pin, pull);
}

bool nora_gpio_rp_set_analog(nora_gpio_rp_t rp, bool analog)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_set_analog(pin, analog);
}

bool nora_gpio_rp_set_open_drain(nora_gpio_rp_t rp, bool enable)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_set_open_drain(pin, enable);
}

bool nora_gpio_rp_config_digital_input(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_config_digital_input(pin);
}

bool nora_gpio_rp_config_digital_output(nora_gpio_rp_t rp, bool initial_high)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_config_digital_output(pin, initial_high);
}

bool nora_gpio_rp_set(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_set(pin);
}

bool nora_gpio_rp_clear(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_clear(pin);
}

bool nora_gpio_rp_toggle(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_toggle(pin);
}

bool nora_gpio_rp_write(nora_gpio_rp_t rp, bool high)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return false;
    }
    return nora_gpio_write(pin, high);
}

nora_gpio_level_t nora_gpio_rp_read(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return NORA_GPIO_LEVEL_ERROR;
    }
    return nora_gpio_read(pin);
}

nora_gpio_level_t nora_gpio_rp_read_output(nora_gpio_rp_t rp)
{
    nora_gpio_pin_t pin;
    if (!nora_gpio_pin_from_rp(rp, &pin))
    {
        return NORA_GPIO_LEVEL_ERROR;
    }
    return nora_gpio_read_output(pin);
}
