#ifndef NORA_TOUCH_H
#define NORA_TOUCH_H

/* Provenance: written from DS70005591 ch.18 (ITC), the DFP SFR header, and
 * measurements taken on this board. No vendor touch-library source, header or
 * binary was consulted, and no vendor detection algorithm was inspected: the
 * thresholds below are derived from this board's own measured figures -- idle
 * magnitude in the low hundreds against press peaks in the thousands -- and each
 * one carries its measurement in the comment beside it.
 */

#include <stdint.h>
#include <stdbool.h>

/*
 * Open capacitive touch — detection layer.
 *
 * nora_itc produces raw signed counts and stops there, deliberately. This layer
 * is what turns those counts into press/release: a tracked baseline, a
 * plausibility guard, per-key thresholds with hysteresis, and debounce.
 *
 * It is a HAL, and its scope follows from the name rather than from where the
 * register access stops. What an application expects of something called "touch"
 * is that it can obtain press and release — so everything needed to deliver that
 * belongs here: identifying the parameters, removing the touch equivalent of
 * switch chatter, and absorbing the individual characteristics of the board in
 * front of you. The last of those is why learn_presses exists: each pad learns
 * its own press amplitude in use and moves its own threshold down to suit it, so
 * a firmware carrying nothing but the defaults below still ends up at the feel a
 * bench operator would have hand-tuned. nora_touch_set_key_thresholds() remains
 * for the integrator who has measured something the pad cannot learn, and pinning
 * a key that way stops the learner from touching it.
 *
 * What is *not* here is event semantics. Long press, double tap, repeat: those
 * are meanings an application assigns to a press/release stream, they differ per
 * application, and the board layer already implements the one this project uses
 * (BUTTON_LONG_PRESS_MS in board/devices/button_led.c). A second copy here would
 * be two sources of truth for one behaviour.
 *
 * What it is not: drift compensation over temperature and humidity, wet-finger
 * rejection, frequency hopping, or a scroller. Those are the integrator's, and
 * saying so is deliberate: pretending otherwise here would be the one dishonest
 * thing this library could do.
 *
 * Scanning is non-blocking. nora_touch_process() polls the scan it started last
 * time and starts the next one, so the main loop never busy-waits on a scan that
 * takes ~5 ms at accumulation 2^8.
 */

#define NORA_TOUCH_KEY_MAX   (8u)

typedef enum {
    NORA_TOUCH_EVENT_NONE = 0,
    NORA_TOUCH_EVENT_PRESSED,
    NORA_TOUCH_EVENT_RELEASED,
} nora_touch_event_t;

typedef struct {
    /* Frequency of the clock feeding the ITC (CLKGEN6 on dsPIC33AK), in Hz.
     *
     * Required, and deliberately not defaulted: which generator feeds the ITC and
     * what the board raised it to are facts about the board and its clock tree, and
     * this layer owns neither. The ITC needs it because every acquisition time it
     * is given is in nanoseconds and lands in an 8-bit timer, so the same 1,200 ns
     * charge is a different count on a 100 MHz board and a 200 MHz one. Get it
     * wrong and nothing reports an error: the pads simply acquire for the wrong
     * time and read a weaker signal, which is the one failure that looks like a
     * hardware problem. nora_touch_init() therefore refuses 0 rather than
     * substituting a number that would be right on exactly one board. */
    uint32_t clock_hz;

    /* Detection thresholds, in raw counts, applied to the key's *activity
     * magnitude*: the mean of |raw - baseline| over the last few scans. They are
     * NOT compared against the signed delta, and that distinction is the whole
     * reason light touches work at all.
     *
     * A scan-resolution trace taken on 2026-08-14 showed that a touched pad's
     * delta does not sit high — it alternates, -4,180 then +2,346 on consecutive
     * scans, nine sign flips in seventeen. Any signed threshold with a
     * consecutive-scan debounce therefore rejects most real touches while the
     * signal is plainly present, which is exactly the "only a hard flat press
     * works" symptom this replaced.
     *
     * Rectified and averaged, the same trace separates cleanly: the touched pad
     * reaches 1,929 while its own idle maximum is 274 and the untouched pads stay
     * at 317 and below. Hence 700 press / 350 release — about 2.5x above the worst
     * idle window and 2.7x below a light touch, with the lower release giving the
     * hysteresis that keeps a finger rolling off from chattering.
     *
     * If you retune these, retune them in magnitude units. A number carried over
     * from the old signed-level scheme will be roughly 3x too large. */
    int32_t  press_threshold;
    int32_t  release_threshold;

    /* Consecutive scans a crossing must survive. Cheap insurance against a
     * single bad conversion, at 1 scan (~5 ms) of latency each.
     *
     * The two directions get separate counts because they fail differently, and
     * measurably so: with 2 scans both ways, the bench run caught four taps out
     * of ten split into two events, every one of them a release 11 ms (two scans)
     * after the press at an unchanged delta. A momentary dip during the press
     * stroke was being read as a lift. Lengthening only the release side buys
     * immunity to that without adding any latency to the press — which is the
     * half a human can feel. */
    uint8_t  debounce_scans;
    uint8_t  release_debounce_scans;

    /* Baseline tracking: baseline += (raw - baseline) >> shift, once per scan,
     * and ONLY while the key is released. Tracking during a press would follow
     * the finger and release the key on its own. A larger shift tracks slower;
     * 6 is ~64 scans, well under a second, which handles hand-warming drift
     * without eating a slow press. */
    uint8_t  baseline_shift;

    /* How many presses each pad must see before it first sets its own thresholds,
     * or 0 to keep the fixed pair above for good. A minimum, not a quota: the pair
     * is recomputed on every press after that, so the estimate keeps improving
     * without the threshold waiting for it.
     *
     * This is the answer to the problem the bench runs exposed: every threshold
     * in this file was measured on one board with one finger, and the pads on
     * that one board already differ enough that one of them dropped 2 taps in 10
     * at a threshold the other two never missed at. A constant cannot follow
     * that, and asking the integrator to re-measure per board is asking them to
     * do what the library is for.
     *
     * The obvious way to do it — measure each quiet pad and scale its noise —
     * was tried and measured *wrong*: pad 1 had the smallest noise tail
     * and needed the highest threshold. Idle noise does not predict a press.
     * Only a press predicts a press, so the pad learns from being touched.
     *
     * That is why detection is not held off at start-up and why there is no
     * hands-off window: a firmware carrying nothing but these defaults comes up
     * detecting at the shipped pair, and each pad walks its own threshold *down*
     * towards its own measured press as it is used. The cost is stated plainly
     * rather than hidden: a touch light enough to fall under the shipped
     * threshold is missed as an event while still being counted as evidence, so
     * the first press or two on a cold boot may not register — accepted by the
     * operator as the price of self-calibration (2026-08-14).
     *
     * Nothing is stored across a power cycle, deliberately. What the board does
     * at boot then never depends on what happened before it, which is worth more
     * than saving the user five taps. The rule itself,
     * its limits and their measured basis are at NORA_TOUCH_LEARN_* in the .c.
     */
    uint8_t  learn_presses;

    /* Cold gate: the stricter pair a pad detects at until it has recorded its
     * first press, or 0 / 0 to detect at the shipped values from the first scan.
     *
     * The problem it answers was observed on hardware (2026-08-15): a board left
     * alone after power-up fired isolated press/release pairs with nobody near it,
     * at magnitudes 523 and 705. Neither is a glitch a debounce can reach --
     * reconstructed from the event timestamps the excursion held above threshold
     * for some 8 scans, and a debounce long enough to reject that (>= 8, ~55 ms)
     * also rejects a short real tap, whose above-threshold plateau is only about
     * 5 scans once the 4-scan magnitude window has eaten each end. So the length
     * of the excursion cannot separate them, and the amplitude has to.
     *
     * The 900 value was measured at charge 2,000 ns. At the default 5,000 ns charge,
     * idle_ref rests at 90..97 and real presses measure 994..1,408, while the
     * measured idle magnitude is 81..119. The shipped value is therefore 800: it
     * leaves about 8x over idle while remaining below the observed taps. The full
     * measurement and its rationale are beside the default in the .c.
     *
     * This costs the learner nothing, which is the reason it is worth doing. The
     * candidate gate (NORA_TOUCH_LEARN_CAND_MIN in the .c) is what records press
     * amplitudes, it is separate from detection, and it never fires an event -- so
     * a pad still gathering evidence there while refusing to report anything under
     * 800 calibrates at exactly the speed it always did. The whole price is paid by
     * the first touch, which may not register: already the accepted trade for
     * self-calibration (operator, 2026-08-14).
     *
     * The gate lifts on the pad's first press *event* -- not on a recorded sample,
     * and not on a full calibration. The distinction is not pedantry, it is the bug
     * this was shipped with for one afternoon: lifting on the first sample was tried
     * and measured wrong on 2026-08-15, when a 3 h soak with nobody in the room saw
     * two pads record a sample from their own noise (mag 679 and 711) and disarm
     * themselves, while the event side stayed correctly silent. The learning path is
     * *meant* to be more permissive than detection, so it cannot also be the thing
     * that decides a human is present. Only an event can.
     *
     * From the first event the pad detects at the shipped pair, and at
     * learn_presses the learned pair takes over as before. One event is trusted to
     * say "a human is here", which is all this decides; it is deliberately not
     * trusted to set a threshold, because a single light excursion would then pin
     * the pad low.
     *
     * Both values may only make a pad *stricter* -- a cold pair below the shipped
     * one, or a cold debounce shorter than debounce_scans, is ignored rather than
     * quietly increasing sensitivity before anything has been learned. An
     * explicitly pinned pair (nora_touch_set_key_thresholds) switches the gate off
     * for that pad: the integrator's number is meant to be the one in force.
     * nora_touch_calibrate() and nora_touch_set_acquisition() re-arm it, both
     * because a pad whose evidence has just been discarded has not, as far as
     * anything here can tell, been touched. Nothing survives a power cycle,
     * deliberately, as with the rest of the learning state.
     *
     * Independent of learn_presses: with learning off the shipped pair never moves,
     * and a pad nobody has touched should still be the stricter of the two. */
    int32_t  cold_press_threshold;
    uint8_t  cold_debounce_scans;

    /* Print a line per event, in the same shape as the vendor demo's, so the
     * behavioural comparison against the vendor demo can be scored
     * from one console log either way. Also prints each pad's measured tail and
     * derived thresholds once per calibration, which is the only way to see that
     * two boards differ without instrumenting anything. */
    bool     verbose;
} nora_touch_config_t;

/* Fill cfg with the bench-derived defaults above. Provided as a function so the
 * numbers live in one place and a caller cannot half-initialize the struct. */
void nora_touch_default_config(nora_touch_config_t *cfg);

/* Configure the ITC for the given electrodes and arm detection.
 *
 * cvdan[] holds CVDANx analog-input numbers — a fact about the board, so the
 * caller supplies them from its own board layer.
 * Returns false if the ITC refused the configuration, in which case nothing is
 * armed and nora_touch_process() does nothing.
 */
bool nora_touch_init(const uint8_t *cvdan, uint8_t key_count,
                     const nora_touch_config_t *cfg);

/* Call from the main loop. Polls the scan in flight; when it completes, updates
 * every key and starts the next scan. Does nothing until nora_touch_init() has
 * succeeded. No time argument: the layer counts scans, not milliseconds, and
 * every timing it owns (debounce, settling) is expressed in scans. */
void nora_touch_process(void);

/* One event per key per occurrence, cleared by reading — the same consume-once
 * contract the application already expects from its button layer. */
nora_touch_event_t nora_touch_get_event(uint8_t key);

bool nora_touch_is_pressed(uint8_t key);

typedef struct {
    bool     initialized;
    uint8_t  key_count;
    uint32_t scans;
    uint32_t rejected_scans;   /* scans the ITC refused or that timed out */

    /* Samples discarded because the count could not have come from an electrode.
     *
     * Measured 2026-08-14: after a reconfiguration a record occasionally reads back
     * near zero against a settled count of about -875,000, which is a delta of some
     * 80 % of full scale. A finger is worth 1.5 %, so this is not a touch and not
     * noise; it is a result that belongs to no scan. They arrive in bursts — tens
     * of consecutive samples, in 12 of 20 sweep points, and never on the list's
     * first record — so a single-sample defence is not enough and the layer also
     * re-seeds the baseline when a burst is long enough to suggest the reference
     * rather than the sample is wrong.
     *
     * Counted rather than silently dropped, for the same reason rejected_scans is:
     * if this number climbs while the keys are idle, the acquisition is broken and
     * no amount of threshold tuning will help. */
    uint32_t implausible_samples;
} nora_touch_status_t;

void nora_touch_get_status(nora_touch_status_t *status);

/* Per-key detail for the console and the tuning procedure: the raw count, the
 * baseline it is being compared against, and the difference that decides the
 * key. Reporting all three is what makes a wrong threshold diagnosable instead
 * of just "the key does not work".
 *
 * peak/trough are the extremes of delta since the last nora_touch_reset_peaks(),
 * recorded every scan and *independently of the thresholds*. They exist because
 * the one thing the event log cannot show is a touch that failed to reach the
 * press threshold: a miss prints nothing at all, so without these the only way
 * to find out how close a light touch came is to lower the threshold and see —
 * which changes the thing being measured. peak answers "how much signal does
 * this electrode really give", trough answers "how far does it wander with
 * nobody there", and the ratio between them is the sensitivity the tuning
 * procedure asks for.
 */
typedef struct {
    uint8_t  cvdan;
    int32_t  raw;
    int32_t  baseline;
    int32_t  delta;
    int32_t  peak;
    int32_t  trough;
    int32_t  mag;      /* mean |delta| over the magnitude window: what the
                        * thresholds actually compare against */
    bool     pressed;

    /* Press events since the last nora_touch_reset_peaks(), which is what makes a
     * counted tap test possible at all. Without it a run's event total includes
     * every touch since boot -- measured 2026-08-14, an acceptance run reported 60
     * events for 30 deliberate taps and the excess could not be attributed, because
     * taps made before the run started are indistinguishable from a single tap
     * split into two events. Cleared together with peak/trough, so "clear, tap N
     * times, read" is one procedure rather than two. */
    uint16_t presses;
} nora_touch_key_state_t;

bool nora_touch_get_key_state(uint8_t key, nora_touch_key_state_t *state);

/* Clear peak/trough and the press counter on every key. Call it immediately before a measurement so
 * the extremes belong to that measurement and not to the whole session. */
void nora_touch_reset_peaks(void);

/* Change both thresholds at runtime, in raw counts. Runtime rather than
 * compile-time for the same reason the acquisition times are: a reflash per data
 * point turns a threshold sweep into an afternoon. Refuses (returns false, and
 * changes nothing) unless release < press and both are positive — equal or
 * inverted values remove the hysteresis, which is the one mistake here that
 * shows up as a key that chatters rather than as an error. */
bool nora_touch_set_thresholds(int32_t press_threshold, int32_t release_threshold);

/* The same, for one key, overriding the pair above from here on.
 *
 * Pads on one board are not equally sensitive: in the bench run the three pads
 * of the Curiosity Platform accepted taps down to 2,004 / 2,120 / 2,935 counts,
 * so a single press threshold of 2,000 sat right on top of pad 1's lightest taps
 * and dropped two of ten while the other pads never missed. Which pad needs what
 * is a fact about the board, not about detection, so the values are set from the
 * integration point that already states the CVDAN numbers.
 *
 * Same validity rule as above: release < press, both positive, or it refuses and
 * changes nothing. Keys not overridden keep whatever the global pair is. */
bool nora_touch_set_key_thresholds(uint8_t key, int32_t press_threshold,
                                   int32_t release_threshold);

/* Forget what every pad has learned and start again from the configured pair.
 * Returns false if the layer is not initialized or learning is switched off
 * (learn_presses == 0).
 *
 * Exposed because the interesting case is not start-up. Moving the board, changing
 * the enclosure or plugging in a supply changes how much signal a finger produces,
 * and a pad that learned in the old conditions will happily go on using the old
 * number. Note that this restores the thresholds as well as clearing the samples:
 * learning can only move a threshold down, so relearning from an already-learned
 * pair would be a second descent rather than a fresh start.
 *
 * Keys release on the way in — a press cannot be held across a change of the
 * thresholds that decided it. No hands-off window is needed: nothing is measured
 * by this call, it only forgets. */
bool nora_touch_calibrate(void);

/* Per-key learning state, for the console and for comparing two boards.
 * calibrated is false while a pad is still collecting presses (samples < needed),
 * which is the normal state for the first few touches after a boot. */
typedef struct {
    bool     calibrated;
    bool     pinned;          /* thresholds set explicitly, so learning left them alone */
    int32_t  idle_ref;        /* decaying max of the pad's quiet magnitude       */
    uint8_t  samples;         /* press amplitudes recorded so far                */
    uint8_t  needed;          /* learn_presses: how many before the rule applies */
    int32_t  press_threshold; /* in force now (learned, configured, or pinned)   */
    int32_t  release_threshold;
    /* True while the pad is still on the cold gate, i.e. press_threshold above is
     * cold_press_threshold and not the configured or learned value. Reported
     * because the two states are indistinguishable from the event log -- a cold
     * pad and an insensitive pad both just fail to report a light touch. */
    bool     cold_gate;
} nora_touch_calibration_t;

/* --- scan-resolution trace -------------------------------------------------
 * A short recording of every key's delta, sampled once per scan, triggered in
 * firmware. It exists because the polled ?ko view runs at a few samples a second
 * against ~146 scans a second, which is too coarse to answer either of the two
 * questions a misbehaving pad raises: did the delta stay above the threshold for
 * the consecutive scans the debounce wants, and did the other pads move at the
 * same instant (coupling) or stay put.
 *
 * arm() clears the buffer and waits; recording starts on the first scan where any
 * key's |delta| exceeds `trigger` (<= 0 means 800) and stops when the buffer is
 * full, so the human can touch whenever they like rather than inside a window
 * somebody else opened. Nothing is recorded until the trigger, so an armed trace
 * costs one comparison per key per scan. */
void     nora_touch_trace_arm( int32_t trigger );
bool     nora_touch_trace_ready( void );
uint16_t nora_touch_trace_count( void );
bool     nora_touch_trace_get( uint8_t key, uint16_t index, int32_t *delta );

bool nora_touch_get_calibration(uint8_t key, nora_touch_calibration_t *out);

/* The thresholds now in force, so the console can report what it is measuring
 * against without keeping a second copy that could drift from this one.
 * These are the global pair; a key overridden above is not reflected here. */
void nora_touch_get_thresholds(int32_t *press_threshold, int32_t *release_threshold);

/* Re-programme the acquisition settings on the list this layer is scanning, and
 * restart measurement: peaks cleared and baselines invalidated, so the next scan
 * re-seeds them.
 *
 * This exists so an acquisition sweep can be run against the *tracked* noise
 * figure (?ko peak/trough over tens of thousands of scans) instead of a
 * six-sample spread, which the first sweep found underestimates the tail
 * by ~3x. Without it a sweep has to go through *ki, which reprogrammes the same
 * list from the console's own record set and silently takes it away from this
 * layer.
 *
 * Returns false and leaves the previous settings running if the ITC refuses the
 * combination — a time that does not fit its 8-bit timer, most often.
 */
bool nora_touch_set_acquisition(uint32_t charge_ns, uint32_t balance_ns,
                               uint8_t cvdcap, uint8_t acc_count);

/* What the list is currently acquiring with. Any pointer may be NULL. */
void nora_touch_get_acquisition(uint32_t *charge_ns, uint32_t *balance_ns,
                                uint8_t *cvdcap, uint8_t *acc_count);

/* ===========================================================================
 * Hardware diagnostics
 *
 * This HAL provides touch, and the acquisition peripheral underneath it is not
 * part of its interface: there is one public header, this one, and the ITC
 * layer is private to the backend. But bring-up genuinely needs to reach the
 * hardware — the question "did the peripheral accept what I wrote" is the fork
 * every touch problem so far has turned on, and an open library has to make its
 * own correctness demonstrable rather than assert it. So the reaching is done
 * through named entry points here, with the peripheral's types kept out:
 * everything below is stated in plain integers and a text string.
 *
 * These are for a console and a bench procedure (the tuning manual drives all of
 * them). An application that only wants presses never calls any of them.
 * ======================================================================== */

/* Fixed hardware limits, restated here so a caller can size arrays and range-
 * check an operator's input without the peripheral's header. */
#define NORA_TOUCH_HW_RECORD_MAX  (32u)  /* electrodes in one scan list         */
#define NORA_TOUCH_HW_CVDCAP_MAX  (7u)   /* internal CVD capacitor code         */
#define NORA_TOUCH_HW_ACC_MAX     (15u)  /* accumulation depth exponent, 2^n    */

/* Why the last call answered false, as text. Owned by this HAL, valid until the
 * next diagnostics call, never NULL. Text rather than a code on purpose: the
 * only thing a console does with an acquisition-layer error is print it, and a
 * public error enum would be the peripheral's vocabulary leaking out through a
 * different door. */
const char *nora_touch_hw_last_error(void);

typedef struct {
    bool     configured;
    bool     hardware_ready;      /* the block reports itself ready            */
    bool     list_busy;
    uint8_t  next_record;         /* how far the current scan got              */
    bool     test_inject_active;
    uint8_t  record_count;
    uint8_t  cvdcap;
    uint8_t  acc_count;
    uint32_t clock_hz;
    /* What the requested times became in hardware counts, not what was asked.
     * Tuning is done against what the hardware got, and a request that rounded
     * to zero counts is the failure this exists to make visible. */
    uint16_t charge_counts;
    uint16_t balance_counts;
    uint32_t scans_completed;
    const char *last_status;      /* the acquisition layer's own last result   */
} nora_touch_hw_info_t;

bool nora_touch_hw_get_info(nora_touch_hw_info_t *info);

/* Programme the acquisition list directly from a caller-supplied electrode set.
 *
 * For a board whose electrodes are being discovered — typed in from a schematic
 * at the console — and for that case only. It takes the list away from the
 * detection layer, so it refuses while nora_touch_init() is in force rather than
 * becoming a second owner of one peripheral; use nora_touch_set_acquisition()
 * to sweep a running list. The electrodes are CVDANx analog-input numbers as the
 * board's pin table names them, not port bits.
 *
 * Software-triggered: nothing is measured until nora_touch_hw_scan_once(). */
bool nora_touch_hw_configure(uint32_t clock_hz,
                             const uint8_t *cvdan, uint8_t count,
                             uint32_t charge_ns, uint32_t balance_ns,
                             uint8_t cvdcap, uint8_t acc_count);

/* One scan, started and polled to completion. The poll is bounded and the
 * timeout is reported: a scan that never completes is the interesting failure,
 * and blocking forever would hide it. Refuses while the detection layer owns
 * the list, whose own scan it would race for the completion flags. */
bool nora_touch_hw_scan_once(void);

/* The raw signed per-electrode results of the last scan, in list order. Reading
 * them consumes the completion state, so read the set you asked for. */
bool nora_touch_hw_read_raw(int32_t *results, uint8_t results_len);

/* Register readback, walked by index from 0 until it answers false. Answers
 * "did the peripheral accept the configuration" — the question that separates a
 * bad configuration from a dead electrode. The caller does the printing. */
bool nora_touch_hw_debug_reg(uint8_t index, const char **name, uint32_t *value);

/* Test injection: every conversion returns `value` instead of the converter's.
 * This drives accumulation, the pseudo-differential arithmetic, sign and every
 * layer above with known numbers, no electrode and no finger — the cheapest
 * proof that the chain is right, which is why it is API and not a debug hook. */
bool nora_touch_hw_test_inject(bool enable, uint16_t value);

#endif /* NORA_TOUCH_H */
