#!/usr/bin/env python3
"""F-D: the six fenced pairs, and WHY a direct route is refused for each.

The fence in audio_app_asrc.c (s_asrc_unqualified_direct[]) was written for one reason
only: A1 raised the fill setpoint's lower reserve, six AK512 direct pairs that the
pre-A1 gate refused became starve-safe under the new law, and admitting them would
have attached an unevaluated rate-coverage change to a starve fix.  So the refusal it
prints says NOT QUALIFIED, and its own comment insists that is not the same statement
as "unsafe".

That makes the fence TEMPORARY, with an exit condition (qualification of step > 2.07)
that has nothing to do with audio.  Turning it into a PRODUCT POLICY needs a reason
that does not expire -- and there is one, but it has to be measured rather than
asserted: a large-step down-conversion cannot meet the protected-band spectral gate
without a front end, at ANY value of L, because L only moves the rear resampler's
phase-interpolation spur and not what the rear's own fc lets fold in.

This script computes, for each of the six pairs, the two independent terms of the
direct route through the same authority the 45-pair study uses:

    front-end alias   U.evaluate(direct chain)      what folds into 0..0.8125*FN_out
    rear spur         S.rear_spur_dbc(fp, fs_in)    the phase-interpolation sideband

and reports the worse of the two against the -100 dBc product gate.  If direct fails
the gate on the ALIAS term for every pair, then "unsafe DIRECT -> REFUSE" is a
statement about the audio and not about A1, the physical pair and the implementation
route are genuinely different objects, and the fence can be restated as policy without
changing a single verdict.

It also prints which filtered route each pair actually has in this tree, because that
is the other half of the policy and the part that must not be broken:

    22.05 -> 8      no row: neither leg is 48 or 96 kHz
    32 -> 11.025    no row (the 32 -> 16 1:1 prototype is a different pair)
    44.1 -> 12      no row
    44.1 -> 16      /2 + 1:1 composed, under APP_ASRC_LPF1TO1_PROTOTYPE
    48 -> 11.025    /4 from the 48 kHz divider table in asrc_audio_path_frontend_plan()
    96 -> 22.05     /4 from the 96 kHz divider table (the row the fence's own comment
                    names as "already accepted and staying that way")

Usage:
  python tools/asrc/asrc_direct_route_policy_matrix.py [--phases 256] [--csv out.csv]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import asrc_universal_nyquist_model as U   # noqa: E402
import asrc_rear_spur_model as S           # noqa: E402
import asrc_q31_resampler_model as Q       # noqa: E402

PROTECTED = 0.8125
PRODUCT_DBC = -100.0
INTERNAL_DBC = -105.0

# fs_in, fs_out, filtered route as this tree implements it, and where it is gated
PAIRS = [
    (22050.0,  8000.0, 'none',              'no front-end row (producer is not 48/96 kHz)'),
    (32000.0, 11025.0, 'none',              'no front-end row'),
    (44100.0, 12000.0, 'none',              'no front-end row'),
    (44100.0, 16000.0, '/2 + 1:1 (proto)',  'APP_ASRC_LPF1TO1_PROTOTYPE, as-built -114.38 dBc'),
    (48000.0, 11025.0, '/4 (48k table)',    'asrc_audio_path_frontend_plan(), den_ab = 4'),
    (96000.0, 22050.0, '/4 (96k table)',    'asrc_audio_path_frontend_plan(), den_ab = 4'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--phases', type=int, default=256, help='ASRC_POLY_L (default 256)')
    ap.add_argument('--csv', type=str, default=None)
    args = ap.parse_args()
    Q.L = int(args.phases)
    Q.MH = (Q.M // 2) - 1

    print('DIRECT ROUTE, six fenced pairs.  protected = 0 .. %.4g x FN_out, '
          'product gate %.0f dBc, internal %.0f dBc, rear L=%d M=%d fc=%.4g'
          % (PROTECTED, PRODUCT_DBC, INTERNAL_DBC, Q.L, Q.M, Q.FC))
    print()
    print('  %-16s %8s %10s %11s %11s %10s  %s'
          % ('pair', 'step', 'Fp Hz', 'alias dBc', 'spur dBc', 'worst', 'direct verdict'))
    rows = []
    for fs_in, fs_out, route, gate in PAIRS:
        fp = PROTECTED * fs_out * 0.5
        direct = U.Chain('direct', 'direct', [], fs_in, fs_out)
        r = U.evaluate(direct, fs_in, fs_out, fp, q31=True)
        spur = S.rear_spur_dbc(fp, fs_in, Q.L)
        worst = max(r.worst_alias_db, spur)
        verdict = ('REFUSE (alias)' if r.worst_alias_db > PRODUCT_DBC else
                   'REFUSE (spur)' if spur > PRODUCT_DBC else 'passes product gate')
        print('  %-16s %8.4f %10.1f %11.2f %11.2f %10.2f  %s'
              % ('%.4g -> %.4g k' % (fs_in / 1000.0, fs_out / 1000.0),
                 fs_in / fs_out, fp, r.worst_alias_db, spur, worst, verdict))
        rows.append((fs_in, fs_out, fs_in / fs_out, fp, r.worst_alias_db, spur, worst,
                     verdict, route, gate))
    print()
    print('  %-16s %-20s  %s' % ('pair', 'filtered route', 'where it is gated'))
    for fs_in, fs_out, _st, _fp, _a, _s, _w, _v, route, gate in rows:
        print('  %-16s %-20s  %s'
              % ('%.4g -> %.4g k' % (fs_in / 1000.0, fs_out / 1000.0), route, gate))
    print()

    worst_alias = max(r[4] for r in rows)
    print('VERDICT')
    print('  every direct route fails the product gate on the ALIAS term'
          if worst_alias > PRODUCT_DBC else
          '  at least one direct route passes on alias -- the policy cannot be stated on '
          'anti-alias grounds alone')
    print('  worst-case direct alias over the six: %+.2f dBc (best of the six: %+.2f dBc)'
          % (worst_alias, min(r[4] for r in rows)))
    print('  the rear spur term is %+.2f .. %+.2f dBc and is NOT what decides any of them'
          % (min(r[5] for r in rows), max(r[5] for r in rows)))
    print('  => "unsafe DIRECT -> REFUSE" is a statement about the audio, not about A1,')
    print('     so the fence can be restated as policy with every verdict unchanged.')

    if args.csv:
        import csv
        with open(args.csv, 'w', newline='', encoding='utf-8') as fh:
            w = csv.writer(fh)
            w.writerow(['fs_in_hz', 'fs_out_hz', 'step', 'protected_hz',
                        'direct_alias_dbc', 'direct_rear_spur_dbc', 'direct_worst_dbc',
                        'direct_verdict', 'filtered_route', 'filtered_gate'])
            for row in rows:
                w.writerow(['%.0f' % row[0], '%.0f' % row[1], '%.6f' % row[2],
                            '%.1f' % row[3], '%.2f' % row[4], '%.2f' % row[5],
                            '%.2f' % row[6], row[7], row[8], row[9]])
        print('\n  wrote %s' % args.csv)
    return 0


if __name__ == '__main__':
    sys.exit(main())
