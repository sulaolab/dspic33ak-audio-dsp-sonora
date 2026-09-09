#ifndef ASRC_H2_TIMING_COEFFS_H
#define ASRC_H2_TIMING_COEFFS_H

/*
 * Frozen H2 coefficients for the Phase-2 component-timing probe only.
 *
 * Candidate:
 *   48 kHz -> FIR49 (Kaiser beta=5, fc=16 kHz) -> elliptic order 10
 *   (five DF2T SOS, fp=15 kHz, rp=0.1 dB, fs=16 kHz, rs=100 dB).
 *
 * tools/asrc/asrc_48_to_32_h2_freeze.py reproduces these values, the prescribed
 * SOS order {0, 2, 1, 3, 4}, and the CRCs below.  The biquad library stores
 * each SOS as [b0, b1, b2, -a1, -a2]; this is intentionally not SciPy's raw
 * [b0, b1, b2, a0, a1, a2] row.  These arrays deliberately have data-space
 * storage in X memory (rather than `const` program-flash storage): the dsPIC
 * FIR primitive reads its coefficients through X modulo addressing.  The H2
 * probe never mutates them.
 */

#define ASRC_H2_FIR49_TAPS             (49u)
#define ASRC_H2_IIR_SOS                (5u)
#define ASRC_H2_COEFF_CRC32            (0x2B0FB181u) /* LE float32: FIR then DF2T SOS */
#define ASRC_H2_FIR49_CRC32            (0xC7CA9014u) /* LE float32 */
#define ASRC_H2_DF2T_SOS_CRC32         (0x335F52EDu) /* LE float32 */

static float asrc_h2_fir49[ ASRC_H2_FIR49_TAPS ] __attribute__((space(xmemory))) = {
    -2.3009364634851738e-18f, -0.001436841208487749f,  0.0019397789146751165f,
    -5.019510214393777e-18f, -0.0032345049548894167f,  0.004048976115882397f,
    -8.47199375993546e-18f,  -0.006081297993659973f,   0.007334272842854261f,
    -1.240853719608858e-17f, -0.01043236069381237f,    0.012344435788691044f,
    -1.6472395309042188e-17f,-0.01714915782213211f,    0.020203957334160805f,
    -2.024608425337648e-17f, -0.02832978218793869f,    0.03393182158470154f,
    -2.3310993563677362e-17f,-0.05115029960870743f,    0.06571326404809952f,
    -2.5310433033644614e-17f,-0.13627678155899048f,    0.27502065896987915f,
     0.6671077013015747f,
     0.27502065896987915f,  -0.13627678155899048f,    -2.5310433033644614e-17f,
     0.06571326404809952f,  -0.05115029960870743f,    -2.3310993563677362e-17f,
     0.03393182158470154f,  -0.02832978218793869f,    -2.024608425337648e-17f,
     0.020203957334160805f, -0.01714915782213211f,    -1.6472395309042188e-17f,
     0.012344435788691044f, -0.01043236069381237f,    -1.240853719608858e-17f,
     0.007334272842854261f, -0.006081297993659973f,   -8.47199375993546e-18f,
     0.004048976115882397f, -0.0032345049548894167f,  -5.019510214393777e-18f,
     0.0019397789146751165f, -0.001436841208487749f,  -2.3009364634851738e-18f
};

static float asrc_h2_df2t_sos[ ASRC_H2_IIR_SOS * 5u ] __attribute__((space(xmemory))) = {
    0.01855133960411724f, 0.036433732988309056f, 0.018551339604117235f,  0.6105665030616743f,  -0.1673341292780982f,
    1.0f,                 1.4708060954823288f,   1.0f,                  -0.35217836475675407f, -0.6827122586276103f,
    1.0f,                 1.7353438415111286f,   1.0000000000000002f,    0.11844640251804227f, -0.42817768910407655f,
    1.0f,                 1.2885190359554397f,   0.9999999999999998f,   -0.634688710661727f,   -0.847834019065842f,
    1.0f,                 1.2029621646878361f,   1.0f,                  -0.7713415545404274f,  -0.9546534863388536f
};

#endif /* ASRC_H2_TIMING_COEFFS_H */
