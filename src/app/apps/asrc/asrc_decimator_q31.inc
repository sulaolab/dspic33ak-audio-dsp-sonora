/*
 * asrc_decimator_q31.inc -- 16-channel Q31 Nyquist front end.
 *
 * THIS FILE IS CODE, not a generated table.  It is #included at the end of
 * asrc_decimator_48_to_8.c rather than compiled on its own because the
 * coefficient tables it converts are `static const` in that translation unit:
 * including it here shares the ONE copy of every coefficient set (and the one
 * set of CRC32 _Static_asserts that pins them) instead of duplicating ~4 KB of
 * float tables in ROM for a second translation unit.
 *
 * What this is.  The float front end above computes 2 channels with a mirrored
 * history and a symmetric-window FIR.  This computes ASRC_CH channels in Q31
 * with the hardware dual-fetch MAC kernel, which needs its two operands in
 * DIFFERENT data spaces: coefficients in X, history in Y.  Measured on this
 * silicon ([internal] report_ak512_fir_kernel_measured_2026-08-21.md):
 *
 *     coefficients in X RAM, history in Y, Y modulo only : 1.052 cycles/MAC
 *     coefficients in PROGRAM FLASH, history in Y        : 3.063 cycles/MAC
 *
 * So the coefficient MASTERS stay in program flash (they are the float tables
 * above) and init() converts the selected chain's masters into an X RAM working
 * buffer.  Every MAC executed from the audio ISR reads X RAM.  Nothing on the
 * audio path touches flash, and nothing on the audio path converts.
 *
 * Y-modulo-only is a SAFETY property, not an optimisation.  MODCON, XMODSRT/
 * XMODEND and YMODSRT/YMODEND are not part of the per-IPL register context
 * (DS70005591C Table 4-2), so whatever this kernel programs is live in every
 * context that can preempt it.  An X modulo would fold a preempting handler's
 * ordinary mov.l [w0] into this kernel's ring; the Y AGU is used only by DSP
 * MAC-class instructions (4.3.16), so with X modulo off the residual condition
 * is "no preempting handler executes a MAC-class instruction" -- which is
 * checkable on the linked image rather than promised in a comment.  Dropping X
 * modulo costs one coefficient-pointer reload per output: measured +1.0 %.
 *
 * Ring geometry -- why 190 samples per channel, and why the batch size varies.
 * The kernel produces `outputs` outputs from one call by stepping the window by
 * `decim` each time, so all of them must be live in the ring at once:
 *
 *     taps + (outputs - 1) * decim <= ring_samples
 *
 * The budget is 190 samples/channel (12,160 B for 16 channels), fixed because
 * /6 is two stages of 43 + 147 taps and a ring can never be shorter than its
 * tap count.  /6 therefore spends the whole budget on taps and takes one output
 * per call; every shallower chain has slack and gets ONE call per channel per
 * block, which is what keeps the ~70-cycle per-call setup from being multiplied
 * by the output count.  The table in init() is that allocation and the
 * _Static_asserts below are what stop it from drifting.
 */

/* Compute width.  ASRC_CH is the ASRC logical width, and this front end takes it verbatim so the
 * two halves of the path carry the same channel count.  A rate does not appear in that sentence on
 * purpose: which rate a build runs may set the WORKLOAD a width costs, and a preset may therefore
 * choose a different ASRC_CH, but the rate never determines how many channels get filtered.  Defined here, not in the header: this file is included at the END of
 * the .c, by which point app_specific_config_defs.h has pulled in
 * apps/asrc/asrc_app_config.h.  The header is included first and would see
 * nothing.
 *
 * NO DEFAULT, deliberately.  This used to fall back to 2 when ASRC_CH was not visible, which is
 * the most dangerous shape a width bug can take: it compiles, it links, stereo audio still comes
 * out, the CPU load drops -- every symptom improves while the front end silently stops filtering
 * the channels the resampler is converting.  An include-path change could trigger it with no
 * diagnostic at all.  A missing width is a build error, not a narrower build. */
#if !defined(ASRC_CH)
#  error "ASRC_CH is not visible here -- the ASRC application configuration owns the logical channel width. Include apps/asrc/asrc_app_config.h before this fragment; do NOT reintroduce a default, which would silently narrow the front end while every symptom looked better."
#endif
#define ASRC_DEC_Q31_CH  ASRC_CH

/* The channel repeat below is a bitmask (`f[c & mask]`), which is what ties this kernel to
 * power-of-two widths -- but it ties only the SOURCE width, never the logical one.  A source
 * narrower than ASRC_CH has to be repeated, and a mask can only repeat a power-of-two count.
 * A source that is ALREADY the full logical width repeats nothing, so all its mask has to do
 * is leave every index below ASRC_CH alone: the smallest 2^k-1 that covers the width does
 * that for any width, power of two or not.  That is what this is, and it is why a
 * non-power-of-two ASRC_CH (12) is carried here while a non-power-of-two SLOT count still
 * is not.  ASRC_CH <= 16 is enforced by the app, so 15u is the widest mask this ever needs. */
#define ASRC_DEC_Q31_FULL_MASK                                            \
    ((uint8_t)(((ASRC_DEC_Q31_CH) <= 2u) ? 1u :                           \
               (((ASRC_DEC_Q31_CH) <= 4u) ? 3u :                          \
                (((ASRC_DEC_Q31_CH) <= 8u) ? 7u : 15u))))
_Static_assert(ASRC_DEC_Q31_CH <= 16u,
               "ASRC_DEC_Q31_FULL_MASK only covers widths up to 16");

/* The kernel.  Declared here rather than in a header because it is the only
 * caller in the application; the bench declares its own copy for the same
 * reason.  The signature is fixed by the .s file. */
extern int32_t *fir_ring_q31_ymod_yonly_block(
    const int32_t *coeff,       /* w0  taps entries, X space, no modulo */
    const int32_t *hist,        /* w1  Y space, oldest sample of window */
    uint32_t       taps,        /* w2  >= 2                             */
    int32_t       *out,         /* w3  outputs written here             */
    uint32_t       outputs,     /* w4  >= 1                             */
    uint32_t       decim_bytes, /* w5  decimation factor * 4            */
    const int32_t *ring,        /* w6  YMODSRT (ring base)              */
    uint32_t       ring_bytes); /* w7  ring length in bytes             */

#if APP_ASRC_Q31_PRE_HALFBAND
/* The half-band twin of the kernel above, MEASUREMENT ONLY.  Same window, same
 * modulo ring, same outputs -- it just steps the history by two samples across the
 * stretches where the filter structure makes the tap zero, so a 4*half-1 filter
 * costs 2*half+1 MACs.  `coeff` is therefore PACKED (19 entries for 35 taps), in the
 * ascending index order dec_q31_hb_coeff_index() below defines; `half` replaces the
 * tap count, and the filter length it implies is 4*half - 1.
 *
 * It is declared under the gate, not always, so that a build without the gate cannot
 * link against it by accident -- and a build WITH the C gate but without the matching
 * -AsDefine fails at link on this symbol, which is the failure we want.  Falling back
 * to the dense kernel would leave a measurement that silently measured nothing. */
extern int32_t *fir_ring_q31_hb_ymod_yonly_block(
    const int32_t *coeff,       /* w0  2*half+1 packed entries, X space */
    const int32_t *hist,        /* w1  Y space, oldest sample of window */
    uint32_t       half,        /* w2  >= 3; filter length 4*half - 1   */
    int32_t       *out,         /* w3  outputs written here             */
    uint32_t       outputs,     /* w4  >= 1                             */
    uint32_t       decim_bytes, /* w5  decimation factor * 4            */
    const int32_t *ring,        /* w6  YMODSRT (ring base)              */
    uint32_t       ring_bytes); /* w7  ring length in bytes             */
#endif

/*
 * PER-CHANNEL HISTORY ARENA, in two parts.
 *
 * The 48 kHz-input chains have always spent a fixed 209-sample budget, and /6 --
 * the deepest of them -- spends all of it (the equality assert below is what says
 * so).  A 96 kHz leg composes a 96 -> 48 kHz PRE-STAGE in front of one of those
 * chains, and both are live inside the same block, so the pre-stage cannot share
 * their rings.  It gets its own reserve at the TOP of the arena instead, which is
 * why every ring_off in init() below is unchanged and a build with no 96 kHz leg
 * allocates exactly the bytes it allocated before this stage existed.
 *
 * The reserve is DERIVED, not chosen.  The kernel emits `b` outputs from ONE call
 * while taps + (b-1)*decim <= ring, and the pre-stage has to clear a whole block
 * in one call -- ceil(APP_BLOCK_FRAMES / 2) outputs -- or it pays the ~70-cycle
 * call setup per output instead of per block.  At APP_BLOCK_FRAMES 16 that is
 * 41 + 7*2 = 55 samples/channel.
 *
 * THE WIDE PRE-STAGE VARIANTS DO NOT USE THE RESERVE (2026-09-03).  The 113- and
 * 169-tap sets serve 96 -> 44.1 kHz and 96 -> 48 kHz, where the pre-stage is the
 * WHOLE chain: nothing runs behind it, so the 209-sample budget is idle and the
 * wide ring is placed at offset 0 to spend it.  That is what keeps this arena --
 * and the 96 kHz build's Y history, 8,448 B at 8 channels -- exactly the size it
 * was with the shared set alone, instead of 12,544 B for a ring that is never
 * live at the same time as the chains it would have been stacked on top of.
 *
 *     composed (shared 41 taps)   ring at DEC_Q31_SECOND_ARENA, 55 samples
 *     pre-stage alone (113/169)   ring at 0, up to 183 samples
 *
 * The asserts below are what enforce it: the wide ring must fit the WHOLE arena,
 * and init() refuses a wide variant with a second stage behind it.
 */
#define DEC_Q31_SECOND_ARENA  (209u)   /* the 48 kHz-input chains' fixed budget */
#if ASRC_DECIMATOR_HAS_96_TO_48
/* The reserve is computed FROM the block size, so state that the block size is
 * visible.  #if reads an undefined macro as 0, which would size the ring from an
 * empty block: the asserts below would fire, but on tap counts rather than on the
 * real cause. */
#if !defined(APP_BLOCK_FRAMES)
#  error "APP_BLOCK_FRAMES is not visible here, and the 96 kHz pre-stage ring is derived from it -- include the application block-size configuration before this fragment."
#endif
#define DEC_Q31_PRE_DECIM     (2u)
#define DEC_Q31_PRE_TAPS      (ASRC_DECIMATOR_96_TO_48_TAPS)
#define DEC_Q31_PRE_OUT       ((((uint16_t)APP_BLOCK_FRAMES) + (DEC_Q31_PRE_DECIM - 1u)) \
                               / DEC_Q31_PRE_DECIM)
#define DEC_Q31_PRE_RING      (DEC_Q31_PRE_TAPS + ((DEC_Q31_PRE_OUT - 1u) * DEC_Q31_PRE_DECIM))
/* The widest variant's ring, which lives at offset 0 and is never composed. */
#define DEC_Q31_PRE_TAPS_MAX  (ASRC_DECIMATOR_96_TO_48_TAPS_MAX)
#define DEC_Q31_PRE_RING_WIDE \
    (DEC_Q31_PRE_TAPS_MAX + ((DEC_Q31_PRE_OUT - 1u) * DEC_Q31_PRE_DECIM))
#define DEC_Q31_PRE_STAGES    (1u)
#else
/* No 96 kHz leg in this build: no reserve, no descriptor, no intermediate buffer.
 * Every quantity below that mentions the pre-stage collapses to what it was. */
#define DEC_Q31_PRE_TAPS      (0u)
#define DEC_Q31_PRE_RING      (0u)
#define DEC_Q31_PRE_TAPS_MAX  (0u)
#define DEC_Q31_PRE_RING_WIDE (0u)
#define DEC_Q31_PRE_STAGES    (0u)
#endif
#define DEC_Q31_HIST_PER_CH   (DEC_Q31_SECOND_ARENA + DEC_Q31_PRE_RING)

/*
 * The /6 cascade's two rings.  They are deliberately wider than their tap
 * counts: the kernel emits `b` outputs from ONE call while
 * taps + (b-1)*decim <= ring, so a ring equal to the taps pins b at 1 and pays
 * one call's setup per output -- 128 calls per 16-frame block at 16 channels.
 * Sized for the whole block instead.  Stage 1 sees 16 inputs and a phase in
 * [0,2], so at most (16+2)/3 = 6 outputs; stage 2 then sees at most 6 inputs and
 * a phase in [0,1], so at most 3.  32 calls per block.
 *
 * Nothing else moves: /2, /3 and 2/3 are single-ring, and /4 (27/129 taps inside
 * 43/147) already batches, so those rows keep their current allocation.
 */
#define DEC_Q31_R6_S1_OUT     (6u)     /* stage-1 outputs per 16-frame block */
#define DEC_Q31_R6_S2_OUT     (3u)     /* stage-2 outputs per 16-frame block */
#define DEC_Q31_R6_S1_RING    (58u)    /* 43 + 5*3  */
#define DEC_Q31_R6_S2_RING    (151u)   /* 147 + 2*2 */
/* The X working buffer holds the LIVE chain's coefficients end to end, so a
 * composed chain needs the pre-stage's on top of the deepest 48 kHz chain's.
 * DEC_Q31_PRE_TAPS is 0 without a 96 kHz leg, so that build's buffer is the size
 * it has always been. */
#define DEC_Q31_COEFF_MAX     (211u + DEC_Q31_PRE_TAPS)
#define DEC_Q31_MAX_BATCH     (16u)    /* outputs per kernel call (scratch)   */
/* Two cascade stages for the 48 kHz chains, plus the 96 kHz pre-stage where one
 * exists: composed /12 is pre + /3 + /2 and is the deepest chain in the file. */
#define DEC_Q31_MAX_STAGES    (2u + DEC_Q31_PRE_STAGES)
#define DEC_Q31_MAX_MID       (8u)     /* inter-stage frames per block        */
/* One intermediate buffer per stage boundary that is not the output: a two-stage
 * cascade needs one, and a composed three-stage chain needs two.  They ping-pong,
 * so two is the most any chain here can want. */
#define DEC_Q31_MID_BUFFERS   (1u + DEC_Q31_PRE_STAGES)

/*
 * The 2/3 rational chain (48 -> 32 kHz AUDIO MODE), and why its numbers are what
 * they are.  Everything below is derived, not chosen -- the _Static_asserts after
 * the state struct re-derive each one so a changed block size cannot quietly
 * leave a constant behind.
 *
 * SCHEDULE.  Output k of an L=2/M=3 resampler is
 *
 *     y[2q]   = sum(h[2i]   * x[3q   - i]), i = 0..48   (49 taps, newest x[3q])
 *     y[2q+1] = sum(h[2i+1] * x[3q+1 - i]), i = 0..47   (48 taps, newest x[3q+1])
 *
 * so output k needs input floor(3k/2) and nothing newer.  Only the two phases
 * that exist are computed: there is no zero-stuffed 96 kHz stream anywhere in
 * here, and no MAC is spent on a coefficient that would meet a stuffed zero.
 * Each row reads CONSECUTIVE inputs and successive outputs of the SAME row hop
 * M=3 inputs, which is exactly the (taps, decim) shape the existing block kernel
 * already implements -- hence no new assembler, and two calls per channel per
 * block instead of one call per output.
 *
 * BLOCK OUTPUT COUNT.  Outputs available after n inputs is the number of k with
 * floor(3k/2) <= n-1, i.e. floor((n-1)/3) + floor((n-2)/3) + 2.  At n = 16 that
 * is 11, and the steady-state block sequence is 11, 11, 10 (period 3, average
 * 32/3).  11 -- not 10 or 11-on-average -- is what every buffer must hold.
 *
 * RING.  One kernel call per row needs every window of the block live at once.
 * The oldest sample any of them reads is (block frames) + (longest row - 1)
 * behind the newest, = 16 + 48 = 64.  That is the whole ring, exactly, and it is
 * also exactly what a 6-output batch of the 49-tap row spans (49 + 5*3 = 64), so
 * the two independent bounds agree at 64 rather than one of them being slack.
 */
#define DEC_Q31_R23_RING      (64u)    /* samples/channel, shared by both rows */
#define DEC_Q31_R23_MAX_IN    (16u)    /* input frames per call (the TDM block) */
#define DEC_Q31_R23_MAX_OUT   (11u)    /* output frames per call, worst case    */
#define DEC_Q31_R23_MAX_ROW_BATCH (6u) /* outputs of ONE row in one call        */

/* Q31 history.  A DECLARED object in Y space -- space(ymemory) -- not the
 * "address the top of the stack region directly" trick the bench used.  That
 * trick is right for a measurement device and forbidden here: shipping storage
 * must be visible to the linker, to the map and to the layout gate.  4-byte
 * alignment is all the Y AGU needs for a non-power-of-two modulo (the bench's
 * M6 case: 11/11 pass at a 760 B ring). */
static int32_t s_q31_hist[ASRC_DEC_Q31_CH][DEC_Q31_HIST_PER_CH]
    __attribute__((space(ymemory), aligned(4)));

/* Active coefficients, in X, written only by init().  space(xmemory) is what
 * forces X: left to the best-fit allocator these land wherever there is room,
 * which on this application means Y -- and then every MAC silently costs 2.0
 * cycles instead of 1.05, with no other symptom. */
static int32_t s_q31_coeff[DEC_Q31_COEFF_MAX]
    __attribute__((space(xmemory), aligned(4)));

/* Stage-1 outputs at full Q31 precision -- no s24 round trip between stages.
 * Ordinary space: read and written by C (MCU-class), never by the MAC. */
static int32_t s_q31_mid[DEC_Q31_MID_BUFFERS * DEC_Q31_MAX_MID * ASRC_DEC_Q31_CH];

/* Which intermediate buffer stage `s` writes into.  The stages alternate, so a
 * stage never reads the buffer it is writing: stage 0 -> buf 0, stage 1 -> buf 1
 * (reading buf 0), stage 2 -> the caller's output.  With one buffer -- no 96 kHz
 * leg, so at most two stages -- this is the constant it was. */
static inline int32_t *dec_q31_mid_buf(uint8_t s)
{
#if DEC_Q31_MID_BUFFERS > 1u
    return &s_q31_mid[(size_t)(s & 1u) * DEC_Q31_MAX_MID * ASRC_DEC_Q31_CH];
#else
    (void)s;
    return s_q31_mid;
#endif
}

typedef struct
{
    uint16_t taps;
    uint16_t ring;        /* samples/channel                              */
    uint16_t ring_off;    /* offset into the per-channel 190-sample arena  */
    uint16_t coeff_off;   /* offset into s_q31_coeff                       */
    uint8_t  decim;
#if APP_ASRC_Q31_PRE_HALFBAND
    /* Non-zero ONLY on a half-band stage, and it is what selects the kernel: the
     * coefficients at coeff_off are packed when this is set, so the two have to be
     * decided together and read together.  Under the gate only, so that the descriptor
     * array of a default build is the size and shape it has always been. */
    uint16_t hb_half;
#endif
} dec_q31_stage_t;

static struct
{
    /* For the integer chains these are CASCADE stages, 1 or 2 of them.  For the
     * 2/3 rational chain they are the two PHASE ROWS of one filter: same ring
     * (ring_off 0 for both), same decim 3, different taps and coefficients.
     * `num` is what tells the two apart -- see asrc_decimator_q31_init().
     *
     * A COMPOSED rational chain is both at once: stages [0, r23_base) are a real
     * cascade (today exactly the 96 -> 48 kHz pre-stage) and stages r23_base and
     * r23_base+1 are the two phase rows behind it.  That is why nothing in the
     * rational code may index stage 0 or wr[0] directly -- see r23_base. */
    dec_q31_stage_t stage[DEC_Q31_MAX_STAGES];
    uint16_t wr[DEC_Q31_MAX_STAGES];
    uint8_t  phase[DEC_Q31_MAX_STAGES];
    uint8_t  stages;
    uint8_t  channels;
    uint8_t  num;
    uint8_t  den;
    /* Rational schedule.  `r23_d` is (newest input the next output needs) minus
     * (inputs pushed so far), so an output is due exactly while it is <= -1; it
     * lives in [-1, +1] between blocks.  `r23_par` is that output's phase row. */
    int8_t   r23_d;
    uint8_t  r23_par;
    /* Where the rational chain's two phase rows START in stage[]/wr[].  0 when the
     * rows ARE the whole chain (a 48 kHz leg), 1 when the 96 -> 48 kHz pre-stage
     * runs in front of them (a 96 kHz leg).  Stored rather than derived from
     * `stages - 2` so the rational code reads the same base init() wrote. */
    uint8_t  r23_base;
} s_q31;

/* The 48 kHz-input chains are checked against THEIR arena, not the whole one: the
 * pre-stage reserve sits above them and is not theirs to spend. */
_Static_assert(ASRC_DECIMATOR_48_TO_24_TAPS <= DEC_Q31_SECOND_ARENA, "/2 ring");
_Static_assert(ASRC_DECIMATOR_48_TO_16_TAPS <= DEC_Q31_SECOND_ARENA, "/3 ring");
_Static_assert((DEC_Q31_R6_S1_RING + DEC_Q31_R6_S2_RING) == DEC_Q31_SECOND_ARENA,
               "/6 spends the whole budget");
_Static_assert((DEC_Q31_SECOND_ARENA + DEC_Q31_PRE_RING) == DEC_Q31_HIST_PER_CH,
               "the arena is exactly the two parts");
#if ASRC_DECIMATOR_HAS_96_TO_48
/* The pre-stage's reserve, re-derived from the batch bound rather than trusted --
 * a narrowed reserve must fail here instead of quietly going back to one kernel
 * call per output, and a reserve shorter than its tap count is not a ring at all. */
_Static_assert(DEC_Q31_PRE_RING >= DEC_Q31_PRE_TAPS, "96 -> 48 kHz pre-stage ring");
_Static_assert(DEC_Q31_PRE_RING >=
                   (DEC_Q31_PRE_TAPS + ((DEC_Q31_PRE_OUT - 1u) * DEC_Q31_PRE_DECIM)),
               "pre-stage ring must span a whole block's batch");
_Static_assert(DEC_Q31_PRE_OUT <= DEC_Q31_MAX_BATCH,
               "the pre-stage batch must fit the per-channel kernel scratch");
/* The pre-stage feeds an intermediate buffer, so a block's worth of its output has
 * to fit one.  This is the bound process() re-checks at runtime before the run. */
_Static_assert(DEC_Q31_PRE_OUT <= DEC_Q31_MAX_MID,
               "the pre-stage's block output must fit the inter-stage buffer");
_Static_assert(DEC_Q31_MAX_STAGES >= 3u,
               "a composed 96 kHz chain is pre + two cascade stages");
/* The wide variants' ring, at offset 0, against the WHOLE arena -- which is theirs to spend
 * because init() refuses to compose one.  Fail here rather than let a wider variant walk off the
 * end of s_q31_hist[] with no other symptom than wrong audio. */
_Static_assert(DEC_Q31_PRE_RING_WIDE <= DEC_Q31_HIST_PER_CH,
               "a wide pre-stage variant must fit the whole per-channel history arena");
_Static_assert(DEC_Q31_PRE_RING_WIDE >= DEC_Q31_PRE_RING,
               "TAPS_MAX must be the widest variant, so the wide ring cannot be the narrow one");
_Static_assert(DEC_Q31_PRE_TAPS_MAX <= DEC_Q31_COEFF_MAX,
               "X coefficient buffer too small for the widest pre-stage variant (it runs alone, "
               "so it needs no room for a second stage's coefficients)");
_Static_assert(DEC_Q31_MID_BUFFERS >= 2u,
               "a three-stage chain needs two intermediate buffers to ping-pong");
#endif
_Static_assert(ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS <= DEC_Q31_R6_S1_RING, "/6 stage 1 ring");
_Static_assert(ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS <= DEC_Q31_R6_S2_RING, "/6 stage 2 ring");
/* Re-derive both rings from the batch bound, so a narrowed ring fails here
 * instead of quietly going back to one kernel call per output. */
_Static_assert(DEC_Q31_R6_S1_RING >=
                   (ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS +
                    ((DEC_Q31_R6_S1_OUT - 1u) * 3u)),
               "/6 stage 1 ring must span a whole block's batch");
_Static_assert(DEC_Q31_R6_S2_RING >=
                   (ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS +
                    ((DEC_Q31_R6_S2_OUT - 1u) * 2u)),
               "/6 stage 2 ring must span a whole block's batch");
_Static_assert(DEC_Q31_R6_S1_OUT <= DEC_Q31_MAX_MID,
               "/6 stage-1 batch must fit the inter-stage buffer");
_Static_assert((DEC_Q31_R6_S1_OUT <= DEC_Q31_MAX_BATCH) &&
                   (DEC_Q31_R6_S2_OUT <= DEC_Q31_MAX_BATCH),
               "/6 batches must fit the per-channel kernel scratch");
_Static_assert(ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS <= 43u, "/4 stage 1 ring");
_Static_assert(ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS <= 147u, "/4 stage 2 ring");
/* The deepest chain in the file, coefficients end to end: the 96 kHz pre-stage in
 * front of /6's two stages.  DEC_Q31_PRE_TAPS is 0 without a 96 kHz leg, so this
 * is byte-for-byte the check it was for a 48 kHz build. */
_Static_assert((DEC_Q31_PRE_TAPS +
                ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS +
                ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS) <= DEC_Q31_COEFF_MAX,
               "X coefficient buffer too small for the deepest chain");
/* The 2/3 rational chain.  Each of these re-derives a constant above from the
 * block size, so DEC_Q31_R23_* cannot be edited into agreement with a wrong
 * ring: change DEC_Q31_R23_MAX_IN and the arithmetic here is what fails. */
_Static_assert(DEC_Q31_R23_MAX_OUT ==
                   ((((DEC_Q31_R23_MAX_IN - 1u) / ASRC_DECIMATOR_48_TO_32_M) +
                     ((DEC_Q31_R23_MAX_IN - 2u) / ASRC_DECIMATOR_48_TO_32_M)) + 2u),
               "2/3 worst-case output count is floor((n-1)/3)+floor((n-2)/3)+2");
/* Same count as the PUBLIC macro the audio path sizes its scratch buffer with, so the front
 * end and its caller can never disagree about how many frames a 16-frame block yields. */
_Static_assert(DEC_Q31_R23_MAX_OUT ==
                   ASRC_DECIMATOR_48_TO_32_OUT_FRAMES(DEC_Q31_R23_MAX_IN),
               "front end and asrc_decimator_48_to_8.h must agree on the 2/3 frame count");
_Static_assert(DEC_Q31_R23_MAX_ROW_BATCH == ((DEC_Q31_R23_MAX_OUT + 1u) / 2u),
               "the two rows alternate, so one row takes at most ceil(outputs/2)");
_Static_assert(DEC_Q31_R23_MAX_ROW_BATCH <= DEC_Q31_MAX_BATCH,
               "a row batch must fit the per-channel kernel scratch");
_Static_assert(ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS >= ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS,
               "phase 0 is assumed to be the longer row by the ring bounds below");
_Static_assert(DEC_Q31_R23_RING >=
                   (DEC_Q31_R23_MAX_IN + ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS - 1u),
               "2/3 ring must span a whole block plus the longest row's window");
_Static_assert(DEC_Q31_R23_RING >=
                   (ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS +
                    ((DEC_Q31_R23_MAX_ROW_BATCH - 1u) * ASRC_DECIMATOR_48_TO_32_M)),
               "2/3 ring must span the widest single-row batch");
_Static_assert(DEC_Q31_R23_RING <= DEC_Q31_SECOND_ARENA,
               "2/3 ring must fit the 48 kHz chains' history arena");
_Static_assert((ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS +
                ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS) <= DEC_Q31_COEFF_MAX,
               "X coefficient buffer too small for both 2/3 phase rows");
_Static_assert(DEC_Q31_MAX_STAGES >= 2u, "the 2/3 chain keeps one descriptor per row");
#if ASRC_DECIMATOR_HAS_96_TO_48
/* THE COMPOSED RATIONAL CHAIN, 96 -> 32 kHz = /2 then 2/3 (2026-09-03).  It is the
 * only chain in the file that is a cascade AND a pair of phase rows, so it is the
 * only one whose bounds are not covered by either group of asserts above.
 *
 * The rows see the PRE-STAGE's output, not the block, so DEC_Q31_R23_MAX_IN (a
 * whole 48 kHz block) is not the binding input here -- DEC_Q31_PRE_OUT is, and it
 * has to be inside the domain the ring and the schedule walk are valid over. */
_Static_assert(DEC_Q31_PRE_OUT <= DEC_Q31_R23_MAX_IN,
               "the pre-stage's block output must fit the 2/3 chain's input domain");
/* Three descriptors: the pre-stage plus both rows.  DEC_Q31_MAX_STAGES is 2 + the
 * pre-stage, so this holds by construction -- and says so, because the rational arm
 * writes stage sbase+1 and an off-by-one here is an out-of-bounds write. */
_Static_assert(DEC_Q31_MAX_STAGES >= (2u + DEC_Q31_PRE_STAGES),
               "a composed rational chain is the pre-stage plus both phase rows");
/* Coefficients end to end for that chain.  Shorter than the /6 case above, so it
 * cannot be the binding one -- stated anyway, because it is the arm that writes at
 * cbase + PHASE0_TAPS and nothing else checks that sum. */
_Static_assert((DEC_Q31_PRE_TAPS +
                ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS +
                ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS) <= DEC_Q31_COEFF_MAX,
               "X coefficient buffer too small for the composed 2/3 chain");
/* The two rings live in the SAME per-channel history, the rows at offset 0 and the
 * pre-stage's reserve at DEC_Q31_SECOND_ARENA, and both are live in one block.  If
 * the rational ring ever grew past the arena they would overlap with no symptom but
 * wrong audio; the arena bound above says it does not, and this says why it matters. */
_Static_assert((DEC_Q31_R23_RING <= DEC_Q31_SECOND_ARENA) &&
                   ((DEC_Q31_SECOND_ARENA + DEC_Q31_PRE_RING) <= DEC_Q31_HIST_PER_CH),
               "composed 2/3: the rows' ring and the pre-stage reserve must not overlap");
#endif
_Static_assert(ASRC_DEC_Q31_CH >= 2u, "front end computes at least the real pair");
/* A GEOMETRY limit of this kernel, and it is an #error on purpose: a width this front end cannot
 * carry must stop the build, never be accommodated.
 *
 * The accommodation to watch for is turning the front end OFF (APP_ASRC_RUNTIME_48K_TO_8 = 0) so a
 * non-power-of-two width will compile.  That trades a build error for silent removal of the
 * anti-alias stage, and every down-conversion then runs with no band limit but the resampler's own
 * ASRC_POLY_FC -- audible aliasing, no counter moving, nothing in the log.  Anti-aliasing is a
 * condition for down-conversion to be correct at all, not an audio-quality option, so:
 *
 *     width unsupported by this geometry  ->  BUILD ERROR   (widen the geometry, or pick a width)
 *     width unsupported by this geometry  --X-->  front end off
 *
 * STILL UNGATED (known, 2026-09-02): nothing here can see that a build with the front end off has
 * rates selectable that NEED it, because leg rates are runtime.  Expressing that needs a gate over
 * the rate menu rather than the width, so it is deliberately left as a documented hole rather than
 * a half-check that would pass the cases it cannot see.
 *
 * WIDENED 2026-09-06 (bench 12 ch study): what the mask actually constrains is the SOURCE width,
 * not the logical one.  Repeating a narrower source into ASRC_CH needs a power-of-two source
 * count; a source that is already full width repeats nothing and only needs a mask that leaves
 * every index below ASRC_CH alone, which ASRC_DEC_Q31_FULL_MASK is for any width up to 16.  So
 * the assertion below states that identity property instead of "ASRC_CH is a power of two", and
 * the source count is checked where it is actually known -- at runtime, in process().  Nothing
 * about the work narrowed: all ASRC_CH channels are still filtered, by the same stages. */
_Static_assert((ASRC_DEC_Q31_CH - 1u) <= ASRC_DEC_Q31_FULL_MASK,
               "ASRC_DEC_Q31_FULL_MASK must leave every channel index below ASRC_DEC_Q31_CH "
               "unchanged -- widen the geometry or choose another width; do NOT disable the "
               "front end to make a width compile");

/* Rounded float -> Q31.  Runs in init(), never on the audio path.  Every
 * coefficient of a unity-gain decimation lowpass is well inside +/-1, so the
 * clamps guard a future table rather than an expected path. */
static int32_t dec_q31_from_float(float c)
{
    const float v = c * 2147483648.0f;
    if (v >= 2147483647.0f)  { return (int32_t)0x7FFFFFFF; }
    if (v <= -2147483648.0f) { return (int32_t)0x80000000; }
    return (int32_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/*
 * Q31 -> s24-left.  This is a MASK, and that is the whole saturation story:
 * sacr.l has already saturated the 40-bit accumulator into Q31, so the value
 * cannot exceed the s24-left range once its eight sub-LSBs are dropped.
 * Masking them off is arithmetically identical to (v >> 8) << 8 for negatives
 * too (the shift is arithmetic), i.e. it truncates toward -inf -- matching
 * float_to_s24_left() above, which also truncates.
 */
static inline int32_t dec_q31_to_s24_left(int32_t v)
{
    return (int32_t)((uint32_t)v & 0xFFFFFF00u);
}

static void dec_q31_load_coeff(uint16_t off, const float *src, uint16_t taps)
{
    for (uint16_t i = 0u; i < taps; ++i)
    {
        s_q31_coeff[off + i] = dec_q31_from_float(src[i]);
    }
}

#if APP_ASRC_Q31_PRE_HALFBAND
/*
 * WHERE TAP k OF A HALF-BAND FILTER LIVES IN THE PACKED ARRAY, and whether it lives
 * there at all.  Length taps = 4*half - 1 puts the centre at M = 2*half - 1, which is
 * ODD, and a half-band prototype is exactly zero at every EVEN offset from its centre.
 * So the survivors are the even ABSOLUTE indices plus M, packed ascending:
 *
 *     k even, k < M   ->  k/2            (h[0], h[2], ... h[M-1])
 *     k == M          ->  half           (the centre)
 *     k even, k > M   ->  k/2 + 1        (h[M+1], h[M+3], ... h[taps-1])
 *     k odd,  k != M  ->  no entry; the tap is zero BY STRUCTURE
 *
 * This is the one place the mapping is written down, and both the coefficient packing
 * and the selftest oracle go through it -- an oracle with its own copy of the rule
 * would agree with a wrong kernel as readily as with a right one.
 *
 * It is a STRUCTURAL rule, never a test for a small coefficient: nothing here inspects
 * a value.  The generator asserts that the taps this drops really are zero (largest
 * dropped magnitude 1.8e-17 against a smallest kept 2.6e-06) so that a cutoff which
 * drifted off fs/4 fails at build time rather than quietly losing real taps.
 */
static bool dec_q31_hb_coeff_index(uint16_t k, uint16_t half, uint16_t *idx)
{
    const uint16_t m = (uint16_t)((2u * half) - 1u);
    if (k == m)            { *idx = half; return true; }
    if ((k & 1u) != 0u)    { return false; }
    *idx = (uint16_t)((k < m) ? (k / 2u) : ((k / 2u) + 1u));
    return true;
}

static void dec_q31_stage_set_hb(uint8_t s, uint16_t taps, uint16_t half,
                                 uint8_t decim, uint16_t ring, uint16_t ring_off,
                                 uint16_t coeff_off, const float *coeff)
{
    s_q31.stage[s].taps      = taps;   /* the WINDOW is still 4*half-1 long */
    s_q31.stage[s].decim     = decim;
    s_q31.stage[s].ring      = ring;
    s_q31.stage[s].ring_off  = ring_off;
    s_q31.stage[s].coeff_off = coeff_off;
    s_q31.stage[s].hb_half   = half;
    s_q31.wr[s]    = 0u;
    s_q31.phase[s] = 0u;
    for (uint16_t k = 0u; k < taps; ++k)
    {
        uint16_t idx = 0u;
        if (dec_q31_hb_coeff_index(k, half, &idx))
        {
            s_q31_coeff[coeff_off + idx] = dec_q31_from_float(coeff[k]);
        }
    }
}
#endif

static void dec_q31_stage_set(uint8_t s, uint16_t taps, uint8_t decim,
                              uint16_t ring, uint16_t ring_off,
                              uint16_t coeff_off, const float *coeff)
{
    s_q31.stage[s].taps      = taps;
    s_q31.stage[s].decim     = decim;
    s_q31.stage[s].ring      = ring;
    s_q31.stage[s].ring_off  = ring_off;
    s_q31.stage[s].coeff_off = coeff_off;
#if APP_ASRC_Q31_PRE_HALFBAND
    s_q31.stage[s].hb_half   = 0u;   /* dense unless dec_q31_stage_set_hb() said so */
#endif
    s_q31.wr[s]    = 0u;
    s_q31.phase[s] = 0u;
    dec_q31_load_coeff(coeff_off, coeff, taps);
}

#if APP_ASRC_THIRDBAND_96_TO_32
/* "The chain this front end was asked for IS the composed 96 -> 32 kHz one" -- resolved
 * once here rather than re-tested per block.  Declared above init() because init() fills
 * it; the third band's own include sits further down, after init(), so that the block path
 * below can dispatch to it. */
static uint8_t s_tb32_eligible;
#endif

/*
 * THE ARM PROLOGUE -- one definition, so that an entry point cannot be added without it.
 *
 * Every front end this file can arm starts from a defined state: the descriptors, wr/phase,
 * and the WHOLE per-channel history arena, zeroed.  The history matters as much as the
 * fields do, because the first block after an arm reads a window wider than what has been
 * pushed into it -- taps - 1 samples of that window are history -- and the third band's
 * retire path depends on this clear outright, retiring being nothing but arming the composed
 * chain over the overlay the third band left in the same arena.
 *
 * It is here, and NOT inside dec_q31_stage_set(), because it is a property of ARMING A CHAIN
 * rather than of configuring a stage.  A cascade calls stage_set once per stage over regions
 * of one arena, and the two phase ROWS of the 2/3 chain share a single ring; per-stage
 * clearing would re-clear shared regions and make the arm's cost depend on how the stages
 * were split.  Cost is not the point either way -- this runs at arm time, off the audio path
 * -- but where the invariant belongs is.
 */
#if APP_ASRC_LPF1TO1_PROTOTYPE
/*
 * PER-STAGE output frame counters -- the composed chain's version of `fe exec:`.
 *
 * `fe exec:` counts blocks the FRONT END ran, which for a single-stage chain is also "the filter
 * ran".  A composition of two stages can run its first and skip its second in exactly the way
 * the ISR gate skipped the whole 1:1 stage on 2026-09-14 while `fe=lp1` kept printing, and one
 * front-end counter cannot tell those apart.  So the evidence is per stage, and it is FRAMES
 * PRODUCED rather than calls: a /2 first stage and a 1:1 second stage must report the SAME
 * number, and either reading 0 says which half never ran.
 *
 * Under the prototype gate: this is measurement, and measurement is opt-in.  One 32-bit add per
 * stage per block, off the MAC loop.
 */
static uint32_t s_dq31_stage_out[DEC_Q31_MAX_STAGES];

uint32_t asrc_decimator_q31_stage_output_frames(uint8_t stage)
{
    return (stage < (uint8_t)DEC_Q31_MAX_STAGES) ? s_dq31_stage_out[stage] : 0u;
}
#endif

static void dec_q31_arm_begin(uint8_t channels)
{
    memset(&s_q31, 0, sizeof(s_q31));
    memset(s_q31_hist, 0, sizeof(s_q31_hist));
#if APP_ASRC_LPF1TO1_PROTOTYPE
    /* Restart with the chain, for the reason the front-end run counters restart: a count carried
     * across an arm would let the PREVIOUS chain's stages satisfy a check about this one. */
    memset(s_dq31_stage_out, 0, sizeof(s_dq31_stage_out));
#endif
    s_q31.channels = channels;
#if APP_ASRC_THIRDBAND_96_TO_32
    s_tb32_eligible = 0u;
#endif
}

/*
 * THE ARM INVARIANT, MEASURED ON THE PATH THAT ACTUALLY ARMS.
 *
 * The boot selftests already prove that a missing clear is DETECTABLE: with the arena memset
 * above removed, the divider check fails on its first pass (`/2 case 1 live 1 blk 0 out 0 ch 0
 * got 1073739520 want 0`, measured 2026-09-14).  What they cannot prove is that the RUNTIME
 * path leaves a zero history, because they call the init entry points directly, whereas a
 * console `*ar` rate change arms through audio_transport's reset_stream_state() ->
 * asrc_transport_reset() -> asrc_audio_path_reset() -> path_decimator_init().  This function is
 * how that path states the invariant as a number instead of an inspection: the caller runs it
 * where it publishes the chain as ready, which is the moment the claim is about.
 *
 * Cost is one pass over the arena (ASRC_DEC_Q31_CH * DEC_Q31_HIST_PER_CH words) at arm time --
 * transport stopped, codec muted, nothing waiting on it.  Nothing on the audio path calls this.
 */
uint16_t asrc_decimator_q31_history_nonzero_words(void)
{
    const int32_t* const p = &s_q31_hist[0][0];
    const uint32_t       n = (uint32_t)(sizeof(s_q31_hist) / sizeof(int32_t));
    uint16_t             dirty = 0u;
    for (uint32_t i = 0u; i < n; ++i)
    {
        if (p[i] != 0) { ++dirty; }
    }
    return dirty;
}

#if APP_ASRC_FRONTEND_SELFTEST
/*
 * THE DETECTOR PROVES ITSELF, then proves the clear -- in four steps, because each of them is a
 * different way for this check to be worthless: an arena that was already empty (nothing was
 * cleared), a scan that cannot see a dirty arena, a scan hardwired to 0 (indistinguishable from a
 * clean arm in a log), and a clear that leaves the poke behind.
 *
 * BOOT ONLY, like every other check in this family: it writes the live arena, so running it
 * while a chain is armed would race the block ISR (the 1:1 check's header records what that
 * costs).  It leaves the arena zeroed and the descriptors cleared -- the caller's chain
 * selection re-arms immediately afterwards.
 */
static void dec_q31_arm_clear_poke(void)
{
    /* Both ENDS of the arena: a scan that walked the first channel only would pass a single
     * poke at index 0. */
    s_q31_hist[0][0] = (int32_t)0x7FFFFFFF;
    s_q31_hist[(uint8_t)ASRC_DEC_Q31_CH - 1u][DEC_Q31_HIST_PER_CH - 1u] = (int32_t)-1;
}

bool asrc_decimator_q31_arm_clear_selftest(uint16_t* out_seen)
{
    /* STEP 1 -- there is something to clear, and the scan sees it.
     *
     * The arena is genuinely dirty when this runs: the checks before it drove the real front end,
     * so a real history is sitting in it (1,023 words, measured 2026-09-14 -- the first version of
     * this function assumed an empty arena, poked two words, and was told `seen 1025`, which is
     * the detector catching the test author).  Real leftover history is a better dirty input than
     * a synthetic poke, so it is used as one; if a future reordering leaves the arena empty here,
     * poke instead rather than fail, because an ordering change is not a defect in the arm. */
    uint16_t before = asrc_decimator_q31_history_nonzero_words();
    if (before == 0u)
    {
        dec_q31_arm_clear_poke();
        before = asrc_decimator_q31_history_nonzero_words();
        if (before != 2u) { if (out_seen != NULL) { *out_seen = before; } return false; }
    }
    if (out_seen != NULL) { *out_seen = before; }

    /* STEP 2 -- an arm clears it.  This is the property the whole exercise is about. */
    dec_q31_arm_begin((uint8_t)ASRC_DEC_Q31_CH);
    if (asrc_decimator_q31_history_nonzero_words() != 0u) { return false; }

    /* STEP 3 -- the scan COUNTS, rather than merely returning 0.  Without this step a checker
     * hardwired to 0 and a clean arm are the same line in a log. */
    dec_q31_arm_clear_poke();
    if (asrc_decimator_q31_history_nonzero_words() != 2u) { return false; }

    /* STEP 4 -- and it clears the poke too, so the caller is handed the state it expects. */
    dec_q31_arm_begin((uint8_t)ASRC_DEC_Q31_CH);
    return (asrc_decimator_q31_history_nonzero_words() == 0u);
}
#endif

#if APP_ASRC_LPF1TO1_PROTOTYPE
#include "asrc_lpf1to1_coeffs.inc"

/*
 * A 1:1 STAGE, and why it needs no new kernel.
 *
 * `decim` is a plain runtime field of dec_q31_stage_t.  dec_q31_stage_run() uses it as
 *   need   = decim - phase          -> 1, so the "absorb without producing" branch is dead
 *   b      = 1 + (avail - need)/decim -> avail, i.e. one output per input frame
 *   span   = taps + (b-1)*decim
 *   bmax   = 1 + (ring - taps)/decim
 * and the assembler kernel receives it only as `decim_bytes` (w5) in
 *   add.l  w10, w5, w10
 * followed by the manual ring fold.  decim == 1 is therefore an ordinary value of the same
 * arithmetic, not a special case: nothing anywhere divides by (decim - 1), tests decim >= 2, or
 * assumes the phase ever advances.  Phase stays 0 across blocks, which is correct -- a 1:1 stage
 * has no decimation phase to carry.  Continuity comes from the ring and wr[], exactly as it does
 * for every other stage.
 *
 * WHAT IS NEW is not arithmetic but ROUTING and SIZING: asrc_decimator_q31_init() refuses
 * den == 1 (rightly -- the pre-stage's shared set alone protects nothing between 24 and 48 kHz),
 * and the caller's output scratch was sized for the smallest ratio being 1/2.  So this is a
 * separate entry point rather than a new `den` case: it shares every line of the run path and
 * none of the divider dispatch.
 *
 * `s_q31.num` MUST stay 1.  process() dispatches the rational chain on
 * `num == ASRC_DECIMATOR_48_TO_32_L`, and with num == 1 and stages == 1 this falls into the
 * single-stage arm, which is the generic one that already does exactly the right thing --
 * including `last = true`, so the s24-left mask is applied on the way out.
 */
bool asrc_decimator_q31_init_lpf1to1(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                     uint8_t channels)
{
    if ((channels == 0u) || (channels > ASRC_DEC_Q31_CH))
    {
        return false;
    }

    uint16_t     taps  = 0u;
    const float *coeff = NULL;

    /* One coefficient set per (source rate, output rate) pair, selected by BOTH -- the source
     * rate alone cannot choose, because the stopband edge is Fs_out - Fp and Fp is derived from
     * Fs_out.  A pair with no set is refused here rather than served by a neighbour's band
     * edges, which is the failure the /4 :11k vs :12k tag exists to make visible. */
    if ((in_rate_hz == 32000u) && (out_rate_hz == 16000u))
    {
        taps  = ASRC_LP1_32K_TO_16K_TAPS;
        coeff = s_lp1_32k_to_16k_coeff;
    }
    else if ((in_rate_hz == 22050u) && (out_rate_hz == 16000u))
    {
        taps  = ASRC_LP1_22K05_TO_16K_TAPS;
        coeff = s_lp1_22k05_to_16k_coeff;
    }
#if APP_ASRC_ANTIALIAS_DIRECT_1TO1
#define ASRC_ANTIALIAS_DIRECT_1TO1_ARM(FS_IN, FS_OUT, TAPS, COEFF, TAG) \
    else if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT)))      \
    {                                                                   \
        taps  = (TAPS);                                                 \
        coeff = (COEFF);                                                \
    }
    ASRC_ANTIALIAS_DIRECT_1TO1_ROWS(ASRC_ANTIALIAS_DIRECT_1TO1_ARM)
#undef ASRC_ANTIALIAS_DIRECT_1TO1_ARM
#endif
#if APP_ASRC_UNIVERSAL_PROFILE
    /* THE UNIVERSAL PROFILE'S 1:1-ONLY ROWS, expanded from the one list (see
     * ASRC_UNIVERSAL_1TO1_ROWS in asrc_decimator_48_to_8.h).  Four rows where the image has a
     * 96 kHz leg, three where it has not -- the count _Static_asserts below name both.  The pairs
     * are those whose uniform
     * fold edge is already above the SOURCE Nyquist's protected band, so nothing has to be
     * decimated first and the whole front end is one band-limit at the source rate.
     *
     * ADDED AS AN ARM OF THIS SAME CHAIN, not as a fourth entry point: the rows differ from the
     * three above only in which (source, output) pair selects them.  96 kHz is a new SOURCE rate
     * for a 1:1 stage, and that is a routing fact (see the pre-stage note in
     * path_decimator_init()), not an arithmetic one -- the ring, the batch and the kernel are
     * sized from the block and the taps, neither of which knows the rate. */
#define ASRC_UNIVERSAL_1TO1_ARM(FS_IN, FS_OUT, TAPS, COEFF, TAG)  \
    else if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT))) \
    {                                                              \
        taps  = (TAPS);                                            \
        coeff = (COEFF);                                           \
    }
    ASRC_UNIVERSAL_1TO1_ROWS(ASRC_UNIVERSAL_1TO1_ARM)
#undef ASRC_UNIVERSAL_1TO1_ARM
#endif
    else
    {
        return false;
    }

    /* The ring has to hold the window of the LAST output of a full batch: taps + (b-1)*1.  b is
     * capped by DEC_Q31_MAX_BATCH, so this is the largest window any call can ask for.  Checked
     * at runtime as well as by the static asserts below, because the tap counts come from a
     * generated include and a regenerated table must fail closed, not overrun the arena. */
    const uint16_t ring = (uint16_t)(taps + (DEC_Q31_MAX_BATCH - 1u));
    if (ring > DEC_Q31_SECOND_ARENA)
    {
        return false;
    }
    if (taps > DEC_Q31_COEFF_MAX)
    {
        return false;
    }

    /*
     * THE SHARED ARM PROLOGUE, and the correction of what this entry point first said about it.
     *
     * The first version of this arm hand-rolled a subset: the s_q31 fields it knew about, plus
     * a loop zeroing [0, ring) of each channel's history.  That is exactly the window the
     * stage reads, so it was correct -- but correct by inspection, and written on a diagnosis
     * that was wrong.  The selftest's got = 161359360 against want = 0 was NOT evidence that
     * "arming does not clear the ring" is a general property of this front end: every other
     * arm path in the ASRC front end has always cleared its own state -- init() zeroes the
     * descriptors and the WHOLE arena for every divider, rational and composed chain, the
     * third band's arm zeroes its overlay, the float front end memsets its state struct, and
     * asrc_reset() zeroes the rear engine's FIFO.  What was missing was one entry point
     * standing outside the prologue.  It is now inside it, so a 1:1 chain that later needs a
     * ring_off other than 0, or a second stage, cannot re-open the hole.
     *
     * ring_off 0 is the 48 kHz chains' arena base, free here by construction: a 1:1 chain has
     * no pre-stage and no second stage, so nothing else is using the arena.
     */
    dec_q31_arm_begin(channels);
    s_q31.num    = 1u;
    s_q31.den    = 1u;
    s_q31.stages = 1u;
    dec_q31_stage_set(0u, taps, 1u, ring, 0u, 0u, coeff);
    return true;
}

_Static_assert((ASRC_LP1_32K_TO_16K_TAPS + DEC_Q31_MAX_BATCH - 1u) <= DEC_Q31_SECOND_ARENA,
               "32 kHz 1:1 LPF ring does not fit the 48 kHz chains' arena");
_Static_assert((ASRC_LP1_22K05_TO_16K_TAPS + DEC_Q31_MAX_BATCH - 1u) <= DEC_Q31_SECOND_ARENA,
               "22.05 kHz 1:1 LPF ring does not fit the 48 kHz chains' arena");
_Static_assert((ASRC_LP1_16K_TO_11K025_TAPS + DEC_Q31_MAX_BATCH - 1u) <= DEC_Q31_SECOND_ARENA,
               "16 kHz 1:1 LPF ring does not fit the 48 kHz chains' arena");
_Static_assert(ASRC_LP1_32K_TO_16K_TAPS <= DEC_Q31_COEFF_MAX, "1:1 LPF coefficient arena");

#if APP_ASRC_ANTIALIAS_DIRECT_1TO1
#define ASRC_ANTIALIAS_DIRECT_1TO1_ASSERT(FS_IN, FS_OUT, TAPS, COEFF, TAG)        \
    _Static_assert(((TAPS) + DEC_Q31_MAX_BATCH - 1u) <= DEC_Q31_SECOND_ARENA,   \
                   "direct anti-alias 1:1 row " TAG ": ring does not fit");   \
    _Static_assert((TAPS) <= DEC_Q31_COEFF_MAX,                                 \
                   "direct anti-alias 1:1 row " TAG ": coefficient arena");
ASRC_ANTIALIAS_DIRECT_1TO1_ROWS(ASRC_ANTIALIAS_DIRECT_1TO1_ASSERT)
#undef ASRC_ANTIALIAS_DIRECT_1TO1_ASSERT

/* The DIRECT5 composed rows: the four inequalities the composed arm applies at runtime, derived from
 * the block as for the Universal rows -- two of them decimate by 3 (out0 = 6, not 8). */
#define ASRC_DIRECT5_OUT0(DECIM)  (((uint16_t)APP_BLOCK_FRAMES + ((DECIM) - 1u)) / (DECIM))
#define ASRC_ANTIALIAS_DIRECT5_COMPOSED_ASSERT(FS_IN, FS_OUT, DEN, DECIM, TAPS0, C0, TAPS1, C1, TAG) \
    _Static_assert((DEN) == (DECIM),                                                          \
                   "direct composed row " TAG ": published ratio and decimation disagree");   \
    _Static_assert(ASRC_DIRECT5_OUT0(DECIM) <= DEC_Q31_MAX_MID,                               \
                   "direct composed row " TAG ": stage-0 output does not fit");               \
    _Static_assert(ASRC_DIRECT5_OUT0(DECIM) <= DEC_Q31_MAX_BATCH,                             \
                   "direct composed row " TAG ": stage 0 cannot clear a block in one call");  \
    _Static_assert(((TAPS0) + ((ASRC_DIRECT5_OUT0(DECIM) - 1u) * (DECIM))                     \
                    + (TAPS1) + (ASRC_DIRECT5_OUT0(DECIM) - 1u)) <= DEC_Q31_SECOND_ARENA,     \
                   "direct composed row " TAG ": the two rings do not fit the arena");         \
    _Static_assert(((TAPS0) + (TAPS1)) <= DEC_Q31_COEFF_MAX,                                  \
                   "direct composed row " TAG ": coefficient arena");
ASRC_ANTIALIAS_DIRECT5_COMPOSED_ROWS(ASRC_ANTIALIAS_DIRECT5_COMPOSED_ASSERT)
#undef ASRC_ANTIALIAS_DIRECT5_COMPOSED_ASSERT
#undef ASRC_DIRECT5_OUT0

const char* asrc_antialias_direct_1to1_tag(uint32_t out_rate_hz, uint32_t in_rate_hz)
{
#define ASRC_ANTIALIAS_DIRECT_1TO1_TAG(FS_IN, FS_OUT, TAPS, COEFF, TAG) \
    if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT))) { return (TAG); }
    ASRC_ANTIALIAS_DIRECT_1TO1_ROWS(ASRC_ANTIALIAS_DIRECT_1TO1_TAG)
#undef ASRC_ANTIALIAS_DIRECT_1TO1_TAG
    return NULL;
}

const char* asrc_antialias_direct_composed_tag(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                               uint32_t den)
{
#define ASRC_ANTIALIAS_DIRECT5_COMPOSED_TAG(FS_IN, FS_OUT, DEN, DECIM, TAPS0, C0, TAPS1, C1, TAG) \
    if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT)) && (den == (DEN))) { return (TAG); }
    ASRC_ANTIALIAS_DIRECT5_COMPOSED_ROWS(ASRC_ANTIALIAS_DIRECT5_COMPOSED_TAG)
#undef ASRC_ANTIALIAS_DIRECT5_COMPOSED_TAG
    (void)out_rate_hz; (void)in_rate_hz; (void)den;
    return NULL;
}
#endif

#if APP_ASRC_UNIVERSAL_PROFILE
/* EVERY Universal 1:1 row, at compile time, from the same list the arm above expands.  The
 * runtime checks in the arm stay -- they are what makes a regenerated table fail CLOSED on a
 * bench build -- but a row that cannot fit must break the BUILD, and a row ADDED to the list gets
 * its assert automatically rather than by someone remembering to write one. */
#define ASRC_UNIVERSAL_1TO1_ASSERT(FS_IN, FS_OUT, TAPS, COEFF, TAG)               \
    _Static_assert(((TAPS) + DEC_Q31_MAX_BATCH - 1u) <= DEC_Q31_SECOND_ARENA,     \
                   "Universal 1:1 row " TAG ": ring does not fit the arena");     \
    _Static_assert((TAPS) <= DEC_Q31_COEFF_MAX,                                   \
                   "Universal 1:1 row " TAG ": coefficient arena");
ASRC_UNIVERSAL_1TO1_ROWS(ASRC_UNIVERSAL_1TO1_ASSERT)
#undef ASRC_UNIVERSAL_1TO1_ASSERT

const char* asrc_universal_1to1_tag(uint32_t out_rate_hz, uint32_t in_rate_hz)
{
#define ASRC_UNIVERSAL_1TO1_TAG(FS_IN, FS_OUT, TAPS, COEFF, TAG)  \
    if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT))) { return (TAG); }
    ASRC_UNIVERSAL_1TO1_ROWS(ASRC_UNIVERSAL_1TO1_TAG)
#undef ASRC_UNIVERSAL_1TO1_TAG
    return NULL;
}
#endif /* APP_ASRC_UNIVERSAL_PROFILE */

/*
 * A COMPOSED chain: an integer decimating stage, then the 1:1 band-limit at the intermediate
 * rate.  44.1 -> /2 -> 22.05 kHz -> 1:1 -> rear ASRC at step 1.378125.
 *
 * WHY THIS IS NOT A NEW KERNEL, A NEW CASCADE, OR EVEN A NEW SHAPE.  The generic integer cascade
 * in dec_q31_process() already runs N stages, writes the intermediates through dec_q31_mid_buf()
 * and applies the s24-left mask on the last one; /4, /6 and /12 are exactly this with two
 * decimating stages.  All that is new is that the LAST stage has decim == 1 -- an ordinary value
 * of the same arithmetic (see the header of asrc_decimator_q31_init_lpf1to1()).  So this entry
 * point only SELECTS coefficients and lays out the arena; it adds no branch to the block path.
 *
 * WHY IT IS A THIRD ENTRY POINT rather than a `den == 2` case of asrc_decimator_q31_init():
 * that function's /2 case is the SHIPPED 48 kHz chain, whose set is chosen by a variant enum and
 * whose ring spends the whole arena.  This chain shares neither: its coefficients are generated
 * against the composition (asrc_lpf1to1_design.py's PRESTAGES, whose stopband is relaxed by the
 * stage behind it), and its arena is split in two.  Folding it into the divider dispatch would
 * make "which /2 is this" a question with three answers.
 *
 * SIZING IS DERIVED FROM THE BLOCK, not chosen.  The first stage must clear a whole block in one
 * kernel call or it pays the ~70-cycle setup per output instead of per block, so it needs
 * ceil(APP_BLOCK_FRAMES / d) outputs from one call; the second stage then needs that many from
 * its own.  Both rings follow, and both are checked against the arena at compile time below and
 * at runtime here -- the tap counts come from a generated include, and a regenerated table must
 * fail closed rather than overrun the arena.
 */
bool asrc_decimator_q31_init_dec_then_lpf1to1(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                              uint32_t den, uint8_t channels)
{
    if ((channels == 0u) || (channels > ASRC_DEC_Q31_CH))
    {
        return false;
    }

    uint16_t     taps0 = 0u, taps1 = 0u;
    const float *c0 = NULL, *c1 = NULL;
    uint8_t      d0 = 0u;

    /* One row per composed chain, selected by BOTH rates AND the published ratio: the ratio is
     * what the rest of the front end and the rear step were computed from, so a mismatch here is
     * a planner bug and must be refused rather than served by a chain with a different ratio. */
    if ((in_rate_hz == 44100u) && (out_rate_hz == 16000u) &&
        (den == ASRC_DEC2_44K1_FOR_16K_DECIM))
    {
        d0    = (uint8_t)ASRC_DEC2_44K1_FOR_16K_DECIM;
        taps0 = ASRC_DEC2_44K1_FOR_16K_TAPS;
        c0    = s_dec2_44k1_for_16k_coeff;
        taps1 = ASRC_LP1_22K05_TO_16K_TAPS;
        c1    = s_lp1_22k05_to_16k_coeff;
    }
#if APP_ASRC_ANTIALIAS_22K05_TO_8K
    /*
     * 22.05 -> 8 kHz, AND IT NAMES THE ROW ABOVE'S TWO ARRAYS ON PURPOSE.
     *
     * This is the one row in this function whose coefficient sets are named for rates that are not
     * its own, so the reason is here rather than left to be rediscovered as a bug:
     * 22050/8000 = 44100/16000 = 2.75625 EXACTLY, and the uniform spec derives both band edges
     * from the output rate alone (passband 0.8125 * FN_out, the 1:1 stage's stopband
     * Fs_out - passband).  Every stage therefore has the same NORMALISED passband and stopband as
     * it does in the 44.1 -> 16 kHz row, and a Kaiser design depends on nothing else -- so these
     * two arrays ARE this pair's correct coefficients, at half the clock.
     *
     * Verified rather than argued: re-deriving both stages at the 22.05 kHz clock reproduces both
     * shipped arrays to 1.0e-10 relative, and the shipped arrays evaluated as a 22.05 -> 8 kHz
     * chain read -114.38 dBc -- the identical figure already on record for 44.1 -> 16 kHz.
     * [internal] study_antialias_frontend_feasibility_20260920.md sections 3.1 and 4,
     * host check [internal tool] antialias_frontend_reuse_check.py.
     *
     * WHY NOT A COPY OF THE ARRAYS UNDER 8 kHz NAMES: two arrays with identical contents and
     * different names is the duplicate that drifts -- a regenerated 44.1 -> 16 kHz set would
     * silently stop matching this pair.  Sharing them makes that impossible.  The tap-count
     * macros are shared for the same reason, so the asserts below are the 44.1 kHz row's asserts
     * over again and cannot disagree with it.
     */
    else if ((in_rate_hz == 22050u) && (out_rate_hz == 8000u) &&
             (den == ASRC_DEC2_44K1_FOR_16K_DECIM))
    {
        d0    = (uint8_t)ASRC_DEC2_44K1_FOR_16K_DECIM;
        taps0 = ASRC_DEC2_44K1_FOR_16K_TAPS;
        c0    = s_dec2_44k1_for_16k_coeff;
        taps1 = ASRC_LP1_22K05_TO_16K_TAPS;
        c1    = s_lp1_22k05_to_16k_coeff;
    }
#endif
#if APP_ASRC_ANTIALIAS_32K_TO_11K025
    /*
     * 32 -> 11.025 kHz: a NEW pre-stage with a SHIPPED partner.
     *
     * s_dec2_32k_for_11k025_coeff (45 taps) is this pair's own set, generated by
     * tools/asrc/asrc_lpf1to1_design.py's PRESTAGES row ("dec2_32k_for_11k025", 32000, 2, 11025).
     * s_lp1_16k_to_11k025_coeff (55 taps) is NOT new -- it already ships as the second stage of the
     * 48 -> 11.025 kHz composed row, and it is shared rather than copied for the same reason the
     * 22.05 kHz row shares its two: two arrays with identical contents and different names is the
     * duplicate that drifts.
     *
     * The generator gates the pre-stage against the WHOLE chain including that partner, so the two
     * are one specification and must be regenerated together -- which is why the pre-stage lives in
     * asrc_lpf1to1_coeffs.inc beside the partner and not with the /2 sets in
     * asrc_decimator_48_to_8_design.py.  Chain figure: -108.53 dBc, passband 0.0003 dB, Q31.
     */
    else if ((in_rate_hz == 32000u) && (out_rate_hz == 11025u) &&
             (den == ASRC_DEC2_32K_FOR_11K025_DECIM))
    {
        d0    = (uint8_t)ASRC_DEC2_32K_FOR_11K025_DECIM;
        taps0 = ASRC_DEC2_32K_FOR_11K025_TAPS;
        c0    = s_dec2_32k_for_11k025_coeff;
        taps1 = ASRC_LP1_16K_TO_11K025_TAPS;
        c1    = s_lp1_16k_to_11k025_coeff;
    }
#endif
#if APP_ASRC_ANTIALIAS_44K1_TO_12K
    /*
     * 44.1 -> 12 kHz: BOTH sets new (s_dec2_44k1_for_12k_coeff N31, s_lp1_22k05_to_12k_coeff N71),
     * because nothing in the image protects 12 kHz from a 22.05 kHz stage rate.  The /3 alternative
     * (dec3_44k1_for_12k + lp1_14k7_to_12k) is generated but not armed -- see the gate's comment for
     * why /2 was chosen; naming it here would admit two routes for one pair.
     */
    else if ((in_rate_hz == 44100u) && (out_rate_hz == 12000u) &&
             (den == ASRC_DEC2_44K1_FOR_12K_DECIM))
    {
        d0    = (uint8_t)ASRC_DEC2_44K1_FOR_12K_DECIM;
        taps0 = ASRC_DEC2_44K1_FOR_12K_TAPS;
        c0    = s_dec2_44k1_for_12k_coeff;
        taps1 = ASRC_LP1_22K05_TO_12K_TAPS;
        c1    = s_lp1_22k05_to_12k_coeff;
    }
#endif
#if APP_ASRC_ANTIALIAS_44K1_TO_8K
    /*
     * 44.1 -> 8 kHz: a /4 pre-stage, the first d0 == 4 row in this function.
     *
     * d0 IS A RUNTIME VALUE and every bound below is derived from it -- out0, ring0, the batch test
     * -- so /4 is an ordinary value of the same arithmetic rather than a new case.  What IS new for a
     * /4 is that out0 drops to 4 (block 16 / 4), which makes the intermediate buffer test the
     * LOOSEST of any row here while the pre-stage's own ring is the largest; both are asserted below
     * rather than reasoned about.
     *
     * The /3 alternative (dec3_44k1_for_8k + lp1_14k7_to_8k) is generated and not armed: /4 keeps
     * more margin on the FILL axis, which is the axis that refused this pair.
     */
    else if ((in_rate_hz == 44100u) && (out_rate_hz == 8000u) &&
             (den == ASRC_DEC4_44K1_FOR_8K_DECIM))
    {
        d0    = (uint8_t)ASRC_DEC4_44K1_FOR_8K_DECIM;
        taps0 = ASRC_DEC4_44K1_FOR_8K_TAPS;
        c0    = s_dec4_44k1_for_8k_coeff;
        taps1 = ASRC_LP1_11K025_TO_8K_TAPS;
        c1    = s_lp1_11k025_to_8k_coeff;
    }
#endif
#if APP_ASRC_UNIVERSAL_PROFILE
    /* THE UNIVERSAL PROFILE'S 11 COMPOSED ROWS, from the one list.
     *
     * ★ SEVEN OF THEM DECIMATE BY 3, WHICH NO SHIPPED ROW DOES.  Every shipped 96 kHz route
     * reaches its 48 kHz chain through the /2 pre-stage; these drop straight to 32 kHz (or, from a
     * 48 kHz leg, to 16 kHz) in one /3 stage.  That is NOT a new kernel -- dec_q31_stage_set()
     * takes the decimation as a runtime value and the block kernel takes it as decim_bytes -- but
     * it IS a new stage rate for the block ARITHMETIC: out0 = ceil(BLOCK/3) = 6 where every
     * existing composed row computes 8, and both rings move with it.  So the asserts below
     * re-derive out0, ring0 and ring1 per row from the same expression the runtime uses, rather
     * than from a /2 assumption written once.
     *
     * The published ratio is checked against the coefficient set's own decimation, not assumed
     * equal to it -- see the note on the argument list. */
#define ASRC_UNIVERSAL_COMPOSED_ARM(FS_IN, FS_OUT, DEN, DECIM, TAPS0, C0, TAPS1, C1, TAG) \
    else if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT)) && (den == (DEN)))       \
    {                                                                                     \
        d0    = (uint8_t)(DECIM);                                                          \
        taps0 = (TAPS0);                                                                   \
        c0    = (C0);                                                                      \
        taps1 = (TAPS1);                                                                   \
        c1    = (C1);                                                                      \
    }
    ASRC_UNIVERSAL_COMPOSED_ROWS(ASRC_UNIVERSAL_COMPOSED_ARM)
#undef ASRC_UNIVERSAL_COMPOSED_ARM
#endif
#if APP_ASRC_ANTIALIAS_DIRECT_1TO1
    /* The DIRECT5 composed rows (asrc_decimator_48_to_8.h), same shape and same ratio key as the
     * Universal ones; the list is empty unless APP_ASRC_ANTIALIAS_DIRECT5. */
#define ASRC_ANTIALIAS_DIRECT5_COMPOSED_ARM(FS_IN, FS_OUT, DEN, DECIM, TAPS0, C0, TAPS1, C1, TAG) \
    else if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT)) && (den == (DEN)))           \
    {                                                                                         \
        d0    = (uint8_t)(DECIM);                                                              \
        taps0 = (TAPS0);                                                                       \
        c0    = (C0);                                                                          \
        taps1 = (TAPS1);                                                                       \
        c1    = (C1);                                                                          \
    }
    ASRC_ANTIALIAS_DIRECT5_COMPOSED_ROWS(ASRC_ANTIALIAS_DIRECT5_COMPOSED_ARM)
#undef ASRC_ANTIALIAS_DIRECT5_COMPOSED_ARM
#endif
    else
    {
        return false;
    }

    const uint16_t out0  = (uint16_t)(((uint16_t)APP_BLOCK_FRAMES + (d0 - 1u)) / d0);
    const uint16_t ring0 = (uint16_t)(taps0 + ((out0 - 1u) * d0));
    const uint16_t ring1 = (uint16_t)(taps1 + (out0 - 1u));

    if (out0 > DEC_Q31_MAX_MID)                        { return false; }
    if (out0 > DEC_Q31_MAX_BATCH)                      { return false; }
    if ((uint32_t)ring0 + (uint32_t)ring1 > DEC_Q31_SECOND_ARENA) { return false; }
    if ((uint32_t)taps0 + (uint32_t)taps1 > DEC_Q31_COEFF_MAX)    { return false; }

    dec_q31_arm_begin(channels);
    s_q31.num    = 1u;
    s_q31.den    = (uint8_t)den;
    s_q31.stages = 2u;
    dec_q31_stage_set(0u, taps0, d0,        ring0, 0u,    0u,    c0);
    dec_q31_stage_set(1u, taps1, 1u,        ring1, ring0, taps0, c1);
    return true;
}

/* The 44.1 kHz row, at compile time.  Written out rather than expressed through the runtime
 * locals above so that a regenerated table breaks the BUILD, not an arm on the bench. */
#define DEC_Q31_C44K1_OUT0 (((uint16_t)APP_BLOCK_FRAMES + (ASRC_DEC2_44K1_FOR_16K_DECIM - 1u)) \
                            / ASRC_DEC2_44K1_FOR_16K_DECIM)
_Static_assert(DEC_Q31_C44K1_OUT0 <= DEC_Q31_MAX_MID,
               "44.1 -> 16 kHz: the /2 stage's block output does not fit an intermediate buffer");
_Static_assert(DEC_Q31_C44K1_OUT0 <= DEC_Q31_MAX_BATCH,
               "44.1 -> 16 kHz: the /2 stage cannot clear a block in one kernel call");
_Static_assert((ASRC_DEC2_44K1_FOR_16K_TAPS
                + ((DEC_Q31_C44K1_OUT0 - 1u) * ASRC_DEC2_44K1_FOR_16K_DECIM)
                + ASRC_LP1_22K05_TO_16K_TAPS + (DEC_Q31_C44K1_OUT0 - 1u))
                   <= DEC_Q31_SECOND_ARENA,
               "44.1 -> 16 kHz composed chain: the two rings do not fit the 48 kHz chains' arena");
_Static_assert((ASRC_DEC2_44K1_FOR_16K_TAPS + ASRC_LP1_22K05_TO_16K_TAPS) <= DEC_Q31_COEFF_MAX,
               "44.1 -> 16 kHz composed chain: coefficient arena");
_Static_assert(DEC_Q31_MAX_STAGES >= 2u, "44.1 -> 16 kHz composed chain needs two stages");

#if APP_ASRC_ANTIALIAS_22K05_TO_8K
/*
 * 22.05 -> 8 kHz, at compile time.  The row SHARES the 44.1 kHz row's two coefficient sets and
 * its decimation, so every quantity above is already the right one and re-checking it here would
 * be the same four inequalities a second time -- which is why this block asserts the ONE thing
 * that is genuinely this row's own: that it really is sharing them.
 *
 * If a future regeneration gave 22.05 -> 8 kHz its own sets, these would stop being the correct
 * asserts and the arm would need its own -- so tie the sharing itself to the build.  ASRC_CH and
 * the arena are unchanged by definition: nothing about this row is larger than the 44.1 kHz row.
 */
_Static_assert(ASRC_DEC2_44K1_FOR_16K_DECIM == 2u,
               "22.05 -> 8 kHz reuses the 44.1 -> 16 kHz chain because the two pairs have the "
               "same ratio (2.75625) and the uniform spec derives both band edges from the output "
               "rate alone.  That argument is about a /2 pre-stage; a different decimation here "
               "means the shared set is no longer this pair's set");
#endif

#if APP_ASRC_ANTIALIAS_32K_TO_11K025
/*
 * 32 -> 11.025 kHz, at compile time.  This row has its OWN pre-stage, so unlike the 22.05 kHz row
 * it needs the full four inequalities rather than a sharing assert -- re-derived here from
 * APP_BLOCK_FRAMES and the generated tap counts, exactly as the runtime derives them, so a
 * regenerated table breaks the BUILD and not an arm on the bench.
 */
#define DEC_Q31_C32K11K_OUT0 (((uint16_t)APP_BLOCK_FRAMES + (ASRC_DEC2_32K_FOR_11K025_DECIM - 1u))                               / ASRC_DEC2_32K_FOR_11K025_DECIM)
_Static_assert(DEC_Q31_C32K11K_OUT0 <= DEC_Q31_MAX_MID,
               "32 -> 11.025 kHz: the /2 stage's block output does not fit an intermediate buffer");
_Static_assert(DEC_Q31_C32K11K_OUT0 <= DEC_Q31_MAX_BATCH,
               "32 -> 11.025 kHz: the /2 stage cannot clear a block in one kernel call");
_Static_assert((ASRC_DEC2_32K_FOR_11K025_TAPS
                + ((DEC_Q31_C32K11K_OUT0 - 1u) * ASRC_DEC2_32K_FOR_11K025_DECIM)
                + ASRC_LP1_16K_TO_11K025_TAPS + (DEC_Q31_C32K11K_OUT0 - 1u))
                   <= DEC_Q31_SECOND_ARENA,
               "32 -> 11.025 kHz composed chain: the two rings do not fit the 48 kHz chains' arena");
_Static_assert((ASRC_DEC2_32K_FOR_11K025_TAPS + ASRC_LP1_16K_TO_11K025_TAPS) <= DEC_Q31_COEFF_MAX,
               "32 -> 11.025 kHz composed chain: coefficient arena");
_Static_assert(DEC_Q31_MAX_STAGES >= 2u, "32 -> 11.025 kHz composed chain needs two stages");
#endif

#if APP_ASRC_ANTIALIAS_44K1_TO_12K
/* 44.1 -> 12 kHz (/2 to 22.05 kHz), the four inequalities the runtime applies, re-derived here from
 * APP_BLOCK_FRAMES and the generated tap counts so a regenerated table breaks the BUILD. */
#define DEC_Q31_C44K12K_OUT0 (((uint16_t)APP_BLOCK_FRAMES + (ASRC_DEC2_44K1_FOR_12K_DECIM - 1u))                               / ASRC_DEC2_44K1_FOR_12K_DECIM)
_Static_assert(DEC_Q31_C44K12K_OUT0 <= DEC_Q31_MAX_MID,
               "44.1 -> 12 kHz: the /2 stage's block output does not fit an intermediate buffer");
_Static_assert(DEC_Q31_C44K12K_OUT0 <= DEC_Q31_MAX_BATCH,
               "44.1 -> 12 kHz: the /2 stage cannot clear a block in one kernel call");
_Static_assert((ASRC_DEC2_44K1_FOR_12K_TAPS
                + ((DEC_Q31_C44K12K_OUT0 - 1u) * ASRC_DEC2_44K1_FOR_12K_DECIM)
                + ASRC_LP1_22K05_TO_12K_TAPS + (DEC_Q31_C44K12K_OUT0 - 1u))
                   <= DEC_Q31_SECOND_ARENA,
               "44.1 -> 12 kHz composed chain: the two rings do not fit the 48 kHz chains' arena");
_Static_assert((ASRC_DEC2_44K1_FOR_12K_TAPS + ASRC_LP1_22K05_TO_12K_TAPS) <= DEC_Q31_COEFF_MAX,
               "44.1 -> 12 kHz composed chain: coefficient arena");
_Static_assert(DEC_Q31_MAX_STAGES >= 2u, "44.1 -> 12 kHz composed chain needs two stages");
#endif

#if APP_ASRC_ANTIALIAS_44K1_TO_8K
/* 44.1 -> 8 kHz (/4 to 11.025 kHz).  The SAME four inequalities, and they are worth having here in
 * particular: this is the first d0 == 4 row, so out0 and ring0 both move away from every value the
 * other rows exercise -- out0 falls to 4 while the pre-stage ring grows to taps + 3*(out0-1). */
#define DEC_Q31_C44K8K_OUT0 (((uint16_t)APP_BLOCK_FRAMES + (ASRC_DEC4_44K1_FOR_8K_DECIM - 1u))                              / ASRC_DEC4_44K1_FOR_8K_DECIM)
_Static_assert(DEC_Q31_C44K8K_OUT0 <= DEC_Q31_MAX_MID,
               "44.1 -> 8 kHz: the /4 stage's block output does not fit an intermediate buffer");
_Static_assert(DEC_Q31_C44K8K_OUT0 <= DEC_Q31_MAX_BATCH,
               "44.1 -> 8 kHz: the /4 stage cannot clear a block in one kernel call");
_Static_assert((ASRC_DEC4_44K1_FOR_8K_TAPS
                + ((DEC_Q31_C44K8K_OUT0 - 1u) * ASRC_DEC4_44K1_FOR_8K_DECIM)
                + ASRC_LP1_11K025_TO_8K_TAPS + (DEC_Q31_C44K8K_OUT0 - 1u))
                   <= DEC_Q31_SECOND_ARENA,
               "44.1 -> 8 kHz composed chain: the two rings do not fit the 48 kHz chains' arena");
_Static_assert((ASRC_DEC4_44K1_FOR_8K_TAPS + ASRC_LP1_11K025_TO_8K_TAPS) <= DEC_Q31_COEFF_MAX,
               "44.1 -> 8 kHz composed chain: coefficient arena");
_Static_assert(DEC_Q31_MAX_STAGES >= 2u, "44.1 -> 8 kHz composed chain needs two stages");
#endif

#if APP_ASRC_UNIVERSAL_PROFILE
/*
 * EVERY Universal composed row, at compile time, from the same list the arm expands -- and derived
 * from the BLOCK, exactly as the runtime derives it, so a changed APP_BLOCK_FRAMES moves both or
 * neither.  Four inequalities per row, and each one is a different failure:
 *
 *   out0 <= DEC_Q31_MAX_MID     the intermediate buffer holds one block of stage-0 output.
 *                               Overflowing it is a WRONG SIGNAL, not a refusal.
 *   out0 <= DEC_Q31_MAX_BATCH   stage 0 clears the block in ONE kernel call.  Failing this is
 *                               only a cost (~70 cycles per output instead of per block), but it
 *                               is a cost nobody would find by listening.
 *   ring0 + ring1 <= arena      the two rings are live in the same block and must not overlap.
 *                               The tightest row is 96 -> 8 kHz at 44 + 156 = 200 of 209.
 *   taps0 + taps1 <= coeff arena  the X working buffer holds the live chain end to end.
 *
 * ★ The /3 rows are what these asserts exist for.  At BLOCK 16 a /3 stage emits 6 frames and a /2
 * stage 8, so ring0 and ring1 differ between the two shapes by (out0 - 1) * d -- and the widest
 * 1:1 set in this profile (151 taps, 96 -> 8 kHz) sits behind a /3.  Nothing about that row is
 * visible in the /2-shaped arithmetic the pre-Universal rows are sized by.
 */
#define ASRC_UNIVERSAL_OUT0(DECIM)  (((uint16_t)APP_BLOCK_FRAMES + ((DECIM) - 1u)) / (DECIM))
#define ASRC_UNIVERSAL_COMPOSED_ASSERT(FS_IN, FS_OUT, DEN, DECIM, TAPS0, C0, TAPS1, C1, TAG)  \
    _Static_assert((DEN) == (DECIM),                                                          \
                   "Universal composed row " TAG ": the published ratio and the coefficient "  \
                   "set's own decimation disagree");                                          \
    _Static_assert(ASRC_UNIVERSAL_OUT0(DECIM) <= DEC_Q31_MAX_MID,                             \
                   "Universal composed row " TAG ": stage-0 block output does not fit an "      \
                   "intermediate buffer");                                                    \
    _Static_assert(ASRC_UNIVERSAL_OUT0(DECIM) <= DEC_Q31_MAX_BATCH,                           \
                   "Universal composed row " TAG ": stage 0 cannot clear a block in one "       \
                   "kernel call");                                                            \
    _Static_assert(((TAPS0) + ((ASRC_UNIVERSAL_OUT0(DECIM) - 1u) * (DECIM))                   \
                    + (TAPS1) + (ASRC_UNIVERSAL_OUT0(DECIM) - 1u)) <= DEC_Q31_SECOND_ARENA,   \
                   "Universal composed row " TAG ": the two rings do not fit the arena");      \
    _Static_assert(((TAPS0) + (TAPS1)) <= DEC_Q31_COEFF_MAX,                                  \
                   "Universal composed row " TAG ": coefficient arena");
ASRC_UNIVERSAL_COMPOSED_ROWS(ASRC_UNIVERSAL_COMPOSED_ASSERT)
#undef ASRC_UNIVERSAL_COMPOSED_ASSERT

/*
 * HOW MANY ROWS THIS IMAGE ACTUALLY CARRIES, as a number the build checks.
 *
 * The two lists are each JOINED from a 48 kHz half and a 96 kHz half (see the header), so a
 * mistake in the join -- a missing backslash, a half left out of one arm of the #if -- silently
 * drops rows.  Everything else here would still pass: the per-row asserts only speak about the
 * rows they are given, the arm compiles, and the profile then quietly serves eleven pairs from
 * the shipped table while claiming fifteen.  That failure has no symptom in a build log, which
 * is why the count is an invariant and not a comment.
 *
 * Phase H qualified 15 rows.  A build with a 96 kHz leg carries all of them; one without carries
 * the 7 that a 48 kHz leg can reach, because the other 8 name a source rate the image cannot
 * produce (see the split's own note).
 */
#define ASRC_UNIVERSAL_COUNT_ROW(...) + 1u
#define ASRC_UNIVERSAL_1TO1_COUNT      (0u ASRC_UNIVERSAL_1TO1_ROWS(ASRC_UNIVERSAL_COUNT_ROW))
#define ASRC_UNIVERSAL_COMPOSED_COUNT  (0u ASRC_UNIVERSAL_COMPOSED_ROWS(ASRC_UNIVERSAL_COUNT_ROW))
#if ASRC_DECIMATOR_HAS_96_TO_48
_Static_assert(ASRC_UNIVERSAL_1TO1_COUNT == 4u,
               "Universal profile with a 96 kHz leg must carry 4 one-stage rows "
               "(48->22.05, 48->24, 48->32, 96->48 kHz) -- check the row-list join");
_Static_assert(ASRC_UNIVERSAL_COMPOSED_COUNT == 11u,
               "Universal profile with a 96 kHz leg must carry 11 composed rows -- "
               "check the row-list join");
#else
_Static_assert(ASRC_UNIVERSAL_1TO1_COUNT == 3u,
               "Universal profile without a 96 kHz leg must carry 3 one-stage rows "
               "(48->22.05, 48->24, 48->32 kHz) -- check the row-list join");
_Static_assert(ASRC_UNIVERSAL_COMPOSED_COUNT == 4u,
               "Universal profile without a 96 kHz leg must carry 4 composed rows "
               "(48->8, 48->11.025, 48->12, 48->16 kHz) -- check the row-list join");
#endif
#undef ASRC_UNIVERSAL_COUNT_ROW

const char* asrc_universal_composed_tag(uint32_t out_rate_hz, uint32_t in_rate_hz, uint32_t den)
{
#define ASRC_UNIVERSAL_COMPOSED_TAG(FS_IN, FS_OUT, DEN, DECIM, TAPS0, C0, TAPS1, C1, TAG)  \
    if ((in_rate_hz == (FS_IN)) && (out_rate_hz == (FS_OUT)) && (den == (DEN)))            \
    {                                                                                      \
        return (TAG);                                                                      \
    }
    ASRC_UNIVERSAL_COMPOSED_ROWS(ASRC_UNIVERSAL_COMPOSED_TAG)
#undef ASRC_UNIVERSAL_COMPOSED_TAG
    return NULL;
}
#endif /* APP_ASRC_UNIVERSAL_PROFILE */
#endif /* APP_ASRC_LPF1TO1_PROTOTYPE */

bool asrc_decimator_q31_init(uint32_t num, uint32_t den, uint8_t channels,
                             asrc_decimator_48_to_24_variant_t v24,
                             asrc_decimator_48_to_12_variant_t v12,
#if ASRC_DECIMATOR_HAS_96_TO_48
                             asrc_decimator_96_to_48_variant_t vpre,
#endif
                             bool with_prestage)
{
    if ((channels == 0u) || (channels > ASRC_DEC_Q31_CH))
    {
        return false;
    }

    dec_q31_arm_begin(channels);
    s_q31.num      = (uint8_t)num;
    s_q31.den      = (uint8_t)den;

#if APP_ASRC_THIRDBAND_96_TO_32
    /* The third band REPLACES the composed 96 -> 32 kHz chain and nothing else: /2 then
     * 2/3.  Resolved here, where the request is still in view, and NOT re-derived at arm
     * time from s_q31 -- a stage count would have to be interpreted, and this front end
     * already refuses to dispatch on stage counts for exactly that reason.
     *
     * Note where this leaves the ARENA: the arm prologue above has just cleared
     * s_q31_hist, so whatever the third band had there is gone.  That is deliberate and is what makes the A/B switch a
     * leg-A restart rather than a flag: the console re-applies the rate, this runs, and the
     * arm hook then builds whichever front end the mode now names. */
#if ASRC_DECIMATOR_HAS_96_TO_48
    s_tb32_eligible = ((num == ASRC_DECIMATOR_48_TO_32_L) &&
                       (den == ASRC_DECIMATOR_48_TO_32_M) && with_prestage) ? 1u : 0u;
#else
    s_tb32_eligible = 0u;
#endif
#endif /* APP_ASRC_THIRDBAND_96_TO_32 */

    /*
     * THE 96 kHz PRE-STAGE, when the caller asked for a composed chain.
     *
     * `num`/`den` describe the SECOND stage -- the 48 kHz-input chain -- and this
     * prepends a fixed /2 in front of it, so the whole composed ratio is den*2.
     * That split is deliberate: everything below stays a 48 kHz chain resolved by
     * the same divider and the same coefficient variants it always was, and the
     * pre-stage is one more cascade stage rather than a second code path.
     *
     * `sbase` and `cbase` are how: the 48 kHz chain's descriptors move up by one
     * stage and its coefficients move up by the pre-stage's tap count.  Both are 0
     * when there is no pre-stage, so a 48 kHz build resolves byte-identical state.
     *
     * WHICH SET the pre-stage loads is `vpre`, and it decides more than the taps:
     * the SHARED set is composed, so its ring sits in the reserve above the 48 kHz
     * chains' arena; a WIDE set is the whole chain, so its ring sits at offset 0
     * and spends that arena instead.  The two combinations that would overlap or
     * under-protect are refused below rather than resolved.
     */
    uint8_t  sbase = 0u;
    uint16_t cbase = 0u;
#if ASRC_DECIMATOR_HAS_96_TO_48
    if (with_prestage)
    {
        /* A RATIONAL CHAIN IS REACHABLE BEHIND THE PRE-STAGE (2026-09-03): /2 then
         * 2/3 is the composed 96 -> 32 kHz front end, and it is the only way that
         * rate exists at all.  Nothing special happens here for it -- the pre-stage
         * is resolved exactly as it is for an integer second stage, and `sbase` /
         * `cbase` below are what the rational arm honours.  What the row waited on
         * was the FILTER: the shared set had to widen 27 -> 41 taps to reach
         * -108.38 dB at 32 kHz's 32000 Hz fold edge (it read -47.95 dB before).
         * The refusal that used to stand here is now in the two places that really
         * cannot serve a rational request: the wide-variant arm just below, and the
         * float front end in asrc_audio_path.c. */
        uint16_t     pre_taps  = DEC_Q31_PRE_TAPS;
        uint16_t     pre_ring  = DEC_Q31_PRE_RING;
        uint16_t     pre_off   = DEC_Q31_SECOND_ARENA;
        const float *pre_coeff = s_96_to_48_coeff;
        if (vpre != ASRC_DECIMATOR_96_TO_48_SHARED)
        {
            /* A WIDE variant is the whole chain, by construction: it is designed to
             * protect a 44.1 or 48 kHz final band, and it is sized to live in the
             * 48 kHz chains' arena.  Composing one would both overlap those rings
             * and apply a stopband the second stage does not need.  Refuse -- and
             * test the RATIO, not den alone, so 2/1 cannot slip through den == 1. */
            if ((den != 1u) || (num != 1u))
            {
                return false;
            }
            if (vpre == ASRC_DECIMATOR_96_TO_48_FOR_44100)
            {
                pre_taps  = ASRC_DECIMATOR_96_TO_48_OUT44K1_TAPS;
                pre_coeff = s_96_to_48_out44k1_coeff;
            }
            else if (vpre == ASRC_DECIMATOR_96_TO_48_FOR_48000)
            {
                pre_taps  = ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS;
                pre_coeff = s_96_to_48_out48k_coeff;
            }
            else
            {
                return false;
            }
            /* Same batch bound the reserve is derived from, one whole block per call. */
            pre_ring = (uint16_t)(pre_taps +
                                  ((DEC_Q31_PRE_OUT - 1u) * DEC_Q31_PRE_DECIM));
            pre_off  = 0u;
        }
        else if (den == 1u)
        {
            /* SHARED alone would be 96 -> 48 kHz with a 32 kHz stopband, i.e. a
             * chain that protects nothing between 24 and 48 kHz -- which is the
             * whole reason those two rows waited for a wide variant.  Refuse rather
             * than route a 48 kHz output through a filter designed for 24 kHz. */
            return false;
        }
#if APP_ASRC_Q31_PRE_HALFBAND
        /*
         * MEASUREMENT BUILD: the SHARED composed pre-stage runs the 35-tap HALF-BAND set
         * through the sparse kernel instead of the 41-tap set through the dense one.  Only
         * this arm is swapped -- a WIDE variant is the whole chain and has no half-band
         * design, and den == 1 was already refused above.
         *
         * EVERYTHING AROUND IT IS DELIBERATELY LEFT ALONE so that the two images differ in
         * the kernel and nothing else:
         *   - the ring stays DEC_Q31_PRE_RING (55 samples), not the 49 that 35 taps would
         *     justify, so the Y history is byte-identical and nothing moves in the arena;
         *   - cbase stays DEC_Q31_PRE_TAPS (41), not the 19 the packed set occupies, so the
         *     second stage reads its coefficients from the SAME addresses in both builds.
         * Both cost a few unused words and buy a comparison that is about one variable.
         *
         * The batch bound moves on its own: bmax = 1 + (55-35)/2 = 11 against 8, and b
         * stays 8 because a block is 16 input frames.  So the call count per block does not
         * change either.
         */
        if (vpre == ASRC_DECIMATOR_96_TO_48_SHARED)
        {
            dec_q31_stage_set_hb(0u, ASRC_DECIMATOR_96_TO_48_HB_COEFF_TAPS,
                                 ASRC_DECIMATOR_96_TO_48_HB_COEFF_HALF,
                                 (uint8_t)DEC_Q31_PRE_DECIM, pre_ring, pre_off, 0u,
                                 s_96_to_48_hb_coeff);
        }
        else
        {
            dec_q31_stage_set(0u, pre_taps, (uint8_t)DEC_Q31_PRE_DECIM,
                              pre_ring, pre_off, 0u, pre_coeff);
        }
#else
        dec_q31_stage_set(0u, pre_taps, (uint8_t)DEC_Q31_PRE_DECIM,
                          pre_ring, pre_off, 0u, pre_coeff);
#endif
        sbase = 1u;
        cbase = pre_taps;
        if (den == 1u)
        {
            /* 96 -> 48 kHz with nothing behind it: the pre-stage IS the chain, and
             * the resampler then runs at step 1.00000 (48 kHz) or 1.08844 (44.1). */
            s_q31.stages = 1u;
            return true;
        }
    }
#else
    if (with_prestage)
    {
        /* This build compiled no pre-stage coefficients, so it cannot serve a
         * composed chain.  Fail closed rather than run the second stage alone at
         * twice its intended input rate. */
        return false;
    }
#endif

    /* The rational chain is selected by the RATIO, before den is looked at: 2/3
     * and 1/3 both have den == 3 and are different filters (32 kHz output against
     * 16 kHz output).  Anything with num != 1 that is not exactly 2/3 is refused
     * rather than approximated. */
    if (num == ASRC_DECIMATOR_48_TO_32_L)
    {
        if (den != ASRC_DECIMATOR_48_TO_32_M)
        {
            return false;
        }
        /* Two PHASE ROWS over ONE shared ring, not a cascade: both descriptors
         * carry ring_off 0 and decim M, and only s_q31.wr[r23_base] is used.
         *
         * `sbase`/`cbase` are honoured here for the same reason every integer arm
         * below honours them -- a composed 96 kHz chain puts the pre-stage at
         * stage 0, so the rows are stages 1 and 2 and their coefficients sit above
         * the pre-stage's.  ring_off stays 0 for both rows either way: the rational
         * ring lives at the BOTTOM of the arena and the pre-stage's reserve at the
         * top, so the two never overlap. */
        s_q31.stages   = (uint8_t)(sbase + 2u);
        s_q31.r23_base = sbase;
        dec_q31_stage_set(sbase, ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS,
                          ASRC_DECIMATOR_48_TO_32_M,
                          DEC_Q31_R23_RING, 0u, cbase, s_48_to_32_phase0_coeff);
        dec_q31_stage_set((uint8_t)(sbase + 1u),
                          ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS,
                          ASRC_DECIMATOR_48_TO_32_M,
                          DEC_Q31_R23_RING, 0u,
                          (uint16_t)(cbase + ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS),
                          s_48_to_32_phase1_coeff);
        /* k = 0 needs input floor(0) = 0, and nothing has been pushed, so the
         * first output is due as soon as one frame arrives. */
        s_q31.r23_d   = 0;
        s_q31.r23_par = 0u;
        return true;
    }
    if (num != 1u)
    {
        return false;
    }

    /* Every ring_off below is an offset into the 48 kHz chains' arena, which starts
     * at 0 whether or not a pre-stage exists (the reserve is at the TOP) -- so these
     * offsets are the ones this file has always used.  Only the STAGE index and the
     * coefficient offset shift, by sbase and cbase. */
    switch (den)
    {
    case 2u:
        s_q31.stages = (uint8_t)(sbase + 1u);
        dec_q31_stage_set(sbase, ASRC_DECIMATOR_48_TO_24_TAPS, 2u,
                          DEC_Q31_SECOND_ARENA, 0u, cbase,
                          (v24 == ASRC_DECIMATOR_48_TO_24_FOR_22050)
                              ? s_48_to_24_out22k_coeff : s_48_to_24_coeff);
        return true;

    case 3u:
        s_q31.stages = (uint8_t)(sbase + 1u);
        dec_q31_stage_set(sbase, ASRC_DECIMATOR_48_TO_16_TAPS, 3u,
                          DEC_Q31_SECOND_ARENA, 0u, cbase, s_48_to_16_coeff);
        return true;

    case 4u:
        s_q31.stages = (uint8_t)(sbase + 2u);
        if (v12 == ASRC_DECIMATOR_48_TO_12_FOR_12000)
        {
            dec_q31_stage_set(sbase, ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS, 2u,
                              43u, 0u, cbase, s_48_to_12_out12k_stage1_coeff);
            dec_q31_stage_set((uint8_t)(sbase + 1u),
                              ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS, 2u,
                              147u, 43u,
                              (uint16_t)(cbase + ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS),
                              s_48_to_12_out12k_stage2_coeff);
        }
        else
        {
            dec_q31_stage_set(sbase, ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS, 2u,
                              43u, 0u, cbase, s_48_to_12_stage1_coeff);
            dec_q31_stage_set((uint8_t)(sbase + 1u),
                              ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS, 2u,
                              147u, 43u,
                              (uint16_t)(cbase + ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS),
                              s_48_to_12_stage2_coeff);
        }
        return true;

    case 6u:
        s_q31.stages = (uint8_t)(sbase + 2u);
        dec_q31_stage_set(sbase, ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS, 3u,
                          DEC_Q31_R6_S1_RING, 0u, cbase, s_stage1_coeff);
        dec_q31_stage_set((uint8_t)(sbase + 1u),
                          ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS, 2u,
                          DEC_Q31_R6_S2_RING, DEC_Q31_R6_S1_RING,
                          (uint16_t)(cbase + ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS),
                          s_stage2_coeff);
        return true;

    default:
        return false;
    }
}

#if APP_ASRC_THIRDBAND_96_TO_32
/* The third-band front end.  Placed here -- after init(), before the block path -- because
 * it calls init() to retire itself and because process_s24_left() below dispatches to it. */
#include "asrc_thirdband_96_to_32.inc"
#endif

/*
 * Outputs the 2/3 chain will emit if `in_frames` more inputs arrive, at the
 * current schedule state.  Pure: this is also what output_frames() answers with,
 * so the all-or-nothing capacity check and the run can never disagree.
 */
static size_t dec_q31_r23_count(size_t in_frames)
{
    if (in_frames > DEC_Q31_R23_MAX_IN)
    {
        /* Outside the ring's domain -- the walk below would be valid arithmetic
         * for a schedule the ring cannot hold.  Report more frames than this
         * chain can ever emit so the capacity check refuses; process() refuses
         * the oversized block outright as well. */
        return (size_t)DEC_Q31_R23_MAX_OUT + 1u;
    }
    int16_t d = (int16_t)((int16_t)s_q31.r23_d - (int16_t)in_frames);
    uint8_t p = s_q31.r23_par;
    size_t  n = 0u;
    while (d <= -1)
    {
        ++n;
        d = (int16_t)(d + ((p == 0u) ? 1 : 2));
        p ^= 1u;
    }
    return n;
}

/*
 * Push `n` input frames into stage `s`'s ring, for every channel.
 *
 * BIT-EXACT restructuring of what used to be a frame-outer / channel-inner pair
 * of nested loops in each of the two runners below.  Same values, same
 * destinations, same ring state on exit; only the address arithmetic changed.
 *
 * Why it was worth changing: the old form recomputed `s_q31_hist[c]` -- a row
 * multiply by DEC_Q31_HIST_PER_CH -- once per (frame, channel), which at 12
 * channels x 16 frames is 192 multiplies per stage per block, and reloaded
 * s_q31.wr[s] from the state struct once per frame.  The measured non-kernel half
 * of one stage was 15.6 us at 12 ch
 * (`[internal] report_ak512_96k32k_12ch_legA_partition_2026-09-06.md` section 4.3)
 * and this loop is the bulk of it.
 *
 * Channel-outer walks both sides with a CONSTANT stride instead: the destination
 * by 1 inside the ring (wrapping at most once per lap) and the source by
 * in_stride.  The write pointer advances identically for every channel, so it is
 * derived once at the end rather than stored per frame.  `in` never aliases
 * s_q31_hist -- it is the caller's block or s_q31_mid -- so the channel-outer
 * order cannot change what any later channel reads.
 *
 * This does NOT skip a channel or a slot: every one of s_q31.channels channels is
 * written, including the ones the input repeat mask feeds from the same source
 * sample.  What was removed is address arithmetic, not work.
 */
static void dec_q31_hist_push(uint8_t s, uint16_t ring_off, uint16_t R,
                              const int32_t *in, size_t n, size_t in_stride,
                              uint8_t in_ch_mask)
{
    const uint8_t  nch = s_q31.channels;
    const uint16_t w0  = s_q31.wr[s];
    int32_t       *base = &s_q31_hist[0][ring_off];

    for (uint8_t c = 0u; c < nch; ++c)
    {
        const int32_t *src = in + (size_t)(c & in_ch_mask);
        int32_t       *dst = base + w0;
        int32_t *const end = base + R;

        for (size_t k = 0u; k < n; ++k)
        {
            *dst = *src;
            src += in_stride;
            ++dst;
            if (dst == end) { dst = base; }
        }
        base += DEC_Q31_HIST_PER_CH;
    }

    /* One modulo per stage per block, in place of a compare-and-branch per frame.
     * Written as a modulo rather than a subtract so it stays correct if a caller
     * ever pushes more than one lap; `n <= R` for every caller today. */
    s_q31.wr[s] = (uint16_t)(((uint32_t)w0 + (uint32_t)n) % (uint32_t)R);
}

/*
 * One block of the 2/3 rational chain.
 *
 * Push the whole block into the shared ring first, then issue ONE kernel call per
 * phase row per channel.  That ordering is why the ring is 64 and not 49: every
 * window the block will read has to still be there when the calls happen.  The
 * two rows' outputs interleave -- row `p0` owns the even output slots of this
 * block and the other row the odd ones -- so nothing has to be re-sorted after.
 */
static size_t dec_q31_r23_run(const int32_t *in, size_t in_frames,
                              size_t in_stride, uint8_t in_ch_mask,
                              int32_t *out, size_t out_stride)
{
    const uint8_t  nch = s_q31.channels;
    const uint16_t R   = DEC_Q31_R23_RING;
    /* The rows are stage rb and rb+1, and their shared write pointer is wr[rb].
     * rb is 0 on a 48 kHz leg and 1 behind the 96 kHz pre-stage; indexing 0 here
     * would share the PRE-STAGE's write pointer with the rows, which is not a
     * build error and not a mute -- it reads back the wrong window. */
    const uint8_t  rb  = s_q31.r23_base;

    /* ring_off is 0 for both phase rows -- they share one ring. */
    dec_q31_hist_push(rb, 0u, R, in, in_frames, in_stride, in_ch_mask);

    /* Walk the schedule once, recording per row how many outputs it owes and
     * where its FIRST window ends (as a distance behind the write pointer). */
    int16_t       d  = (int16_t)((int16_t)s_q31.r23_d - (int16_t)in_frames);
    const uint8_t p0 = s_q31.r23_par;
    uint8_t       p  = p0;
    uint16_t      rows[2]  = { 0u, 0u };
    int16_t       first[2] = { 0, 0 };
    size_t        produced = 0u;
    while (d <= -1)
    {
        if (rows[p] == 0u) { first[p] = d; }
        rows[p]++;
        ++produced;
        d = (int16_t)(d + ((p == 0u) ? 1 : 2));
        p ^= 1u;
    }
    s_q31.r23_d   = (int8_t)d;
    s_q31.r23_par = p;

    for (uint8_t r = 0u; r < 2u; ++r)
    {
        if (rows[r] == 0u)
        {
            continue;
        }
        const dec_q31_stage_t *const st = &s_q31.stage[rb + r];
        /* first[r] is negative: the row's first output has its newest sample
         * -first[r] frames behind the write pointer, and its window reaches
         * taps-1 further back still. */
        const uint16_t back  = (uint16_t)((uint16_t)(-first[r]) +
                                          (uint16_t)(st->taps - 1u));
        const uint16_t start = (uint16_t)(((uint32_t)s_q31.wr[rb] + R - back) % R);
        const int32_t *const coeff = &s_q31_coeff[st->coeff_off];
        /* Row r owns output slots (r ^ p0), (r ^ p0) + 2, ... of this block. */
        const size_t slot0 = (size_t)((r ^ p0) & 1u);

        /* Both walks are constant-stride, so neither the ring base nor the output
         * slot needs a multiply per channel or per output.  Successive outputs of
         * ONE row are two slots apart because the other row owns the slots in
         * between; the channel index is the fastest axis of `out`, hence ++dst. */
        int32_t     *ring    = &s_q31_hist[0][st->ring_off];
        int32_t     *dst     = &out[slot0 * out_stride];
        const size_t dst_hop = out_stride * 2u;

        for (uint8_t c = 0u; c < nch; ++c)
        {
            int32_t tmp[DEC_Q31_R23_MAX_ROW_BATCH];

            (void)fir_ring_q31_ymod_yonly_block(coeff, ring + start, st->taps,
                                                tmp, (uint32_t)rows[r],
                                                (uint32_t)st->decim * 4u,
                                                ring, (uint32_t)R * 4u);
            {
                int32_t *o = dst;
                for (size_t j = 0u; j < (size_t)rows[r]; ++j)
                {
                    *o = dec_q31_to_s24_left(tmp[j]);
                    o += dst_hop;
                }
            }
            ring += DEC_Q31_HIST_PER_CH;
            ++dst;
        }
    }
    return produced;
}

size_t asrc_decimator_q31_output_frames(size_t input_frames)
{
    if (s_q31.stages == 0u)
    {
        return 0u;
    }
#if APP_ASRC_THIRDBAND_96_TO_32
    /* Armed means the third band IS the front end, and its ratio is a plain 3:1 with a
     * 0..2 frame remainder.  Answering with the composed chain's count here would make the
     * caller's all-or-nothing capacity check disagree with what actually runs. */
    if (s_tb32_armed != 0u)
    {
        return asrc_thirdband_96_to_32_output_frames(input_frames);
    }
#endif
    if (s_q31.num == ASRC_DECIMATOR_48_TO_32_L)
    {
        /* Any cascade stage AHEAD of the phase rows runs first, so what the rows
         * see is its output, not the block.  Composed 96 -> 32 kHz is /2 then 2/3:
         * counting the block here would over-predict by exactly the pre-stage's
         * decimation, and process() would then refuse a capacity that was in fact
         * sufficient -- a silent mute, not an error. */
        size_t pre = input_frames;
        for (uint8_t s = 0u; s < s_q31.r23_base; ++s)
        {
            pre = ((size_t)s_q31.phase[s] + pre) / s_q31.stage[s].decim;
        }
        return dec_q31_r23_count(pre);
    }
    /* One term per cascade stage, in order.  Written as a loop rather than as the
     * two unrolled lines it used to be because a composed 96 kHz chain has three
     * stages -- and because the loop cannot fall out of step with s_q31.stages the
     * way a hand-unrolled pair silently did for the third. */
    size_t n = input_frames;
    for (uint8_t s = 0u; s < s_q31.stages; ++s)
    {
        n = ((size_t)s_q31.phase[s] + n) / s_q31.stage[s].decim;
    }
    return n;
}

/*
 * One stage.  `out` receives full-precision Q31 when this is not the last
 * stage; the last stage masks to s24-left on the way out.
 *
 * Batching: `b` outputs come out of ONE kernel call, bounded by the ring
 * (taps + (b-1)*decim <= ring), by the scratch and by the input on hand.
 * Everything else here is the ring bookkeeping the kernel does not do: the AGU
 * wraps the READ window, the writes below are plain C.
 */
static size_t dec_q31_stage_run(uint8_t s,
                                const int32_t *in, size_t in_frames,
                                size_t in_stride, uint8_t in_ch_mask,
                                int32_t *out, size_t out_stride,
                                bool last)
{
    const dec_q31_stage_t *const st = &s_q31.stage[s];
    const uint16_t T   = st->taps;
    const uint16_t R   = st->ring;
    const uint8_t  D   = st->decim;
    const uint8_t  nch = s_q31.channels;
    const int32_t *const coeff = &s_q31_coeff[st->coeff_off];

    uint16_t bmax = (uint16_t)(1u + (uint16_t)((R - T) / D));
    if (bmax > DEC_Q31_MAX_BATCH) { bmax = DEC_Q31_MAX_BATCH; }

    size_t produced = 0u;
    size_t i = 0u;
    while (i < in_frames)
    {
        const size_t need  = (size_t)D - (size_t)s_q31.phase[s];
        const size_t avail = in_frames - i;

        if (avail < need)
        {
            /* Not enough for another output.  Absorb, and leave the phase. */
            dec_q31_hist_push(s, st->ring_off, R, in + (i * in_stride), avail,
                              in_stride, in_ch_mask);
            i += avail;
            s_q31.phase[s] = (uint8_t)((size_t)s_q31.phase[s] + avail);
            break;
        }

        size_t b = 1u + ((avail - need) / D);
        if (b > bmax) { b = bmax; }
        const size_t push_n = need + ((b - 1u) * D);

        dec_q31_hist_push(s, st->ring_off, R, in + (i * in_stride), push_n,
                          in_stride, in_ch_mask);
        i += push_n;
        s_q31.phase[s] = 0u;

        /* Oldest sample of the FIRST output's window.  The batch spans
         * T + (b-1)*D samples and ends at wr-1, so the start is that far back
         * -- modulo the ring by hand, because only the AGU wraps. */
        const uint16_t span = (uint16_t)(T + ((b - 1u) * D));
        const uint16_t w0   = (uint16_t)(((uint32_t)s_q31.wr[s] + R - span) % R);

        /* Constant-stride walks, as in the rational runner: the ring base advances
         * by one channel row and the output column by one channel, so neither the
         * kernel setup nor the scatter pays a multiply per channel or per output. */
        int32_t *ring = &s_q31_hist[0][st->ring_off];
        int32_t *dst  = &out[produced * out_stride];

        for (uint8_t c = 0u; c < nch; ++c)
        {
            int32_t tmp[DEC_Q31_MAX_BATCH];

#if APP_ASRC_Q31_PRE_HALFBAND
            /* One test on a descriptor field already in the cache line, once per channel
             * per batch -- 12 tests per block against the 22 MACs per output it selects
             * away.  The rational runner needs no such test: a phase ROW is never a
             * half-band stage, so its hb_half is 0 by construction. */
            if (st->hb_half != 0u)
            {
                (void)fir_ring_q31_hb_ymod_yonly_block(coeff, ring + w0,
                                                       (uint32_t)st->hb_half,
                                                       tmp, (uint32_t)b,
                                                       (uint32_t)D * 4u,
                                                       ring, (uint32_t)R * 4u);
            }
            else
#endif
            (void)fir_ring_q31_ymod_yonly_block(coeff, ring + w0, T,
                                                tmp, (uint32_t)b,
                                                (uint32_t)D * 4u,
                                                ring, (uint32_t)R * 4u);
            {
                int32_t *o = dst;
                if (last)
                {
                    for (size_t k = 0u; k < b; ++k)
                    {
                        *o = dec_q31_to_s24_left(tmp[k]);
                        o += out_stride;
                    }
                }
                else
                {
                    for (size_t k = 0u; k < b; ++k)
                    {
                        *o = tmp[k];
                        o += out_stride;
                    }
                }
            }
            ring += DEC_Q31_HIST_PER_CH;
            ++dst;
        }
        produced += b;
    }
#if APP_ASRC_LPF1TO1_PROTOTYPE
    s_dq31_stage_out[s] += (uint32_t)produced;
#endif
    return produced;
}

#if APP_ASRC_LEG_PROFILE
#include "nora_high_res_timer.h"
/*
 * Block-level split of THIS front end into its two arithmetic parts, peak-held over the
 * telemetry window (see APP_ASRC_LEG_PROFILE in asrc_app_config.h).  Only the composed
 * rational chain is instrumented, because that is the one whose two halves are the study
 * subject: `pre` is every cascade stage AHEAD of the phase rows (for 96 -> 32 kHz, the
 * 96 -> 48 kHz /2 pre-stage) and `r23` is the L/M phase rows themselves (the 97-tap
 * 48 -> 32 kHz resampler).  Anything this function costs outside the two -- the argument
 * and bound checks, the dispatch, the loop -- is not measured here and shows up as the
 * caller's feA minus these two, which is exactly where it belongs.
 *
 * Written only from a block ISR, read only from the foreground telemetry print, and a torn
 * 32-bit read can misprint one line but cannot corrupt the peak -- the same treatment the
 * neighbouring dbg_ counters get, and for the same reason: no guard in the audio ISR.
 */
static volatile uint32_t s_dq31_pre_ticks;
static volatile uint32_t s_dq31_r23_ticks;
static volatile uint32_t s_dq31_pre_last;
static volatile uint32_t s_dq31_r23_last;

/* Per-CALL values, read-and-clear.  The peaks above answer "how bad does each half get";
 * these answer "what did the two halves cost in THE block the caller is attributing", which
 * is the only form that can be added to a caller's other rows without mixing blocks. */
void asrc_decimator_q31_profile_take_last( uint32_t* pre_ticks, uint32_t* r23_ticks )
{
#if APP_ASRC_THIRDBAND_96_TO_32
    /* ONE telemetry line has to describe whichever front end is live.  When the third band
     * is armed its two halves take these two fields -- `pre` is everything ahead of the
     * arithmetic (the channel repeat and the headroom shift), `r23` is the 3:1 kernel --
     * because reporting 0 here would read as "this chain did not run", which is exactly the
     * misreading the two fields exist to prevent. */
    if( s_tb32_armed != 0u )
    {
        asrc_thirdband_96_to_32_profile_take_last( pre_ticks, r23_ticks );
        return;
    }
#endif
    if( pre_ticks != NULL ) { *pre_ticks = s_dq31_pre_last; }
    if( r23_ticks != NULL ) { *r23_ticks = s_dq31_r23_last; }
    s_dq31_pre_last = 0u;
    s_dq31_r23_last = 0u;
}

void asrc_decimator_q31_profile_read( uint32_t* pre_ticks, uint32_t* r23_ticks )
{
    if( pre_ticks != NULL ) { *pre_ticks = s_dq31_pre_ticks; }
    if( r23_ticks != NULL ) { *r23_ticks = s_dq31_r23_ticks; }
    s_dq31_pre_ticks = 0u;
    s_dq31_r23_ticks = 0u;
}
#endif /* APP_ASRC_LEG_PROFILE */

/*
 * `input_channels` is how many REAL channels the source frame carries -- 2 for a
 * TDM/I2S block, whatever the slot count is.  The front end computes ASRC_CH of
 * them by repeating the source channels, which is the same arrangement the
 * resampler behind it already uses (asrc_push reads p[c & 1u] and emits only
 * channels 0/1).  So the extra channels are the WORKLOAD, not data: the audio
 * that reaches the codec is still channels 0 and 1, computed by exactly the
 * kernel and coefficients the other fourteen exercise.  Must be a power of two,
 * because the repeat is a mask -- a modulo per sample would cost more than the
 * MAC it feeds.
 */
bool asrc_decimator_q31_process_s24_left(const int32_t *input,
                                         size_t input_frames,
                                         size_t input_stride,
                                         uint8_t input_channels,
                                         int32_t *output,
                                         size_t output_capacity_frames,
                                         size_t output_stride,
                                         size_t *output_frames)
{
    if (output_frames != NULL) { *output_frames = 0u; }
    if ((s_q31.stages == 0u) || (output_frames == NULL) ||
        ((input_frames != 0u) && (input == NULL)) ||
        (input_channels == 0u) ||
        /* The repeat is a bitmask, so a source NARROWER than the logical width must be a
         * power of two.  A source at or above the logical width repeats nothing and needs
         * no such property -- which is the only reason a 12 ch logical width can run here. */
        ((input_channels < s_q31.channels) &&
         ((input_channels & (uint8_t)(input_channels - 1u)) != 0u)) ||
        (input_stride < input_channels) ||
        (output_stride < s_q31.channels))
    {
        return false;
    }
    if (asrc_decimator_q31_output_frames(input_frames) > output_capacity_frames)
    {
        return false;
    }

    /* Repeat a narrower source; pass a full-width one straight through.  The `>=` arm is the
     * identity mask, so it is also what a 16 ch source into an 8 ch width has always got. */
    const uint8_t in_mask = (input_channels >= s_q31.channels)
                                ? ASRC_DEC_Q31_FULL_MASK
                                : (uint8_t)(input_channels - 1u);

#if APP_ASRC_THIRDBAND_96_TO_32
    /* THE THIRD BAND REPLACES THE WHOLE COMPOSED CHAIN, not a stage of it: one 3:1 stage
     * instead of /2 then 2/3.  Dispatched before the rational arm below, and only ever
     * armed for the chain it replaces (see s_tb32_eligible), so no other ratio can reach
     * it.  The validity and capacity checks above are shared deliberately -- this stage is
     * a different filter, not a different contract. */
    if (s_tb32_armed != 0u)
    {
        return tb32_process(input, input_frames, input_stride, in_mask,
                            output, output_stride, output_frames);
    }
#endif

    /* The rational chain is dispatched before the cascade arms: its last two stage
     * descriptors are phase rows and not cascade stages, so this branch must be
     * taken on `num` and never on a stage count -- a composed 96 -> 32 kHz chain
     * has THREE stages and would otherwise fall into the generic cascade below,
     * which would run both rows as if each decimated its predecessor's output. */
    if (s_q31.num == ASRC_DECIMATOR_48_TO_32_L)
    {
        const uint8_t rb = s_q31.r23_base;
        /* Walk the stages AHEAD of the rows before running any of them, exactly as
         * the integer cascade below does: once a stage has written, refusing is no
         * bound at all.  Both bounds are checked here -- every intermediate buffer,
         * and then the rational ring's own input domain. */
        {
            size_t n = input_frames;
            for (uint8_t s = 0u; s < rb; ++s)
            {
                n = ((size_t)s_q31.phase[s] + n) / s_q31.stage[s].decim;
                if (n > DEC_Q31_MAX_MID)
                {
                    return false;
                }
            }
            /* The ring spans exactly one block plus the longest row's window, so a
             * longer block is refused rather than silently reading samples this
             * call has already overwritten. */
            if (n > DEC_Q31_R23_MAX_IN)
            {
                return false;
            }
        }
        const int32_t *src        = input;
        size_t         n          = input_frames;
        size_t         src_stride = input_stride;
        uint8_t        src_mask   = in_mask;
#if APP_ASRC_LEG_PROFILE
        const uint32_t prof_t0 = nora_high_res_timer_get_count();
#endif
        for (uint8_t s = 0u; s < rb; ++s)
        {
            /* Never `last`: the phase rows are downstream, so an intermediate stage
             * must hand on full-precision Q31 and not the s24-left mask. */
            int32_t *const dst = dec_q31_mid_buf(s);
            n = dec_q31_stage_run(s, src, n, src_stride, src_mask, dst,
                                  (size_t)ASRC_DEC_Q31_CH, false);
            src        = dst;
            src_stride = (size_t)ASRC_DEC_Q31_CH;
            src_mask   = ASRC_DEC_Q31_FULL_MASK;   /* full-width intermediate: identity */
        }
#if APP_ASRC_LEG_PROFILE
        const uint32_t prof_t1 = nora_high_res_timer_get_count();
#endif
        *output_frames = dec_q31_r23_run(src, n, src_stride, src_mask,
                                         output, output_stride);
#if APP_ASRC_LEG_PROFILE
        {
            const uint32_t prof_t2 = nora_high_res_timer_get_count();
            const uint32_t pre     = prof_t1 - prof_t0;
            const uint32_t r23     = prof_t2 - prof_t1;
            if (pre > s_dq31_pre_ticks) { s_dq31_pre_ticks = pre; }
            if (r23 > s_dq31_r23_ticks) { s_dq31_r23_ticks = r23; }
            s_dq31_pre_last = pre;
            s_dq31_r23_last = r23;
        }
#endif
        return true;
    }

    /* s24-left IS Q31 with eight zero sub-LSBs, so the input needs no
     * conversion at all: the same int32 word is both. */
    if (s_q31.stages == 1u)
    {
        *output_frames = dec_q31_stage_run(0u, input, input_frames,
                                           input_stride, in_mask,
                                           output, output_stride, true);
        return true;
    }

    /* Every intermediate buffer holds DEC_Q31_MAX_MID frames, and stage s writes
     * (phase + in) / decim of them.  Refusing AFTER a run would be no bound at all
     * -- the write has happened by then -- so the whole cascade's counts are walked
     * here first, with exactly the arithmetic the stages are about to do.  With the
     * 16-frame block and /2 in the first stage this is 8, i.e. the buffers are sized
     * to the block and not to a guess; a larger block is refused, not overrun. */
    {
        size_t n = input_frames;
        for (uint8_t s = 0u; (s + 1u) < s_q31.stages; ++s)
        {
            n = ((size_t)s_q31.phase[s] + n) / s_q31.stage[s].decim;
            if (n > DEC_Q31_MAX_MID)
            {
                return false;
            }
        }
    }

    /* Run the cascade.  The stages alternate between the two intermediate buffers,
     * so no stage reads the buffer it is writing, and the LAST one writes the
     * caller's output at the caller's stride with the s24-left mask applied.  Only
     * the first stage repeats the source's real channels up to ASRC_CH; from then on
     * the intermediate is already that wide, so the mask is the full width. */
    const int32_t *src        = input;
    size_t         n          = input_frames;
    size_t         src_stride = input_stride;
    uint8_t        src_mask   = in_mask;
    for (uint8_t s = 0u; s < s_q31.stages; ++s)
    {
        const bool     last       = ((s + 1u) == s_q31.stages);
        int32_t *const dst        = last ? output : dec_q31_mid_buf(s);
        const size_t   dst_stride = last ? output_stride : (size_t)ASRC_DEC_Q31_CH;

        n = dec_q31_stage_run(s, src, n, src_stride, src_mask, dst, dst_stride,
                              last);
        src        = dst;
        src_stride = (size_t)ASRC_DEC_Q31_CH;
        src_mask   = ASRC_DEC_Q31_FULL_MASK;   /* full-width intermediate: identity */
    }
    *output_frames = n;
    return true;
}

#if APP_ASRC_FRONTEND_SELFTEST
/* The boot bit-exactness check.  Its own file because it is a startup check, not
 * part of the front end -- and so that a build without it carries none of it. */
#include "asrc_decimator_q31_selftest.inc"
#if APP_ASRC_LPF1TO1_PROTOTYPE
/* AFTER the divider selftest, because the 1:1 checks re-use its oracle
 * (dec_q31_st_ref_stage) rather than carrying a second copy of one. */
#include "asrc_lpf1to1_selftest.inc"
#endif
#endif
