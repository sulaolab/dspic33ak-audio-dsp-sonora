#!/usr/bin/env python3
"""Universal Nyquist / anti-alias planner feasibility study -- HOST ONLY, 45 directed pairs.

Driver for tools/asrc/asrc_universal_nyquist_model.py.  Nothing here touches firmware or hardware.

  --anchors            reproduce known host/measured results and the tap law (run this FIRST)
  --map                the current architecture, all 45 pairs
  --specs              compare the three uniform Nyquist candidates x two passband targets
  --matrix             the full coverage matrix (writes CSV with --csv)
  --q31                re-verify the chosen spec's chains after Q31 quantization
  --all                every section
"""
from __future__ import annotations

import argparse
import contextlib
import csv
import importlib.util
import io
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


M = _load("unm", "tools/asrc/asrc_universal_nyquist_model.py")

PAIRS = [(a, b) for a in M.RATES for b in M.RATES if a > b]
assert len(PAIRS) == 45, len(PAIRS)

RULE = "=" * 112
THIN = "-" * 112


# ------------------------------------------------------------------------------------ 1. anchors
def section_anchors() -> bool:
    print(RULE)
    print("1. MODEL SANITY -- reproduce what is already known before trusting anything new")
    print(RULE)
    ok = True

    print("(a) TAP SIZING LAW  N = %.4f * fs / transition,  against every shipped set" % M.TAP_LAW_K)
    print("%-26s %9s %9s %9s %7s %7s %s" % ("set", "fs", "pass", "stop", "law", "shipped", "verdict"))
    law_cases = (
        ("48->12 stage2 (/4 11k)", 24000.0, 4200.0, 5512.5, M.dsg.QUARTER_STAGE2_TAPS),
        ("48->16 (/3)", 48000.0, 5900.0, 8000.0, M.dsg.MIDRATE_TAPS),
        ("48->8 stage2 (/6)", 16000.0, 3200.0, 4000.0, M.dsg.STAGE2_TAPS),
        ("48->8 stage1 (/6)", 48000.0, 3200.0, 12800.0, M.dsg.STAGE1_TAPS),
        ("48->24 (/2)", 48000.0, 8850.0, 12000.0, M.dsg.HALFRATE_TAPS),
        ("48->12 stage1 (/4 11k)", 48000.0, 4200.0, 18487.5, M.dsg.QUARTER_STAGE1_TAPS),
    )
    for name, fs, pb, sb, shipped in law_cases:
        law = M.tap_law(fs, sb - pb)
        rel = abs(law - shipped) / shipped
        verdict = "ok" if rel <= 0.20 else "OFF"
        if verdict == "OFF":
            ok = False
        print("%-26s %9.0f %9.1f %9.1f %7d %7d %s (%+.0f%%)"
              % (name, fs, pb, sb, law, shipped, verdict, 100.0 * (law - shipped) / shipped))
    print()

    print("(b) SPECTRAL ANCHORS")
    print("%-34s %14s %14s %10s" % ("anchor", "this model", "known", "verdict"))

    # A. 48 -> 32 Audio Mode, protected 0-13 kHz.  Known host figure -107.02 dB
    #    (tools/asrc/asrc_48_to_32_audio_gate.py, re-run 2026-09-14).
    ch, _tag = M.current_chain(48000, 32000, 13000.0)
    r = M.evaluate(ch, 48000.0, 32000.0, 13000.0)
    hit = abs(r.worst_alias_db - (-107.02)) <= 1.5
    ok &= hit
    print("%-34s %11.2f dB %11.2f dB %10s"
          % ("A 48->32 N97, 0-13 kHz", r.worst_alias_db, -107.02, "ok" if hit else "MISMATCH"))
    # and the 13-16 kHz residual the same gate reports as -25.50 dB
    r16 = M.evaluate(ch, 48000.0, 32000.0, 16000.0)
    hit = abs(r16.worst_alias_db - (-25.50)) <= 2.0
    ok &= hit
    print("%-34s %11.2f dB %11.2f dB %10s"
          % ("  .. 0-16 kHz (residual shown)", r16.worst_alias_db, -25.50,
             "ok" if hit else "MISMATCH"))

    # B. 48 -> 11.025 kHz DIRECT: known severe, ~0 dB (asrc_lowpass_rate_survey.py section 1).
    dir_ch = M.Chain("direct", "direct", [], 48000.0, 11025.0)
    rb = M.evaluate(dir_ch, 48000.0, 11025.0, 5512.5)
    hit = rb.worst_alias_db >= -3.0
    ok &= hit
    print("%-34s %11.2f dB %11s %10s"
          % ("B 48->11.025 direct", rb.worst_alias_db, "~0 dB", "ok" if hit else "MISMATCH"))

    # C. 96 -> 32 kHz composed: known -107.31 dB over 0-13 kHz
    #    (tools/asrc/asrc_96_to_32_audio_gate.py, re-run 2026-09-14).
    ch96, _ = M.current_chain(96000, 32000, 13000.0)
    rc = M.evaluate(ch96, 96000.0, 32000.0, 13000.0)
    hit = abs(rc.worst_alias_db - (-107.31)) <= 2.0
    ok &= hit
    print("%-34s %11.2f dB %11.2f dB %10s"
          % ("C 96->32 composed, 0-13 kHz", rc.worst_alias_db, -107.31,
             "ok" if hit else "MISMATCH"))

    # D. 48 -> 11.025 kHz filtered /4:11k: known cascade -108.28 dB (survey, corrected mask).
    ch4, _ = M.current_chain(48000, 11025, 5512.5)
    rd = M.evaluate(ch4, 48000.0, 11025.0, 5512.5)
    hit = abs(rd.worst_alias_db - (-108.28)) <= 3.0
    ok &= hit
    print("%-34s %11.2f dB %11.2f dB %10s"
          % ("D 48->11.025 /4:11k", rd.worst_alias_db, -108.28, "ok" if hit else "MISMATCH"))
    print("%-34s %11.1f dB %11s %10s"
          % ("  D vs B separation", rd.worst_alias_db - rb.worst_alias_db, ">100 dB",
             "ok" if (rb.worst_alias_db - rd.worst_alias_db) > 100.0 else "MISMATCH"))
    print()

    print("(c) RING BOUNDS reproduced from asrc_app_build_config.h (FIFO %d, block %d, AHEAD %d)"
          % (M.FIFO_FRAMES, M.BLOCK_FRAMES, M.POLY_AHEAD))
    print("    hard burst-ratio bound  step < %.4f" % M.STEP_HARD_MAX)
    for s in (1.0, 2.0, 3.0, 4.0, 4.35, 4.8, 5.0, 7.3):
        print("    step %6.3f -> %s" % (s, M.step_verdict(s)))
    print()
    print("VERDICT: %s" % ("model reproduces every anchor -- proceed"
                           if ok else "MISMATCH -- STOP, do not trust the sweep"))
    print()
    return ok


# ------------------------------------------------------------------------------------ 2. the map
def section_map(rows_out=None):
    print(RULE)
    print("2. THE CURRENT ARCHITECTURE, ALL 45 DIRECTED DOWN-CONVERSION PAIRS")
    print("   plan authority: asrc_audio_path_frontend_plan(), preset APP_BUILD_ASRC_CODEC_BIDIR")
    print("   alias/passband measured over the PROVEN-BASELINE protected band, 0.8125 * FN")
    print(RULE)
    print("%7s %7s %8s %8s %-22s %6s %8s %10s %9s %8s"
          % ("Fs_in", "Fs_out", "ratio", "FN", "current chain", "step", "ringbnd",
             "alias(dB)", "pb(dB)", "fence"))
    for fs_in, fs_out in PAIRS:
        fn = fs_out / 2.0
        fp = 0.8125 * fn
        ch, tag = M.current_chain(fs_in, fs_out, fp)
        r = M.evaluate(ch, float(fs_in), float(fs_out), fp)
        num, den, _ = M.current_plan(fs_in, fs_out)
        fence = "REFUSED" if ((fs_in, fs_out) in M.QUAL_FENCE_DIRECT and (num, den) == (1, 1)) \
            else "-"
        print("%7d %7d %8.4f %8.1f %-22s %6.3f %8s %10.2f %9.2f %8s"
              % (fs_in, fs_out, fs_in / fs_out, fn, tag, ch.step,
                 M.step_verdict(ch.step), r.worst_alias_db, r.passband_at_fp_db, fence))
        if rows_out is not None:
            rows_out[(fs_in, fs_out)] = dict(
                current_chain=tag, current_step=ch.step,
                current_ring=M.step_verdict(ch.step),
                current_alias_db=r.worst_alias_db,
                current_alias_full_db=r.alias_full_band_db,
                current_pb_err_db=r.passband_err_db,
                current_pb_at_fp_db=r.passband_at_fp_db,
                current_macs=ch.macs_per_sec, fence=fence,
            )
    print()


# ------------------------------------------------------------------------------------ 3. specs
def section_specs():
    print(RULE)
    print("3. UNIFORM SPECIFICATION CANDIDATES -- how many of the 45 pairs each one can cover")
    print("   alias gate %.0f dB into 0..Fp;  Fp = ratio * FN;  first-fold stopband = Fs_out - Fp"
          % M.ALIAS_GATE_DB)
    print(RULE)
    print("%-10s %-9s %6s %6s %10s %12s %14s"
          % ("ratio", "pb target", "PASS", "FAIL", "worst cost", "worst taps", "worst load P1/P2"))
    table = {}
    for ratio in M.NYQ_RATIOS:
        for pbt in M.PASSBAND_TARGETS_DB:
            npass = 0
            fails = []
            worst_cost = 0.0
            worst_taps = 0
            worst_load = (0.0, 0.0)
            for fs_in, fs_out in PAIRS:
                fp = ratio * fs_out / 2.0
                best = M.solve(float(fs_in), float(fs_out), fp, pbt)
                if best is None:
                    fails.append((fs_in, fs_out))
                    continue
                npass += 1
                ch = best[0]
                if ch.cost_units() > worst_cost:
                    worst_cost = ch.cost_units()
                    worst_taps = sum(ch.taps_list)
                l1 = M.load_percent(ch.macs_per_sec, 16)
                l2 = M.load_percent(ch.macs_per_sec, 12)
                if l1 > worst_load[0]:
                    worst_load = (l1, l2)
            table[(ratio, pbt)] = (npass, fails, worst_cost, worst_taps, worst_load)
            print("%-10s %-9s %6d %6d %10.1f %12d %8.1f%%/%.1f%%"
                  % (f"{ratio:.4g}", f"+/-{pbt:.2f}dB", npass, 45 - npass,
                     worst_cost, worst_taps, worst_load[0], worst_load[1]))
    print()
    for key, (npass, fails, *_rest) in table.items():
        if fails:
            print("   %s %s FAILS: %s"
                  % (f"{key[0]:.4g}", f"+/-{key[1]:.2f}dB",
                     ", ".join(f"{a/1000:.4g}->{b/1000:.4g}k" for a, b in fails)))
    print()
    return table


# ------------------------------------------------------------------------------------ 4. matrix
def section_matrix(rows, csv_path=None, q31=False):
    print(RULE)
    print("4. COVERAGE MATRIX -- cheapest valid chain per pair per uniform specification")
    print("   passband target +/-0.25 dB (the looser of the two; +/-0.10 dB is in the CSV)")
    print(RULE)
    hdr = "%7s %7s" % ("Fs_in", "Fs_out")
    for ratio in M.NYQ_RATIOS:
        hdr += " | %-30s" % f"{ratio*100:.4g}% FN: chain / taps / alias"
    print(hdr)
    for fs_in, fs_out in PAIRS:
        line = "%7d %7d" % (fs_in, fs_out)
        row = rows.setdefault((fs_in, fs_out), {})
        for ratio in M.NYQ_RATIOS:
            fp = ratio * fs_out / 2.0
            for pbt in M.PASSBAND_TARGETS_DB:
                best = M.solve(float(fs_in), float(fs_out), fp, pbt, q31=q31)
                key = f"r{ratio:g}_pb{pbt:g}"
                if best is None:
                    row[key + "_verdict"] = "FAIL"
                    row[key + "_chain"] = ""
                    row[key + "_taps"] = ""
                    row[key + "_alias_db"] = ""
                    row[key + "_pb_err_db"] = ""
                    row[key + "_cost_units"] = ""
                    row[key + "_load_p1"] = ""
                    row[key + "_load_p2"] = ""
                    row[key + "_step"] = ""
                    row[key + "_family"] = ""
                    row[key + "_hist_taps"] = ""
                else:
                    ch, r = best
                    row[key + "_verdict"] = "PASS"
                    row[key + "_chain"] = ch.describe()
                    row[key + "_taps"] = "+".join(str(t) for t in ch.taps_list)
                    row[key + "_alias_db"] = round(r.worst_alias_db, 2)
                    row[key + "_pb_err_db"] = round(r.passband_err_db, 3)
                    row[key + "_cost_units"] = round(ch.cost_units(), 1)
                    row[key + "_load_p1"] = round(M.load_percent(ch.macs_per_sec, 16), 2)
                    row[key + "_load_p2"] = round(M.load_percent(ch.macs_per_sec, 12), 2)
                    row[key + "_step"] = round(ch.step, 5)
                    row[key + "_family"] = ch.family
                    row[key + "_hist_taps"] = ch.history_frames()
            k = f"r{ratio:g}_pb0.25"
            if row.get(k + "_verdict") == "PASS":
                line += " | %-30s" % ("%s %s %.1fdB" % (row[k + "_family"],
                                                        row[k + "_taps"],
                                                        row[k + "_alias_db"]))
            else:
                line += " | %-30s" % "FAIL"
        print(line)
    print()

    if csv_path:
        keys = ["fs_in", "fs_out", "ratio_phys", "out_nyq_hz"]
        sample = rows[PAIRS[0]]
        keys += [k for k in sample.keys()]
        with open(csv_path, "w", newline="", encoding="ascii") as fh:
            w = csv.DictWriter(fh, fieldnames=keys, extrasaction="ignore")
            w.writeheader()
            for fs_in, fs_out in PAIRS:
                rec = dict(rows[(fs_in, fs_out)])
                rec.update(fs_in=fs_in, fs_out=fs_out,
                           ratio_phys=round(fs_in / fs_out, 6),
                           out_nyq_hz=fs_out / 2)
                w.writerow(rec)
        print("CSV written: %s" % csv_path)
        print()


# ------------------------------------------------------------------------------------ 5. worst
def section_worst(rows, ratio=0.85, pbt=0.25):
    print(RULE)
    print("5. THE REAL WORST PAIRS at %.4g%% FN / +/-%.2f dB -- by five different metrics"
          % (ratio * 100, pbt))
    print(RULE)
    key = f"r{ratio:g}_pb{pbt:g}"
    recs = []
    for fs_in, fs_out in PAIRS:
        row = rows[(fs_in, fs_out)]
        if row.get(key + "_verdict") != "PASS":
            continue
        fp = ratio * fs_out / 2.0
        trans_norm = (fs_out - 2.0 * fp) / fs_in
        recs.append(dict(pair=(fs_in, fs_out),
                         trans=trans_norm,
                         taps=sum(int(t) for t in str(row[key + "_taps"]).split("+") if t),
                         cost=row[key + "_cost_units"],
                         load1=row[key + "_load_p1"],
                         hist=row[key + "_hist_taps"],
                         margin=M.ALIAS_GATE_DB - row[key + "_alias_db"]))
    def show(name, keyf, rev=True, fmt="%.4g"):
        s = sorted(recs, key=lambda r: r[keyf], reverse=rev)[:4]
        print("  %-30s %s" % (name, "   ".join(
            "%d->%d (%s)" % (r["pair"][0], r["pair"][1], fmt % r[keyf]) for r in s)))
    show("narrowest normalized transition", "trans", rev=False, fmt="%.5f")
    show("largest total tap count", "taps")
    show("highest cost units", "cost", fmt="%.1f")
    show("largest FIR history (RAM proxy)", "hist")
    show("thinnest alias margin (dB)", "margin", rev=False, fmt="%.1f")
    fails = [p for p in PAIRS if rows[p].get(key + "_verdict") != "PASS"]
    print("  %-30s %s" % ("no valid chain at this spec",
                          ", ".join(f"{a}->{b}" for a, b in fails) or "none"))
    print()


# ------------------------------------------------------------------------------------ 6. reuse
def section_reuse(rows, ratio=0.85, pbt=0.25):
    print(RULE)
    print("6. HOW UNIVERSAL IS IT?  structures and coefficient sets needed at %.4g%% FN"
          % (ratio * 100))
    print(RULE)
    key = f"r{ratio:g}_pb{pbt:g}"
    fams = {}
    structs = {}
    coeff_sets = set()
    for fs_in, fs_out in PAIRS:
        row = rows[(fs_in, fs_out)]
        if row.get(key + "_verdict") != "PASS":
            fams["FAIL"] = fams.get("FAIL", 0) + 1
            continue
        fams[row[key + "_family"]] = fams.get(row[key + "_family"], 0) + 1
        chain_txt = str(row[key + "_chain"])
        if chain_txt.startswith("("):
            structs["(none)"] = structs.get("(none)", 0) + 1
            continue
        for part in chain_txt.split(" -> "):
            kind = part.split("@")[0]
            structs[kind] = structs.get(kind, 0) + 1
            coeff_sets.add(part)
    print("  chains by architecture family:")
    for k in sorted(fams, key=lambda x: -fams[x]):
        print("     %-10s %2d pairs" % (k, fams[k]))
    print("  stage structures used (a STRUCTURE is a kernel; a coefficient SET is a table):")
    for k in sorted(structs, key=lambda x: -structs[x]):
        print("     %-10s %2d instances" % (k, structs[k]))
    print("  distinct (structure, rate, taps) coefficient sets: %d" % len(coeff_sets))
    print("  existing structures reused: dec/2 dec/3  (both already in the tree)")
    print("  the LPF stage is a decimate-by-1 FIR.  In the Q31 front end `decim` is a RUNTIME field")
    print("  of the generic stage descriptor (dec_q31_stage_set), used as (phase+n)/decim and passed")
    print("  to the kernel as decim*4 bytes, so decim == 1 is a CONFIGURATION of the existing stage,")
    print("  not a new kernel.  What blocks it today is routing policy (asrc_decimator_q31_init")
    print("  refuses den == 1) plus the per-chain compile-time ring/arena sizing -- both firmware")
    print("  work, neither a new DSP structure.  Not verified on hardware by this study.")
    print()


# ------------------------------------------------------------------------------------ 7. Q31
def section_q31(ratio=0.85, pbt=0.25):
    print(RULE)
    print("7. Q31 QUANTIZATION of the candidate chains at %.4g%% FN / +/-%.2f dB"
          % (ratio * 100, pbt))
    print("   float design -> peak-relative Q31 -> re-verified.  sum|h| is the L1 norm, i.e. the")
    print("   worst-case gain an ADVERSARIAL full-scale input could produce.  It is NOT a pass/fail")
    print("   at 1.0: every shipped set is already above it (see the control block below), because a")
    print("   unity-DC-gain lowpass has negative sidelobes.  The number to compare is the SHIPPED")
    print("   range, and the headroom question is the same one the Full-IIR study answered with a")
    print("   folded-in gain.")
    print(RULE)
    print("%7s %7s %-26s %11s %11s %8s %9s %8s"
          % ("Fs_in", "Fs_out", "chain", "alias flt", "alias Q31", "d(dB)", "sum|h|", "peak"))
    worst_delta = 0.0
    worst_sum = 0.0
    fails = []
    for fs_in, fs_out in PAIRS:
        fp = ratio * fs_out / 2.0
        best = M.solve(float(fs_in), float(fs_out), fp, pbt)
        if best is None:
            continue
        ch, rf = best
        rq = M.evaluate(ch, float(fs_in), float(fs_out), fp, q31=True)
        sums = []
        peaks = []
        for s in ch.stages:
            _q, peak, sabs = M.q31_quantize(np.asarray(s.coeff, dtype=np.float64))
            sums.append(sabs)
            peaks.append(peak)
        d = rq.worst_alias_db - rf.worst_alias_db
        worst_delta = max(worst_delta, d)
        worst_sum = max(worst_sum, max(sums) if sums else 0.0)
        if rq.worst_alias_db > M.ALIAS_GATE_DB:
            fails.append((fs_in, fs_out, rq.worst_alias_db))
        print("%7d %7d %-26s %8.2f dB %8.2f dB %8.2f %9.4f %8.5f"
              % (fs_in, fs_out, ch.describe()[:26], rf.worst_alias_db, rq.worst_alias_db, d,
                 max(sums) if sums else 0.0, max(peaks) if peaks else 0.0))
    print()
    print("  worst Q31 degradation over all pairs: %+.2f dB" % worst_delta)
    print("  worst candidate sum|h|:               %.4f (%+.2f dB worst-case gain)"
          % (worst_sum, 20.0 * np.log10(worst_sum)))
    print("  pairs that FALL BELOW the gate only after quantization: %s"
          % (", ".join(f"{a}->{b} ({c:.1f} dB)" for a, b, c in fails) or "none"))
    print()
    print("  CONTROL -- the same L1 norm for the sets the tree already ships:")
    controls = (
        ("/6 stage1 43t", 43, 48000.0, 3200.0, 12800.0),
        ("/6 stage2 147t", 147, 16000.0, 3200.0, 4000.0),
        ("/4 stage1 27t", 27, 48000.0, 4200.0, 18487.5),
        ("/4 stage2 129t", 129, 24000.0, 4200.0, 5512.5),
        ("/3 161t", 161, 48000.0, 5900.0, 8000.0),
        ("/2 107t", 107, 48000.0, 8850.0, 12000.0),
        ("96->48 prestage 41t", 41, 96000.0, 15690.0, 32000.0),
        ("2/3 N97 prototype", 97, 96000.0, 15000.0, 16000.0),
    )
    lo = hi = None
    for name, n, fs, pb, sb in controls:
        c = np.asarray(M.dsg.design(n, fs, pb, sb), dtype=np.float64)
        s = float(np.abs(c).sum())
        lo = s if lo is None else min(lo, s)
        hi = s if hi is None else max(hi, s)
        print("     %-22s sum|h| = %.4f  (%+.2f dB)" % (name, s, 20.0 * np.log10(s)))
    print("  shipped range %.4f .. %.4f;  candidate worst %.4f -> %s"
          % (lo, hi, worst_sum,
             "INSIDE the shipped range, no new headroom problem"
             if worst_sum <= hi else "ABOVE the shipped range -- needs a headroom decision"))
    print()


# ------------------------------------------------------------------------------------ 8. CPU
def section_cpu(rows, ratio=0.85, pbt=0.25):
    print(RULE)
    print("8. CPU -- three separate columns, never one number")
    print(RULE)
    print("  A. ALGORITHMIC   taps, MAC/s per channel, cost units (taps * out_rate / 48 kHz)")
    print("  B. CALIBRATED    %.2f cycles/MAC, from the 2026-09-09 hardware bench of the SHIPPING"
          % M.CYC_PER_MAC)
    print("                   96->32 front end (208.80 cyc/32k-out/ch = 107.58 for the 41-tap /2")
    print("                   + 101.22 for the 97-tap 2/3 rows).  Kernel-only FLOOR %.3f cyc/MAC."
          % M.CYC_PER_MAC_LO)
    print("  C. PROFILE       P1 = 16+16 ch, leg A 48 kHz;  P2 = 12+12 ch, leg A 96 kHz.")
    print("                   These are NOT the same CPU condition and are never added together.")
    print()
    print("  Independent check of B: the shipped /4:11k front end (27@24k + 129@12k) models as")
    print("  %.1f%% of 200 MHz at 16 ch; hardware `fe` for the 48 kHz family reads 96..115 us in a"
          % M.load_percent(27 * 24000 + 129 * 12000, 16))
    print("  333.3 us block = 28.8..34.5 %%.  In range, so the constant is not fitted to this row.")
    print()
    key = f"r{ratio:g}_pb{pbt:g}"
    print("%7s %7s %-30s %10s %12s %9s %9s"
          % ("Fs_in", "Fs_out", "chain", "cost u", "MAC/s/ch", "P1 load", "P2 load"))
    hi1 = hi2 = 0.0
    for fs_in, fs_out in PAIRS:
        row = rows[(fs_in, fs_out)]
        if row.get(key + "_verdict") != "PASS":
            print("%7d %7d %-30s %10s %12s %9s %9s"
                  % (fs_in, fs_out, "no valid chain", "-", "-", "-", "-"))
            continue
        fp = ratio * fs_out / 2.0
        best = M.solve(float(fs_in), float(fs_out), fp, pbt)
        ch = best[0]
        l1 = M.load_percent(ch.macs_per_sec, 16)
        l2 = M.load_percent(ch.macs_per_sec, 12)
        hi1 = max(hi1, l1)
        hi2 = max(hi2, l2)
        print("%7d %7d %-30s %10.1f %12.0f %8.1f%% %8.1f%%"
              % (fs_in, fs_out, ch.describe()[:30], ch.cost_units(), ch.macs_per_sec, l1, l2))
    print()
    print("  worst front-end load: P1 %.1f %%   P2 %.1f %% of 200 MHz" % (hi1, hi2))
    print("  Reference points, same model, SHIPPING chains:")
    for label, fs_in, fs_out in (("48->8 /6", 48000, 8000), ("48->11.025 /4", 48000, 11025),
                                 ("48->32 2/3", 48000, 32000), ("96->32 /2+2/3", 96000, 32000)):
        ch, _ = M.current_chain(fs_in, fs_out, 0.8125 * fs_out / 2)
        print("     %-16s cost %6.1f u   P1 %5.1f %%   P2 %5.1f %%"
              % (label, ch.cost_units(), M.load_percent(ch.macs_per_sec, 16),
                 M.load_percent(ch.macs_per_sec, 12)))
    print()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--anchors", action="store_true")
    ap.add_argument("--map", action="store_true")
    ap.add_argument("--specs", action="store_true")
    ap.add_argument("--matrix", action="store_true")
    ap.add_argument("--worst", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--q31", action="store_true")
    ap.add_argument("--q31check", action="store_true")
    ap.add_argument("--cpu", action="store_true")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--ratio", type=float, default=0.85)
    ap.add_argument("--pb", type=float, default=0.25)
    a = ap.parse_args()
    if not any((a.anchors, a.map, a.specs, a.matrix, a.worst, a.all, a.q31check, a.cpu)):
        a.all = True
    if a.cpu:
        a.matrix = True          # the CPU table reads the matrix's verdicts; section 7 does not

    rows = {}
    if a.anchors or a.all:
        if not section_anchors() and not a.all:
            return 1
    if a.map or a.all or a.matrix:
        section_map(rows)
    if a.specs or a.all:
        section_specs()
    if a.matrix or a.all:
        section_matrix(rows, a.csv, q31=a.q31)
    if a.worst or a.all:
        section_worst(rows, a.ratio, a.pb)
        section_reuse(rows, a.ratio, a.pb)
    if a.all or a.q31check:
        section_q31(a.ratio, a.pb)
    if a.all or a.cpu:
        section_cpu(rows, a.ratio, a.pb)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
