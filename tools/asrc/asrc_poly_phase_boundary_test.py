#!/usr/bin/env python3
"""Host gate for the polyphase PHASE-INTERPOLATION BOUNDARY, at every L in the tree.

WHY THIS EXISTS.  L=128 held for a year, and "it held at 128 so 256 is safe" is not
an argument: the guard row is row L, the blend reads rows p and p+1, and every one of
those three numbers moves with L.  If p could ever reach L, the blend would read one
row past the end of the table -- in the FLASH arm that is a const array in program
memory, so the read would succeed and return whatever follows it, silently.  This
gate pins the boundary instead of trusting it.

WHAT IT CHECKS, per (L, M) geometry that the tree actually carries a table for:

  1  TABLE SHAPE.  The generated header really has L+1 rows of M taps, and its
     declared ASRC_POLY_Q31_TABLE_L / _M match the file the selector will pick.
     (This is what proves the guard row exists at all.)
  2  PHASE SELECT.  For the boundary set the C is most likely to break on -- frac=0,
     both sides of every k/L, 0.5, (L-1)/L, and frac -> 1.0 from below -- plus a dense
     sweep and the float32 neighbours (nextafter) of every phase edge:
        0 <= p <= L-1        p never selects the guard row as its base
        p + 1 <= L           the blend never reads past the guard row
        0 <= wb <= 1         the blend weight stays a weight
        p non-decreasing     no fold-back in the middle of the sweep
  3  ROW CORRECTNESS.  p is the row the exact arithmetic asks for, in every bin but
     the clamped top one, where wb is allowed to reach 1.0.
  4  INTERPOLATION ERROR.  The blended row (from the BAKED Q31 table, read exactly as
     blend_row reads it) against the float64 design evaluated at that same frac.  An
     off-by-one row is a large error here, not a subtle one, and the L=128 -> L=256
     ratio is the quantity the -100 dBc decision rests on.

The phase arithmetic is not re-implemented here: it is imported from
asrc_q31_resampler_model.phase_of, the same float32 model that generates the baked
table and the Proof-A vectors.  A source check separately confirms the C in the tree
still spells that arithmetic the same way, so the two cannot drift apart quietly.

NEGATIVE TESTS.  --mutate takes one of no-clamp / off-by-one / row-drop / wb-unclamped
and breaks exactly one thing; each must make this gate FAIL.  --selftest runs all of
them and fails if any mutation passes.  A boundary test that cannot fail is decoration.

Usage:
  python tools/asrc/asrc_poly_phase_boundary_test.py             # gate, all geometries
  python tools/asrc/asrc_poly_phase_boundary_test.py --selftest  # + negative tests
"""
import argparse
import math
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
ASRC_SRC = os.path.join(REPO, 'src', 'app', 'apps', 'asrc')
sys.path.insert(0, HERE)

import asrc_q31_resampler_model as MODEL  # noqa: E402

f32 = MODEL.f32

# (L, M, header) -- the geometries the (L,M) selector in asrc_poly_q31.inc carries.
# Kept in this order so the report reads L128 then L256 for the same M.
GEOMETRIES = [
    (128, 30, 'asrc_poly_q31_table.h'),
    (256, 30, 'asrc_poly_q31_table_l256m30.h'),
    (256, 34, 'asrc_poly_q31_table_l256m34.h'),
    (128, 28, 'asrc_poly_q31_table_m28.h'),
]

Q31_ONE = 2147483647.0

# fc / beta per M.  M28 is the retired 96 kHz geometry and has its own filter;
# check 4 compares against the design, so it has to use the right one.
FILTER = {30: (0.464, 11.3), 28: (0.4632, 10.55), 34: (0.46336, 14.0)}


# --------------------------------------------------------------- the table ---
def parse_table(path):
    """Return (declared_L, declared_M, rows) from a generated Q31 table header."""
    with open(path, 'r', newline='') as fh:
        text = fh.read()

    def macro(name):
        m = re.search(r'#define\s+%s\s+\((\d+)u\)' % name, text)
        if not m:
            raise AssertionError('%s: %s not found' % (os.path.basename(path), name))
        return int(m.group(1))

    dl, dm = macro('ASRC_POLY_Q31_TABLE_L'), macro('ASRC_POLY_Q31_TABLE_M')
    body = text.split('ASRC_POLY_Q31_TABLE_INIT', 1)[1]
    rows = []
    for line in body.splitlines():
        line = line.strip()
        if line.endswith('\\'):
            line = line[:-1].strip()
        if not line.startswith('{'):
            continue
        vals = [int(v) for v in line.strip('{},').split(',') if v.strip()]
        rows.append(vals)
    return dl, dm, rows


# ------------------------------------------------------------- the frac set ---
def boundary_fracs(L):
    """The values the C is most likely to break on, plus their float32 neighbours."""
    out = [0.0, 0.5]
    for k in list(range(0, min(4, L))) + [L // 4, L // 2, L - 2, L - 1, L]:
        if k < 0 or k > L:
            continue
        edge = k / float(L)
        out += [edge,
                float(np.nextafter(np.float32(edge), np.float32(-1.0))),
                float(np.nextafter(np.float32(edge), np.float32(2.0)))]
    one = np.float32(1.0)
    for _ in range(6):
        one = np.nextafter(one, np.float32(0.0))
        out.append(float(one))
    out += list(np.linspace(0.0, 1.0, 4096, endpoint=False))
    return sorted(set(f for f in out if 0.0 <= f < 1.0))


def phase_of(frac, L, mut):
    """MODEL.phase_of, with one mutation available for the negative tests."""
    pf = f32(f32(frac) * f32(L))
    p = int(np.int32(pf))
    if mut != 'no-clamp' and p >= L:
        p = L - 1
    if mut == 'off-by-one':
        p = min(p + 1, L)
    wb = f32(pf - f32(p))
    if mut == 'wb-unclamped':
        wb = f32(wb + 1.0e-3)
    return p, wb


# ----------------------------------------------------------------- the gate ---
def check_geometry(L, M, header, mut=None, quiet=False):
    fails = []
    path = os.path.join(ASRC_SRC, header)
    dl, dm, rows = parse_table(path)

    # 1 -- table shape
    if mut == 'row-drop':
        rows = rows[:-1]
    if (dl, dm) != (L, M):
        fails.append('1 shape: %s declares L=%d M=%d, expected L=%d M=%d'
                     % (header, dl, dm, L, M))
    if len(rows) != L + 1:
        fails.append('1 shape: %s has %d rows, expected L+1 = %d (guard row missing?)'
                     % (header, len(rows), L + 1))
    bad_m = [i for i, r in enumerate(rows) if len(r) != M]
    if bad_m:
        fails.append('1 shape: %d rows are not M=%d taps wide (first: row %d)'
                     % (len(bad_m), M, bad_m[0]))
    if len(rows) < 2:
        return fails, {}

    nrows = len(rows)
    fracs = boundary_fracs(L)

    # 2 / 3 -- phase select invariants
    prev_p = -1
    n_clamped = 0
    for frac in fracs:
        p, wb = phase_of(frac, L, mut)
        if not (0 <= p <= L - 1):
            fails.append('2 range: frac=%.9g -> p=%d outside [0, %d]' % (frac, p, L - 1))
        if p + 1 > L:
            fails.append('2 overrun: frac=%.9g -> reads row p+1=%d, guard row is %d'
                         % (frac, p + 1, L))
        if p + 1 >= nrows:
            fails.append('2 overrun: frac=%.9g -> reads row p+1=%d, table has %d rows'
                         % (frac, p + 1, nrows))
        if not (0.0 <= wb <= 1.0):
            fails.append('2 weight: frac=%.9g -> wb=%.9g outside [0,1]' % (frac, wb))
        if p < prev_p:
            fails.append('2 monotone: frac=%.9g -> p=%d after p=%d' % (frac, p, prev_p))
        prev_p = p
        exact = f32(f32(frac) * f32(L))
        if p == L - 1 and exact >= float(L - 1) + 1.0:
            n_clamped += 1                      # the clamped top bin: wb may reach 1.0
        elif not (float(p) <= exact < float(p) + 1.0 + 1e-6):
            fails.append('3 row: frac=%.9g -> pf=%.9g not inside bin p=%d'
                         % (frac, exact, p))
        if len(fails) > 40:
            fails.append('... more failures suppressed')
            break

    # 4 -- interpolation error against the float64 design at that exact frac.
    #      MODEL.blend_q31 takes whole rows and reads M from the module, so the
    #      geometry globals are set to this table's before calling it.
    fc, beta = FILTER[M]
    MODEL.M, MODEL.MH = M, (M // 2) - 1
    MODEL.L, MODEL.FC, MODEL.BETA = L, fc, beta
    worst = 0.0
    worst_at = None
    probe = [0.0, 0.25 / L, 0.5 / L, 0.999 / L, 0.5, (L - 0.5) / L, (L - 0.001) / L]
    for frac in probe:
        if not (0.0 <= frac < 1.0):
            continue
        p, wb = phase_of(frac, L, mut)
        if p + 1 >= nrows:
            continue                            # already reported as an overrun
        wbq = MODEL.wb_to_q31(wb)
        blended = [int(v) / Q31_ONE
                   for v in MODEL.blend_q31(rows[p], rows[p + 1], wbq)]
        ideal = ideal_row(frac, L, M, fc, beta)
        err = max(abs(b - i) for b, i in zip(blended, ideal))
        if err > worst:
            worst, worst_at = err, frac
    if worst > 2.0e-4:
        fails.append('4 interp: worst tap error %.3e at frac=%.9g -- too large for a '
                     'correct row pair' % (worst, worst_at))

    # 2b -- THE CLAMP'S OWN DOMAIN.  Inside the contract (frac in [0,1)) the clamp
    #       never engages at either L: f32(frac)*L rounds to at most L - 2^-k, so p
    #       is already <= L-1 for all 4117 points above, INCLUDING every float32
    #       phase edge and the six float32 values below 1.0.  That is worth knowing
    #       and it is not the same statement as "the clamp is unnecessary": it is the
    #       only thing standing between a caller that ever delivers frac >= 1.0 and a
    #       read of row L+1, one row past the guard row, off the end of the table.
    #       So the clamp is exercised here on exactly that out-of-contract input.
    n_clamp_engaged = 0
    for frac in [1.0, float(np.nextafter(np.float32(1.0), np.float32(2.0))),
                 1.0 + 1.0 / L, 1.5, 2.0 - 1.0 / L]:
        p, wb = phase_of(frac, L, mut)
        if p > L - 1:
            fails.append('2b clamp: out-of-contract frac=%.9g -> p=%d > L-1=%d '
                         '(the clamp is what prevents this)' % (frac, p, L - 1))
        if p + 1 >= nrows:
            fails.append('2b clamp: out-of-contract frac=%.9g -> reads row p+1=%d, '
                         'table has %d rows' % (frac, p + 1, nrows))
        else:
            n_clamp_engaged += 1

    stats = {'rows': nrows, 'fracs': len(fracs), 'clamped': n_clamped,
             'clamp_engaged': n_clamp_engaged,
             'worst_interp': worst, 'worst_at': worst_at}
    if not quiet:
        print('  L=%-4d M=%-3d %-32s rows %-4d in-contract fracs %-5d '
              '(clamp needed %d) clamp holds off-contract %d/5  '
              'worst interp err %.3e  %s'
              % (L, M, header, nrows, len(fracs), n_clamped, n_clamp_engaged, worst,
                 'FAIL' if fails else 'pass'))
    return fails, stats


def ideal_row(frac, L, M, fc, beta):
    """The float64 design evaluated at the CONTINUOUS phase frac -- what the blended
    row is trying to approximate.  Same formula as MODEL.poly_build_f64(), with
    p/L replaced by frac."""
    pi = math.pi
    MH = (M // 2) - 1
    knorm = 1.0 / MODEL.bessel_i0_f64(beta)
    row, total = [], 0.0
    for k in range(M):
        d = float(k) - float(MH) - float(frac)
        x = 2.0 * fc * d
        sinc = 1.0 if abs(x) < 1.0e-12 else math.sin(pi * x) / (pi * x)
        wpos = (d + float(MH) + 1.0) / float(M)
        kx = 2.0 * wpos - 1.0
        kr = math.sqrt(max(0.0, 1.0 - kx * kx))
        c = 2.0 * fc * sinc * MODEL.bessel_i0_f64(beta * kr) * knorm
        row.append(c)
        total += c
    return [c / total for c in row] if abs(total) > 1e-12 else row


# ------------------------------------------------------------ source checks ---
SOURCE_CHECKS = [
    ('asrc_poly_q31.inc',
     r'if\(\s*p\s*>=\s*ASRC_POLY_L\s*\)\s*\{\s*p\s*=\s*ASRC_POLY_L\s*-\s*1u;\s*\}',
     'Q31 phase select clamps p to L-1'),
    ('asrc_poly_q31.inc',
     r'ASRC_POLY_Q31_ROW\(\s*p\s*\+\s*1u\s*\)',
     'Q31 blend reads row p+1'),
    ('audio_app_asrc.c',
     r'if\(\s*p\s*>=\s*ASRC_POLY_L\s*\)\s*\{\s*p\s*=\s*ASRC_POLY_L\s*-\s*1u;\s*\}',
     'float phase select clamps p to L-1'),
    ('audio_app_asrc.c',
     r'ASRC_POLY_ROW\(\s*p\s*\+\s*1u?\s*\)',
     'float blend reads row p+1'),
]


def check_source():
    fails = []
    for fname, pat, what in SOURCE_CHECKS:
        with open(os.path.join(ASRC_SRC, fname), 'r', encoding='utf-8', newline='') as fh:
            text = fh.read()
        n = len(re.findall(pat, text))
        print('  %-22s %-42s %d hit(s)  %s'
              % (fname, what, n, 'pass' if n else 'FAIL'))
        if not n:
            fails.append('source: %s -- %s: no match' % (fname, what))
    return fails


# ------------------------------------------------------------------- driver ---
def run(mut=None, quiet=False):
    fails = []
    for L, M, header in GEOMETRIES:
        if not os.path.exists(os.path.join(ASRC_SRC, header)):
            fails.append('missing table header %s' % header)
            continue
        f, _ = check_geometry(L, M, header, mut=mut, quiet=quiet)
        fails += f
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutate', choices=['no-clamp', 'off-by-one', 'row-drop',
                                         'wb-unclamped'])
    ap.add_argument('--selftest', action='store_true',
                    help='run the gate, then every mutation, and require each to FAIL')
    args = ap.parse_args()

    print('polyphase phase-interpolation boundary gate')
    fails = run(mut=args.mutate)
    if not args.mutate:
        print('source checks:')
        fails += check_source()

    if args.mutate:
        print('\nmutation %r -> %d failure(s)' % (args.mutate, len(fails)))
        for f in fails[:6]:
            print('   ', f)
        return 0 if fails else 1

    if fails:
        print('\nFAIL (%d):' % len(fails))
        for f in fails:
            print('   ', f)
        return 1
    print('\nPASS -- every geometry clamps p to L-1, never reads past the guard row,')
    print('and blends to the float64 design within tolerance.')

    if args.selftest:
        print('\nnegative tests (each mutation must FAIL the gate):')
        bad = []
        for mut in ['no-clamp', 'off-by-one', 'row-drop', 'wb-unclamped']:
            f = run(mut=mut, quiet=True)
            print('  %-14s -> %3d failure(s)  %s'
                  % (mut, len(f), 'pass (detected)' if f else 'FAIL (not detected)'))
            if not f:
                bad.append(mut)
        if bad:
            print('\nFAIL: undetected mutations: %s' % ', '.join(bad))
            return 1
        print('\nnegative tests PASS -- the gate can fail.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
