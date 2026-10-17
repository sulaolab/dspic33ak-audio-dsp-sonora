#!/usr/bin/env python3
"""THE AS-BUILT SIZING RULE, in one place, for the planner to call (Phase G, G-A).

WHY THIS FILE EXISTS
====================
Phase F-2 found that the planner's 45-pair table and the firmware image were sizing the same
front-end stage by two different rules, and then the two numbers were compared as if they
described one chain (F-C: 9.45 dB of alias and 18.2 % of MACs apart on 44.1 -> 16 kHz).  The
two rules are:

  EXPERIMENTAL   asrc_universal_nyquist_model.build_candidates.relaxed_cascade()
                 every stage sized against what IT ALONE folds in:
                     sb_i = rates[i+1] - Fp
                 taps from the tap law, then grown by GROW_SCALES until the verified alias
                 clears the gate.  Answers "how cheap could this chain be?"

  AS BUILT       asrc_lpf1to1_design.design_prestage() / design_one()
                 every stage sized against WHAT ITS PARTNER CANNOT CLEAN UP:
                     sb_i = rates[i+1] - Fstop(the stage behind it)
                 and then the SMALLEST ODD TAP COUNT whose Q31 COMPOSED CHAIN passes.
                 Answers "does the image meet the spec?"

asrc_lpf1to1_design is the authority for the second rule and stays the authority: it is what
writes asrc_lpf1to1_coeffs.inc, so anything that disagrees with it disagrees with the image.
This module does NOT re-implement it.  It

  * re-exports the uniform specification constants from it,
  * delegates the two shapes it already covers straight to design_one() / design_prestage(),
  * generalises the SAME rule to a cascade of more than one decimating stage, which the
    generator has no row for yet, and
  * self-checks that generalisation against design_prestage() on the one shape both cover
    (--self-check), so the generalisation cannot drift away from the authority silently.

Usage:
  python tools/asrc/asrc_asbuilt_sizing.py --self-check
  python tools/asrc/asrc_asbuilt_sizing.py --pair 44100 16000
"""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


U = _load("unm_asb", "tools/asrc/asrc_universal_nyquist_model.py")
L = _load("lpf11_asb", "tools/asrc/asrc_lpf1to1_design.py")

# ---- the uniform specification.  ONE owner (asrc_lpf1to1_design), re-exported, never re-typed.
PROTECT_RATIO = L.PROTECT_RATIO              # 0.8125, owner-fixed 2026-09-14
PASSBAND_LIMIT_DB = L.PASSBAND_LIMIT_DB      # 0.10
ALIAS_PRODUCT_DB = L.ALIAS_PRODUCT_DB        # -100.0
ALIAS_DESIGN_DB = L.ALIAS_DESIGN_DB          # -105.0
MAX_TAPS = 401                               # the generator's own search ceiling

MODE_QUALIFICATION = "qualification"
MODE_EXPERIMENTAL = "experimental"


def uniform_spec(fs_out: float) -> tuple:
    """(Fp, Fstop) of the uniform spec at one output rate.  Fstop is the first fold into Fp."""
    fp = PROTECT_RATIO * fs_out * 0.5
    return fp, fs_out - fp


def partner_aware_stopband(fs_stage_out: float, partner_fstop: float) -> float:
    """The as-built rule: a stage only has to reach what the stage BEHIND it cannot remove.

    fs_stage_out is this stage's own output rate (where its images land), partner_fstop the
    stopband edge of the next stage in the chain.  For the shipped 44.1 -> 16 kHz composition
    that is 22050 - 9500 = 12550 Hz, against the experimental rule's 22050 - 6500 = 15550 Hz.
    """
    return fs_stage_out - partner_fstop


def q31(coeff):
    """dec_q31_from_float(), via the generator that owns it."""
    return L.q31_round_trip(np.asarray(coeff, dtype=np.float64))


# --------------------------------------------------------------- shapes the generator already has
def size_lpf1to1(fs: float, fs_out: float):
    """Smallest odd N for a 1:1 stage at fs serving fs_out.  Straight to the authority."""
    return L.design_one(fs, fs_out, verbose=False)


def size_prestage(fs_in: float, d: int, fs_out: float):
    """Smallest odd N for a /d stage in front of the 1:1 stage.  Straight to the authority.

    Only valid for a shape the generator has a partner row for; use size_cascade() otherwise.
    """
    return L.design_prestage(fs_in, d, fs_out, verbose=False)


# ------------------------------------------------------------------- the generalisation (G-A)
def _stage(kind, fs_in, fp, sb, coeff, d=1):
    return U.Stage(kind, fs_in, fs_in / d, len(coeff), fp, sb,
                   np.asarray(coeff, dtype=np.float64), d, 1, 1, 0.0)


def size_cascade(fs_in: float, combo, fs_out: float):
    """As-built sizing for `combo` decimating stages followed by one 1:1 stage.

    THE SAME RULE as design_prestage(), applied back-to-front so it works for any cascade
    length:

      1. the 1:1 stage at the end is sized first, by design_one() -- it carries the sharp edge
         and its stopband IS the uniform spec's Fstop;
      2. every decimating stage is then sized against the stopband of the stage BEHIND it
         (partner_aware_stopband), back to front;
      3. each stage's tap count is the smallest odd N whose Q31 COMPOSED CHAIN -- this stage,
         everything behind it, and the rear resampler -- clears the design gate, with the
         stages behind it already frozen at their chosen counts.

    Step 3 is why the search is per stage and not global: that is exactly what design_prestage()
    does with its one stage, and holding the stages behind it frozen is what makes the result
    reproducible rather than dependent on a search order over several stages at once.

    Returns (chain, per_stage) or (None, per_stage) if some stage cannot be sized <= MAX_TAPS.
    """
    fp, _fstop_final = uniform_spec(fs_out)
    rates = [float(fs_in)]
    for c in combo:
        rates.append(rates[-1] / c)
    r_last = rates[-1]

    # 1. the 1:1 stage, from the authority.
    #
    # STRUCTURAL PRECONDITION, checked before the search so a shape that CANNOT hold the uniform
    # spec is not reported as "needs more taps": the 1:1 stage must reach Fstop = Fs_out - Fp,
    # and it can only do that if Fstop is below ITS OWN Nyquist.  Fstop = 0.59375 * Fs_out, so
    # the rate the 1:1 stage runs at has to be at least 1.1875 * Fs_out.  A cascade that
    # decimates further than that has already folded the band the 1:1 stage was meant to clean,
    # and no tap count anywhere in the chain can undo it (Phase F-D, and report section 12 of the
    # /3 study).  This is the same wall that makes the shipped A1 routes carry LEGACY per-rate
    # specifications instead of the uniform one -- see asrc_coeff_provenance.py.
    fstop_needed = fs_out - fp
    if fstop_needed >= r_last * 0.5:
        return None, [("STRUCTURAL: 1:1 stage at %.4g kHz needs Fstop %.1f Hz, above its own "
                       "Nyquist %.1f Hz" % (r_last / 1000.0, fstop_needed, r_last * 0.5), 0,
                       fp, fstop_needed)]
    chosen, _rows, _n0, pfp, pfstop = L.design_one(r_last, fs_out, verbose=False)
    if chosen is None:
        return None, []
    lp_n, lp_coeff = chosen[0], chosen[1]
    behind = [_stage("lpf", r_last, pfp, pfstop, q31(lp_coeff))]
    per_stage = [("lpf@%.4gk" % (r_last / 1000.0), lp_n, pfp, pfstop)]

    # 2 + 3. decimating stages, back to front.
    next_fstop = pfstop
    for i in range(len(combo) - 1, -1, -1):
        fs_stage_in, fs_stage_out, d = rates[i], rates[i + 1], combo[i]
        sb = partner_aware_stopband(fs_stage_out, next_fstop)
        if sb <= fp:
            return None, per_stage
        n = U.tap_law(fs_stage_in, sb - fp)
        n = n if n % 2 else n + 1
        picked = None
        while n <= MAX_TAPS:
            coeff = L.M.dsg.design(n, fs_stage_in, fp, sb, L.M.KAISER_BETA)
            s = _stage("dec", fs_stage_in, fp, sb, q31(coeff), d=d)
            ch = U.Chain("q", "asbuilt", [s] + behind, r_last, float(fs_out))
            r = U.evaluate(ch, float(fs_in), float(fs_out), fp)
            if (r.worst_alias_db <= ALIAS_DESIGN_DB) and (r.passband_err_db <= PASSBAND_LIMIT_DB):
                picked = (n, s)
                break
            n += 2
        if picked is None:
            return None, per_stage
        behind = [picked[1]] + behind
        per_stage.insert(0, ("/%d@%.4gk" % (d, fs_stage_in / 1000.0), picked[0], fp, sb))
        next_fstop = sb

    label = "as-built " + " -> ".join("%s N%d" % (t[0], t[1]) for t in per_stage)
    return U.Chain(label, "A1+A2" if combo else "A2", behind, r_last, float(fs_out)), per_stage


def size_decimator_only(fs_in: float, combo, fs_out: float):
    """As-built sizing for a DECIMATORS-ONLY structure -- the shape the shipped A1 routes have.

    WHY THIS EXISTS SEPARATELY FROM size_cascade() (Phase H SS4).  size_cascade() always appends
    a 1:1 stage, because that is the shape asrc_lpf1to1_design has a row for.  Every shipped A1
    route is the OTHER shape: decimating stages only, the last one carrying the sharp edge.  When
    a shipped row has to migrate to the uniform spec, sizing it as a 1:1 cascade is a topology
    change, and a topology change is the expensive kind: a 1:1 stage runs at fs_in, a /2 stage's
    filter runs at fs_in but only produces fs_in/2 outputs, so replacing "/2@48k N107" with
    "LPF@48k N83" nearly doubles the MAC rate at a comparable tap count.  The owner instruction
    for the migration is "route topology change kept to the minimum", so the FIRST candidate to
    try is the shipped STRUCTURE with its band edges re-specified to the uniform spec.

    THE PARTNER OF THE LAST STAGE IS THE REAR RESAMPLER, and that is what decides whether this
    shape exists at all.  The rear must be handed a band already clean above Fstop = Fs_out - Fp,
    so by the same partner-aware rule the last decimating stage's stopband is

        sb_last = its own output rate - (Fs_out - Fp)

    which is only above Fp when the rear step exceeds (Fs_out - Fp + Fp) / Fs_out ... i.e. when
    the front end does NOT decimate all the way down to Fs_out.  A structure whose rear step is
    1.0 therefore has no uniform-spec sizing in this shape at all, whatever its tap counts --
    the same wall asrc_rear_passband_law.py measures from the other side (the rear prototype's
    own droop at 0.8125 x FN is -0.3892 dB at step 1.0, already 3.9x the whole budget).

    Returns (chain, per_stage) or (None, per_stage) with the reason in per_stage[0][0].
    """
    if not combo:
        return None, [("STRUCTURAL: a decimators-only structure needs at least one stage", 0,
                       0.0, 0.0)]
    fp, fstop_final = uniform_spec(fs_out)
    rates = [float(fs_in)]
    for c in combo:
        rates.append(rates[-1] / c)
    r_last = rates[-1]
    if r_last < fs_out - 1e-9:
        return None, [("STRUCTURAL: cascade lands at %.4g kHz, below Fs_out" % (r_last / 1000.0),
                       0, fp, 0.0)]

    behind = []
    per_stage = []
    next_fstop = fstop_final                 # the partner of the last stage is the REAR resampler
    for i in range(len(combo) - 1, -1, -1):
        fs_stage_in, fs_stage_out, d = rates[i], rates[i + 1], combo[i]
        sb = partner_aware_stopband(fs_stage_out, next_fstop)
        if sb <= fp:
            return None, [("STRUCTURAL: stage /%d@%.4gk would need Fstop %.1f Hz <= Fp %.1f Hz "
                           "(rear step %.5f leaves the rear nothing to clean)"
                           % (d, fs_stage_in / 1000.0, sb, fp, r_last / fs_out), 0, fp, sb)]
        if sb >= fs_stage_in * 0.5:
            return None, [("STRUCTURAL: stage /%d@%.4gk would need Fstop %.1f Hz, above its own "
                           "Nyquist %.1f Hz" % (d, fs_stage_in / 1000.0, sb, fs_stage_in * 0.5),
                           0, fp, sb)]
        n = U.tap_law(fs_stage_in, sb - fp)
        n = n if n % 2 else n + 1
        picked = None
        while n <= MAX_TAPS:
            coeff = L.M.dsg.design(n, fs_stage_in, fp, sb, L.M.KAISER_BETA)
            s = _stage("dec", fs_stage_in, fp, sb, q31(coeff), d=d)
            ch = U.Chain("q", "asbuilt", [s] + behind, r_last, float(fs_out))
            # EVALUATED AT THIS PARTIAL CHAIN'S OWN INPUT RATE, which is where this differs from
            # size_cascade() -- deliberately, and it is a fix rather than a divergence.  evaluate()
            # propagates the rate from the fs_in it is GIVEN, so handing it the full fs_in while
            # the chain starts at stage i models the wrong geometry: a /2 stage designed at 24 kHz
            # gets driven at 48 kHz.  size_cascade() survives that because its partial chain always
            # ends in the 1:1 stage that carries the sharp edge, so an over-charged intermediate
            # reading only buys extra taps; here the partial chain IS the sharp edge, and the wrong
            # geometry makes every tap count fail -- a false "structurally impossible" on rows that
            # are merely composed.  The OUTERMOST iteration passes the real fs_in either way, so
            # the chain that is returned is still verified end to end.
            r = U.evaluate(ch, float(fs_stage_in), float(fs_out), fp)
            if (r.worst_alias_db <= ALIAS_DESIGN_DB) and (r.passband_err_db <= PASSBAND_LIMIT_DB):
                picked = (n, s)
                break
            n += 2
        if picked is None:
            return None, [("no tap count <= %d clears the design gate for /%d@%.4gk"
                           % (MAX_TAPS, d, fs_stage_in / 1000.0), 0, fp, sb)]
        behind = [picked[1]] + behind
        per_stage.insert(0, ("/%d@%.4gk" % (d, fs_stage_in / 1000.0), picked[0], fp, sb))
        next_fstop = sb

    label = "re-spec " + " -> ".join("%s N%d" % (t[0], t[1]) for t in per_stage)
    return U.Chain(label, "A1", behind, r_last, float(fs_out)), per_stage


# ------------------------------------------------------------------------------------ self-check
def self_check(verbose: bool = True) -> bool:
    """The generalisation must agree with design_prestage() where both apply.

    Shape covered by both: one decimating stage plus the 1:1 partner, i.e. the shipped
    44.1 -> 16 kHz composition.  Agreement is checked on the TAP COUNTS, because those are what
    a caller reads -- not on the float bytes, which come from the same design() call either way.
    """
    ok = True
    for fs_in, d, fs_out in ((44100.0, 2, 16000.0),):
        auth, _rows, _n0, _fp, _fstop = L.design_prestage(fs_in, d, fs_out, verbose=False)
        gen, per = size_cascade(fs_in, (d,), fs_out)
        if auth is None or gen is None:
            print("  %.4gk /%d -> %.4gk : one side failed to size" %
                  (fs_in / 1000, d, fs_out / 1000))
            return False
        auth_pre_n, auth_lp_n = auth[0], auth[8]
        gen_pre_n, gen_lp_n = per[0][1], per[1][1]
        same = (auth_pre_n == gen_pre_n) and (auth_lp_n == gen_lp_n)
        ok = ok and same
        if verbose:
            print("  %.4gk /%d -> %.4gk : authority N%d + N%d   generalised N%d + N%d   %s"
                  % (fs_in / 1000, d, fs_out / 1000, auth_pre_n, auth_lp_n,
                     gen_pre_n, gen_lp_n, "AGREE" if same else "** DISAGREE **"))
    return bool(ok)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-check", action="store_true")
    ap.add_argument("--pair", nargs=2, type=float, metavar=("FS_IN", "FS_OUT"))
    ap.add_argument("--combo", type=str, default="",
                    help="e.g. 2 or 2,2 (default: try both 1:1 and /2)")
    a = ap.parse_args()

    print("AS-BUILT SIZING (mode = %s)" % MODE_QUALIFICATION)
    print("  spec: protected 0..%.4g x FN, passband +/-%.2f dB, alias <= %.0f dB product / "
          "%.0f dB design" % (PROTECT_RATIO, PASSBAND_LIMIT_DB,
                              ALIAS_PRODUCT_DB, ALIAS_DESIGN_DB))
    print()

    rc = 0
    if a.self_check or not a.pair:
        print("self-check against asrc_lpf1to1_design.design_prestage():")
        if not self_check():
            rc = 1
        print()

    if a.pair:
        fs_in, fs_out = a.pair
        combos = [tuple(int(x) for x in a.combo.split(",") if x)] if a.combo else [(), (2,)]
        fp, _ = uniform_spec(fs_out)
        print("  %-46s %10s %8s %12s %6s" % ("chain", "alias dBc", "pb dB", "MACs/s/ch", "hist"))
        for combo in combos:
            dtot = 1
            for c in combo:
                dtot *= c
            if fs_in / dtot < fs_out:
                continue
            ch, per = size_cascade(fs_in, combo, fs_out)
            if ch is None:
                why = per[0][0] if per and str(per[0][0]).startswith("STRUCTURAL") \
                    else "no tap count <= %d clears the design gate" % MAX_TAPS
                print("  %-46s  %s" % (("/" + "/".join(str(c) for c in combo)) if combo
                                       else "1:1", why))
                rc = 1
                continue
            r = U.evaluate(ch, fs_in, fs_out, fp)
            print("  %-46s %10.2f %8.4f %12.0f %6d"
                  % (ch.label, r.worst_alias_db, r.passband_err_db,
                     ch.macs_per_sec, ch.history_frames()))
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
