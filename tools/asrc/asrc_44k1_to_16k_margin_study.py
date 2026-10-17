#!/usr/bin/env python3
"""What the last 0.07 dB costs on 44.1 -> 16 kHz.

EXPERIMENTAL OPTIMIZER MODE.  READ NOTHING HERE AS A PRODUCT NUMBER.
====================================================================
This script sweeps tap counts around N35 + N53, which is the chain the PLANNER sized for this
row -- not the chain the image runs.  F-C established that the image runs N51 + N53 (the /2
stage sized partner-aware against 12550 Hz, not 15550 Hz) and reads -114.38 dBc, i.e. 9.45 dB
better and already 9.4 dB inside the -105 dBc internal target.  So:

  * the premise below -- "0.07 dB outside the internal target" -- is a property of the
    EXPERIMENTAL sizing, not of the product;
  * the row this script calls "shipping" is not shipping.  It is kept because the owner asked
    for the light-chain search to survive Phase G, and because the sweep itself is sound: it
    prices tap counts around a reference point.  Only the label was wrong.
  * for the as-built answer run
    tools/asrc/asrc_44k1_to_16k_chain_authority.py, or the requalification driver in
    --mode qualification.

At L=256 this pair's rear phase-interpolation spur is -115.09 dBc and its own
FRONT-END ALIAS is -104.93 dBc, so the pair reads MARGIN WATCH: 5.07 dB inside the
-100 dBc product gate, 0.07 dB outside the -105 dBc internal design target.  L is
not the variable any more -- L=512 would not move -104.93 by anything.  The only
thing that moves it is the front end, and that front end is a chain already
Hard-RT PASS on hardware, so this is a costing exercise and NOT a change:

    /2@44.1k N35 -> LPF@22.05k N53      rear 22.05 -> 16 kHz, step 1.378125

The question the owner asked is narrow: is there a variant that reaches -105 dBc at
the SAME CPU tier, the same structure, the same RAM class and the same passband?
This script sweeps the two stages' tap counts around that reference point and prints
the cost of each answer.  It does not write firmware and nothing selects from it.

Everything is evaluated by asrc_universal_nyquist_model.evaluate with Q31
quantisation on, i.e. the same authority the 45-pair requalification used.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import asrc_universal_nyquist_model as U  # noqa: E402

FS_IN, FS_OUT = 44100.0, 16000.0
PROTECTED = 0.8125                        # of the OUTPUT Nyquist (owner-fixed)
PB_SPEC_DB = 0.10
PRODUCT_DBC = -100.0
INTERNAL_DBC = -105.0
# THE EXPERIMENTAL REFERENCE POINT, not the image.  N35 is what relaxed_cascade() sizes this
# stage to (stopband Fs_mid - Fp = 15550 Hz); the image is N51 (stopband Fs_mid - Fstop(1:1) =
# 12550 Hz).  Renamed from SHIP_* on 2026-09-15 so the two cannot be confused again.
REF_N0, REF_N1 = 35, 53
ASBUILT_N0, ASBUILT_N1 = 51, 53          # what asrc_lpf1to1_coeffs.inc contains
ASBUILT_DBC = -114.38                    # the as-built authority, F-C


def chain_for(n0, n1, fp):
    """The experimental reference structure with the two tap counts overridden.

    Sized exactly as asrc_universal_nyquist_model.build_candidates sizes the
    'A1 /2 + A2 LPF@22.05k' candidate: the /2 stage is relaxed (it only has to
    handle what IT folds in, stopband 22.05k - fp), and the decimate-by-1 LPF
    carries the sharp edge (stopband fs_out - fp).
    """
    s0 = U.make_stage('dec', FS_IN, fp, (FS_IN / 2.0) - fp, d=2, taps=n0)
    s1 = U.make_stage('lpf', FS_IN / 2.0, fp, FS_OUT - fp, d=1, taps=n1)
    return U.Chain('/2 + LPF', 'A1+A2', [s0, s1], FS_IN / 2.0, FS_OUT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extra', type=int, default=10,
                    help='how many extra taps to explore per stage (default 10)')
    args = ap.parse_args()

    fp = PROTECTED * FS_OUT * 0.5
    print('PLANNER MODE = EXPERIMENTAL (EXPERIMENTAL OPTIMIZER) -- NOT a product verdict')
    print('  the AS-BUILT image for this pair is /2@44.1k N%d -> LPF@22.05k N%d at %.2f dBc,'
          % (ASBUILT_N0, ASBUILT_N1, ASBUILT_DBC))
    print('  already %.2f dB inside the -105 dBc internal target.  Everything below prices tap'
          % (INTERNAL_DBC - ASBUILT_DBC))
    print('  counts around the EXPERIMENTAL reference N%d, which no image contains.'
          % REF_N0)
    print()
    print('44.1 -> 16 kHz, protected band 0 .. %.1f Hz (%.4g x FN_out), '
          'passband +/-%.2f dB' % (fp, PROTECTED, PB_SPEC_DB))
    print('experimental reference chain: /2@44.1k N%d -> LPF@22.05k N%d, rear step %.6f'
          % (REF_N0, REF_N1, (FS_IN / 2.0) / FS_OUT))
    print()

    ship = chain_for(REF_N0, REF_N1, fp)
    r = U.evaluate(ship, FS_IN, FS_OUT, fp, q31=True)
    base_macs = ship.macs_per_sec
    base_alias = r.worst_alias_db
    print('reference (EXPERIMENTAL taps): alias %+.2f dBc  passband %.3f dB  '
          'MACs/s/ch %.0f  cpu16 @1.90 %.2f%% @1.42 %.2f%%'
          % (base_alias, r.passband_err_db, base_macs,
             U.load_percent(base_macs, 16) * 1.0,
             U.load_percent(base_macs, 16) * (1.42 / 1.90)))
    print('   (the 45-pair EXPERIMENTAL column reads -104.93 dBc for this row; this script '
          'reproduces that chain.')
    print('    the QUALIFICATION column reads %.2f dBc, on a different chain -- do not compare '
          'them)' % ASBUILT_DBC)
    print()

    rows = []
    n0s = [REF_N0 + 2 * k for k in range(0, args.extra // 2 + 1)]
    n1s = [REF_N1 + 2 * k for k in range(0, args.extra // 2 + 1)]
    for n0 in n0s:
        for n1 in n1s:
            ch = chain_for(n0, n1, fp)
            res = U.evaluate(ch, FS_IN, FS_OUT, fp, q31=True)
            macs = ch.macs_per_sec
            cpu190 = U.load_percent(macs, 16)
            rows.append({
                'n0': n0, 'n1': n1,
                'alias': res.worst_alias_db,
                'pb': res.passband_err_db,
                'macs': macs,
                'cpu190': cpu190,
                'cpu142': cpu190 * (1.42 / 1.90),
                'd_macs_pct': 100.0 * (macs - base_macs) / base_macs,
                'history': ch.history_frames(),
            })

    print('%-4s %-4s %10s %8s %12s %8s %8s %8s %7s  %s'
          % ('N0', 'N1', 'alias dBc', 'pb dB', 'MACs/s/ch', 'd MACs%',
             'cpu@1.90', 'cpu@1.42', 'hist', 'verdict'))
    for row in sorted(rows, key=lambda r: (r['macs'], r['n0'])):
        if row['alias'] <= INTERNAL_DBC:
            v = 'reaches -105'
        elif row['alias'] <= PRODUCT_DBC:
            v = 'product ok, margin watch'
        else:
            v = 'FAILS product gate'
        if row['pb'] > PB_SPEC_DB:
            v += ' + PASSBAND FAIL'
        mark = ('  <-- EXPERIMENTAL reference (not the image)'
                if (row['n0'], row['n1']) == (REF_N0, REF_N1) else
                '  <-- AS BUILT (this one is in the image)'
                if (row['n0'], row['n1']) == (ASBUILT_N0, ASBUILT_N1) else '')
        print('%-4d %-4d %10.2f %8.3f %12.0f %8.1f %8.2f %8.2f %7d  %s%s'
              % (row['n0'], row['n1'], row['alias'], row['pb'], row['macs'],
                 row['d_macs_pct'], row['cpu190'], row['cpu142'], row['history'],
                 v, mark))

    ok = [r for r in rows if r['alias'] <= INTERNAL_DBC and r['pb'] <= PB_SPEC_DB]
    print()
    if not ok:
        print('NO variant in this sweep reaches -105 dBc within the passband spec.')
        print('The 0.07 dB is therefore not a tap count away in this structure.')
        return 0
    best = min(ok, key=lambda r: r['macs'])
    print('cheapest variant reaching -105 dBc: N0=%d N1=%d, alias %.2f dBc, '
          'passband %.3f dB' % (best['n0'], best['n1'], best['alias'], best['pb']))
    print('   cost: MACs/s/ch %.0f (%+.1f %%), cpu16 %.2f%% @1.90 / %.2f%% @1.42 '
          '(experimental reference %.2f%% / %.2f%%)'
          % (best['macs'], best['d_macs_pct'], best['cpu190'], best['cpu142'],
             U.load_percent(base_macs, 16), U.load_percent(base_macs, 16) * (1.42 / 1.90)))
    print('   FIR history %d frames/ch (experimental reference %d) -- same RAM class'
          % (best['history'], ship.history_frames()))
    print()
    print('REPORTED, NOT APPLIED.  The AS-BUILT chain is Hard-RT PASS on hardware and')
    print('5.07 dB inside the product gate; 0.07 dB of internal margin does not buy a')
    print('change to a qualified chain.  This is the price tag, for the owner.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
