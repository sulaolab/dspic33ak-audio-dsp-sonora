#ifndef NORA_ITC_INTERNAL_H
#define NORA_ITC_INTERNAL_H

/* Provenance: written from DS70005591 ch.18 (ITC) and the DFP SFR header only.
 * No vendor touch-library source, header or binary was consulted.
 */

/* INTERNAL to the touch HAL. Not an application include file.
 *
 * A caller asks this HAL for touch, not for a CVD scan, so the acquisition
 * peripheral is not part of the public interface: `nora_touch.h` is. Everything
 * a bring-up console legitimately needs from the layer below — hardware state,
 * a single scan, raw counts, a register dump, test injection — is reachable
 * through the `nora_touch_hw_*` entry points in that header, which exist for
 * exactly that reason. Include this file only from the touch HAL's own
 * translation units.
 *
 * The name keeps `itc` deliberately: ITC is the data sheet's own name for the
 * block (DS70005591 ch.18) and the SFR prefix, so a file with `itc` in its name
 * is the layer that sits against the silicon.
 */

#include <stdint.h>
#include <stdbool.h>

/*
 * Integrated Touch Controller (ITC) HAL — dsPIC33AK512MPS512.
 *
 * The ITC is a micro-sequencer that drives electrode pins and the internal CVD
 * capacitor array, converts on ADC 5, and does the pseudo-differential
 * arithmetic itself. So this HAL is a *configurator plus a result reader*, not a
 * sampling driver: there is no per-sample entry point, because there is no
 * per-sample work for the CPU to do.
 *
 * Scope of this layer, stated as a limit:
 *   - one scan list at a time, described whole and applied whole
 *   - CVD pseudo-differential acquisition only (the sole documented use)
 *   - raw signed per-record results; no baseline, filter, threshold or gesture
 *   - no pin ownership: the caller sets TRIS/LAT/ANSEL for its electrodes
 *   - no clock ownership: CLKGEN6 is raised by the project's clock boot code
 *     before this HAL is initialised, and the ITC inherits it. This HAL only
 *     *reads* the frequency it was told, to convert microseconds into timer
 *     counts.
 *   - AK512 only. dsPIC33AK128MC106 has no ITCCON1; there is no fallback.
 *
 * Detection and filtering belong in nora_touch, which consumes this. Gestures
 * and scrollers belong above both -- see the Non-goals in nora_touch.h.
 */

/* ---------------------------------------------------------------------------
 * Fixed hardware limits (DS70005591 ch.18). Named so callers can size arrays
 * and so a wrong index is a compile-time or an argument error, never silence.
 * ------------------------------------------------------------------------- */
#define NORA_ITC_LIST_COUNT        (3u)   /* ITCLS0/1/2 — independent scan lists */
#define NORA_ITC_RECORD_MAX        (32u)  /* ITCREC0..15, two records each       */
#define NORA_ITC_RESULT_MAX        (32u)  /* ITCRES0..31, one per record         */
#define NORA_ITC_CVDCAP_MAX        (7u)   /* DFP header declares CVDCAP:3        */
#define NORA_ITC_ACCCNT_MAX        (15u)  /* accumulation depth 2^ACCCNT         */

typedef enum {
    NORA_ITC_LIST_0 = 0,
    NORA_ITC_LIST_1 = 1,
    NORA_ITC_LIST_2 = 2,
} nora_itc_list_t;

typedef enum {
    NORA_ITC_OK = 0,
    NORA_ITC_ERR_INVALID_ARG,
    NORA_ITC_ERR_UNSUPPORTED,     /* not an AK512, or a mode this HAL omits    */
    NORA_ITC_ERR_NOT_INITIALIZED,
    NORA_ITC_ERR_ADC_NOT_READY,   /* ADC 5 never raised ADRDY                  */
    NORA_ITC_ERR_NOT_READY,       /* ITCSTAT.DRDY never came up                */
    NORA_ITC_ERR_TIMING,          /* a requested time does not fit its timer   */
    NORA_ITC_ERR_BUSY,            /* a scan is in flight                       */
    NORA_ITC_ERR_TIMEOUT,
} nora_itc_status_t;

/* ---------------------------------------------------------------------------
 * Trigger
 * ------------------------------------------------------------------------- */
typedef enum {
    /* ITCLSxCON.SSRC = 0 — the SAMP arm(1)/start(0) handshake. Used while
     * bringing a board up, because it puts the scan under console control. */
    NORA_ITC_TRIGGER_SOFTWARE = 0,
    /* ITCLSxCON.SSRC = 7 — the ITC's own timer, period ITCCON2.TMRPR. The
     * periodic scan costs no CPU and no general-purpose timer. */
    NORA_ITC_TRIGGER_INTERNAL_TIMER = 1,
} nora_itc_trigger_t;

/* ITCLSxCON.MODE. Only the two whole-list modes are exposed: a per-record
 * interrupt buys nothing for a panel scan, and the comparator-gated modes
 * belong with the comparator, which this HAL does not use. Mode 3 does not
 * exist in the data sheet's table. */
typedef enum {
    NORA_ITC_SCAN_LIST_NO_IRQ  = 0,   /* MODE = 4: all records, no interrupt   */
    NORA_ITC_SCAN_LIST_IRQ_END = 1,   /* MODE = 5: all records, one interrupt  */
} nora_itc_scan_mode_t;

/* ---------------------------------------------------------------------------
 * One measurement target
 * ------------------------------------------------------------------------- */
/* ITCRECx.GRDAn / GRDBn are 2-bit *relative* selectors, not pin numbers: a
 * record's guard can only be the electrode's immediate neighbour in the CVDANx
 * numbering. So guarding is a board-layout constraint before it is a software
 * one — an electrode whose neighbours are used for something else cannot have a
 * hardware guard, and no amount of configuration fixes that. */
typedef enum {
    NORA_ITC_GUARD_NONE    = 0,
    NORA_ITC_GUARD_PIN_M1  = 1,   /* the pin at PIN - 1 */
    NORA_ITC_GUARD_PIN_P1  = 2,   /* the pin at PIN + 1 */
} nora_itc_guard_t;

typedef struct {
    /* CVDANx analog-input number for the electrode, as the data sheet's pin
     * table names it — not an RPn and not a port bit. 7 bits in ITCRECx.PINn. */
    uint8_t cvdan;
    nora_itc_guard_t guard_a;   /* ITCRECx.GRDAn */
    nora_itc_guard_t guard_b;   /* ITCRECx.GRDBn */
} nora_itc_record_config_t;

/* ---------------------------------------------------------------------------
 * One scan list
 *
 * Times are given in nanoseconds and converted here against clock_hz, because
 * the data sheet's example assumes ~320 MHz and this board runs the ITC's ADC
 * clock at 200 MHz. A copied timer count would be wrong by 1.6x and would look
 * like a marginal electrode.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Frequency of the clock feeding ADC 5 / the ITC, in hertz. Passed in, not
     * queried, so this HAL owns no clock: the board states what it built. */
    uint32_t clock_hz;

    const nora_itc_record_config_t *records;
    uint8_t                         record_count;   /* 1..NORA_ITC_RECORD_MAX  */

    uint32_t charge_ns;    /* ITCLSxTMR.TMRA — charge / discharge time         */
    uint32_t balance_ns;   /* ITCLSxTMR.TMRB — charge-balance settling time    */

    /* ITCLSxSEQ.CVDCAP: internal CVD capacitor code. The data sheet documents
     * 0..17.5 pF in 2.5 pF steps, its own example writes a value that fits
     * neither the field width nor the step, and the header says 3 bits. So this
     * is a *code*, not a capacitance, and the mapping is measured on the bench
     * per board — the reason the tuning manual opens with it. Per list, which
     * is why it lives here and not in the record. */
    uint8_t cvdcap;

    /* Hardware oversampling: 2^acc_count acquisitions are summed per record
     * before the result is stored. 0 means one measurement — which still
     * requires ACCEN, so there is no "accumulation off" to express here. */
    uint8_t acc_count;

    nora_itc_trigger_t   trigger;
    /* Scan period, used only with NORA_ITC_TRIGGER_INTERNAL_TIMER (TMRPR). */
    uint32_t             period_us;
    nora_itc_scan_mode_t mode;
} nora_itc_list_config_t;

/* ---------------------------------------------------------------------------
 * Lifecycle
 *
 * Enable order is not the caller's to get right: nora_itc_init() brings ADC 5
 * up and waits ADRDY, then ITCCON1.ON and waits DRDY, then CVDEN, then ACCEN,
 * because ITCSTAT.DRDY not being polled after ON is a documented way to get a
 * dead peripheral that reads as configured.
 * ------------------------------------------------------------------------- */

/* Configure ADC 5 channel 5 (PINSEL = 5, NINSEL = VSS, TRG1SRC = 26 = ITC) and
 * the ITC itself, then apply one list. SIGN is set unconditionally: the
 * pseudo-differential result is signed, and clearing it is not a mode, it is a
 * broken measurement that looks electrical. */
nora_itc_status_t nora_itc_init(nora_itc_list_t list,
                                const nora_itc_list_config_t *config);

/* Stop the list and turn the ITC off. Leaves ADC 5 and the clock alone: this
 * HAL did not create them. */
nora_itc_status_t nora_itc_deinit(nora_itc_list_t list);

/* ---------------------------------------------------------------------------
 * Running a scan
 * ------------------------------------------------------------------------- */

/* Software trigger: arm (SAMP = 1), settle, start (SAMP = 0). Refuses unless
 * the list was configured for NORA_ITC_TRIGGER_SOFTWARE, so the two trigger
 * models cannot be half-mixed at runtime. */
nora_itc_status_t nora_itc_scan_start(nora_itc_list_t list);

/* True once every record in the list has produced a result. Polling form, for
 * bring-up and for the console; the interrupt form is nora_itc_irq_*. */
bool nora_itc_scan_complete(nora_itc_list_t list);

/* Read one record's signed result (ITCRESx, Sample A - Sample B).
 *
 * Reading ITCRESx clears that record's ACCDONE flag, so this is the call that
 * consumes completion — read the results you asked for, or the next
 * nora_itc_scan_complete() answers about a state you threw away. */
nora_itc_status_t nora_itc_read(nora_itc_list_t list,
                                uint8_t record_index,
                                int32_t *result);

/* Read the whole list into a caller array of at least record_count entries.
 * The normal path: one interrupt, one call, a complete consistent set. */
nora_itc_status_t nora_itc_read_all(nora_itc_list_t list,
                                    int32_t *results,
                                    uint8_t results_len);

/* ---------------------------------------------------------------------------
 * Interrupt
 *
 * _ITCIF / _ITCIE / _ITCIP live in IFS10 / IEC10 / IPC40; there is no
 * _ITCxVECTOR-style name. Writes to those shared words must name one bit with a
 * compile-time-constant value: the compiler emits a single-bit atomic write
 * only then, and otherwise read-modify-writes the whole shared word and can
 * drop another subsystem's bit. That is why enabling the interrupt is a
 * function here and not the caller's register write.
 * ------------------------------------------------------------------------- */
typedef void (*nora_itc_scan_callback_t)(nora_itc_list_t list);

nora_itc_status_t nora_itc_irq_enable(nora_itc_list_t list,
                                      uint8_t priority,
                                      nora_itc_scan_callback_t callback);
nora_itc_status_t nora_itc_irq_disable(nora_itc_list_t list);

/* ---------------------------------------------------------------------------
 * Test injection — ITCSTAT.TSTEN / TSTDATA
 *
 * Every conversion returns the injected value instead of the converter's. This
 * drives the whole chain above the ADC — accumulation, the A-B arithmetic,
 * sign, and every layer we build on top — with known numbers, no electrode and
 * no finger. For an open library whose correctness has to be demonstrable
 * rather than asserted, this is the most valuable bit in the peripheral, so it
 * is a first-class API and reachable from the console, not a debug hook.
 * ------------------------------------------------------------------------- */
nora_itc_status_t nora_itc_test_inject_enable(uint16_t value);
nora_itc_status_t nora_itc_test_inject_disable(void);

/* ---------------------------------------------------------------------------
 * Introspection, for the console and the tuning manual
 * ------------------------------------------------------------------------- */
typedef struct {
    bool     initialized;
    bool     hardware_ready;      /* ITCSTAT.DRDY                              */
    bool     list_busy;           /* ITCLSxSTAT.BUSY                           */
    uint8_t  next_record;         /* ITCLSxSTAT.NEXT — where the scan got to   */
    bool     test_inject_active;  /* ITCSTAT.TSTEN                             */
    bool     scan_running;
    uint8_t  record_count;
    uint8_t  cvdcap;
    uint8_t  acc_count;
    uint32_t clock_hz;
    uint16_t charge_counts;       /* what charge_ns became, not what was asked */
    uint16_t balance_counts;
    uint32_t scans_completed;
    nora_itc_status_t last_status;
} nora_itc_info_t;

/* The converted timer counts are reported, not just the requested times,
 * because tuning is done against what the hardware got. A request that rounded
 * to zero counts is the failure this exists to make visible. */
nora_itc_status_t nora_itc_get_info(nora_itc_list_t list, nora_itc_info_t *info);

/* Raw register readback, walked by index from 0 until it returns
 * NORA_ITC_ERR_INVALID_ARG. Every configuration field this HAL writes goes into
 * one of these words, so a dump answers "did the peripheral accept what I wrote"
 * — the question that separates a bad configuration from a bad electrode, and the
 * one that cost the most time during bring-up. The table lives beside the writes
 * so it cannot drift from them; the caller does the printing. */
nora_itc_status_t nora_itc_debug_reg(uint8_t index,
                                     const char **name,
                                     uint32_t *value);

#endif /* NORA_ITC_INTERNAL_H */
