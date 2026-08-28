#!/usr/bin/env python3
"""48 kHz -> 32 kHz anti-alias band limiting: can a TRUE 16-channel AK512 BiDir ASRC afford it?

48/32 = 1.5 has no integer decimation of 48 kHz, so the three levers that exist are priced here
against the MEASURED hardware cost model, not against the 2026-07 unit model:

  A  make the resampler itself do the band limiting -- retarget ASRC_POLY_FC to the OUTPUT
     Nyquist and grow ASRC_POLY_M until the stopband gate passes.  Costs a bigger phase table
     and a longer inner loop, nothing else.
  B  a dedicated L=2/M=3 rational polyphase stage (conceptually up 2 / LPF at 96 kHz / down 3),
     computing only the phases that are needed, at the 32 kHz OUTPUT rate.  L = 2, so
     MAC/output = N/2, and the existing decim-by-3 Q31 block kernel can serve it in two calls
     per channel (phase-0 windows start at input 3m, phase-1 at 3m+1 -- both stride 3).
  C  a plain decimate-by-1 FIR at the full 48 kHz input rate, kept as the costed baseline.

Cost model, fitted to the two MEASURED 16-channel front ends in
[internal] report_ak512_fir_kernel_measured_2026-08-21.md section 12.4:

    /3  13739 MACs,  16 calls -> +104.3 us      solving the pair gives
    /6  11776 MACs, 171 calls -> +143.4 us      us = 0.00719 * MACs + 0.343 * calls

i.e. 1.438 effective cycles/MAC at 200 MHz plus ~69 cycles per kernel call, the latter matching
the ~70-cycle per-call setup documented for fir_ring_q31_ymod_yonly_block.  For the FLOAT
resampler (option A) the measured effective rate is used instead: cbA = 120.4 us for
16 out * 16 ch * 30 taps = 7680 MACs -> 3.135 cycles/MAC.  (The stream16 kernels hoist the
two-phase-row blend out of the channel loop -- see the "ce" path in mchp_stream8_pair_slot_f32.s
-- so the real MAC count per output is M, not 2*M; float mac.s itself floors at 2 cycles/MAC.)

Deadline: APP_BLOCK_FRAMES = 16 at 48 kHz = 333.33 us for BOTH legs together (TDMsum).
Run from the repository root.  Only NumPy is required.
"""
import contextlib
import dataclasses
import importlib.util
import io
import sys

import numpy as np


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


chk = _load("chk", "tools/asrc/asrc_headroom_filter_check.py")
dsg = _load("dsg", "tools/asrc/asrc_decimator_48_to_8_design.py")

L = chk.L                            # 128 resampler phases
SHIPPED = chk.CANDIDATES[1]          # headroom-m30-kaiser11: 30 taps, fc 0.465 of the INPUT rate
IN_HZ = 48000.0
OUT_HZ = 32000.0
OUT_NYQ = 16000.0
GATE = -100.0                        # release gate; the scan target elsewhere is -105
EDGE_GATE = -1.1                     # same passband-loss gate as the shipping candidate
NFFT = 1 << 16                       # 96 kHz / 65536 = 1.46 Hz bins -- far finer than any edge here
FREQS = np.linspace(0.0, 24000.0, 24001)      # 1 Hz steps over the 48 kHz input band
BLOCK_US = 1000000.0 * 16.0 / 48000.0
CH = 16


def cost_us(macs, calls):            # fitted Q31 front-end model
    return 0.00719 * macs + 0.343 * calls


def mag_at(coeff, fs, freqs_hz):
    spectrum = np.abs(np.fft.rfft(coeff, NFFT))
    axis = np.linspace(0.0, fs * 0.5, spectrum.size)
    return np.interp(freqs_hz, axis, spectrum)


def fold(f, rate):
    r = f % rate
    return np.minimum(r, rate - r)


def prototype(cutoff, taps):
    """Interleaved polyphase prototype, exactly as the firmware builds its phase rows at init."""
    cand = dataclasses.replace(SHIPPED, cutoff=cutoff, taps=taps)
    rows = chk.phase_rows(cand)
    proto = np.zeros(taps * L)
    for p in range(L):
        for t in range(taps):
            proto[t * L - p + (L - 1)] = rows[p, t]
    return proto / proto.sum()


PROTO_SHIPPED = prototype(SHIPPED.cutoff, SHIPPED.taps)


def coarse_fine(lo, hi, coarse, step, ok):
    """Smallest n in range(lo, hi, step) with ok(n) true, found coarsely then refined."""
    hit = None
    n = lo
    while n < hi:
        if ok(n):
            hit = n
            break
        n += coarse
    if hit is None:
        return None
    n = hit
    while n - step >= lo and ok(n - step):
        n -= step
    return n


# ---------------------------------------------------------------- A: resampler does the job
def optA_eval(passband, m):
    cutoff = 0.5 * (passband + OUT_NYQ) / IN_HZ
    proto = prototype(cutoff, m)
    mask = FREQS > OUT_NYQ
    mag = mag_at(proto, float(L) * IN_HZ, fold(FREQS, IN_HZ))
    worst = float(20.0 * np.log10(np.maximum(mag[mask], 1e-15)).max())
    f = np.linspace(0.0, passband, 2000)
    edge = float((20.0 * np.log10(np.maximum(
        mag_at(proto, float(L) * IN_HZ, f), 1e-15))).min())
    return worst, edge, cutoff * IN_HZ


def optA(passband):
    def ok(m):
        w, e, _ = optA_eval(passband, m)
        return w <= GATE and e >= EDGE_GATE
    m = coarse_fine(30, 481, 16, 2, ok)
    if m is None:
        return None
    w, e, fc = optA_eval(passband, m)
    return m, fc, w, e


# -------------------------------------------------------------- B: L=2/M=3 rational polyphase
def optB_alias(coeff96, with_resampler):
    """Worst alias of the up2 / LPF / down3 chain, referenced to the 48 kHz input band.

    Zero-stuffing by 2 makes the 48 kHz-rate spectrum periodic with 48 kHz on the 96 kHz axis,
    so an input at f appears there at BOTH f and 48000-f.  Each is shaped by H at its own
    frequency and then folded by the 3:1 decimation into fold(., 32000).  A contribution is an
    alias whenever its landing frequency differs from f -- which makes even f BELOW the output
    Nyquist an alias source through its 48000-f image, so the mask cannot be the plain
    `f > out_nyq` that serves the integer front ends in asrc_lowpass_rate_survey.py.
    """
    worst = -300.0
    for g in (FREQS, IN_HZ - FREQS):
        land = fold(g, OUT_HZ)
        mag = mag_at(coeff96, 96000.0, g)
        if with_resampler:              # step ~1.0, fc 0.465 * 32 kHz, taken at the LANDING freq
            mag = mag * mag_at(PROTO_SHIPPED, float(L) * OUT_HZ, land)
        alias = np.abs(land - FREQS) > 0.5
        if alias.any():
            worst = max(worst, float(20.0 * np.log10(np.maximum(mag[alias], 1e-15)).max()))
    return worst


def optB_edge(coeff96, passband, with_resampler):
    f = np.linspace(0.0, passband, 2000)
    mag = mag_at(coeff96, 96000.0, f)
    if with_resampler:
        mag = mag * mag_at(PROTO_SHIPPED, float(L) * OUT_HZ, f)
    return float((20.0 * np.log10(np.maximum(mag, 1e-15))).min())


def optB(passband, with_resampler=True):
    def ok(n):
        return optB_alias(dsg.design(n, 96000.0, passband, OUT_NYQ), with_resampler) <= GATE
    n = coarse_fine(33, 2049, 32, 4, ok)
    if n is None:
        return None
    c = dsg.design(n, 96000.0, passband, OUT_NYQ)
    return n, optB_alias(c, with_resampler), optB_edge(c, passband, with_resampler)


# ---------------------------------------------------------------- C: decimate-by-1 at 48 kHz
def optC_eval(passband, n):
    c = dsg.design(n, IN_HZ, passband, OUT_NYQ)
    mask = FREQS > OUT_NYQ
    res = mag_at(PROTO_SHIPPED, float(L) * IN_HZ, fold(FREQS, IN_HZ))
    mag = res * mag_at(c, IN_HZ, FREQS)
    w = float(20.0 * np.log10(np.maximum(mag[mask], 1e-15)).max())
    f = np.linspace(0.0, passband, 2000)
    e = float((20.0 * np.log10(np.maximum(
        mag_at(PROTO_SHIPPED, float(L) * IN_HZ, f) * mag_at(c, IN_HZ, f), 1e-15))).min())
    return w, e


def optC(passband):
    n = coarse_fine(33, 1025, 16, 2, lambda n: optC_eval(passband, n)[0] <= GATE)
    if n is None:
        return None
    w, e = optC_eval(passband, n)
    return n, w, e


PASSBANDS = (15000.0, 14000.0, 13600.0, 12800.0, 12000.0, 11000.0)

print("=" * 104)
print("0. WHAT HAPPENS TODAY   fe=direct, resampler cutoff fixed at 0.465*48000 = 22320 Hz")
print("=" * 104)
_mask = FREQS > OUT_NYQ
_mag = mag_at(PROTO_SHIPPED, float(L) * IN_HZ, fold(FREQS, IN_HZ))
print("   worst alias in the 0-16 kHz output band : %.2f dB"
      % float(20.0 * np.log10(np.maximum(_mag[_mask], 1e-15)).max()))
print("   fold source band 16000-22320 Hz lands at 9680-16000 Hz  (release gate is %.0f dB)" % GATE)
sys.stdout.flush()

print()
print("=" * 104)
print("A. RESAMPLER DOES IT   retarget ASRC_POLY_FC to the output Nyquist, grow ASRC_POLY_M")
print("=" * 104)
print("%9s %5s %8s %9s %8s %11s %9s %8s %9s"
      % ("passband", "M", "fc(Hz)", "worst", "edge", "phase tbl", "MAC/blk", "us/blk", "% of 333"))
for P in PASSBANDS:
    r = optA(P)
    if r is None:
        print("%9.0f %5s %8s   no solution <= 480 taps" % (P, "-", "-"))
        sys.stdout.flush()
        continue
    m, fc, worst, edge = r
    tbl = (L + 1) * m * 4
    macs = 16 * CH * m                                # 16 output frames * 16 ch * M taps
    us = macs * 3.135 / 200.0                         # measured float resampler rate
    print("%9.0f %5d %8.0f %8.2fdB %7.2fdB %10dB %9d %8.1f %8.1f%%"
          % (P, m, fc, worst, edge, tbl, macs, us, 100.0 * us / BLOCK_US))
    sys.stdout.flush()
print("   phase tbl = (L+1)*M*4 B of s_poly, against 65024 B of TOTAL data RAM (62230 B in use)")

print()
print("=" * 104)
print("B. DEDICATED L=2/M=3 POLYPHASE  prototype at 96 kHz, L=2, MAC/output = N/2, run at 32 kHz")
print("=" * 104)
print("%9s %6s %5s %9s %8s %9s %6s %8s %9s %8s %7s  %s"
      % ("passband", "N", "N/2", "worst", "edge", "MAC/blk", "calls", "us/blk", "% of 333",
         "hist/ch", "coeffX", "metric"))
for P in PASSBANDS:
    for tag, wr in (("+resampler", True), ("stage only", False)):
        r = optB(P, with_resampler=wr)
        if r is None:
            print("%9.0f %6s   no solution <= 2048 taps  (%s)" % (P, "-", tag))
            sys.stdout.flush()
            continue
        n, worst, edge = r
        half = n // 2
        outs = 11                                     # worst case: ceil(16*2/3) outputs per block
        macs = outs * half * CH
        calls = 2 * CH
        us = cost_us(macs, calls)
        hist = half + (6 - 1) * 3                     # ring: taps + (outputs/phase - 1)*decim
        print("%9.0f %6d %5d %8.2fdB %7.2fdB %9d %6d %8.1f %8.1f%% %7ds %6dB  %s"
              % (P, n, half, worst, edge, macs, calls, us, 100.0 * us / BLOCK_US,
                 hist, 2 * half * 4, tag))
        sys.stdout.flush()
print("   hist/ch = samples per channel, against DEC_Q31_HIST_PER_CH = 190 (12160 B already in Y)")
print("   coeffX  = bytes of X-space coefficients, against s_q31_coeff = 844 B and 2794 B free")

print()
print("=" * 104)
print("C. DECIMATE-BY-1 FIR AT 48 kHz   the costed baseline, Q31, 16 channels")
print("=" * 104)
print("%9s %6s %9s %8s %9s %6s %8s %9s %8s %7s"
      % ("passband", "N", "worst", "edge", "MAC/blk", "calls", "us/blk", "% of 333",
         "hist/ch", "coeffX"))
for P in PASSBANDS:
    r = optC(P)
    if r is None:
        print("%9.0f %6s   no solution <= 1024 taps" % (P, "-"))
        sys.stdout.flush()
        continue
    n, worst, edge = r
    macs = 16 * n * CH
    us = cost_us(macs, CH)
    print("%9.0f %6d %8.2fdB %7.2fdB %9d %6d %8.1f %8.1f%% %7ds %6dB"
          % (P, n, worst, edge, macs, CH, us, 100.0 * us / BLOCK_US, n + 15, n * 4))
    sys.stdout.flush()

print()
print("=" * 104)
print("D. BUDGET   measured TDMsum of the nearest hardware cases (sections 12.4 / 12.8)")
print("=" * 104)
print("   48<->44.1 direct   267.5 us   80.3%%   margin  65.8 us")
print("   48<->48   steady   246.6 us   74.0%%   margin  86.7 us")
print("   48->16k  via /3    332.6 us   99.8%%   margin   0.7 us  <- front ends already at the wall")
print("   48->8k   via /6    332.8 us   99.8%%   margin   0.5 us")
print("   deadline (both legs, one 16-frame TDM window) = %.1f us" % BLOCK_US)
print("   48<->32 today is fe=direct on both legs, and its A leg makes 10.67 rather than 16")
print("   outputs per block, so its margin should sit ABOVE 86.7 us -- estimated 95-125 us,")
print("   which is the one number in this table that has NOT been measured.")

print()
print("=" * 104)
print("E. PER-CHANNEL COST   the same options divided by the channel count they would serve")
print("=" * 104)
print("   Any option's us/blk above scales linearly in channels (the MAC term dominates), so")
print("   'how many channels fit in a 95-125 us budget' = budget / (us_per_block / 16).")
for P in (13600.0, 12000.0):
    rb = optB(P)
    if rb is None:
        continue
    n, _w, _e = rb
    per_ch = cost_us(11 * (n // 2) * 1, 2)
    print("   B at %.0f Hz passband: %6.2f us per channel -> %4.1f ch at 95 us, %4.1f ch at 125 us"
          % (P, per_ch, 95.0 / per_ch, 125.0 / per_ch))


# ---------------------------------------------------------------- F: relax WHAT is protected
def optB_alias_protect(coeff96, protect_hz, with_resampler=True):
    """Same chain as optB_alias, but an alias only counts where it LANDS at or below protect_hz.

    The gate convention inherited from asrc_lowpass_rate_survey.py protects the entire output
    band (mask f > out_nyq), i.e. it demands -100 dB even for energy that lands in the top
    hundreds of Hz below 16 kHz.  Audibility does not, and the transition width is what the tap
    count is spent on -- so this row prices "protect 0..protect_hz, let the rest fold into the
    strip above it", which is the one design knob that buys back taps without moving the passband.
    """
    worst = -300.0
    for g in (FREQS, IN_HZ - FREQS):
        land = fold(g, OUT_HZ)
        mag = mag_at(coeff96, 96000.0, g)
        if with_resampler:
            mag = mag * mag_at(PROTO_SHIPPED, float(L) * OUT_HZ, land)
        sel = (np.abs(land - FREQS) > 0.5) & (land <= protect_hz)
        if sel.any():
            worst = max(worst, float(20.0 * np.log10(np.maximum(mag[sel], 1e-15)).max()))
    return worst


print()
print("=" * 104)
print("F. HOW MUCH ALIAS SUPPRESSION PER MICROSECOND   option B structure, passband 15000 Hz,")
print("   stopband edge on the output Nyquist, tap count swept -- the cost/benefit curve")
print("=" * 104)
print("%6s %5s %8s %9s %9s %9s %9s"
      % ("N", "N/2", "us/blk", "% of 333", "worst all", "<13 kHz", "<10 kHz"))
for n in (33, 49, 65, 81, 97, 129, 161, 201, 269):
    c = dsg.design(n, 96000.0, 15000.0, OUT_NYQ)
    half = n // 2
    us = cost_us(11 * half * CH, 2 * CH)
    print("%6d %5d %8.1f %8.1f%% %8.2fdB %8.2fdB %8.2fdB"
          % (n, half, us, 100.0 * us / BLOCK_US,
             optB_alias_protect(c, 16000.0), optB_alias_protect(c, 13000.0),
             optB_alias_protect(c, 10000.0)))
    sys.stdout.flush()

print()
print("=" * 104)
print("G. PROTECT ONLY PART OF THE OUTPUT BAND   passband held at 15000 Hz; the -100 dB wall is")
print("   required only up to `protect`, so the transition may run 15000 -> (32000 - protect)")
print("=" * 104)
print("%9s %9s %6s %5s %9s %8s %8s %9s  %s"
      % ("protect", "stopband", "N", "N/2", "worst", "edge", "us/blk", "% of 333", "verdict"))
for protect in (15900.0, 14900.0, 13900.0, 12900.0, 11900.0, 9900.0):
    stop = OUT_HZ - protect
    def ok(n, stop=stop, protect=protect):
        c = dsg.design(n, 96000.0, 15000.0, stop)
        return optB_alias_protect(c, protect) <= GATE   # front-end gate only; see section H
    n = coarse_fine(33, 2049, 32, 4, ok)
    if n is None:
        print("%9.0f %9.0f %6s   no solution <= 2048 taps" % (protect, stop, "-"))
        sys.stdout.flush()
        continue
    c = dsg.design(n, 96000.0, 15000.0, stop)
    half = n // 2
    us = cost_us(11 * half * CH, 2 * CH)
    print("%9.0f %9.0f %6d %5d %8.2fdB %7.2fdB %8.1f %8.1f%%  %s"
          % (protect, stop, n, half, optB_alias_protect(c, protect),
             optB_edge(c, 15000.0, True), us, 100.0 * us / BLOCK_US,
             "fits 95-125 us" if us <= 125.0 else "over budget"))
    sys.stdout.flush()

print()
print("=" * 104)
print("H. THE PASSBAND CEILING IS NOT THE FRONT END   the resampler's own cutoff is a fixed")
print("   fraction of ITS INPUT rate, so with a 2/3 front end it sits at 0.465 * 32000 Hz")
print("=" * 104)
print("   ASRC_POLY_FC = %.3f  ->  -6 dB point at %.0f Hz on a 32 kHz input" % (SHIPPED.cutoff, SHIPPED.cutoff * OUT_HZ))
print("%9s %12s %12s" % ("f (Hz)", "resampler", "resampler"))
print("%9s %12s %12s" % ("", "@32k in", "@48k in (today)"))
for f in (10000.0, 12000.0, 12800.0, 13000.0, 13600.0, 14000.0, 14500.0, 15000.0, 15500.0):
    a = 20.0 * np.log10(max(float(mag_at(PROTO_SHIPPED, float(L) * OUT_HZ, np.array([f]))[0]), 1e-15))
    b = 20.0 * np.log10(max(float(mag_at(PROTO_SHIPPED, float(L) * IN_HZ, np.array([f]))[0]), 1e-15))
    print("%9.0f %11.2fdB %11.2fdB" % (f, a, b))
print("   So a 'flat to 15 kHz' output spec at a 32 kHz rate is out of reach for ANY front end:")
print("   the 30-tap resampler running at 32 kHz is already past its own -6 dB point there.")
print("   Raising ASRC_POLY_FC is free in cycles but is global to every rate, and trades the")
print("   resampler's own image rejection -- a separate decision, not part of the front end.")
