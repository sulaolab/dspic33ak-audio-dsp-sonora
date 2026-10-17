#!/usr/bin/env python3
"""Which 44.1 -> 16 kHz chain is the AS-BUILT one, and what does IT read?

Phase F-2 was briefed to take the first /2 stage of this pair from N35 to N37, on the
strength of the planner's number for the row: front-end alias -104.93 dBc, and -107.76
dBc with two more taps.  Before touching firmware this script asks the only question
that decides whether that change means anything: WHAT IS ACTUALLY IN THE IMAGE?

The answer is that two different chains have been called "the shipping chain":

  PLANNER MODEL   asrc_universal_nyquist_model.build_candidates.relaxed_cascade()
                  sizes each decimation stage against what IT alone folds in:
                      /2 stopband = Fs_mid - Fp = 22050 - 6500 = 15550 Hz
                  and the tap law then yields N35.  This is the row the 45-pair
                  requalification reads, and the -104.93 dBc comes from here.

  AS BUILT        asrc_lpf1to1_design.design_prestage() sizes the SAME stage against
                  what the PARTNER cannot clean up:
                      /2 stopband = Fs_mid - Fstop(1:1) = 22050 - 9500 = 12550 Hz
                  and then searches for the smallest odd tap count whose Q31 COMPOSED
                  chain passes, which is N51.  That is what asrc_lpf1to1_coeffs.inc
                  contains (ASRC_DEC2_44K1_FOR_16K_TAPS = 51u) and therefore what the
                  hardware has been running.

A 51-tap stage designed to a 3 kHz tighter stopband is not "N35 plus two taps", so the
N35 -> N37 sizing exercise does not describe the firmware at all.  This script settles
it numerically instead of by argument: it reads the SHIPPING COEFFICIENTS out of the
generated include, checks they are what the design tool's parameters reproduce, and
runs them through the SAME authority the planner uses (U.evaluate with q31=True), next
to the planner's N35 and N37 models.

Usage:
  python tools/asrc/asrc_44k1_to_16k_chain_authority.py
"""
import os
import pathlib
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import asrc_universal_nyquist_model as U   # noqa: E402

REPO = pathlib.Path(HERE).resolve().parents[1]
INC = REPO / 'src' / 'app' / 'apps' / 'asrc' / 'asrc_lpf1to1_coeffs.inc'

FS_IN, FS_MID, FS_OUT = 44100.0, 22050.0, 16000.0
PROTECTED = 0.8125
FP = PROTECTED * FS_OUT * 0.5              # 6500.0 Hz
FSTOP_PARTNER = FS_OUT - FP                # 9500.0 Hz, the 1:1 stage's stopband
FSTOP_PRE = FS_MID - FSTOP_PARTNER         # 12550.0 Hz, as built
FSTOP_PRE_PLANNER = FS_MID - FP            # 15550.0 Hz, as the planner sizes it
PRODUCT_DBC, INTERNAL_DBC = -100.0, -105.0


def read_set(name):
    """Pull one coefficient array out of the generated include."""
    txt = INC.read_text(encoding='utf-8')
    m = re.search(r'static const float %s\[[^\]]*\]\s*=\s*\{(.*?)\};' % re.escape(name),
                  txt, re.S)
    if not m:
        sys.exit('coefficient set %s not found in %s' % (name, INC))
    vals = [float(v) for v in re.findall(r'[-+]?\d*\.?\d+e?[-+]?\d*f', m.group(1)
                                        .replace('f,', ',').replace('f\n', '\n'))]
    body = m.group(1)
    vals = [float(t[:-1]) for t in re.findall(r'[-+][\d.]+e?[-+]?\d*f', body)]
    return np.asarray(vals, dtype=np.float64)


def stage_from(kind, fs_in, pb, sb, taps, coeff, d=1):
    return U.Stage(kind, fs_in, fs_in / d, len(coeff), pb, sb,
                   np.asarray(coeff, dtype=np.float64), d, 1, 1, 0.0)


def report(tag, chain, base_macs=None):
    r = U.evaluate(chain, FS_IN, FS_OUT, FP, q31=True)
    macs = chain.macs_per_sec
    cpu190 = U.load_percent(macs, 16)
    d = '' if base_macs is None else ' %+7.1f%%' % (100.0 * (macs - base_macs) / base_macs)
    print('  %-34s %9.2f %8.3f %12.0f%s %8.2f %8.2f %6d'
          % (tag, r.worst_alias_db, r.passband_err_db, macs, d, cpu190,
             cpu190 * (1.42 / 1.90), chain.history_frames()))
    return r, macs


def main():
    print('44.1 -> 16 kHz, protected 0 .. %.1f Hz (%.4g x FN_out), rear step %.6f, '
          'Q31 on' % (FP, PROTECTED, FS_MID / FS_OUT))
    print()

    # ---- the shipping coefficients, straight out of the generated include -----
    pre_ship = read_set('s_dec2_44k1_for_16k_coeff')
    lp_ship = read_set('s_lp1_22k05_to_16k_coeff')
    print('as-built include %s' % INC.relative_to(REPO))
    print('  /2 stage            N=%d   sum|h| = %.4f' % (pre_ship.size, np.abs(pre_ship).sum()))
    print('  1:1 stage @22.05k   N=%d   sum|h| = %.4f' % (lp_ship.size, np.abs(lp_ship).sum()))

    # ---- identity: does the design tool's own parameter set reproduce them? ----
    pre_re = U.make_stage('dec', FS_IN, FP, FSTOP_PRE, d=2, taps=pre_ship.size).coeff
    lp_re = U.make_stage('lpf', FS_MID, FP, FSTOP_PARTNER, d=1, taps=lp_ship.size).coeff
    for nm, a, b in (('/2 stage', pre_ship, pre_re), ('1:1 stage', lp_ship, lp_re)):
        err = float(np.max(np.abs(np.asarray(a) - np.asarray(b))))
        # the include stores float32; anything at that level is the print, not a design
        ok = err <= 6e-8 * max(1.0, float(np.max(np.abs(a))))
        print('  identity %-10s max|include - redesign| = %.3g   %s'
              % (nm, err, 'MATCH (float32 print)' if ok else '** DIFFERENT DESIGN **'))
    print()

    print('  %-34s %9s %8s %12s %8s %8s %8s %6s'
          % ('chain', 'alias dBc', 'pb dB', 'MACs/s/ch', 'dMACs', '@1.90', '@1.42', 'hist'))

    # ---- planner model: what the 45-pair row actually evaluated ---------------
    plan35 = U.Chain('/2 + LPF', 'A1+A2', [
        U.make_stage('dec', FS_IN, FP, FSTOP_PRE_PLANNER, d=2, taps=35),
        U.make_stage('lpf', FS_MID, FP, FSTOP_PARTNER, d=1, taps=53)], FS_MID, FS_OUT)
    r35, m35 = report('PLANNER  /2 N35(sb 15550) + N53', plan35)

    plan37 = U.Chain('/2 + LPF', 'A1+A2', [
        U.make_stage('dec', FS_IN, FP, FSTOP_PRE_PLANNER, d=2, taps=37),
        U.make_stage('lpf', FS_MID, FP, FSTOP_PARTNER, d=1, taps=53)], FS_MID, FS_OUT)
    r37, _ = report('PLANNER  /2 N37(sb 15550) + N53', plan37, m35)

    # ---- as built ------------------------------------------------------------
    ship = U.Chain('/2 + LPF', 'A1+A2', [
        stage_from('dec', FS_IN, FP, FSTOP_PRE, pre_ship.size, pre_ship, d=2),
        stage_from('lpf', FS_MID, FP, FSTOP_PARTNER, lp_ship.size, lp_ship)], FS_MID, FS_OUT)
    rs, ms = report('AS BUILT /2 N%d(sb 12550) + N%d'
                    % (pre_ship.size, lp_ship.size), ship, m35)

    # ---- and the as-built structure with two more taps, the briefed change ----
    ship2 = U.Chain('/2 + LPF', 'A1+A2', [
        U.make_stage('dec', FS_IN, FP, FSTOP_PRE, d=2, taps=pre_ship.size + 2),
        stage_from('lpf', FS_MID, FP, FSTOP_PARTNER, lp_ship.size, lp_ship)], FS_MID, FS_OUT)
    r2, _ = report('AS BUILT + 2 taps  /2 N%d + N%d'
                   % (pre_ship.size + 2, lp_ship.size), ship2, ms)
    print()

    print('VERDICT')
    print('  the planner row and the image are %.2f dB apart: planner %.2f dBc, '
          'as built %.2f dBc' % (rs.worst_alias_db - r35.worst_alias_db,
                                 r35.worst_alias_db, rs.worst_alias_db))
    for nm, v in (('planner N35', r35.worst_alias_db), ('planner N37', r37.worst_alias_db),
                  ('as built', rs.worst_alias_db), ('as built +2 taps', r2.worst_alias_db)):
        gate = ('reaches internal -105' if v <= INTERNAL_DBC else
                'product ok, margin watch' if v <= PRODUCT_DBC else 'FAILS product gate')
        print('    %-18s %9.2f dBc   %s' % (nm, v, gate))
    print('  as built costs %+.1f %% MACs against the planner model (N%d vs N35), so the '
          'planner also UNDERSTATES this row\'s CPU'
          % (100.0 * (ms - m35) / m35, pre_ship.size))
    if rs.worst_alias_db <= INTERNAL_DBC:
        print('  => the image ALREADY meets the internal -105 dBc target.  Adding taps to')
        print('     the /2 stage buys %.2f dB on a number that is already %.2f dB inside'
              % (r2.worst_alias_db - rs.worst_alias_db, INTERNAL_DBC - rs.worst_alias_db))
        print('     the target.  The defect is in the PLANNER MODEL of this row, not in')
        print('     the firmware, and that is a HOST-side fix with no image risk.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
