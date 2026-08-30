/* Provenance: written from DS70005591 ch.18 (ITC), the DFP SFR header, and
 * measurements taken on this board. No vendor touch-library source, header or
 * binary was consulted, and no vendor detection algorithm was inspected.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "nora_touch.h"
#include "nora_itc_internal.h"

/*===========================================================================
 * nora_touch.c — raw ITC counts to key events.
 *
 * The whole file is one state machine per key plus a non-blocking scan pump.
 * Every constant that could have been a magic number is either in
 * nora_touch_default_config() with its bench justification, or named below.
 *=========================================================================== */

/* The clock feeding the ITC is a board fact and arrives in nora_touch_config_t;
 * this layer owns no clock and now states none either. It is kept in s_clock_hz
 * because nora_touch_set_acquisition() has to re-apply the list long after
 * nora_touch_init() returned. */

/* The reference settings from the bench sweeps:
 * all three analog knobs saturate, so these sit just past their knees, and the
 * signal-to-noise is bought with accumulation depth instead.
 *
 * Re-measured 2026-08-14 against the tracked noise tail rather than a six-sample
 * spread: the analog knobs are flatter than the first sweep reported (the tail does
 * not move at all across CVDCAP 0-7, charge 500-5000 ns or balance 250-4000 ns),
 * and depth is worth less than it first reported -- ~3x from 2^4 to 2^8, not ~40x,
 * because the absolute tail grows 5x while the count grows 16x.
 *
 * Two corrections, 2026-08-15, after re-reading the ITC hardware documentation,
 * because the text
 * above was wrong in a way that closed off real levers:
 *
 *  - 2^8 is NOT "the maximum". ACCCNT[3:0] accepts up to 15, and ITCRESx is
 *    32-bit signed, so 2^15 would not overflow either. What stops us is the scan
 *    rate: 2^8 gives ~146 scan/s and every further doubling halves it, which
 *    collides with the 4-scan moving average below and with the ~5-scan plateau
 *    of a short real tap. The constraint is this file, not the hardware.
 *
 *  - The sweeps those flat results came from were taken under a measurement
 *    condition the data sheet rules out: all three electrodes were floating when
 *    idle (TRISx = 1, now fixed at the integration point in main.c, DS p.1487),
 *    and the board's touch shield copper was being held statically High by
 *    BOARD_DBG_PIN_E4 through R10 = 100 ohm. So "the knobs are flat" is a
 *    statement about that broken condition, not about this hardware, and CVDCAP
 *    is worth re-sweeping once both are corrected -- scored on
 *    min(light-touch mag) - max(idle tail), not on the idle tail alone. */
#define NORA_TOUCH_CHARGE_NS   (2000UL)
#define NORA_TOUCH_BALANCE_NS  (1000UL)
#define NORA_TOUCH_CVDCAP      (4u)
#define NORA_TOUCH_ACC_COUNT   (8u)     /* 2^8: ~3x better SNR than 2^4       */

#define NORA_TOUCH_LIST        (NORA_ITC_LIST_0)

/* A scan at 2^8 over three records takes roughly 5 ms. This is not a timeout on
 * that: it counts *process() calls* spent waiting, and only exists so a scan
 * that never completes gets re-armed instead of wedging the key layer forever. */
#define NORA_TOUCH_STALL_POLLS (20000UL)

/* Scans thrown away after a (re)configuration, before the baseline is seeded.
 *
 * Not defensive padding — measured 2026-08-14. Seeding from the very first scan
 * after *kc03 produced a baseline 24,000 counts away from where the count then
 * settled, so all three keys came up PRESSED and stayed there. The cause is
 * already known as a fact about the peripheral (a record's first few
 * repeats read low, which is why the count is not linear in accumulation depth);
 * what was missing was the consequence, that the first scan after a reconfigure
 * is not a measurement. At ~5 ms/scan this costs 40 ms once. */
#define NORA_TOUCH_SETTLE_SCANS (8u)

/* Scans whose median seeds the baseline, instead of taking the first one.
 *
 * The settle count above deals with the *systematic* error (early repeats read
 * low). This deals with the occasional single result that reads near zero — see
 * implausible_samples in the header. That one cannot be waited out, because it is
 * not early, it is random, and if it lands on the seeding scan the baseline is
 * ~80 % of full scale away from the count and the key is unusable until the next
 * reconfigure. Three samples and a median is the cheapest thing that survives one
 * of them; the guard below then catches the rest. */
#define NORA_TOUCH_SEED_SAMPLES (3u)

/* A sample is rejected when |delta| exceeds this fraction of the baseline's
 * magnitude, expressed as a right shift: 2 means a quarter.
 *
 * Ratio and not an absolute count, because the count scales with accumulation
 * depth and an absolute limit would reject every legitimate reading at 2^4 while
 * accepting garbage at 2^8. A quarter leaves ~16x headroom over the largest touch
 * measured (13,036 against 874,000, 1.5 %), so nothing a finger can do comes near
 * it, and the failure it catches is at 80 %. */
#define NORA_TOUCH_IMPLAUSIBLE_SHIFT (2u)

/* Consecutive rejected samples after which the baseline, not the sample, is
 * assumed to be the wrong one — and is thrown away and re-seeded.
 *
 * This is not belt-and-braces; without it the guard above is a worse bug than the
 * one it fixes. Measured 2026-08-14: the bad readings do not arrive singly, they
 * arrive in runs of dozens, so a run that straddles the seeding scans defeats the
 * median and the baseline latches near zero. Every later sample is then a quarter
 * of full scale away from it, so the guard rejects all of them, and the key is
 * dead until the next reconfigure — observed as 2,445 rejections out of 2,473
 * scans with the electrode working perfectly. A guard that can wedge a key needs
 * a way out, and re-seeding is the only correct one: after this many rejections in
 * a row the reference is what has to go.
 *
 * 16 at ~5 ms is 80 ms, which is short enough that no user notices and long enough
 * that a real burst is ridden out rather than answered by re-seeding. */
#define NORA_TOUCH_REJECT_RUN_MAX (16u)

/* Trace depth. 64 scans is ~0.44 s at the measured ~146 scans/s, which covers a
 * tap whole and the first half of a hold. Costs 64 x 2 bytes x KEY_MAX of RAM. */
#define NORA_TOUCH_TRACE_LEN   (64u)

/* Scans averaged to get a key's activity magnitude. Detection works on the mean
 * of |delta| over this many scans, not on delta itself, and that is the single
 * most important decision in this file -- see the block above
 * nora_touch_default_config(). 4 scans is ~27 ms at the measured ~146 scans/s:
 * long enough to average the alternation out, short enough that a 60 ms tap is
 * still several windows long. */
#define NORA_TOUCH_MAG_SCANS   (4u)

/* Seeding samples must agree within baseline magnitude >> this, or the window is
 * thrown away and re-collected. The median handles one bad sample among three; the
 * agreement check is what handles two, which is the case actually measured. 5 is
 * ~3 %, roughly 50x the noise tail and 2x the largest touch, so a finger resting
 * on the pad during a reconfigure does not stall seeding forever. */
#define NORA_TOUCH_SEED_AGREE_SHIFT (5u)

/* --- per-pad learning (header: learn_presses) -------------------------------
 *
 * The pad teaches the library its own press amplitude, because measuring the
 * quiet pad cannot: that was tried on 2026-08-14 -- thresholds from each pad's
 * idle noise tail -- and it was measured wrong on hardware. Pad 1 had the
 * *smallest* tail and needed the *highest* threshold, so the tail does not
 * predict the press. Only a press predicts the press.
 *
 * That forces the shape of this code: two thresholds, not one.
 *
 *   candidate  low, learns, never fires an event
 *   press      the one the app sees, moved once enough presses are known
 *
 * Without the candidate the design cannot start: a first touch below the press
 * threshold produces no event *and* no sample, so the pad would never learn what
 * it just failed to detect. With it, the first touch after a boot may well be
 * missed as an event while still being recorded as evidence -- which is the
 * accepted trade (operator decision 2026-08-14: a poor first touch is allowed).
 *
 * candidate = max(idle_ref * CAND_MULT, CAND_MIN), where idle_ref is a decaying
 * maximum of the key's own quiet magnitude. Decaying, not all-time: the noise is
 * low-frequency drift (A.7), so an all-time peak would ratchet up and never come
 * back down, and the candidate would drift out from under the learner.
 *
 * An excursion must hold for LEARN_RUN scans before it counts, so a single bad
 * conversion cannot manufacture a sample, and CAND_MIN keeps the candidate above
 * the idle magnitudes actually measured.
 *
 * CAND_MIN is 800, and it has been raised twice by the same argument: each time,
 * a longer idle run produced magnitudes the previous number sat inside. 600 was
 * set from a run of minutes; over a 3 h 7 min soak on 2026-08-15 two pads produced
 * quiet-pad excursions of 679 and 711, i.e. 600 was again inside the noise, and
 * the samples they fed the learner were noise recorded as presses. The lightest
 * touch measured is 780 and real taps fire at 826..1,520, so 800 still excludes
 * nothing a finger does -- but note that 780 says the headroom above is now thin,
 * and a third rise would start refusing real presses. If a longer soak clears 800,
 * the answer is no longer this constant: it is a slower idle_ref or a pad that
 * needs its own pinned pair.
 *
 * The history, kept because the reasoning is what transfers: 600 was not the 400
 * first tried, and that difference was measured,
 * not argued. 400 came from the A.7 trace, whose idle windows reached 317. Over a
 * longer idle run on the same board the tracked extremes are wider -- |delta| to
 * 895 and four-scan magnitudes past 470 -- so a 400 gate sits *inside* the noise:
 * the learner recorded quiet-pad excursions as presses, the median came out small,
 * the clamp pinned press at its floor, and the board then fired events with
 * nobody touching it (observed 2026-08-14, "press, mag 402" on an untouched pad).
 * The gate has to clear the noise the pad actually produces over minutes, not the
 * noise it produced during one seventeen-scan trace -- and, as the soak above then
 * showed, over hours and not merely minutes.
 *
 * LEARN_RUN is 3 for the same reason: a real touch spans dozens of scans, so
 * asking for three consecutive windows costs a press nothing, while a noise
 * excursion that clears the candidate for one window rarely clears it for three.
 * Rarely, and not never: the two 2026-08-15 excursions did clear three, which is
 * the other half of why the constant had to move rather than the run length.
 *
 * press = second-smallest(samples) * NUM/DEN, capped at the configured default and
 * then raised to max(FLOOR_MIN, idle_ref * FLOOR_MULT) -- floor last, so the floor
 * outranks the cap.
 *
 *   sample  the second-smallest recorded press (the smallest, below four
 *           samples), because the threshold must clear the weakest press rather
 *           than the average one -- see nora_touch_learn_apply() for the run of
 *           firm taps that showed a median cannot do this. Second-smallest and not
 *           smallest so one unusually light excursion cannot drag the pad alone.
 *   NUM/DEN 35/100, and the fraction is small because of what it multiplies. A
 *           sample is the *peak* magnitude of the excursion, whereas the threshold
 *           is crossed on the way up: measured 2026-08-14, the same taps peaked at
 *           1,400..2,400 while firing at 826..1,520, so peaks run about 1.5x the
 *           magnitude that has to be cleared. 45/100 was derived from A.7.1's
 *           *fire-time* figures and then applied to peaks, which is a unit
 *           mismatch, and it landed the three pads at 700/638/665 -- 28..40 % above
 *           the hand-tuned 500. Against peaks, 35 % of the second-smallest gives
 *           496/517/560, i.e. it converges onto the pair a bench operator chose,
 *           while still leaving 1.65x to the lightest press measured.
 *   ceiling the configured default is *known* to work on this hardware, so
 *           learning is never allowed to make a pad less sensitive than the value
 *           it shipped with -- unless the floor says otherwise, which is why the
 *           floor is applied after it.
 *   floor   max(FLOOR_MIN, FLOOR_MULT times the pad's own quiet magnitude), because
 *           below that the key presses itself. Both halves are needed and neither
 *           is sufficient: an absolute clamp inside the noise band turns every
 *           noise improvement into a loss of protection (measured: 500 gave 21
 *           false presses in 30 min on an untouched board, and the *noisier*
 *           configuration had had none), while a purely relative floor collapses
 *           with idle_ref -- at idle 74 it was 222, which stopped nothing. See
 *           FLOOR_MIN / FLOOR_MULT for the soak numbers behind 700 and 6.
 */
/* Presses before the pair is set for the first time -- a minimum, not a quota.
 * The pair is recomputed on every press after this one, over a median of up to
 * SAMPLES_MAX, so the estimate keeps improving without the threshold waiting for
 * it.
 *
 * Three, not five, and the acceptance criterion is what fixes it: from a
 * default-only power-up, ten taps per pad with no miss from the third tap
 * onwards (operator, 2026-08-14). A one-shot rule that converged at the fifth
 * press could not meet that -- taps three and four would still be judged by the
 * shipped 700, which is exactly the threshold the light taps fall under. Three
 * is the largest minimum that still has the pad learned before the first tap the
 * criterion counts.
 *
 * The cost of deciding on three samples instead of five is bounded rather than
 * argued: the ceiling in nora_touch_learn_apply() is the configured pair, so an
 * unlucky median cannot take the pad under the floor -- FLOOR_MIN, or FLOOR_MULT
 * times its own idle magnitude, whichever is larger -- and the next press
 * recomputes it from one more sample. */
#define NORA_TOUCH_LEARN_PRESSES     (3u)
#define NORA_TOUCH_LEARN_SAMPLES_MAX (8u)
#define NORA_TOUCH_LEARN_CAND_MIN    (800)
#define NORA_TOUCH_LEARN_CAND_MULT   (4)
#define NORA_TOUCH_LEARN_RUN         (3u)
#define NORA_TOUCH_LEARN_NUM         (35)
#define NORA_TOUCH_LEARN_DEN         (100)
/* The floor: max(FLOOR_MIN, idle_ref * FLOOR_MULT), and both halves moved on
 * 2026-08-15 because 500/3 was measured to invert -- improving the noise floor
 * *removed* protection. The mechanism, from the 30-minute soaks:
 *
 *   CVDAN10 was quiet in the 20:29 baseline *because its noise was high*.
 *   idle_ref 236 * 3 = 708 beat FLOOR_MIN, so its threshold was the shipped 700
 *   and it saw zero false presses. Grounding the idle electrodes (DS70005591C
 *   p.1487) then cut idle_ref to 123, 369 fell under 500, the absolute clamp won,
 *   and the threshold landed *inside* the pad's own noise band (measured 502..596)
 *   -- fifteen false presses in thirty minutes on an untouched board, where the
 *   noisier configuration had had none.
 *
 * So an absolute clamp that sits inside the noise band turns every noise
 * improvement into a loss of protection. 700 is the smallest value the hardware
 * has been measured clean at: 700 gave 0 events in 31 min, 900 gave 0 in 34 min
 * (08-14) and 31 min (08-15), 500 gave 21 in 30 min. It is also still under every
 * deliberate tap -- the lightest measured was 1,085 after the RE4 shield fix and
 * 528 before it, and the only taps that ever came under 700 were the 4th/5th of a
 * fast six-tap burst, i.e. partial contact.
 *
 * MULT is 6 rather than 3 so the relative half keeps a vote at the noise levels
 * this board actually shows: with FLOOR_MIN at 700, a MULT of 3 would need
 * idle_ref past 233 to matter at all, which is above anything measured since the
 * electrodes were grounded (66..146). Six puts the crossover at 117, so a pad
 * whose noise creeps up is still protected above the flat 700 -- which is exactly
 * what saved CVDAN10 in the baseline, and the capability this pair is meant to
 * preserve rather than replace.
 *
 * Consequence, deliberately accepted: with FLOOR_MIN equal to the shipped default
 * (700), learning can no longer make a pad *more* sensitive than shipped -- the
 * floor and the ceiling meet. Downward learning below 700 was measured unsafe on
 * this hardware (it is what produced the 21 events), so this is the point rather
 * than a side effect; what learning still does is scale release with press, raise
 * a noisy pad above 700, and clear the cold gate. An integrator who needs a
 * lighter pad than 700 sets it explicitly -- nora_touch_set_key_thresholds pins
 * the pair and outranks the learner. */
#define NORA_TOUCH_LEARN_FLOOR_MIN  (700)
#define NORA_TOUCH_LEARN_FLOOR_MULT  (6)
/* Cap on the relative half, and the reason it exists is measured, 2026-08-29:
 * idle_ref is not "this pad's noise", it is "how close a hand is to the board".
 * A hand near the pads lifts every pad's quiet magnitude -- CVDAN8 read mag 163
 * while only CVDAN10 was being tapped, and both fell back to 110 / 73 two minutes
 * after the hand left -- so idle_ref runs 140..240 for as long as somebody is
 * operating the board and 60..120 when nobody is. Multiplied by MULT that made
 * the floor 840..1440 during use, against real presses of 931..2610: the pad went
 * unresponsive exactly while it was being used, the operator pressed harder, and
 * the harder press raised idle_ref again. The gate in nora_touch_update_key()
 * (all pads quiet, s_all_quiet) is the fix for the cause; this cap bounds the
 * damage if some other common-mode source lifts idle_ref anyway. 900 is under the
 * weakest press measured on this board (931) and above the 798 that the
 * 2026-08-16 soak needed on CVDAN10, so it keeps that pad's protection intact. */
#define NORA_TOUCH_LEARN_FLOOR_MAX  (900)

/* Release as a fraction of press, so hysteresis scales with a learned-down pad
 * instead of being lost by it. Learning it separately is what A.6 got wrong: a
 * release level derived from idle noise landed inside the noise band. */
#define NORA_TOUCH_LEARN_REL_NUM     (1)
#define NORA_TOUCH_LEARN_REL_DEN     (2)

/* How fast idle_ref follows the quiet magnitude, in both directions: 2^-6 of the
 * gap per scan, so a few hundred milliseconds at ~146 scans/s -- fast enough to
 * follow drift, far slower than a tap.
 *
 * Symmetric on purpose, and this is the correction of a bug that cost a whole
 * hardware run (2026-08-14). The first version let idle_ref rise *instantly* --
 * "quiet, so this is what quiet looks like" -- which is wrong, because mag is a
 * 4-scan mean: the scan on which a touch first falls below the candidate still
 * contains touch. Measured consequence: one tap pushed idle_ref to roughly 650,
 * the candidate to ~1,950, and every following tap (mag 780..1,400) then sat
 * *under* the candidate. Ten taps per pad produced zero samples while the press
 * threshold, being a fixed 700, kept firing events perfectly -- so the log looked
 * healthy and the learner was starved.
 *
 * A slow rise costs nothing that the floor does not already cover: if the real
 * noise climbs, the candidate lags it for a few hundred milliseconds and a
 * spurious sample may be taken, and such a sample can only ever push the
 * threshold *down* to the floor of max(idle_ref*2, CAND_MIN). */
#define NORA_TOUCH_IDLE_SHIFT        (6u)

/* Consecutive scans below the candidate before idle_ref is allowed to move at
 * all. NORA_TOUCH_MAG_SCANS would be the minimum that flushes the touch out of
 * the averaging window; twice that leaves margin for the release tail without
 * being long enough to miss drift. */
#define NORA_TOUCH_IDLE_QUIET_RUN    (8u)

typedef struct {
    uint8_t  cvdan;
    int32_t  baseline;
    int32_t  raw;
    bool     baseline_valid;
    bool     pressed;
    int32_t  press_threshold;   /* this key's own pair — seeded from the config  */
    int32_t  release_threshold;
    uint8_t  debounce;          /* consecutive scans agreeing with the crossing */
    int32_t  mag_win[NORA_TOUCH_MAG_SCANS]; /* |delta| ring for the magnitude    */
    int32_t  mag_sum;           /* sum of mag_win, so mag = mag_sum / MAG_SCANS  */
    uint8_t  mag_i;
    int32_t  mag;               /* the value the thresholds are compared against */
    uint8_t  settle;            /* scans still to discard after a reconfigure   */
    int32_t  seed[NORA_TOUCH_SEED_SAMPLES];
    uint8_t  seed_n;            /* seeding samples collected so far             */
    uint8_t  reject_run;        /* consecutive samples the guard has rejected   */
    int32_t  idle_ref;          /* decaying max of mag while quiet              */
    int32_t  learn_peak;        /* highest mag in the excursion under way        */
    uint8_t  learn_run;         /* scans the excursion has held                 */
    uint8_t  quiet_run;         /* scans below the candidate, for idle_ref       */
    uint16_t presses;           /* press events since the last reset_peaks()     */
    bool     learn_active;
    int32_t  learn_mag[NORA_TOUCH_LEARN_SAMPLES_MAX]; /* press amplitudes seen  */
    uint8_t  learn_n;
    bool     cal_done;
    /* True until this pad has reported a press of its own -- the cold gate. Stored
     * rather than derived from learn_n, and the difference was measured: see the
     * cold-gate block below for the 2026-08-15 soak in which learn_n reached 1 on
     * noise alone, twice, with nobody in the room. */
    bool     cold_gate;
    bool     cal_pinned;        /* integrator set the pair; learning defers     */
    int32_t  peak;              /* max delta since reset — see the header       */
    int32_t  trough;            /* min delta since reset                        */
    nora_touch_event_t event;
} nora_touch_key_t;

static nora_touch_key_t     s_keys[NORA_TOUCH_KEY_MAX];
static nora_touch_config_t  s_cfg;
/* The record set and the acquisition settings are kept here rather than being
 * local to nora_touch_init(), because nora_touch_set_acquisition() has to re-init
 * the same list with the same electrodes and only the timings changed. */
static nora_itc_record_config_t s_records[NORA_TOUCH_KEY_MAX];
static uint32_t             s_charge_ns  = NORA_TOUCH_CHARGE_NS;
static uint32_t             s_balance_ns = NORA_TOUCH_BALANCE_NS;
static uint8_t              s_cvdcap     = NORA_TOUCH_CVDCAP;
static uint8_t              s_acc_count  = NORA_TOUCH_ACC_COUNT;
static uint8_t              s_key_count;
static uint32_t             s_clock_hz;   /* from nora_touch_config_t; board fact */
static bool                 s_initialized;
static bool                 s_scan_in_flight;
static uint32_t             s_stall_polls;
static uint32_t             s_scans;
/* True when every key was quiet on the previous scan, which is the only condition
 * under which idle_ref is allowed to move. A hand near the board is a common-mode
 * lift on all pads at once (see NORA_TOUCH_LEARN_FLOOR_MAX for the measurement),
 * so "all pads quiet" is the cheapest available test for "nobody is there" -- and
 * what the floor is meant to protect against is the noise present when nobody is.
 * One scan of lag (5 ms) against a quantity whose time constant is 320 ms. */
static bool                 s_all_quiet;
static uint32_t             s_rejected;
static uint32_t             s_implausible;

/* --- scan-resolution trace -------------------------------------------------
 * Why this exists: the console's ?ko view is sampled by whoever polls it, which
 * on a PC is a few times a second against ~146 scans a second. Two of the three
 * questions that came up on hardware -- does a press persist for the consecutive
 * scans the debounce asks for, and does one pad's touch move the other pads --
 * cannot be answered at that rate. So the layer records the deltas itself, and
 * the trigger is in firmware so that a human can touch whenever they like
 * instead of inside a window somebody opened for them. */
static int16_t  s_trace[NORA_TOUCH_KEY_MAX][NORA_TOUCH_TRACE_LEN];
static uint16_t s_trace_n;
static int32_t  s_trace_trigger;
static bool     s_trace_armed;
static bool     s_trace_running;

void nora_touch_default_config( nora_touch_config_t *cfg )
{
    if( !cfg ) { return; }

    /* These are magnitudes -- the mean of |delta| over NORA_TOUCH_MAG_SCANS -- and
     * not signed deltas. The change came from a scan-resolution trace taken on
     * 2026-08-14: while a pad is touched its delta does not
     * sit high, it *alternates*, swinging between -4,180 and +2,346 on consecutive
     * scans (9 sign flips in 17 scans). A signed threshold with a two-scan debounce
     * therefore almost never sees two agreeing scans, which is why pads answered
     * only a hard flat press -- the signal was always there, the test was wrong.
     *
     * Rectified, the same trace separates by a factor of seven: the touched pad's
     * 4-scan mean |delta| peaks at 1,929 while its own idle maximum is 274 and the
     * two untouched pads stay at 317 and below. So 700 sits ~2.5x above the worst
     * idle window and ~2.7x below a light touch, with the release at half of it for
     * hysteresis. Both numbers have more margin than any signed pair could have. */
    cfg->press_threshold   = 700;
    cfg->release_threshold = 350;
    cfg->debounce_scans         = 2u;
    /* 4 scans (~20 ms) of sustained sub-threshold delta before a release, against
     * the measured 11 ms dip that split four taps out of ten in the bench run. */
    cfg->release_debounce_scans = 4u;
    cfg->baseline_shift    = 6u;
    /* On by default, and it has to be: the shipped pair is a compromise across
     * pads, and letting each pad move its own threshold down to its own measured
     * press is the whole point. It costs learn_presses touches per pad per boot,
     * and nothing is kept across a power cycle -- deliberately, so what the board
     * does at boot never depends on what happened before it.
     *
     * This replaces the idle-noise calibration of A.6, which was measured wrong:
     * the pad with the smallest noise tail needed the highest threshold. */
    cfg->learn_presses     = NORA_TOUCH_LEARN_PRESSES;
    /* Cold gate -- see nora_touch_config_t for the measurement it comes from. 900
     * clears the 705 an untouched pad reached on 2026-08-15 and sits under the
     * 826..1,520 the same board's real taps fired at; 4 scans (~27 ms) is the
     * longest press debounce that a short tap's ~5-scan plateau still survives,
     * and is well inside the ~50 ms a human reads as instant. */
    cfg->cold_press_threshold   = 900;
    cfg->cold_debounce_scans    = 4u;
    cfg->verbose           = true;
    /* No clock: the board states it. See nora_touch_config_t.clock_hz -- a default
     * here would be right on one board and silently wrong on every other. */
    cfg->clock_hz          = 0uL;
}

/* Start a scan, remembering whether it actually started. A refusal is counted
 * rather than swallowed: "the keys stopped responding" has to be answerable
 * without a debugger, and rejected_scans is that answer. */
static void nora_touch_arm_scan( void )
{
    if( nora_itc_scan_start( NORA_TOUCH_LIST ) == NORA_ITC_OK )
    {
        s_scan_in_flight = true;
        s_stall_polls    = 0u;
    }
    else
    {
        s_scan_in_flight = false;
        s_rejected++;
    }
}

/* Push the record set plus the current acquisition settings at the ITC. The one
 * place nora_itc_init() is called from, so the console's sweep and the initial
 * bring-up cannot drift apart in what they configure. */
static nora_itc_status_t nora_touch_apply_list( void )
{
    nora_itc_list_config_t list;
    nora_itc_status_t      status;
    int32_t                discard[NORA_TOUCH_KEY_MAX];

    list.clock_hz     = s_clock_hz;
    list.records      = s_records;
    list.record_count = s_key_count;
    list.charge_ns    = s_charge_ns;
    list.balance_ns   = s_balance_ns;
    list.cvdcap       = s_cvdcap;
    list.acc_count    = s_acc_count;
    list.trigger      = NORA_ITC_TRIGGER_SOFTWARE;
    list.period_us    = 0u;
    list.mode         = NORA_ITC_SCAN_LIST_NO_IRQ;

    status = nora_itc_init( NORA_TOUCH_LIST, &list );
    if( status != NORA_ITC_OK )
    {
        return status;
    }

    /* Consume any completion left over from the previous configuration. ACCDONE
     * survives an init and is only cleared by reading ITCRESx, so without this
     * the next scan_complete() answers true immediately, the results read back
     * belong to no scan at all, and the settle scans below get spent on them in
     * microseconds instead of on real acquisitions. Measured 2026-08-14: the
     * first reconfigure after boot seeded a baseline of -47,453 against a count
     * of -874,000 and pinned two keys PRESSED.
     * The read is expected to fail when nothing is pending; that is the normal
     * case and not an error. */
    (void)nora_itc_read_all( NORA_TOUCH_LIST, discard, NORA_TOUCH_KEY_MAX );
    return NORA_ITC_OK;
}

bool nora_touch_init( const uint8_t *cvdan, uint8_t key_count,
                      const nora_touch_config_t *cfg )
{
    uint8_t i;

    s_initialized    = false;
    s_scan_in_flight = false;

    if( !cvdan || !cfg || (key_count == 0u) || (key_count > NORA_TOUCH_KEY_MAX) ||
        (cfg->clock_hz == 0uL) )
    {
        return false;
    }

    s_cfg       = *cfg;
    s_key_count = key_count;
    s_clock_hz  = cfg->clock_hz;

    for( i = 0u; i < key_count; i++ )
    {
        s_records[i].cvdan   = cvdan[i];
        /* No hardware guard: GRDAn/GRDBn can only select an immediate neighbour
         * in the CVDANx numbering, and this board's pads have no spare ones. */
        s_records[i].guard_a = NORA_ITC_GUARD_NONE;
        s_records[i].guard_b = NORA_ITC_GUARD_NONE;

        s_keys[i].cvdan            = cvdan[i];
        s_keys[i].baseline         = 0;
        s_keys[i].raw              = 0;
        s_keys[i].baseline_valid   = false;
        s_keys[i].pressed          = false;
        /* Every key starts on the global pair; a caller that knows this board's
         * pads overrides individual ones afterwards. */
        s_keys[i].press_threshold   = s_cfg.press_threshold;
        s_keys[i].release_threshold = s_cfg.release_threshold;
        s_keys[i].debounce         = 0u;
        {
            uint8_t m;
            for( m = 0u; m < NORA_TOUCH_MAG_SCANS; m++ ) { s_keys[i].mag_win[m] = 0; }
        }
        s_keys[i].mag_sum          = 0;
        s_keys[i].mag_i            = 0u;
        s_keys[i].mag              = 0;
        s_keys[i].settle           = NORA_TOUCH_SETTLE_SCANS;
        s_keys[i].seed_n           = 0u;
        s_keys[i].reject_run       = 0u;
        s_keys[i].peak             = 0;
        s_keys[i].trough           = 0;
        s_keys[i].idle_ref         = 0;
        s_keys[i].learn_peak       = 0;
        s_keys[i].learn_run        = 0u;
        s_keys[i].quiet_run        = 0u;
        s_keys[i].presses          = 0u;
        s_keys[i].learn_active     = false;
        s_keys[i].learn_n          = 0u;
        s_keys[i].cal_done         = false;
        s_keys[i].cold_gate        = true;
        s_keys[i].cal_pinned       = false;
        s_keys[i].event            = NORA_TOUCH_EVENT_NONE;
    }

    if( nora_touch_apply_list() != NORA_ITC_OK )
    {
        return false;
    }

    s_scans       = 0u;
    s_all_quiet   = false;
    s_rejected    = 0u;
    s_implausible = 0u;
    s_initialized = true;
    nora_touch_arm_scan();
    return true;
}

/* --- cold gate --------------------------------------------------------------
 * The gate lifts on the pad's first press *event*, and this was first written the
 * other way -- lifting on the first recorded learn sample, on the assumption that
 * a sample means a finger. Measured 2026-08-15, it does not: over a 3 h 7 min soak
 * with nobody in the room, two of the three pads recorded a sample and lifted
 * their own gate (CVDAN10 at mag 679 after 7 min, CVDAN8 at mag 711 after 15 min)
 * while the event side, correctly, reported nothing at all. The learning path is
 * deliberately more permissive than detection -- that is what lets a pad learn
 * from a touch too light to report -- so it is exactly the wrong thing to ask
 * "was that a human". Only an event answers that, so only an event lifts the gate.
 *
 * Hence a stored flag rather than a function of learn_n: the two now mean
 * different things, and the flag is cleared at the one place a press is reported.
 * It has to be re-armed everywhere the learning state is cleared, and it is --
 * init, nora_touch_calibrate() and nora_touch_set_acquisition().
 *
 * Independent of learn_presses on purpose: with learning switched off the shipped
 * pair never moves, and a pad that has never been touched should still be the
 * stricter of the two. The gate is about whether a human has arrived, not about
 * calibration.
 *
 * Both accessors are one-sided: the cold pair may only make a pad stricter. A
 * configuration whose cold values are the weaker ones is a mistake that would
 * raise sensitivity before anything is known about the pad, which is the opposite
 * of what the gate is for, so it is ignored rather than obeyed.
 */
static bool nora_touch_key_is_cold( const nora_touch_key_t *k )
{
    return ( (s_cfg.cold_press_threshold > 0) &&
             !k->cal_pinned &&
             k->cold_gate );
}

static int32_t nora_touch_key_press_threshold( const nora_touch_key_t *k )
{
    if( nora_touch_key_is_cold( k ) &&
        (s_cfg.cold_press_threshold > k->press_threshold) )
    {
        return s_cfg.cold_press_threshold;
    }
    return k->press_threshold;
}

static uint8_t nora_touch_key_debounce_scans( const nora_touch_key_t *k )
{
    if( nora_touch_key_is_cold( k ) &&
        (s_cfg.cold_debounce_scans > s_cfg.debounce_scans) )
    {
        return s_cfg.cold_debounce_scans;
    }
    return s_cfg.debounce_scans;
}

/* Median of what the pad has taught us, then the arithmetic and its two limits.
 * Split out so the rule can be read without the state machine around it. */
static void nora_touch_learn_apply( nora_touch_key_t *k )
{
    int32_t sorted[NORA_TOUCH_LEARN_SAMPLES_MAX];
    int32_t press;
    int32_t floor_thr;
    uint8_t i;
    uint8_t j;

    k->cal_done = true;

    /* An explicit pair outranks the measurement: an integrator who has called
     * nora_touch_set_key_thresholds knows something about this pad that a handful
     * of taps cannot tell, and silently overwriting it would make the setter look
     * like it worked while doing nothing. */
    if( k->cal_pinned )
    {
        return;
    }

    for( i = 0u; i < k->learn_n; i++ ) { sorted[i] = k->learn_mag[i]; }
    /* Insertion sort: n is five, and the alternative is a qsort call plus a
     * comparator for five numbers. */
    for( i = 1u; i < k->learn_n; i++ )
    {
        int32_t v = sorted[i];
        for( j = i; (j > 0u) && (sorted[j - 1u] > v); j-- ) { sorted[j] = sorted[j - 1u]; }
        sorted[j] = v;
    }

    /* Low order statistic, not the median -- measured 2026-08-14. The median ties
     * the threshold to how hard the operator happened to press: a run of ordinary
     * firm taps gave medians past 1,556 on all three pads, so 45 % of them cleared
     * the shipped 700 and the ceiling held, i.e. learning did nothing at all. What
     * the threshold has to sit under is the *weakest* press the pad will see, and
     * that is the bottom of the sample set, not its middle.
     *
     * Second-smallest rather than smallest once there are four samples, so a single
     * unusually light or clipped excursion cannot drag the pad down on its own; with
     * fewer samples there is nothing to spare and the smallest is used. */
    i = ( k->learn_n >= 4u ) ? 1u : 0u;
    press = (sorted[i] * NORA_TOUCH_LEARN_NUM) / NORA_TOUCH_LEARN_DEN;

    floor_thr = k->idle_ref * NORA_TOUCH_LEARN_FLOOR_MULT;
    /* Cap the relative half before the absolute one, so FLOOR_MIN still wins on a
     * quiet pad and FLOOR_MAX only ever bites a noisy one -- see FLOOR_MAX. */
    if( floor_thr > NORA_TOUCH_LEARN_FLOOR_MAX ) { floor_thr = NORA_TOUCH_LEARN_FLOOR_MAX; }
    if( floor_thr < NORA_TOUCH_LEARN_FLOOR_MIN ) { floor_thr = NORA_TOUCH_LEARN_FLOOR_MIN; }

    /* Ceiling first, floor last, and the order is the whole point: the floor is a
     * safety limit and the ceiling is a preference, so the floor has to outrank it.
     * Applied the other way round -- floor then ceiling, as this did until
     * 2026-08-15 -- the configured default silently undoes the floor, and
     * idle_ref * MULT can then never raise a threshold above the shipped value no
     * matter how noisy the pad gets. That is what made the baseline's protection of
     * CVDAN10 (idle 236 * 3 = 708, clamped straight back to 700) look like it was
     * working when it had no headroom at all.
     *
     * Ceiling: never less sensitive than the configured value, which is the one
     * already proven on hardware -- except where the pad's own noise says that
     * value would press itself. */
    if( press > s_cfg.press_threshold ) { press = s_cfg.press_threshold; }
    if( press < floor_thr )             { press = floor_thr; }

    k->press_threshold   = press;
    k->release_threshold = (press * NORA_TOUCH_LEARN_REL_NUM) / NORA_TOUCH_LEARN_REL_DEN;

    if( s_cfg.verbose )
    {
        printf( " NORA_TOUCH(CVDAN%u): learned from %u press(es), median %ld,"
                " idle %ld -> press %ld release %ld\n",
                (unsigned)k->cvdan, (unsigned)k->learn_n,
                (long)sorted[k->learn_n / 2u], (long)k->idle_ref,
                (long)k->press_threshold, (long)k->release_threshold );
    }
}

/* Record one finished excursion, newest-wins once the buffer is full, and re-apply
 * the rule on every press from the learn_presses'th onwards -- not once. A median
 * of three is a usable threshold immediately and a median of six is a better one
 * later, and there is no reason to make the pad wait for the second in order to
 * have the first.
 *
 * Kept out of the detection path on purpose: this runs whether or not the
 * excursion fired an event, which is the entire reason a first touch too light to
 * detect is still not wasted. */
static void nora_touch_learn_sample( nora_touch_key_t *k, int32_t magnitude )
{
    uint8_t i;

    if( (s_cfg.learn_presses == 0u) || k->cal_pinned )
    {
        return;
    }

    if( k->learn_n >= NORA_TOUCH_LEARN_SAMPLES_MAX )
    {
        for( i = 1u; i < NORA_TOUCH_LEARN_SAMPLES_MAX; i++ )
        {
            k->learn_mag[i - 1u] = k->learn_mag[i];
        }
        k->learn_n = (uint8_t)(NORA_TOUCH_LEARN_SAMPLES_MAX - 1u);
    }

    k->learn_mag[k->learn_n] = magnitude;
    k->learn_n++;

    if( k->learn_n >= s_cfg.learn_presses )
    {
        nora_touch_learn_apply( k );
    }
}

/* One key's state machine, given a fresh raw count. Split out because the
 * press/release asymmetry is the whole substance of this file and reads badly
 * inline in the scan pump. */
static void nora_touch_update_key( nora_touch_key_t *k )
{
    int32_t delta;

    /* Discard the settling scans first: a baseline seeded from them is wrong by
     * more than a finger is worth, and every later delta is measured against it. */
    if( k->settle != 0u )
    {
        k->settle--;
        return;
    }

    /* The first readings define the baseline: there is nothing to average against
     * yet, and seeding from zero would report a 874,000-count "touch". The median
     * of three is taken rather than the first, so one near-zero result cannot
     * become the reference every later delta is measured against. */
    if( !k->baseline_valid )
    {
        k->seed[k->seed_n] = k->raw;
        k->seed_n++;
        if( k->seed_n < NORA_TOUCH_SEED_SAMPLES )
        {
            return;
        }

        /* Refuse a window whose samples disagree: with a run of bad readings the
         * median is bad too, and a bad baseline is the one error here that cannot
         * be recovered from by looking at later samples — every one of them then
         * looks wrong instead. Cheaper to wait for three that agree. */
        {
            int32_t lo = k->seed[0], hi = k->seed[0], mag, i;

            for( i = 1; i < (int32_t)NORA_TOUCH_SEED_SAMPLES; i++ )
            {
                if( k->seed[i] < lo ) { lo = k->seed[i]; }
                if( k->seed[i] > hi ) { hi = k->seed[i]; }
            }
            mag = (lo < 0) ? -lo : lo;

            if( (hi - lo) > (mag >> NORA_TOUCH_SEED_AGREE_SHIFT) )
            {
                k->seed_n = 0u;
                s_implausible++;
                return;
            }
        }

        /* Median of exactly three, written out: a sort would be more general and
         * less obviously correct at this size. */
        if( ((k->seed[0] >= k->seed[1]) && (k->seed[0] <= k->seed[2])) ||
            ((k->seed[0] <= k->seed[1]) && (k->seed[0] >= k->seed[2])) )
        {
            k->baseline = k->seed[0];
        }
        else if( ((k->seed[1] >= k->seed[0]) && (k->seed[1] <= k->seed[2])) ||
                 ((k->seed[1] <= k->seed[0]) && (k->seed[1] >= k->seed[2])) )
        {
            k->baseline = k->seed[1];
        }
        else
        {
            k->baseline = k->seed[2];
        }

        k->baseline_valid = true;
        return;
    }

    /* A touch makes the count less negative, so delta is positive on a press. */
    delta = k->raw - k->baseline;

    /* Reject what an electrode cannot have produced, before it reaches the
     * baseline tracker, the peaks or the thresholds. Deliberately the first thing
     * done with delta: a result of this size would otherwise fire a press, drag
     * the baseline, and — worst of the three, because it is the one that outlives
     * the event — leave a peak that a tuning sweep would read as signal. */
    {
        int32_t magnitude = (k->baseline < 0) ? -k->baseline : k->baseline;
        int32_t limit     = magnitude >> NORA_TOUCH_IMPLAUSIBLE_SHIFT;

        if( (limit > 0) && ((delta > limit) || (delta < -limit)) )
        {
            s_implausible++;
            k->reject_run++;
            if( k->reject_run >= NORA_TOUCH_REJECT_RUN_MAX )
            {
                /* The baseline is what is wrong. Drop it and re-seed; the key
                 * releases on the way through, because a press cannot be held
                 * across a reference it was never measured against. */
                k->baseline_valid = false;
                k->seed_n         = 0u;
                k->reject_run     = 0u;
                k->pressed        = false;
                k->debounce       = 0u;
            }
            return;
        }

        k->reject_run = 0u;
    }

    /* Recorded before any threshold is consulted, on purpose: a touch that does
     * not reach press_threshold produces no event and no log line, so this is the
     * only place a near-miss leaves a trace. */
    if( delta > k->peak )   { k->peak   = delta; }
    if( delta < k->trough ) { k->trough = delta; }

    /* Rectify, then average over a few scans: this is what the thresholds see.
     * A touch shows up as activity of either sign (the trace in A.7), so the sign
     * carries no information about whether a finger is there -- averaging |delta|
     * keeps the alternation as signal instead of cancelling it, which is exactly
     * what averaging delta itself would have done. */
    {
        int32_t magnitude_now = (delta < 0) ? -delta : delta;

        k->mag_sum -= k->mag_win[k->mag_i];
        k->mag_win[k->mag_i] = magnitude_now;
        k->mag_sum += magnitude_now;
        k->mag_i = (uint8_t)((k->mag_i + 1u) % NORA_TOUCH_MAG_SCANS);
        k->mag = k->mag_sum / (int32_t)NORA_TOUCH_MAG_SCANS;
    }

    /* The learning path. It sits above the detection path and shares nothing with
     * it but the magnitude, so what the app is told and what the pad is learning
     * cannot disagree about the same scan. */
    {
        int32_t candidate = k->idle_ref * NORA_TOUCH_LEARN_CAND_MULT;

        if( candidate < NORA_TOUCH_LEARN_CAND_MIN )
        {
            candidate = NORA_TOUCH_LEARN_CAND_MIN;
        }

        if( k->mag >= candidate )
        {
            if( k->mag > k->learn_peak ) { k->learn_peak = k->mag; }
            if( k->learn_run < 0xFFu )   { k->learn_run++; }
            if( k->learn_run >= NORA_TOUCH_LEARN_RUN ) { k->learn_active = true; }
            k->quiet_run = 0u;
        }
        else
        {
            if( k->learn_active )
            {
                nora_touch_learn_sample( k, k->learn_peak );
            }
            k->learn_active = false;
            k->learn_run    = 0u;
            k->learn_peak   = 0;

            if( k->quiet_run < 0xFFu ) { k->quiet_run++; }

            /* Below the candidate is not yet quiet: mag is a mean over
             * NORA_TOUCH_MAG_SCANS, so the first scans under the candidate still
             * carry the touch that just ended. Wait for the window to flush, then
             * follow the quiet magnitude slowly in *both* directions -- see
             * NORA_TOUCH_IDLE_SHIFT for the run this cost. */
            /* ...and only while *no* pad is active: quiet_run alone clears 40 ms
             * after this pad's own touch, which is still inside the window where a
             * hand hovering over the board lifts every pad. See s_all_quiet. */
            if( (k->quiet_run >= NORA_TOUCH_IDLE_QUIET_RUN) && s_all_quiet )
            {
                k->idle_ref += (k->mag - k->idle_ref) >> NORA_TOUCH_IDLE_SHIFT;
            }
        }
    }

    if( !k->pressed )
    {
        /* Track the baseline only while released — see the header. */
        k->baseline += (k->raw - k->baseline) >> s_cfg.baseline_shift;

        /* Through the accessors, not the stored pair: until this pad has recorded
         * a press it detects on the cold gate. Only the press side is gated -- a
         * press already reported must be able to end at its own release threshold,
         * whatever decided it. */
        if( k->mag >= nora_touch_key_press_threshold( k ) )
        {
            k->debounce++;
            if( k->debounce >= nora_touch_key_debounce_scans( k ) )
            {
                /* Read the pair that decided this press before the gate is
                 * cleared, so the line below reports what was actually in force
                 * rather than what will be in force from the next scan on. */
                int32_t was_thr  = nora_touch_key_press_threshold( k );
                uint8_t was_db   = nora_touch_key_debounce_scans( k );
                bool    was_cold = nora_touch_key_is_cold( k );

                k->pressed          = true;
                k->debounce         = 0u;
                k->event            = NORA_TOUCH_EVENT_PRESSED;
                if( k->presses < 0xFFFFu ) { k->presses++; }
                /* A reported press is the only evidence that a human is here, so
                 * it is the only thing that lifts the gate. Cleared before the
                 * printf so an early return could not leave the pad cold with the
                 * log already claiming otherwise. */
                k->cold_gate        = false;
                if( s_cfg.verbose )
                {
                    printf( " NORA_TOUCH(CVDAN%u): press, mag %ld\n",
                            (unsigned)k->cvdan, (long)k->mag );
                    /* Second line, after the event's own: the first press is also
                     * the moment the pad becomes as sensitive as the shipped pair,
                     * and while it was cold the log showed only an absence -- the
                     * light touches it declined to report. Kept off the event line
                     * so that line stays the shape appendix A scores against. */
                    if( was_cold )
                    {
                        printf( " NORA_TOUCH(CVDAN%u): cold gate off"
                                " (press %ld -> %ld, debounce %u -> %u)\n",
                                (unsigned)k->cvdan,
                                (long)was_thr, (long)k->press_threshold,
                                (unsigned)was_db, (unsigned)s_cfg.debounce_scans );
                    }
                }
            }
        }
        else
        {
            k->debounce = 0u;
        }
        return;
    }

    if( k->mag < k->release_threshold )
    {
        k->debounce++;
        if( k->debounce >= s_cfg.release_debounce_scans )
        {
            k->pressed  = false;
            k->debounce = 0u;
            k->event    = NORA_TOUCH_EVENT_RELEASED;
            if( s_cfg.verbose )
            {
                printf( " NORA_TOUCH(CVDAN%u): release\n", (unsigned)k->cvdan );
            }
        }
        return;
    }

    k->debounce = 0u;
}

void nora_touch_process( void )
{
    int32_t results[NORA_TOUCH_KEY_MAX];
    uint8_t i;

    if( !s_initialized ) { return; }

    if( !s_scan_in_flight )
    {
        nora_touch_arm_scan();
        return;
    }

    if( !nora_itc_scan_complete( NORA_TOUCH_LIST ) )
    {
        s_stall_polls++;
        if( s_stall_polls >= NORA_TOUCH_STALL_POLLS )
        {
            s_rejected++;
            s_scan_in_flight = false;
        }
        return;
    }

    s_scan_in_flight = false;

    /* Reading the results is what clears ACCDONE, so a failed read must not be
     * treated as data: the counts would be from the previous scan. */
    if( nora_itc_read_all( NORA_TOUCH_LIST, results, NORA_TOUCH_KEY_MAX )
        != NORA_ITC_OK )
    {
        s_rejected++;
        nora_touch_arm_scan();
        return;
    }

    s_scans++;

    for( i = 0u; i < s_key_count; i++ )
    {
        s_keys[i].raw = results[i];
        nora_touch_update_key( &s_keys[i] );
    }

    /* One AND over the pads, for the next scan to use. A key that has no baseline
     * yet counts as quiet: it carries no evidence either way, and treating it as
     * active would freeze idle_ref on every other pad for as long as it is
     * re-seeding. */
    {
        bool all_quiet = true;

        for( i = 0u; i < s_key_count; i++ )
        {
            if( s_keys[i].baseline_valid &&
                (s_keys[i].quiet_run < NORA_TOUCH_IDLE_QUIET_RUN) )
            {
                all_quiet = false;
                break;
            }
        }
        s_all_quiet = all_quiet;
    }

    /* Trace after the keys are updated, so what is recorded is the same delta the
     * thresholds saw -- not a separately computed one that could differ. Every key
     * is stored on every traced scan, because "did the other pads move too" is
     * only answerable if the other pads were recorded at the same instant. */
    if( s_trace_armed && (s_trace_n < NORA_TOUCH_TRACE_LEN) )
    {
        if( !s_trace_running )
        {
            for( i = 0u; i < s_key_count; i++ )
            {
                int32_t d = s_keys[i].raw - s_keys[i].baseline;
                if( s_keys[i].baseline_valid &&
                    ((d > s_trace_trigger) || (d < -s_trace_trigger)) )
                {
                    s_trace_running = true;
                    break;
                }
            }
        }

        if( s_trace_running )
        {
            for( i = 0u; i < s_key_count; i++ )
            {
                int32_t d = s_keys[i].raw - s_keys[i].baseline;
                if( d >  32767 ) { d =  32767; }
                if( d < -32768 ) { d = -32768; }
                s_trace[i][s_trace_n] = (int16_t)d;
            }
            s_trace_n++;
        }
    }

    nora_touch_arm_scan();
}

void nora_touch_trace_arm( int32_t trigger )
{
    s_trace_trigger = (trigger > 0) ? trigger : 800;
    s_trace_n       = 0u;
    s_trace_running = false;
    s_trace_armed   = true;
}

bool nora_touch_trace_ready( void )
{
    return (s_trace_n >= NORA_TOUCH_TRACE_LEN);
}

uint16_t nora_touch_trace_count( void )
{
    return s_trace_n;
}

bool nora_touch_trace_get( uint8_t key, uint16_t index, int32_t *delta )
{
    if( !delta || (key >= s_key_count) || (index >= s_trace_n) ) { return false; }
    *delta = (int32_t)s_trace[key][index];
    return true;
}

nora_touch_event_t nora_touch_get_event( uint8_t key )
{
    nora_touch_event_t event;

    if( !s_initialized || (key >= s_key_count) )
    {
        return NORA_TOUCH_EVENT_NONE;
    }

    event             = s_keys[key].event;
    s_keys[key].event = NORA_TOUCH_EVENT_NONE;
    return event;
}

bool nora_touch_is_pressed( uint8_t key )
{
    if( !s_initialized || (key >= s_key_count) ) { return false; }
    return s_keys[key].pressed;
}

void nora_touch_get_status( nora_touch_status_t *status )
{
    if( !status ) { return; }

    status->initialized    = s_initialized;
    status->key_count      = s_key_count;
    status->scans          = s_scans;
    status->rejected_scans = s_rejected;
    status->implausible_samples = s_implausible;
}

bool nora_touch_get_key_state( uint8_t key, nora_touch_key_state_t *state )
{
    if( !state || !s_initialized || (key >= s_key_count) ) { return false; }

    state->cvdan    = s_keys[key].cvdan;
    state->raw      = s_keys[key].raw;
    state->baseline = s_keys[key].baseline;
    state->delta    = s_keys[key].raw - s_keys[key].baseline;
    state->mag      = s_keys[key].mag;
    state->peak     = s_keys[key].peak;
    state->trough   = s_keys[key].trough;
    state->pressed  = s_keys[key].pressed;
    state->presses  = s_keys[key].presses;
    return true;
}

void nora_touch_reset_peaks( void )
{
    uint8_t i;

    for( i = 0u; i < NORA_TOUCH_KEY_MAX; i++ )
    {
        s_keys[i].peak   = 0;
        s_keys[i].trough = 0;
        s_keys[i].presses = 0u;
    }
}

bool nora_touch_set_thresholds( int32_t press_threshold, int32_t release_threshold )
{
    if( (press_threshold <= 0) || (release_threshold <= 0) ||
        (release_threshold >= press_threshold) )
    {
        return false;
    }

    s_cfg.press_threshold   = press_threshold;
    s_cfg.release_threshold = release_threshold;

    /* Applied to every key, per-key overrides included: this is the console's
     * sweep knob, and a sweep that silently left one pad on an old value would
     * report the sweep's numbers while measuring something else. */
    {
        uint8_t i;
        for( i = 0u; i < s_key_count; i++ )
        {
            s_keys[i].press_threshold   = press_threshold;
            s_keys[i].release_threshold = release_threshold;
            /* Pinned for the same reason: a calibration started later must not
             * quietly move the numbers the sweep is measuring at. *kz re-arms
             * calibration explicitly when that is what is wanted. */
            s_keys[i].cal_pinned        = true;
        }
    }
    return true;
}

bool nora_touch_set_key_thresholds( uint8_t key, int32_t press_threshold,
                                    int32_t release_threshold )
{
    if( (key >= s_key_count) ||
        (press_threshold <= 0) || (release_threshold <= 0) ||
        (release_threshold >= press_threshold) )
    {
        return false;
    }

    s_keys[key].press_threshold   = press_threshold;
    s_keys[key].release_threshold = release_threshold;
    /* Pinned: calibration will measure this pad's tail and report it, but will not
     * overwrite a pair someone asked for by name. */
    s_keys[key].cal_pinned        = true;
    return true;
}

bool nora_touch_calibrate( void )
{
    uint8_t i;

    if( !s_initialized || (s_cfg.learn_presses == 0u) )
    {
        return false;
    }

    for( i = 0u; i < s_key_count; i++ )
    {
        /* Forget the samples *and* the thresholds they produced. The ceiling in
         * nora_touch_learn_apply() is the configured value, so a relearn that
         * started from an already-learned pair could only ever ratchet downwards;
         * returning each key to the configured pair is what makes this a fresh
         * start rather than a second descent. */
        s_keys[i].learn_n      = 0u;
        s_keys[i].learn_peak   = 0;
        s_keys[i].learn_run    = 0u;
        s_keys[i].quiet_run    = 0u;
        s_keys[i].learn_active = false;
        s_keys[i].cal_done     = false;
        /* Re-armed with the rest of it: *kl is "forget what this pad taught us",
         * and a pad whose evidence has been thrown away has not been touched as far
         * as anything here can tell. */
        s_keys[i].cold_gate    = true;
        if( !s_keys[i].cal_pinned )
        {
            s_keys[i].press_threshold   = s_cfg.press_threshold;
            s_keys[i].release_threshold = s_cfg.release_threshold;
        }
        /* A press cannot be held across a change of the thresholds that decided
         * it, so the key releases on the way in. */
        s_keys[i].pressed      = false;
        s_keys[i].debounce     = 0u;
        s_keys[i].event        = NORA_TOUCH_EVENT_NONE;
    }

    return true;
}

bool nora_touch_get_calibration( uint8_t key, nora_touch_calibration_t *out )
{
    if( (key >= s_key_count) || !out )
    {
        return false;
    }

    out->calibrated        = s_keys[key].cal_done;
    out->pinned            = s_keys[key].cal_pinned;
    out->idle_ref          = s_keys[key].idle_ref;
    out->samples           = s_keys[key].learn_n;
    out->needed            = s_cfg.learn_presses;
    /* The pair in force, which on a pad that has not been pressed yet is the cold
     * one -- reporting the stored value here would have ?kl print a threshold the
     * pad is not detecting at. */
    out->cold_gate         = nora_touch_key_is_cold( &s_keys[key] );
    out->press_threshold   = nora_touch_key_press_threshold( &s_keys[key] );
    out->release_threshold = s_keys[key].release_threshold;
    return true;
}

void nora_touch_get_thresholds( int32_t *press_threshold, int32_t *release_threshold )
{
    if( press_threshold )   { *press_threshold   = s_cfg.press_threshold; }
    if( release_threshold ) { *release_threshold = s_cfg.release_threshold; }
}

bool nora_touch_set_acquisition( uint32_t charge_ns, uint32_t balance_ns,
                                 uint8_t cvdcap, uint8_t acc_count )
{
    uint32_t old_charge  = s_charge_ns;
    uint32_t old_balance = s_balance_ns;
    uint8_t  old_cvdcap  = s_cvdcap;
    uint8_t  old_acc     = s_acc_count;
    uint8_t  i;

    if( !s_initialized ) { return false; }

    s_charge_ns  = charge_ns;
    s_balance_ns = balance_ns;
    s_cvdcap     = cvdcap;
    s_acc_count  = acc_count;

    if( nora_touch_apply_list() != NORA_ITC_OK )
    {
        /* Put the working settings back and re-apply them, so a refused sweep
         * point leaves the list acquiring with what the caller last saw rather
         * than with a configuration the ITC rejected. */
        s_charge_ns  = old_charge;
        s_balance_ns = old_balance;
        s_cvdcap     = old_cvdcap;
        s_acc_count  = old_acc;
        (void)nora_touch_apply_list();
        s_scan_in_flight = false;
        return false;
    }

    /* Everything measured so far belonged to the old settings: the baseline is an
     * absolute count and the peaks are extremes of a delta against it, and the
     * §1.1 corollary applies inside one image too. Invalidate rather than adjust
     * -- the first scan after this re-seeds the baseline outright. */
    for( i = 0u; i < s_key_count; i++ )
    {
        s_keys[i].baseline_valid  = false;
        s_keys[i].pressed         = false;
        s_keys[i].debounce        = 0u;
        s_keys[i].settle          = NORA_TOUCH_SETTLE_SCANS;
        s_keys[i].seed_n          = 0u;
        s_keys[i].reject_run      = 0u;
        s_keys[i].event           = NORA_TOUCH_EVENT_NONE;
        s_keys[i].peak            = 0;
        s_keys[i].trough          = 0;
        /* A press amplitude scales with the acquisition settings just as the count
         * does, so presses learned under the old timings say nothing about the new
         * ones. Forget them and re-learn; unpinned keys go back to the configured
         * pair, since a learned threshold can only move down from it. */
        s_keys[i].learn_n         = 0u;
        s_keys[i].learn_peak      = 0;
        s_keys[i].learn_run       = 0u;
        s_keys[i].quiet_run       = 0u;
        s_keys[i].learn_active    = false;
        s_keys[i].idle_ref        = 0;
        s_keys[i].cal_done        = false;
        /* Same reasoning one line up, applied to the gate: a press reported under
         * the old timings does not vouch for the pad under the new ones. */
        s_keys[i].cold_gate       = true;
        if( !s_keys[i].cal_pinned )
        {
            s_keys[i].press_threshold   = s_cfg.press_threshold;
            s_keys[i].release_threshold = s_cfg.release_threshold;
        }
    }

    s_scans          = 0u;
    s_all_quiet      = false;
    s_rejected       = 0u;
    s_implausible    = 0u;
    s_scan_in_flight = false;
    return true;
}

void nora_touch_get_acquisition( uint32_t *charge_ns, uint32_t *balance_ns,
                                 uint8_t *cvdcap, uint8_t *acc_count )
{
    if( charge_ns )  { *charge_ns  = s_charge_ns; }
    if( balance_ns ) { *balance_ns = s_balance_ns; }
    if( cvdcap )     { *cvdcap     = s_cvdcap; }
    if( acc_count )  { *acc_count  = s_acc_count; }
}

/*===========================================================================
 * Hardware diagnostics
 *
 * Thin translation, deliberately: these functions add no policy beyond one
 * ownership check and one bounded poll. The point is that the acquisition
 * layer's header stops at this file, not that this file reinterprets it -- so
 * each entry point turns a nora_itc_* status into `false` plus text, and does
 * nothing else the caller cannot see.
 *=========================================================================== */

#define NORA_TOUCH_HW_SCAN_POLLS   (200000UL)

static nora_itc_record_config_t s_hw_records[NORA_TOUCH_HW_RECORD_MAX];
static uint8_t                  s_hw_record_count;
static bool                     s_hw_configured;
static const char              *s_hw_error = "ok";

static const char *nora_touch_hw_text( nora_itc_status_t status )
{
    switch( status )
    {
    case NORA_ITC_OK:                  return "ok";
    case NORA_ITC_ERR_INVALID_ARG:     return "invalid argument";
    case NORA_ITC_ERR_UNSUPPORTED:     return "unsupported";
    case NORA_ITC_ERR_NOT_INITIALIZED: return "not initialized";
    case NORA_ITC_ERR_ADC_NOT_READY:   return "ADC 5 never raised ADRDY";
    case NORA_ITC_ERR_NOT_READY:       return "ITCSTAT.DRDY never came up";
    case NORA_ITC_ERR_TIMING:          return "a requested time does not fit its timer";
    case NORA_ITC_ERR_BUSY:            return "a scan is in flight";
    case NORA_ITC_ERR_TIMEOUT:         return "timeout";
    default:                           return "?";
    }
}

/* Record the outcome and answer it in one place, so no path can return false
 * while leaving the previous call's reason standing as the explanation. */
static bool nora_touch_hw_result( nora_itc_status_t status )
{
    s_hw_error = nora_touch_hw_text( status );
    return (status == NORA_ITC_OK);
}

static bool nora_touch_hw_fail( const char *why )
{
    s_hw_error = why;
    return false;
}

const char *nora_touch_hw_last_error( void )
{
    return s_hw_error;
}

bool nora_touch_hw_get_info( nora_touch_hw_info_t *info )
{
    nora_itc_info_t   raw;
    nora_itc_status_t status;

    if( !info ) { return nora_touch_hw_fail( "invalid argument" ); }

    status = nora_itc_get_info( NORA_TOUCH_LIST, &raw );
    if( status != NORA_ITC_OK ) { return nora_touch_hw_result( status ); }

    info->configured         = raw.initialized;
    info->hardware_ready     = raw.hardware_ready;
    info->list_busy          = raw.list_busy;
    info->next_record        = raw.next_record;
    info->test_inject_active = raw.test_inject_active;
    info->record_count       = raw.record_count;
    info->cvdcap             = raw.cvdcap;
    info->acc_count          = raw.acc_count;
    info->clock_hz           = raw.clock_hz;
    info->charge_counts      = raw.charge_counts;
    info->balance_counts     = raw.balance_counts;
    info->scans_completed    = raw.scans_completed;
    info->last_status        = nora_touch_hw_text( raw.last_status );

    return nora_touch_hw_result( NORA_ITC_OK );
}

bool nora_touch_hw_configure( uint32_t clock_hz,
                              const uint8_t *cvdan, uint8_t count,
                              uint32_t charge_ns, uint32_t balance_ns,
                              uint8_t cvdcap, uint8_t acc_count )
{
    nora_itc_list_config_t list;
    uint8_t                i;

    /* The refusal is the feature. Detection is scanning this list continuously,
     * and reprogramming it from outside would take it over silently -- the same
     * two-owners-on-one-peripheral failure that is invisible from the console. */
    if( s_initialized )
    {
        return nora_touch_hw_fail( "the detection layer owns the list" );
    }
    if( !cvdan || (count == 0u) || (count > NORA_TOUCH_HW_RECORD_MAX) )
    {
        return nora_touch_hw_fail( "invalid argument" );
    }

    for( i = 0u; i < count; i++ )
    {
        s_hw_records[i].cvdan   = cvdan[i];
        s_hw_records[i].guard_a = NORA_ITC_GUARD_NONE;
        s_hw_records[i].guard_b = NORA_ITC_GUARD_NONE;
    }
    s_hw_record_count = count;

    list.clock_hz     = clock_hz;
    list.records      = s_hw_records;
    list.record_count = count;
    list.charge_ns    = charge_ns;
    list.balance_ns   = balance_ns;
    list.cvdcap       = cvdcap;
    list.acc_count    = acc_count;
    list.trigger      = NORA_ITC_TRIGGER_SOFTWARE;
    list.period_us    = 0u;
    list.mode         = NORA_ITC_SCAN_LIST_NO_IRQ;

    s_hw_configured = (nora_itc_init( NORA_TOUCH_LIST, &list ) == NORA_ITC_OK);
    return nora_touch_hw_result( s_hw_configured ? NORA_ITC_OK
                                                 : NORA_ITC_ERR_TIMING );
}

bool nora_touch_hw_scan_once( void )
{
    nora_itc_status_t status;
    uint32_t          polls = NORA_TOUCH_HW_SCAN_POLLS;

    if( s_initialized )
    {
        return nora_touch_hw_fail( "the detection layer owns the list" );
    }

    status = nora_itc_scan_start( NORA_TOUCH_LIST );
    if( status != NORA_ITC_OK ) { return nora_touch_hw_result( status ); }

    while( !nora_itc_scan_complete( NORA_TOUCH_LIST ) && (polls != 0u) )
    {
        polls--;
    }

    return nora_touch_hw_result( (polls == 0u) ? NORA_ITC_ERR_TIMEOUT
                                               : NORA_ITC_OK );
}

bool nora_touch_hw_read_raw( int32_t *results, uint8_t results_len )
{
    if( s_initialized )
    {
        return nora_touch_hw_fail( "the detection layer owns the list" );
    }
    if( !results || (results_len < s_hw_record_count) )
    {
        return nora_touch_hw_fail( "invalid argument" );
    }

    return nora_touch_hw_result( nora_itc_read_all( NORA_TOUCH_LIST, results,
                                                    results_len ) );
}

bool nora_touch_hw_debug_reg( uint8_t index, const char **name, uint32_t *value )
{
    return nora_touch_hw_result( nora_itc_debug_reg( index, name, value ) );
}

bool nora_touch_hw_test_inject( bool enable, uint16_t value )
{
    return nora_touch_hw_result( enable ? nora_itc_test_inject_enable( value )
                                        : nora_itc_test_inject_disable() );
}
