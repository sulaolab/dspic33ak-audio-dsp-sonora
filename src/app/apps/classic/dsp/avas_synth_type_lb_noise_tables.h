/* =========================================================================
 * Type_LB AVAS : L3 NOISE bank coefficients  (GENERATED -- DO NOT EDIT)
 *
 * GENERATED, and the generator is not part of this repository: this table is the
 * shipped artifact, not something to hand-edit or re-derive here.
 *
 * Model, the NOISE half of L3:
 *     noise(t) = sum_b NG[b] * 10^(g_b(t)/20) * noise_b(t)
 *     g_b = a slow random walk, sd 1.5 dB at 1.2 Hz, independent per band
 * Uniform movement across bands with no per-band onset ramp is what makes this L3
 * rather than L8, and it is why the table below is only {F, gain} per band.
 *
 * WHAT THIS IS: ONE white source, tilted once, feeding 12 Chamberlin
 * state-variable bandpasses whose outputs are summed with FITTED gains.
 *
 * WHY THE GAINS ARE FITTED AND NOT THE TARGETS.  The coefficient set's band levels
 * are per-band targets with no skirt overlap; a filter bank cannot be that, so the
 * composite is NOT the sum of the targets.  The gains are fitted on the COHERENT
 * sum |sum_b g_b H_b(f)| -- coherent because one shared source means overlapping
 * skirts add with their phases -- in LOG magnitude, so a band 14 dB down counts as
 * much as the loudest one.
 *
 * WHY A STATE-VARIABLE FORM.  Not for the reason the dsPIC33CK (Q15) port needed it
 * (a Q15 direct-form `a1` at the lowest band's centre moves the pole ANGLE by
 * 100 %), which does not apply here -- but the SVF tunes on F = 2 sin(pi f0 / fs),
 * it is three multiplies, and it is the form that port's fitted response was
 * measured on, so keeping it is what makes the two boards' noise the same noise.
 *
 * WHY THE SOURCE IS TILTED.  A 2nd-order skirt falls at 6 dB/octave and this target
 * falls at 12.4 dB/octave above 1 kHz, so no vector of gains can fix it: the
 * loudest band puts a floor across the whole top of the spectrum.  The tilt costs a
 * handful of operations ONCE for the entire bank.
 *
 * BANDS.  12 of the target's 18 are built, against the Q15 port's 12.  Two ceilings
 * decide that, and NEITHER is a word length:
 *   - the SVF's own stability bound F < 2 - 1/Q = 1.626, reached near fs/4, which
 *     excludes the top band (target -51 dB);
 *   - a band the FIT refuses, whose gain goes to zero.  It cannot contribute and
 *     still costs three multiplies a sample, so it is not built.  This is the same
 *     event as the Q15 port's band 11, decided by the fit instead of by the
 *     quantiser.
 * What the extra bands buy back is the 3.6-5.3 kHz air that port recorded as its
 * own worst omission at -12.05 dB.
 *
 *   #     band Hz        f0 Hz        F        order    gain          err dB
 *   0      20-    29      24.1  0.00315217   2nd   3.359783e-01   -0.05
 *   1      29-    42      34.9  0.00456975   2nd   2.945952e-01   +0.45
 *   2      42-    61      50.6  0.00662483   2nd   4.033021e-01   +0.09
 *   3      61-    88      73.4  0.00960410   2nd   4.413892e-01   +0.21
 *   4      88-   128     106.4  0.01392315   2nd   3.335978e-01   +0.24
 *   5     128-   186     154.2  0.02018443   2nd   1.500595e-01   +0.60
 *   6     186-   269     223.5  0.02926114   2nd   4.695273e-01   -0.27
 *   7     269-   390     324.1  0.04241865   2nd   5.143284e-01   -0.01
 *   8     390-   566     469.8  0.06148990   2nd   5.039216e-01   -0.07
 *   9     566-   820     681.1  0.08912738   2nd   2.470168e-01   -0.01
 *  10     820-  1189     987.4  0.12916220   2nd   1.000000e+00   -0.95
 *  11    1189-  1724    1431.5  0.18710484   NOT BUILT (the fit refused it)
 *  12    1724-  2499    2075.2  0.27081157   2nd   9.927671e-02   -0.24
 *  13    2499-  3622    3008.5  0.39126960   NOT BUILT (the fit refused it)
 *  14    3622-  5251    4361.4  0.56318971   NOT BUILT (the fit refused it)
 *  15    5251-  7613    6322.9  0.80423742   NOT BUILT (the fit refused it)
 *  16    7613- 11037    9166.3  1.12917823   NOT BUILT (the fit refused it)
 *  17   11037- 16000   13288.6  1.52831679   NOT BUILT (above the SVF bound)
 *
 * How well the built bank matches its per-band targets, in the frequency domain:
 * rms 0.38 dB over the 12 built bands, 3.53 dB over all 17 the SVF can hold.  Both
 * numbers matter -- a bank scored only on the bands it chose to build would look
 * good for pruning whatever it fits badly.
 *
 * MEASURED over 20 s of the arithmetic this header configures:
 *   band shape error mean +0.29 dB, worst -0.73 dB, rms 0.49 dB
 *   noise rms 2.698742e-02 against the target 2.698742e-02 (+0.00 dB)
 *   noise/tone -17.85 dB against the coefficient set's -17.85 dB
 *   gust sd 1.56 dB against the model's 1.50 dB
 *   peak |noise| 0.119694, folded into AVAS_TYPE_LB_L3_PEAK_ABS
 *   the difference equations agree with the transfer function the fit used to
 *   -168.3 dB (a mismatch there would make every number above wrong)
 *
 * THE LEVEL HAS NO FREE PARAMETER.  NG[b] and the tone's AMP[] are in the SAME
 * units, so tone + noise is a sum with nothing to tune: the gains below already
 * carry the one scalar that converts this bank's white source into those units,
 * measured (k = 1.887511e-01) rather than derived.
 * ========================================================================= */

#ifndef _AVAS_SYNTH_TYPE_LB_NOISE_TABLES_H
#define _AVAS_SYNTH_TYPE_LB_NOISE_TABLES_H

#define AVAS_TYPE_LB_L3_NOISE_BANDS      (12u)

/* The first band that runs a SECOND cascaded section (4th order overall).  The rule
 * is a frequency cutoff and the bands are frequency-ascending, so the 4th-order
 * bands are the top ones and one index replaces a per-band flag -- which is what
 * keeps the per-sample loop branch-free. */
#define AVAS_TYPE_LB_L3_NOISE_BANDS4_FIRST (12u)
#define AVAS_TYPE_LB_L3_NOISE_BANDS4     (0u)

/* 1/Q, SHARED by every band: the bands are geometric with a common ratio, so they
 * have a common Q -- which is what puts the -3 dB edges on the band edges. */
#define AVAS_TYPE_LB_L3_NOISE_Q1         (0.37350464f)

/* The source tilt: 1 one-pole at 348 Hz, `y += a*(x - y)`. */
#define AVAS_TYPE_LB_L3_NOISE_TILT_POLES (1u)
#define AVAS_TYPE_LB_L3_NOISE_TILT_A     (0.04556274f)

/* The gust, at the engine's control rate (once per AVAS_TYPE_LB_DEC samples).  WALK is
 * a one-pole random walk normalised to UNIT sd, so the clamp is in sd and the dB
 * depth is one constant: gain *= 1 + K*walk, the first-order 10^(x/20).  The model
 * uses a Gaussian drive and pow(); the realised sd is MEASURED (1.56 dB against the
 * model's 1.50 dB) rather than assumed equal to it. */
#define AVAS_TYPE_LB_L3_NOISE_GUST_A     (0.00502655f)
#define AVAS_TYPE_LB_L3_NOISE_GUST_DRIVE (34.505972f)
#define AVAS_TYPE_LB_L3_NOISE_GUST_K     (0.17269388f)
#define AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP (4.0f)

/* Seeds: the same xorshift32 and the same seeds the Q15 port uses. */
#define AVAS_TYPE_LB_L3_NOISE_SEED       (0x01234567uL)
#define AVAS_TYPE_LB_L3_NOISE_GUST_SEED  (0x00C0FFEEuL)

#if defined(AVAS_TYPE_LB_L3_TABLE_DEFINE_DATA)

/* F = 2 sin(pi f0 / fs), one per built band. */
static const float s_type_lb_l3_noise_f[AVAS_TYPE_LB_L3_NOISE_BANDS] =
{
    0.00314331f, 0.00457764f, 0.00662231f, 0.00961304f,
    0.01391602f, 0.02017212f, 0.02926636f, 0.04241943f,
    0.06149292f, 0.08914185f, 0.12915039f, 0.27081299f,
};

/* Fitted gains, INCLUDING the measured scalar that puts the bank's output in the
 * same units as the line amplitudes -- so the caller simply ADDS the bank to the
 * carrier sum. */
static const float s_type_lb_l3_noise_g[AVAS_TYPE_LB_L3_NOISE_BANDS] =
{
    6.34162522e-02f, 5.56051487e-02f, 7.61236963e-02f, 8.33126764e-02f,
    6.29669409e-02f, 2.83238906e-02f, 8.86237660e-02f, 9.70800343e-02f,
    9.51157376e-02f, 4.66246839e-02f, 1.88751052e-01f, 1.87385837e-02f,
};

#endif  //defined(AVAS_TYPE_LB_L3_TABLE_DEFINE_DATA)

#endif  //!_AVAS_SYNTH_TYPE_LB_NOISE_TABLES_H
