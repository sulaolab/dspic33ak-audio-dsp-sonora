/* Provenance: written from DS70005591 ch.18 (ITC), ch.30 (ADC) and the DFP SFR
 * header p33AK512MPS512.h only. No vendor touch-library source, header or
 * binary was consulted; the data sheet's Example 18-4 was read for facts about
 * the silicon and not transcribed.
 */

#include "nora_itc_internal.h"

#include <stddef.h>

#include <xc.h>

#include "nora_itc_dspic33ak_reg.h"

/* ---------------------------------------------------------------------------
 * Is there an ITC on this part?
 *
 * The dsPIC33AK128MC106 configuration targets a motor-control part with no ITC
 * and no ADC 5: every register below is simply absent from its DFP header, so
 * this file failed to compile for that configuration with 20 "undeclared here"
 * errors. It went unnoticed because that configuration had not been built since
 * open touch became the default.
 *
 * The guard is the DFP header's own object-like macro (p33AK512MPS512.h has
 * "#define ITCLS0CON ITCLS0CON" beside the SFR declaration), so the question
 * asked is "does this device have the peripheral", not "which device name did
 * someone type". The stubs below answer NORA_ITC_ERR_UNSUPPORTED, which
 * nora_itc_internal.h already defines as "not an AK512" -- so this is the designed
 * answer, not a workaround: nora_touch_init() fails cleanly, main.c already
 * handles that, and nothing needs excluding from the project.
 * ------------------------------------------------------------------------- */
#if defined(ITCLS0CON)

/* ---------------------------------------------------------------------------
 * Per-list register set, indexed by list number — the same shape the other
 * dsPIC33AK backends use for their instances (see nora_spi_dspic33ak.c). The
 * row is pasted from the number rather than written out, so an index and its
 * ITCLSx registers cannot disagree. The table is a definition, so it stays
 * here; its type and the field positions it is used with are in
 * nora_itc_dspic33ak_reg.h.
 *
 * Unlike SPI there is no per-row #if and no unused [0]: the three lists exist
 * on every part that has an ITC at all, and list 0 is a real list.
 * ------------------------------------------------------------------------- */
#define ITC_LIST_REG_ROW(n)   { &ITCLS##n##CON, &ITCLS##n##SEQ, &ITCLS##n##TMR, \
                                &ITCLS##n##STAT, &ITCLS##n##MUL }

static const itc_list_regs_t itc_list_regs[NORA_ITC_LIST_COUNT] = {
    ITC_LIST_REG_ROW(0),   /* [0] ITCLS0 */
    ITC_LIST_REG_ROW(1),   /* [1] ITCLS1 */
    ITC_LIST_REG_ROW(2),   /* [2] ITCLS2 */
};

/* ---------------------------------------------------------------------------
 * Driver state
 * ------------------------------------------------------------------------- */
typedef struct {
    bool                     initialized;
    bool                     scan_running;
    uint8_t                  record_count;
    uint8_t                  cvdcap;
    uint8_t                  acc_count;
    uint32_t                 clock_hz;
    uint16_t                 charge_counts;
    uint16_t                 balance_counts;
    uint32_t                 scans_completed;
    nora_itc_trigger_t       trigger;
    nora_itc_status_t        last_status;
    nora_itc_scan_callback_t callback;
} itc_list_state_t;

static itc_list_state_t itc_state[NORA_ITC_LIST_COUNT];
static bool             itc_hardware_started;

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* TAD is the ITC's unit of time, not a core clock cycle: §18.3.7 says the
 * acquisition and math sequencers run from the ADC 5 TAD clock, and spec AD50
 * gives TAD = FIN / 4 with 12.5 ns min and 125 ns max. On this board CLKGEN6 is
 * 200 MHz, so TAD is 20 ns, not the data sheet example's 12.5 ns. Every timer
 * value is therefore computed, never copied: a count lifted from the example
 * would be short by 1.6x and would present as a weak electrode. */
static uint32_t itc_tad_ps(uint32_t clock_hz)
{
    /* picoseconds, so 200 MHz -> 20000 ps with no rounding error to argue about */
    return (uint32_t)((4000000000UL / (clock_hz / 1000UL)));
}

static bool itc_ns_to_counts(uint32_t clock_hz, uint32_t ns, uint32_t max,
                             uint16_t *counts)
{
    uint32_t tad_ps = itc_tad_ps(clock_hz);
    uint32_t value;

    if (tad_ps == 0u) {
        return false;
    }

    /* Round up: a charge time that is short measures a partly charged
     * capacitor, which reads as a smaller signal and not as an error. */
    value = ((ns * 1000UL) + (tad_ps - 1u)) / tad_ps;

    /* A request that rounds to zero counts is rejected rather than silently
     * turned into "no delay" — it is the failure mode that looks electrical. */
    if ((value == 0u) || (value > max)) {
        return false;
    }

    *counts = (uint16_t)value;
    return true;
}

static bool itc_list_valid(nora_itc_list_t list)
{
    return ((unsigned)list < NORA_ITC_LIST_COUNT);
}

static void itc_reg_field_write(volatile uint32_t *reg, uint32_t shift,
                                uint32_t mask, uint32_t value)
{
    uint32_t tmp = *reg;

    tmp &= ~(mask << shift);
    tmp |= ((value & mask) << shift);
    *reg = tmp;
}

/* ---------------------------------------------------------------------------
 * The acquisition sequence
 *
 * Pseudo-differential CVD is two samples with the drive polarities swapped, and
 * the result is their difference. Sample A charges the internal CVD capacitor
 * to VDD while the electrode is held low, breaks the connection, connects the
 * two so the charge divides, and converts. Sample B does the same with both
 * polarities inverted. Subtracting cancels the converter's offset and most of
 * the common-mode injected noise, and it is why ITCCON1.SIGN must be set.
 *
 * Eight commands, in two groups of four, both carrying sequence number 4 so the
 * hardware runs them as one sequence; the SECOND bit marks the sample-B half so
 * the math sequence can tell which conversion it is looking at.
 *
 * The polarity assignment below (electrode low + capacitor high in sample A) is
 * the documented CVD arrangement, but which of PC0/PCC the silicon routes to
 * the capacitor node is a fact worth confirming with a scope on the first
 * board — the tuning manual's first bring-up step. Swapping the two samples
 * only flips the sign of every result, which is why a wrong guess here is
 * cheap; getting break-before-make wrong is not, and that is written as 3.
 * ------------------------------------------------------------------------- */
static void itc_program_acquisition_sequence(void)
{
    volatile uint32_t *cmd = ITC_DATACMD_ARRAY;
    /* Group B is never selected (ITCTXB stays 0) and the per-record GRDA/GRDB
     * fields are NONE, so PCB drives nothing; it is written Low rather than left
     * at 0 because 0 means "hand the pin back to TRIS/LAT" and only 1/2 are
     * driven levels -- see the DCMD_PC_* comment in the register header.
     *
     * PCA is the shield. With ENA_TOUCH_SHIELD_PHASE off, PCA is a static Low and
     * ITCTXA is 0, so it drives nothing at all and the shield's static Low comes
     * from RE4 as a GPIO (main.c) -- that is the condition every measurement up
     * to 2026-08-30 was taken under. With the switch on, ITCTXA selects CVDTX23
     * and PCA follows the sample's driven polarity: Low through sample A
     * (electrode Low, capacitor High) and High through sample B (both swapped).
     * That is the squarest available approximation of "the guard follows the CVD
     * waveform as closely as possible" (DS70005591 p.1485). It is held for all
     * four steps of a sample rather than only the charge step, because the
     * electrode is high-Z from break-before-make onwards and a shield that stops
     * driving mid-sample couples its own transition into the node being
     * measured. */
    uint32_t pcb_off  = ((uint32_t)DCMD_PC_LOW << DCMD_PCB_SHIFT);
    uint32_t shield_a = ((uint32_t)DCMD_PC_LOW << DCMD_PCA_SHIFT) | pcb_off;
#if defined(ENA_TOUCH_SHIELD_PHASE)
    uint32_t shield_b = ((uint32_t)DCMD_PC_HIGH << DCMD_PCA_SHIFT) | pcb_off;
#else
    uint32_t shield_b = shield_a;
#endif

    /* ---- Sample A ---------------------------------------------------- */
    /* charge the CVD capacitor, electrode held low, for timer A */
    cmd[0] = DCMD_CHRG |
             ((uint32_t)DCMD_PC_LOW  << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_HIGH << DCMD_PCC_SHIFT) |
             shield_a |
             ((uint32_t)DCMD_LOOP_TMRA << DCMD_LOOP_SHIFT);

    /* break before make: everything tri-stated, no wait */
    cmd[1] = ((uint32_t)DCMD_PC_TRISTATE << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PCC_SHIFT) |
             shield_a;

    /* connect electrode and capacitor, settle for timer B */
    cmd[2] = DCMD_BAL |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PCC_SHIFT) |
             shield_a |
             ((uint32_t)DCMD_LOOP_TMRB << DCMD_LOOP_SHIFT);

    /* convert, and wait for end of conversion before moving on */
    cmd[3] = DCMD_CONV | DCMD_MSTART |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PCC_SHIFT) |
             shield_a |
             ((uint32_t)DCMD_LOOP_ADC_EOC << DCMD_LOOP_SHIFT);

    /* ---- Sample B: the same four with both polarities swapped -------- */
    cmd[4] = DCMD_SECOND | DCMD_DISCHRG |
             ((uint32_t)DCMD_PC_HIGH << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_LOW  << DCMD_PCC_SHIFT) |
             shield_b |
             ((uint32_t)DCMD_LOOP_TMRA << DCMD_LOOP_SHIFT);

    cmd[5] = DCMD_SECOND |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PCC_SHIFT) |
             shield_b;

    cmd[6] = DCMD_SECOND | DCMD_BAL |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PCC_SHIFT) |
             shield_b |
             ((uint32_t)DCMD_LOOP_TMRB << DCMD_LOOP_SHIFT);

    /* last command of the sequence: convert, run the math, END */
    cmd[7] = DCMD_SECOND | DCMD_CONV | DCMD_MSTART | DCMD_END |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PC0_SHIFT) |
             ((uint32_t)DCMD_PC_TRISTATE << DCMD_PCC_SHIFT) |
             shield_b |
             ((uint32_t)DCMD_LOOP_ADC_EOC << DCMD_LOOP_SHIFT);

    /* SDATAMAP: groups 0 and 1 (SDATACMD0..3 and SDATACMD4..7) both belong to
     * sequence 4, at 8-command granularity, so the eight commands above are one
     * sequence. Groups 2 and 3 are left as they are — nothing points at them. */
    itc_reg_field_write(&SDATAMAP, DATAMAP_SEQ_SHIFT + (0u * MAP_GROUP_STRIDE),
                        0x7u, ITC_SEQ_ACQUISITION);
    itc_reg_field_write(&SDATAMAP, DATAMAP_SPLIT_SHIFT + (0u * MAP_GROUP_STRIDE),
                        0x3u, MAP_SPLIT_8CMD);
    itc_reg_field_write(&SDATAMAP, DATAMAP_SEQ_SHIFT + (1u * MAP_GROUP_STRIDE),
                        0x7u, ITC_SEQ_ACQUISITION);
    itc_reg_field_write(&SDATAMAP, DATAMAP_SPLIT_SHIFT + (1u * MAP_GROUP_STRIDE),
                        0x3u, MAP_SPLIT_8CMD);
}

/* ---------------------------------------------------------------------------
 * The math sequence
 *
 * The ALU makes the subtraction free, which is the whole reason the CPU has no
 * per-sample work to do here:
 *
 *   command 0, run after sample A's conversion: take the ADC result as A,
 *   function = A, store it in accumulator B, write nothing anywhere.
 *   command 1, run after sample B's conversion: B input = accumulator B (that
 *   is sample A), A input = the new ADC result, function = B - A, always save.
 *   The save target is the record's own ITCRESx, chosen by BIN's encoding of
 *   where results live rather than by an address we compute.
 *
 * Accumulation across the 2^ACCCNT repeats is the hardware's, not ours: with
 * ACCEN set the sequencer re-runs the pair and sums into ITCRESx.
 * ------------------------------------------------------------------------- */
static void itc_program_math_sequence(void)
{
    volatile uint32_t *cmd = ITC_MATHCMD_ARRAY;
    uint8_t            group;

    /* There are *two* math sequences, not one sequence of two commands, and the
     * difference is the whole pseudo-differential measurement.
     *
     * SDATACMDx.MSTART launches "the math sequence" after each conversion, and
     * SDATACMDx.SECOND is what says which one. So the sample-A command and the
     * sample-B command have to live in separate sequences, selected by the
     * acquisition command that triggered them. Put both in one sequence — the
     * first attempt here — and every MSTART runs both: sample A latches itself
     * into accumulator B and then immediately computes AccB - A = 0 and writes
     * it. Found on hardware 2026-08-13: the list scanned, the records were
     * accepted, the timers were right, and all three counts read exactly 0.
     *
     * The mapping is 8-command granularity (SPLIT = 2), so the first sequence
     * starts at SMATHCMD0 (group 0) and the second at SMATHCMD8 (group 2).
     * One command each, so each carries END. */

    /* First math sequence — sample A: latch the conversion into accumulator B
     * and write nothing out. */
    cmd[0] = ((uint32_t)MCMD_AIN_ADC5 << MCMD_AIN_SHIFT) |
             ((uint32_t)MCMD_F_A      << MCMD_F_SHIFT)   |
             ((uint32_t)MCMD_WM_NEVER << MCMD_WM_SHIFT)  |
             MCMD_ACCB | MCMD_END;

    /* Second math sequence — sample B, in two steps, because the difference has
     * to be *added to* ITCRESx and not written over it.
     *
     * ACCCNT repeats the acquisition+math pair 2^ACCCNT times per record before
     * the result is stored, but nothing in the hardware sums the repeats for
     * you: that is what F = BIN + AIN with BIN = ITCRESx is for (§8). Writing
     * the difference with WM = always instead makes every repeat overwrite the
     * last, so the stored result is a single measurement whatever ACCCNT says.
     * Found on hardware 2026-08-14: raising ACCCNT from 4 to 8 left the count
     * unchanged at ~-3410 instead of scaling by 16, and a finger showed up as
     * per-scan scatter of ~200 counts — mains hum, sampled at one phase per
     * scan and never averaged — rather than as a shift.
     *
     * So: latch Sample A - Sample B into accumulator A, writing nothing, then
     * add accumulator A into ITCRESx. */
    cmd[8] = ((uint32_t)MCMD_AIN_ADC5      << MCMD_AIN_SHIFT) |
             ((uint32_t)MCMD_BIN_ACCB      << MCMD_BIN_SHIFT) |
             ((uint32_t)MCMD_F_B_MINUS_A   << MCMD_F_SHIFT)   |
             ((uint32_t)MCMD_WM_NEVER      << MCMD_WM_SHIFT)  |
             MCMD_ACCA;

    cmd[9] = ((uint32_t)MCMD_AIN_ACCA      << MCMD_AIN_SHIFT) |
             ((uint32_t)MCMD_BIN_RESULT    << MCMD_BIN_SHIFT) |
             ((uint32_t)MCMD_F_B_PLUS_A    << MCMD_F_SHIFT)   |
             ((uint32_t)MCMD_WM_ALWAYS     << MCMD_WM_SHIFT)  |
             MCMD_END;

    /* The data sheet states three "must match" rules for the MAP registers, all
     * of which fail silently: MATHSEQn must equal ITCLSxSEQ.MATHSEQ, SECONDn
     * must be set iff the acquisition command's SECOND is, and ACCn must be set
     * whenever ACCCNT != 0. This HAL always accumulates (ACCEN is required even
     * for a single measurement), so ACCn is unconditional. CMPn is clear
     * because this HAL leaves the comparator off, CM = 0. */
    for (group = 0u; group <= 2u; group += 2u) {
        uint32_t base = (uint32_t)group * MAP_GROUP_STRIDE;

        itc_reg_field_write(&SMATHMAP, MATHMAP_SEQ_SHIFT + base, 0x7u,
                            ITC_SEQ_MATH);
        itc_reg_field_write(&SMATHMAP, MATHMAP_SPLIT_SHIFT + base, 0x3u,
                            MAP_SPLIT_8CMD);
        itc_reg_field_write(&SMATHMAP, MATHMAP_ACC_BIT + base, 0x1u, 1u);
        itc_reg_field_write(&SMATHMAP, MATHMAP_CMP_BIT + base, 0x1u, 0u);
        itc_reg_field_write(&SMATHMAP, MATHMAP_SECOND_BIT + base, 0x1u,
                            (group == 0u) ? 0u : 1u);
    }
}

/* ---------------------------------------------------------------------------
 * ADC 5
 * ------------------------------------------------------------------------- */
static nora_itc_status_t itc_adc5_start(void)
{
    uint32_t timeout = AD5_READY_TIMEOUT;

    if (AD5CONbits.ON == 0u) {
        AD5CONbits.ON = 1u;
    }

    while ((AD5CONbits.ADRDY == 0u) && (timeout != 0u)) {
        timeout--;
    }
    if (AD5CONbits.ADRDY == 0u) {
        return NORA_ITC_ERR_ADC_NOT_READY;
    }

    /* Channel 5 is the CVD channel: its positive input is the touch mux, its
     * negative input is VSS, and it is triggered by the ITC and by nothing
     * else. FRAC/DIFF stay clear — the difference we want is the ITC's
     * A-minus-B in the math ALU, not the converter's differential mode. */
    AD5CH5CON1bits.TRG1SRC = AD5_TRG1SRC_ITC;
    AD5CH5CON1bits.TRG2SRC = 0u;
    AD5CH5CON1bits.PINSEL  = AD5_PINSEL_CVD;
    AD5CH5CON1bits.NINSEL  = AD5_NINSEL_VSS;
    AD5CH5CON1bits.DIFF    = 0u;
    AD5CH5CON1bits.FRAC    = 0u;

    return NORA_ITC_OK;
}

/* ---------------------------------------------------------------------------
 * ITC core
 * ------------------------------------------------------------------------- */
static nora_itc_status_t itc_core_start(void)
{
    uint32_t timeout = ITC_READY_TIMEOUT;

    if (itc_hardware_started) {
        return NORA_ITC_OK;
    }

    /* SIGN is set unconditionally. The pseudo-differential result is signed,
     * and a cleared SIGN is not a mode — it is a measurement that reads as a
     * huge positive number whenever the difference goes the other way, which
     * looks like an electrical fault and is not one. */
    ITCCON1bits.SIGN = 1u;
    ITCCON1bits.ON   = 1u;

    /* DRDY after ON is the documented handshake. Skipping it leaves a
     * peripheral that reads back as configured and does nothing. */
    while ((ITCSTATbits.DRDY == 0u) && (timeout != 0u)) {
        timeout--;
    }
    if (ITCSTATbits.DRDY == 0u) {
        ITCCON1bits.ON = 0u;
        return NORA_ITC_ERR_NOT_READY;
    }

    ITCCON1bits.CVDEN = 1u;

#if defined(ENA_TOUCH_SHIELD_PHASE)
    /* ITCTXA bit x selects CVDTXx into group A, whose drive is the commands' PCA
     * field (DS70005591 p.1456-1458). Bit 23 is CVDTX23 = RP69 = RE4, which on
     * this board is the touch shield copper through R10 = 100 ohm (R11 DNP, so
     * that copper has no ground of its own and is meant to be driven). */
    ITCTXA = (1uL << 23u);
#else
    ITCTXA = 0uL;
#endif

    itc_program_acquisition_sequence();
    itc_program_math_sequence();

    itc_hardware_started = true;
    return NORA_ITC_OK;
}

static void itc_program_records(const nora_itc_list_config_t *config)
{
    volatile uint32_t *rec = ITC_REC_ARRAY;
    uint8_t i;

    for (i = 0u; i < config->record_count; i++) {
        const nora_itc_record_config_t *rc = &config->records[i];
        uint32_t shift = ITC_REC_SHIFT(i);
#if defined(ENA_TOUCH_SHIELD_PHASE)
        /* Guards are forced off, not merely expected to be off. Group A holds
         * both the CVDTXx pins selected in ITCTXA *and* whatever GRDA/GRDB
         * select, and GRDA/GRDB select the adjacent CVDANx *electrode* -- on this
         * board the adjacent channel of every pad is that pad's own redundant
         * channel (CVDAN8's PIN+1 is CVDAN9, same copper). A phase-driven PCA
         * with a guard selected would therefore drive the node being measured.
         * Overriding here rather than asserting keeps the shield experiment from
         * depending on a caller that is free to change. */
        uint32_t half  = ((uint32_t)rc->cvdan & ITC_REC_PIN_MASK) |
                         ((uint32_t)NORA_ITC_GUARD_NONE << ITC_REC_GRDA_SHIFT) |
                         ((uint32_t)NORA_ITC_GUARD_NONE << ITC_REC_GRDB_SHIFT);
#else
        uint32_t half  = ((uint32_t)rc->cvdan & ITC_REC_PIN_MASK) |
                         ((uint32_t)rc->guard_a << ITC_REC_GRDA_SHIFT) |
                         ((uint32_t)rc->guard_b << ITC_REC_GRDB_SHIFT);
#endif
        uint32_t word  = rec[ITC_REC_WORD(i)];

        word &= ~(ITC_REC_HALF_MASK << shift);
        word |= (half << shift);
        rec[ITC_REC_WORD(i)] = word;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
nora_itc_status_t nora_itc_init(nora_itc_list_t list,
                                const nora_itc_list_config_t *config)
{
    const itc_list_regs_t *regs;
    itc_list_state_t      *state;
    nora_itc_status_t      status;
    uint16_t               charge_counts = 0u;
    uint16_t               balance_counts = 0u;
    uint16_t               period_counts = 0u;
    uint32_t               con;
    uint32_t               seq;

    if (!itc_list_valid(list) || (config == NULL) ||
        (config->records == NULL) ||
        (config->record_count == 0u) ||
        (config->record_count > NORA_ITC_RECORD_MAX) ||
        (config->cvdcap > NORA_ITC_CVDCAP_MAX) ||
        (config->acc_count > NORA_ITC_ACCCNT_MAX) ||
        (config->clock_hz < 1000000UL)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    regs  = &itc_list_regs[list];
    state = &itc_state[list];

    if (!itc_ns_to_counts(config->clock_hz, config->charge_ns,
                          ITC_TIMER_MAX, &charge_counts) ||
        !itc_ns_to_counts(config->clock_hz, config->balance_ns,
                          ITC_TIMER_MAX, &balance_counts)) {
        state->last_status = NORA_ITC_ERR_TIMING;
        return NORA_ITC_ERR_TIMING;
    }

    if (config->trigger == NORA_ITC_TRIGGER_INTERNAL_TIMER) {
        /* TMRPR is documented in the same TAD unit as the sequencer timers, so
         * the period is converted the same way; at 20 ns per count the whole
         * 16-bit range is only 1.31 ms, which is why a panel scan period is
         * built by the caller re-triggering rather than by one long period.
         * A period that does not fit is refused, not truncated. */
        if (!itc_ns_to_counts(config->clock_hz, config->period_us * 1000UL,
                              ITC_TMRPR_MAX, &period_counts)) {
            state->last_status = NORA_ITC_ERR_TIMING;
            return NORA_ITC_ERR_TIMING;
        }
    }

    status = itc_adc5_start();
    if (status != NORA_ITC_OK) {
        state->last_status = status;
        return status;
    }

    status = itc_core_start();
    if (status != NORA_ITC_OK) {
        state->last_status = status;
        return status;
    }

    /* Stop the list before rewriting it: a list that is armed while its record
     * count and sequence numbers change scans a half-described panel. */
    *regs->CON &= ~((1uL << LSCON_TRGEN_BIT) | (1uL << LSCON_SAMP_BIT));

    /* Self-capacitance connects no pins together — ITCLSxMUL is the mutual-capacitance
     * / gang-drive knob and self-cap wants it empty. */
    *regs->MUL = 0u;

    itc_program_records(config);

    *regs->TMR = ((uint32_t)charge_counts) |
                 ((uint32_t)balance_counts << 8u);

    seq  = 0u;
    seq |= ((uint32_t)config->acc_count << LSSEQ_ACCCNT_SHIFT);
    seq |= (1uL << LSSEQ_ACCEN_BIT);   /* required even for a single sample */
    seq |= ((uint32_t)ITC_SEQ_MATH        << LSSEQ_MATHSEQ_SHIFT);
    seq |= ((uint32_t)ITC_SEQ_ACQUISITION << LSSEQ_DATASEQ_SHIFT);
    seq |= ((uint32_t)config->cvdcap      << LSSEQ_CVDCAP_SHIFT);
    *regs->SEQ = seq;

    con  = 0u;
    con |= ((uint32_t)(config->record_count - 1u) & LSCON_RECCNT_MASK)
           << LSCON_RECCNT_SHIFT;
    con |= ((uint32_t)((config->mode == NORA_ITC_SCAN_LIST_IRQ_END)
                        ? ITC_MODE_LIST_IRQ_END : ITC_MODE_LIST_NO_IRQ))
           << LSCON_MODE_SHIFT;
    con |= (1uL << LSCON_TRGCLR_BIT);  /* one scan per trigger, not a free run */

    if (config->trigger == NORA_ITC_TRIGGER_INTERNAL_TIMER) {
        con |= ((uint32_t)ITC_SSRC_INTERNAL_TIMER << LSCON_SSRC_SHIFT);
    } else {
        con |= ((uint32_t)ITC_SSRC_SOFTWARE << LSCON_SSRC_SHIFT);
    }
    *regs->CON = con;

    /* TMRPR belongs to the internal timer alone. */
    if (config->trigger == NORA_ITC_TRIGGER_INTERNAL_TIMER) {
        ITCCON2bits.TMRPR = period_counts;
    }

    /* TRGEN gates the list's trigger whatever the source is, software included:
     * it is "this list may run", not "the timer may run". Found on hardware
     * 2026-08-13 — with it set only on the timer path, a software-triggered list
     * reported configured and DRDY-ready and then never started, because the
     * SAMP 1->0 edge was gated off. BUSY never set, ACCDONE never set, and
     * nora_itc_scan_complete() timed out with next_record still 0, which reads
     * exactly like a dead electrode. Enable it for both paths. */
    *regs->CON |= (1uL << LSCON_TRGEN_BIT);
    switch (list) {
    case NORA_ITC_LIST_0: ITCCON2bits.TRGEN0 = 1u; break;
    case NORA_ITC_LIST_1: ITCCON2bits.TRGEN1 = 1u; break;
    default:              ITCCON2bits.TRGEN2 = 1u; break;
    }

    state->initialized     = true;
    state->scan_running    = false;
    state->record_count    = config->record_count;
    state->cvdcap          = config->cvdcap;
    state->acc_count       = config->acc_count;
    state->clock_hz        = config->clock_hz;
    state->charge_counts   = charge_counts;
    state->balance_counts  = balance_counts;
    state->scans_completed = 0u;
    state->trigger         = config->trigger;
    state->last_status     = NORA_ITC_OK;

    return NORA_ITC_OK;
}

nora_itc_status_t nora_itc_deinit(nora_itc_list_t list)
{
    const itc_list_regs_t *regs;
    uint8_t i;

    if (!itc_list_valid(list)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    regs = &itc_list_regs[list];
    *regs->CON &= ~((1uL << LSCON_TRGEN_BIT) | (1uL << LSCON_SAMP_BIT));

    switch (list) {
    case NORA_ITC_LIST_0: ITCCON2bits.TRGEN0 = 0u; break;
    case NORA_ITC_LIST_1: ITCCON2bits.TRGEN1 = 0u; break;
    default:              ITCCON2bits.TRGEN2 = 0u; break;
    }

    itc_state[list].initialized  = false;
    itc_state[list].scan_running = false;
    itc_state[list].callback     = NULL;

    /* Turn the core off only when no list still wants it, and leave ADC 5 and
     * the clock alone in every case: this HAL did not create either. */
    for (i = 0u; i < NORA_ITC_LIST_COUNT; i++) {
        if (itc_state[i].initialized) {
            return NORA_ITC_OK;
        }
    }
    ITCCON1bits.CVDEN   = 0u;
    ITCCON1bits.ON      = 0u;
    itc_hardware_started = false;

    return NORA_ITC_OK;
}

nora_itc_status_t nora_itc_scan_start(nora_itc_list_t list)
{
    const itc_list_regs_t *regs;
    itc_list_state_t      *state;

    if (!itc_list_valid(list)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    state = &itc_state[list];
    if (!state->initialized) {
        return NORA_ITC_ERR_NOT_INITIALIZED;
    }
    /* Refusing here keeps the two trigger models from being half-mixed: a
     * software start on a timer-driven list would race the timer. */
    if (state->trigger != NORA_ITC_TRIGGER_SOFTWARE) {
        return NORA_ITC_ERR_UNSUPPORTED;
    }

    regs = &itc_list_regs[list];
    if ((*regs->STAT & (1uL << LSSTAT_BUSY_BIT)) != 0u) {
        return NORA_ITC_ERR_BUSY;
    }

    /* Re-arm the trigger every scan. TRGCLR is set, which is what makes one
     * trigger mean one scan — and it clears TRGEN when the scan finishes, so a
     * TRGEN written once at init only ever buys the first scan. Confirmed on
     * hardware 2026-08-13: a register dump taken after a successful scan showed
     * TRGEN back at 0. Setting it here is idempotent and costs one OR. */
    *regs->CON |= (1uL << LSCON_TRGEN_BIT);

    /* Arm, then start. SAMP = 1 holds the acquisition, SAMP = 0 releases it —
     * writing 0 without the 1 first does nothing at all. */
    *regs->CON |= (1uL << LSCON_SAMP_BIT);
    *regs->CON &= ~(1uL << LSCON_SAMP_BIT);

    state->scan_running = true;
    return NORA_ITC_OK;
}

bool nora_itc_scan_complete(nora_itc_list_t list)
{
    const itc_list_regs_t *regs;
    volatile uint32_t     *rec;
    uint8_t                i;

    if (!itc_list_valid(list) || !itc_state[list].initialized) {
        return false;
    }

    regs = &itc_list_regs[list];
    if ((*regs->STAT & (1uL << LSSTAT_BUSY_BIT)) != 0u) {
        return false;
    }

    /* BUSY going away is necessary but not sufficient — a list that never
     * started is also not busy. Completion is every record's ACCDONE. */
    rec = ITC_REC_ARRAY;
    for (i = 0u; i < itc_state[list].record_count; i++) {
        uint32_t word = rec[ITC_REC_WORD(i)] >> ITC_REC_SHIFT(i);
        if ((word & (1uL << ITC_REC_ACCDONE_BIT)) == 0u) {
            return false;
        }
    }

    return true;
}

nora_itc_status_t nora_itc_read(nora_itc_list_t list, uint8_t record_index,
                                int32_t *result)
{
    itc_list_state_t *state;

    if (!itc_list_valid(list) || (result == NULL)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }
    state = &itc_state[list];
    if (!state->initialized) {
        return NORA_ITC_ERR_NOT_INITIALIZED;
    }
    if (record_index >= state->record_count) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    /* Reading ITCRESx is what clears that record's ACCDONE, so this call is the
     * one that consumes completion. Results are signed by construction. */
    *result = (int32_t)ITC_RES_ARRAY[record_index];
    return NORA_ITC_OK;
}

nora_itc_status_t nora_itc_read_all(nora_itc_list_t list, int32_t *results,
                                    uint8_t results_len)
{
    itc_list_state_t *state;
    uint8_t           i;

    if (!itc_list_valid(list) || (results == NULL)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }
    state = &itc_state[list];
    if (!state->initialized) {
        return NORA_ITC_ERR_NOT_INITIALIZED;
    }
    if (results_len < state->record_count) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    for (i = 0u; i < state->record_count; i++) {
        results[i] = (int32_t)ITC_RES_ARRAY[i];
    }

    state->scan_running = false;
    state->scans_completed++;
    return NORA_ITC_OK;
}

/* ---------------------------------------------------------------------------
 * Interrupt
 *
 * One vector serves all three lists; ITCSTAT.INT0/1/2 says which fired. Writes
 * to IFS10 / IEC10 / IPC40 name one bit with a compile-time-constant value,
 * because the compiler emits a single-bit atomic write only then: the shared
 * word carries other subsystems' bits, and a read-modify-write of the whole
 * word can drop one of theirs.
 *
 * The ISR is compiled only under NORA_ITC_USE_INTERRUPT. This HAL is an
 * independent implementation that drives the ITC directly, and any other code
 * that defines _ITCInterrupt in the same image fails the link on a duplicate
 * symbol; the two could not share the peripheral either, so exactly one owner
 * of touch per build is the rule and not a workaround. Bring-up is by polling
 * (nora_itc_scan_complete), which is what the console wants anyway, and
 * nora_itc_irq_enable() refuses instead of half-working when the ISR is not
 * compiled in.
 * ------------------------------------------------------------------------- */
#if defined(NORA_ITC_USE_INTERRUPT)
nora_itc_status_t nora_itc_irq_enable(nora_itc_list_t list, uint8_t priority,
                                      nora_itc_scan_callback_t callback)
{
    if (!itc_list_valid(list) || (callback == NULL) ||
        (priority == 0u) || (priority > 7u)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }
    if (!itc_state[list].initialized) {
        return NORA_ITC_ERR_NOT_INITIALIZED;
    }

    itc_state[list].callback = callback;

    _ITCIF = 0;
    _ITCIP = priority;
    _ITCIE = 1;

    return NORA_ITC_OK;
}

nora_itc_status_t nora_itc_irq_disable(nora_itc_list_t list)
{
    uint8_t i;

    if (!itc_list_valid(list)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    itc_state[list].callback = NULL;

    /* The vector is shared, so it is only disabled once no list wants it. */
    for (i = 0u; i < NORA_ITC_LIST_COUNT; i++) {
        if (itc_state[i].callback != NULL) {
            return NORA_ITC_OK;
        }
    }
    _ITCIE = 0;
    _ITCIF = 0;

    return NORA_ITC_OK;
}

/*
 * `context` not `no_auto_psv`: the alternate W0-W7 array is inherently tied to the IPL
 * on dsPIC33A, so each nesting level gets its own bank and this thunk needs no prologue
 * push -- and a prologue push at an ISR's first instruction is the documented trigger of
 * the A1 silicon STACK ERROR. Rationale in full above the vectors in
 * nora_i2c_dspic33ak_device.c; the DO-NOT-REVERT case is in the application-level
 * ASRC clock control.
 */
void __attribute__((interrupt, context)) _ITCInterrupt(void)
{
    uint8_t i;

    for (i = 0u; i < NORA_ITC_LIST_COUNT; i++) {
        const itc_list_regs_t *regs = &itc_list_regs[i];

        if ((*regs->STAT & (1uL << LSSTAT_INT_BIT)) == 0u) {
            continue;
        }
        *regs->STAT &= ~(1uL << LSSTAT_INT_BIT);

        if (itc_state[i].callback != NULL) {
            itc_state[i].callback((nora_itc_list_t)i);
        }
    }

    _ITCIF = 0;
}

#else  /* !NORA_ITC_USE_INTERRUPT */

nora_itc_status_t nora_itc_irq_enable(nora_itc_list_t list, uint8_t priority,
                                      nora_itc_scan_callback_t callback)
{
    (void)list;
    (void)priority;
    (void)callback;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_irq_disable(nora_itc_list_t list)
{
    (void)list;
    return NORA_ITC_ERR_UNSUPPORTED;
}

#endif /* NORA_ITC_USE_INTERRUPT */

/* ---------------------------------------------------------------------------
 * Test injection
 * ------------------------------------------------------------------------- */
nora_itc_status_t nora_itc_test_inject_enable(uint16_t value)
{
    if (!itc_hardware_started) {
        return NORA_ITC_ERR_NOT_INITIALIZED;
    }

    /* Every conversion now returns `value` instead of the converter's output,
     * which drives accumulation, the A-B arithmetic and every layer above it
     * with known numbers and no electrode. TSTDATA is written before TSTEN so
     * no scan can observe the enable with a stale value behind it. */
    ITCSTATbits.TSTDATA = value;
    ITCSTATbits.TSTEN   = 1u;
    return NORA_ITC_OK;
}

nora_itc_status_t nora_itc_test_inject_disable(void)
{
    if (!itc_hardware_started) {
        return NORA_ITC_ERR_NOT_INITIALIZED;
    }
    ITCSTATbits.TSTEN = 0u;
    return NORA_ITC_OK;
}

/* ---------------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------------- */
nora_itc_status_t nora_itc_get_info(nora_itc_list_t list, nora_itc_info_t *info)
{
    const itc_list_regs_t *regs;
    const itc_list_state_t *state;
    uint32_t stat;

    if (!itc_list_valid(list) || (info == NULL)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    regs  = &itc_list_regs[list];
    state = &itc_state[list];
    stat  = *regs->STAT;

    info->initialized       = state->initialized;
    info->hardware_ready    = (ITCSTATbits.DRDY != 0u);
    info->list_busy         = ((stat & (1uL << LSSTAT_BUSY_BIT)) != 0u);
    info->next_record       = (uint8_t)(stat & LSSTAT_NEXT_MASK);
    info->test_inject_active = (ITCSTATbits.TSTEN != 0u);
    info->scan_running      = state->scan_running;
    info->record_count      = state->record_count;
    info->cvdcap            = state->cvdcap;
    info->acc_count         = state->acc_count;
    info->clock_hz          = state->clock_hz;
    info->charge_counts     = state->charge_counts;
    info->balance_counts    = state->balance_counts;
    info->scans_completed   = state->scans_completed;
    info->last_status       = state->last_status;

    return NORA_ITC_OK;
}

/* ---------------------------------------------------------------------------
 * Raw register readback
 *
 * Deliberately a flat table rather than a struct: it is read once, by a human,
 * during bring-up, and a table can carry the ADC 5 registers this HAL depends on
 * but does not own (AD5CON, AD5CH5CON1) next to the ITC's own — which is where
 * the coupling that makes the peripheral work either is or is not.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char              *name;
    volatile const uint32_t *reg;
} itc_debug_reg_t;

#define ITC_DBG(sfr) { #sfr, (volatile const uint32_t *)&(sfr) }

static const itc_debug_reg_t itc_debug_regs[] = {
    /* The comparator hit bits are ADHIT, not "ITCHIT": the name this document
     * set once used does not exist in the DFP header. */
    ITC_DBG(ITCCON1),   ITC_DBG(ITCCON2),   ITC_DBG(ITCSTAT),  ITC_DBG(ADHIT),
    ITC_DBG(ITCLS0CON), ITC_DBG(ITCLS0SEQ), ITC_DBG(ITCLS0TMR),
    ITC_DBG(ITCLS0STAT), ITC_DBG(ITCLS0MUL),
    ITC_DBG(SDATAMAP),  ITC_DBG(SMATHMAP),
    ITC_DBG(SDATACMD0), ITC_DBG(SDATACMD1), ITC_DBG(SDATACMD2), ITC_DBG(SDATACMD3),
    ITC_DBG(SDATACMD4), ITC_DBG(SDATACMD5), ITC_DBG(SDATACMD6), ITC_DBG(SDATACMD7),
    /* The math sequence is two *sequences* of one command, at 0 and 8. */
    ITC_DBG(SMATHCMD0), ITC_DBG(SMATHCMD8), ITC_DBG(SMATHCMD9),
    ITC_DBG(ITCREC0),   ITC_DBG(ITCREC1),
    ITC_DBG(ITCRES0),   ITC_DBG(ITCRES1),   ITC_DBG(ITCRES2),
    ITC_DBG(AD5CON),    ITC_DBG(AD5CH5CON1),
};

nora_itc_status_t nora_itc_debug_reg(uint8_t index,
                                     const char **name,
                                     uint32_t *value)
{
    if ((name == NULL) || (value == NULL)) {
        return NORA_ITC_ERR_INVALID_ARG;
    }
    if (index >= (uint8_t)(sizeof(itc_debug_regs) / sizeof(itc_debug_regs[0]))) {
        return NORA_ITC_ERR_INVALID_ARG;
    }

    /* ITCRESx is in the table and reading it clears that record's ACCDONE, so a
     * dump consumes completion exactly as nora_itc_read() would. That is a fair
     * price for seeing the result words, but it means a dump between a scan and
     * a read changes the answer — stated here because it will surprise someone. */
    *name  = itc_debug_regs[index].name;
    *value = *itc_debug_regs[index].reg;
    return NORA_ITC_OK;
}

#else /* !defined(ITCLS0CON) -- no ITC on this device */

/* Every entry point, answering NORA_ITC_ERR_UNSUPPORTED. Deliberately the full
 * API rather than a shorter "the caller should not have linked this": the touch
 * layer and the console are compiled for every configuration, and a link error
 * would push a device fact out to five project files where it would be gotten
 * wrong once. A caller that ignores the status gets zero scans and a
 * nora_itc_get_info() that says initialized = false, never stale numbers. */

nora_itc_status_t nora_itc_init(nora_itc_list_t list,
                                const nora_itc_list_config_t *config)
{
    (void)list; (void)config;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_deinit(nora_itc_list_t list)
{
    (void)list;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_scan_start(nora_itc_list_t list)
{
    (void)list;
    return NORA_ITC_ERR_UNSUPPORTED;
}

bool nora_itc_scan_complete(nora_itc_list_t list)
{
    (void)list;
    return false;
}

nora_itc_status_t nora_itc_read(nora_itc_list_t list, uint8_t record_index,
                                int32_t *result)
{
    (void)list; (void)record_index; (void)result;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_read_all(nora_itc_list_t list, int32_t *results,
                                    uint8_t results_len)
{
    (void)list; (void)results; (void)results_len;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_irq_enable(nora_itc_list_t list, uint8_t priority,
                                      nora_itc_scan_callback_t callback)
{
    (void)list; (void)priority; (void)callback;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_irq_disable(nora_itc_list_t list)
{
    (void)list;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_test_inject_enable(uint16_t value)
{
    (void)value;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_test_inject_disable(void)
{
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_get_info(nora_itc_list_t list, nora_itc_info_t *info)
{
    (void)list;
    if (info == NULL) {
        return NORA_ITC_ERR_INVALID_ARG;
    }
    /* Zeroed, then last_status says why: initialized = false and
     * hardware_ready = false are the truth about a part with no ITC. */
    *info = (nora_itc_info_t){ 0 };
    info->last_status = NORA_ITC_ERR_UNSUPPORTED;
    return NORA_ITC_ERR_UNSUPPORTED;
}

nora_itc_status_t nora_itc_debug_reg(uint8_t index,
                                     const char **name,
                                     uint32_t *value)
{
    (void)index; (void)name; (void)value;
    /* Not INVALID_ARG: the console walks index upward until INVALID_ARG, so
     * returning it would print "0 registers" and read as a healthy empty dump.
     * UNSUPPORTED stops the walk AND says why. */
    return NORA_ITC_ERR_UNSUPPORTED;
}

#endif /* defined(ITCLS0CON) */
