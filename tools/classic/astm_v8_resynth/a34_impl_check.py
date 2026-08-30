"""Step 34: render what the C will actually do, before writing the C.

The sound is frozen (section 38): `formant_strong` tables, jitter 0.020, and the
descent chord masked by `clip(|d rpm/dt| / 1200)` x a band-limited residual noise
x6 -- `adopt_mask1200_x6.wav`. That render is a *prototype*, and five things in it
are not what a dsPIC will do:

  1  tables are float, 2048 points per cycle          -> int16, 256..1024 points
  2  each noise block is normalised by its own std()  -> one baked constant
  3  the masking low-pass is a brick wall FFT filter  -> a 2nd-order IIR
  4  jitter/offsets come from numpy's PCG64           -> a 32-bit LCG
  5  the idle drift is a normalised random walk       -> a clamped 1-pole OU

Each one is a place where the firmware can sound different from the file that was
approved, and none of them had been measured. This step implements the target
structure in Python -- reading the *same quantised tables the header will hold*,
via `gen_engine_v8_tables.build()`, so the render is evidence about the firmware
and not about a second prototype -- and reports the difference.

The point-count schedule is the one real question here. Section 21 measured 256
points per cycle as free, but on an *acceleration* clip: a P-point table is band-
limited to crank order P/4, which is 7.3 kHz at 6875 rpm and 960 Hz at 900 rpm.
The cam clatter the owner judges lives at 1.5-8 kHz, so at the settled idle a
256-point table cannot carry it at all. Part 1 measures section 32's imp/fold at
the idle for every schedule; part 2 renders the survivors for listening.

Nothing here is sample-exact against the prototype and it is not supposed to be:
the LCG draws a different noise realisation, so the comparison is spectral
(octave bands, harmonicity) and structural (imp/fold), never a difference signal.
"""
import os
import sys

import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from a09_resynth import EngineTables  # noqa: E402
from a18_noise_calib import harmonicity  # noqa: E402
import a20_portable_synth as A20  # noqa: E402
from a20_portable_synth import octaves  # noqa: E402
from a29_chord_fix import IDLE_RPM  # noqa: E402
from a31_formant import flare_traj  # noqa: E402
from a32_clatter import CLAT_LO, clatter, lowpass, render, save  # noqa: E402
import gen_engine_v8_tables as GEN  # noqa: E402

OUT = os.path.join(HERE, "out")
STRONG = os.path.join(OUT, "tables_v3_low_formant_strong.npz")
JIT = 0.020                  # adopted (section 36)
MASK_SCALE, MASK_AMT = 1200.0, 6.0   # adopted (section 38)
BLOCK = 32                   # APP_BLOCK_FRAMES on the target
SEED = 0x5EED1234            # the LCG seed the firmware will use

DRIFT_RPM = 40.0             # section 25: +-40 rpm, "drift40_rough"
DRIFT_FC = 0.15              # Hz, the OU corner that replaces the random walk
DRIFT_STD = 13.0             # rpm; +-3 sigma lands on DRIFT_RPM
GATE_FC = 8.0                # Hz, smoothing on the mask gate (per block)

IDLE_DUR = 8.0

# What part 1 and part 2 settle on. Both parts print every candidate, so these
# are the answer, not an assumption -- change them only with the table in hand.
NCYC, NPTS = GEN.NOISE_CYCLES, GEN.NOISE_PTS
PICK = GEN.DEFAULT_SCHEDULE


class Lcg(object):
    """The 32-bit LCG the firmware will use. Numerical Recipes constants.

    Written out here rather than using numpy so that the C and this render draw
    the *same kind* of numbers; the sequence itself is not compared.
    """

    def __init__(self, seed=SEED):
        self.x = int(seed) & 0xFFFFFFFF

    def u32(self):
        self.x = (1664525 * self.x + 1013904223) & 0xFFFFFFFF
        return self.x

    def unit(self):
        """Uniform in [-0.5, 0.5)."""
        return self.u32() * (1.0 / 4294967296.0) - 0.5

    def tri(self):
        """Approximately normal, unit variance: three uniforms, sigma = 1/2."""
        return (self.unit() + self.unit() + self.unit()) * 2.0

    def below(self, n):
        return self.u32() % n


def ou_drift(n, rng, fc=DRIFT_FC, std=DRIFT_STD, clamp=DRIFT_RPM, fs_upd=None):
    """Bounded idle drift: a 1-pole filtered noise, clamped.

    a25's drift is a random walk normalised to +-amp over the whole array, which
    is not causal and not bounded in real time. A 1-pole OU process with the same
    slowness is bounded by construction, and the clamp makes that explicit.
    """
    a = np.exp(-2.0 * np.pi * fc / fs_upd)
    step = std * np.sqrt(max(1.0 - a * a, 1e-12))
    y = np.empty(n)
    v = 0.0
    for i in range(n):
        v = a * v + step * rng.tri()
        y[i] = min(max(v, -clamp), clamp)
    return y


def _read(tbl, pts, base, phase01):
    """Linear interpolation into a periodic int16 table, as the C will do it."""
    idx = base + phase01 * pts
    i0 = np.floor(idx).astype(np.int64)
    f = idx - i0
    n = len(tbl)
    return tbl[i0 % n] * (1.0 - f) + tbl[(i0 + 1) % n] * f


def _read_win(tbl, phase01):
    """The overlap-add window, which is the one table here that is NOT periodic:
    it holds the RISING HALF, so the neighbour above the last entry is 1.0. Read
    with `_read` it interpolates 0.99996 -> 0 over the last 1/256 of every cycle
    and notches the noise at the cycle rate. `local_read_win()` in engine_v8.c is
    this function."""
    n = len(tbl)
    idx = phase01 * n
    i0 = np.floor(idx).astype(np.int64)
    f = idx - i0
    hi = np.where(i0 + 1 < n, tbl[np.minimum(i0 + 1, n - 1)], 1.0)
    return tbl[i0] * (1.0 - f) + hi * f


def impl_render(b, tg, rg, fs, jitter=JIT, mask_scale=MASK_SCALE,
                mask_amt=MASK_AMT, drift=0.0, seed=SEED, quant=True,
                const_norm=True, iir=True, shared_env=True, win_sym=False,
                win_wrap=False, gate_fc=GATE_FC, noise=1.0, pot_noise_rpm=0.0,
                pot_slew_per_s=3000.0):
    """The target structure, sample by sample in the parts where it matters.

    b       the table bundle from gen_engine_v8_tables.build()
    drift   rpm; 0 disables it (the reference renders have no drift)

    The keyword switches all default to what the firmware does; setting one back
    to the prototype's choice is how part 3 attributes a metric to a cause.
      quant       int16 tables            -> float
      const_norm  baked noise_norm        -> per-block 1/std()
      iir         2nd-order Butterworth   -> brick-wall FFT low-pass
      shared_env  one shared noise env    -> the per-bin envelopes (+6.4 kB ROM)
      win_sym     periodic Hann (COLA)    -> numpy's symmetric one
      noise       scales the residual noise, and with it the masking term that
                  is derived from it -- 0 is the firmware's STAGE_NOISE off, so
                  a35's ladder and the board's "*cy" masks mean the same thing
      pot_noise_rpm  rpm of uniform noise added to the commanded rpm at BLOCK
                  rate, then slew-limited, i.e. the ADC in the loop. 0 (default)
                  is the analytic trajectory every measurement here uses; set it
                  to reproduce section 39's defect, where an unfiltered POT read
                  held the mask gate open through a steady idle. Enabling it also
                  enables the slew limiter, because the two only make sense
                  together: it is the limiter that turns a +-40 rpm wander into a
                  permanent 3000 rpm/s, and 3000 is 2.5 x MASK_SCALE.
      win_wrap    (not a prototype choice) reads the window table periodically,
                  which is the bug local_read_win() exists to avoid: the table is
                  the rising HALF, so wrapping interpolates 0.99996 -> 0 over the
                  last 1/256 of every cycle. The two readers' weights still sum to
                  1, so it is not a level error -- it swaps which reader is heard,
                  once per cycle, i.e. it injects a cycle-locked transient into
                  exactly the band the clatter metrics measure. Kept as a switch
                  because it flattered those metrics, and an artefact that helps a
                  number is the one that must be shown, not described.
    """
    rng = Lcg(seed)
    wave_i = b["q_wave"].astype(np.float64) * (1.0 / (1 << GEN.WAVE_SHIFT)) \
        if quant else np.concatenate(b["wave"])
    noise_i = b["q_noise"].astype(np.float64) * (1.0 / (1 << GEN.NOISE_SHIFT)) \
        if quant else b["noise"]
    env_i = b["q_env"].astype(np.float64) * (1.0 / (1 << GEN.ENV_SHIFT)) \
        if quant else b["env"]
    win_i = b["q_win"].astype(np.float64) * (1.0 / (1 << GEN.WIN_SHIFT)) \
        if quant else b["win"]
    if win_sym:
        win_i = np.hanning(2 * b["win_pts"])[:b["win_pts"]]
    nenv_i = b["nenv"]
    rpm_b, gain_b, nrms_b = b["rpm"], b["gain"], b["nrms"]
    pts, off = b["pts"], b["off"]
    npts, wpts, epts = b["noise_pts"], b["win_pts"], b["env_pts"]
    nlen = len(noise_i)

    # ---- the block-rate part: rpm command, drift, and the mask gate ----------
    nsamp = int(t_end_samples(tg, fs))
    nblk = nsamp // BLOCK
    tb = np.arange(nblk) * (BLOCK / fs)
    rpm_cmd = np.interp(tb, tg, rg)
    if pot_noise_rpm > 0.0:
        # its own generator: drawing from `rng` would shift the jitter and the
        # noise offsets too, and then an A/B against the same render without the
        # ADC would differ by the realisation as well as by the defect
        arng = Lcg(seed ^ 0xA5A5A5A5)
        pot = rpm_cmd + np.array([2.0 * pot_noise_rpm * arng.unit()
                                  for _ in range(nblk)])
        cap = pot_slew_per_s * (BLOCK / fs)
        v = pot[0]
        for i in range(nblk):
            v += min(max(pot[i] - v, -cap), cap)
            rpm_cmd[i] = v
    slew = np.gradient(rpm_cmd, BLOCK / fs) if nblk > 2 else np.zeros(nblk)
    gate = np.clip(np.abs(slew) / mask_scale, 0.0, 1.0)
    ga = np.exp(-2.0 * np.pi * gate_fc / (fs / BLOCK))
    gate = signal.lfilter([1.0 - ga], [1.0, -ga], gate)
    if drift > 0.0:
        rpm_cmd = rpm_cmd + ou_drift(nblk, rng, clamp=drift, fs_upd=fs / BLOCK)
    rpm_s = np.repeat(rpm_cmd, BLOCK)[:nsamp]
    gate_s = np.repeat(gate, BLOCK)[:nsamp]

    # ---- cycle phase, with the boundary jitter -------------------------------
    c_raw = np.cumsum(rpm_s / 120.0) / fs
    ncyc = int(np.floor(c_raw[-1]))
    if ncyc < 4:
        raise SystemExit("trajectory too short")
    # the firmware perturbs each cycle's duration by 1 + d[k+1] - d[k], which is
    # this map from unjittered to jittered cycle count, seen from the other side
    d = np.array([0.0] + [jitter * rng.tri() for _ in range(ncyc - 1)] + [0.0])
    edges = np.maximum.accumulate(np.arange(ncyc + 1, dtype=float) + d)
    c = np.interp(c_raw, edges, np.arange(ncyc + 1, dtype=float))
    k_of = np.floor(c).astype(np.int64)
    ph = c - k_of
    keep = k_of < ncyc
    nsamp = int(keep.sum())
    k_of, ph, gate_s = k_of[:nsamp], ph[:nsamp], gate_s[:nsamp]
    rpm_cyc = np.interp(np.arange(ncyc) + 0.5, c, rpm_s[:len(c)])

    # ---- per cycle, vectorised inside the cycle ------------------------------
    wave_out = np.zeros(nsamp)
    noise_out = np.zeros(nsamp)
    bounds = np.searchsorted(k_of, np.arange(ncyc + 1))
    off_old = rng.below(nlen)
    off_new = rng.below(nlen)
    for k in range(ncyc):
        s0, s1 = bounds[k], bounds[k + 1]
        if s1 <= s0:
            off_old, off_new = off_new, rng.below(nlen)
            continue
        p = ph[s0:s1]
        r = min(max(rpm_cyc[k], rpm_b[0]), rpm_b[-1])
        j = min(max(int(np.searchsorted(rpm_b, r, side="right") - 1), 0),
                len(rpm_b) - 2)
        a = (r - rpm_b[j]) / (rpm_b[j + 1] - rpm_b[j])
        lo = wave_i[off[j]:off[j] + pts[j]]
        hi = wave_i[off[j + 1]:off[j + 1] + pts[j + 1]]
        g = (1.0 - a) * gain_b[j] + a * gain_b[j + 1]
        nr = (1.0 - a) * nrms_b[j] + a * nrms_b[j + 1]
        wave_out[s0:s1] = g * ((1.0 - a) * _read(lo, pts[j], 0.0, p)
                               + a * _read(hi, pts[j + 1], 0.0, p))
        w = _read(win_i, wpts, 0.0, p) if win_wrap \
            else _read_win(win_i, p)                   # rising half, top = 1.0
        v_new = _read(noise_i, npts, float(off_new), p)
        v_old = _read(noise_i, npts, float(off_old) + npts, p)
        if shared_env:
            env = _read(env_i, epts, 0.0, p)
        else:
            e = (1.0 - a) * nenv_i[j] + a * nenv_i[j + 1]
            env = _read(e, len(e), 0.0, p)
        blk = (v_old * (1.0 - w) + v_new * w) * env
        if const_norm:
            amp = g * nr * b["noise_norm"]
        else:
            # what the prototype does: normalise this block by its own std
            amp = g * nr * np.sqrt(0.5) / (blk.std() + 1e-30)
        noise_out[s0:s1] = blk * amp * noise
        off_old, off_new = off_new, rng.below(nlen)

    # ---- the masking: out = wave + n + gate * LP(  (amt-1) * n  ) ------------
    extra = (mask_amt - 1.0) * noise_out
    if iir:
        bq, aq = signal.butter(2, CLAT_LO / (fs * 0.5))
        extra = signal.lfilter(bq, aq, extra)
    else:
        extra = lowpass(extra, fs, CLAT_LO)
    return wave_out + noise_out + gate_s * extra


def t_end_samples(tg, fs):
    return np.floor(tg[-1] * fs)


def mask(base, hi, tg, rg, fs):
    """a33's masking, applied to any pair of renders that share a noise draw.

    `hi - base` is exactly (MASK_AMT - 1) x the noise, because both renders draw
    the same numbers and `noise_scale` only multiplies the block at the end.
    """
    n = min(len(base), len(hi))
    w = np.interp(np.arange(n) / fs, tg,
                  np.clip(np.abs(np.gradient(rg, tg)) / MASK_SCALE, 0.0, 1.0))
    return base[:n] + w * lowpass(hi[:n] - base[:n], fs, CLAT_LO)


def fft_render(tab, tg, rg, fs, noise=1.0):
    """The FFT prototype -- what the owner actually approved (a09/a32)."""
    return render(tab, rg, tg, fs, jitter=JIT, noise=noise)


def por_render(tab, tbl, tg, rg, fs, noise=1.0):
    """The portable prototype of section 20: shared ROM noise table, no FFT.

    This, not the FFT render, is the right thing to gate the C against. The FFT
    prototype uses the **per-bin measured** residual spectrum; the ROM table is
    one shared shape, which costs 3.31 dB of mean per-bin deviation on its own --
    a difference that was rendered as `ab_portable_tuned.wav` and passed its
    listening test in section 20 (LISTEN group 7). Scoring the firmware against
    the FFT render would charge it for that accepted gap a second time and hide
    whatever the int16/constant-norm/IIR/LCG substitutions actually cost.
    """
    A20.RNG = np.random.default_rng(GEN.NOISE_SEED)
    y, _ = A20.synth_portable(rg, tg, tab, fs, tbl, noise_scale=noise,
                              jitter=JIT)
    return y


def rom_bytes(b):
    return ((len(b["q_wave"]) + len(b["q_noise"]) + len(b["q_env"])
             + len(b["q_win"])) * 2 + len(b["rpm"]) * 12 + len(b["pts"]) * 4)


class Take(object):
    """One flare render plus one idle render, and the numbers they are judged by."""

    def __init__(self, flare, idle, fs, tg, rg):
        self.flare, self.idle, self.rms = flare, idle, flare.std()
        # the octave bands are measured on the rms-matched signal, because that
        # is how every file is written and listened to. Left raw, a plain level
        # difference between two renders would show up as a spectral one in all
        # eight bands at once.
        self.o, self.edges = octaves(flare / (self.rms + 1e-30), fs)
        self.h = np.array(harmonicity(flare, fs, tg, rg))
        self.imp, self.fold = clatter(idle, fs, IDLE_RPM, t0=1.0, t1=7.0)

    def vs(self, other):
        return dict(oe=float(np.sqrt(((self.o - other.o) ** 2).mean())),
                    he=float(np.sqrt(((self.h - other.h) ** 2).mean())),
                    dimp=self.imp - other.imp, dfold=self.fold - other.fold)


class Ref(object):
    """Both prototypes: the one that was approved, and the one C is gated on."""

    def __init__(self):
        self.tab = tab = EngineTables(STRONG)
        self.fs = fs = tab.fs
        self.tg, self.rg = flare_traj()
        self.tgi = np.arange(0.0, IDLE_DUR, 0.002)
        self.rgi = np.full(len(self.tgi), IDLE_RPM)
        self.tbl = GEN.shared_noise(tab, GEN.NOISE_CYCLES)

        fb = fft_render(tab, self.tg, self.rg, fs)
        pb = por_render(tab, self.tbl, self.tg, self.rg, fs)
        fi = fft_render(tab, self.tgi, self.rgi, fs)
        pi = por_render(tab, self.tbl, self.tgi, self.rgi, fs)
        self.fft = Take(mask(fb, fft_render(tab, self.tg, self.rg, fs, MASK_AMT),
                             self.tg, self.rg, fs), fi, fs, self.tg, self.rg)
        self.por = Take(mask(pb, por_render(tab, self.tbl, self.tg, self.rg, fs,
                                            MASK_AMT),
                             self.tg, self.rg, fs), pi, fs, self.tg, self.rg)
        # the same pair with no masking at all, so part 3 can say whether a
        # difference is in the model or in the mask path (a block-rate gate with
        # an 8 Hz smoother, against `mask()`'s per-sample gate off the trajectory)
        self.por_base = Take(pb, pi, fs, self.tg, self.rg)

    def take(self, b, **kw):
        y = impl_render(b, self.tg, self.rg, self.fs, **kw)
        yi = impl_render(b, self.tgi, self.rgi, self.fs, **kw)
        return Take(y * (self.fft.rms / (y.std() + 1e-30)),
                    yi * (self.fft.idle.std() / (yi.std() + 1e-30)),
                    self.fs, self.tg, self.rg)


HEAD2 = ("  %-22s %7s | %-37s | %-11s"
         % ("", "", "vs portable prototype (the gate)", "vs FFT"))
HEAD = ("  %-22s %7s | %10s %5s %9s %9s | %10s"
        % ("candidate", "ROM", "octave rms", "harm", "idle imp", "idle fold",
           "octave rms"))


def row(label, rom, t, R):
    p, f = t.vs(R.por), t.vs(R.fft)
    print("  %-22s %6.1fk | %9.2f dB %5.2f %5.2f%+5.2f %5.2f%+5.2f | %9.2f dB"
          % (label, rom / 1024.0, p["oe"], p["he"], t.imp, p["dimp"],
             t.fold, p["dfold"], f["oe"]))


def main():
    R = Ref()
    print("Two prototypes, because they are not the same thing:")
    print("  FFT (a09)      per-bin measured residual spectrum, an inverse FFT")
    print("                 per engine cycle. This is `adopt_mask1200_x6.wav`,")
    print("                 the render the owner approved.")
    print("  portable (a20) one shared ROM noise shape, no FFT -- the structure")
    print("                 the firmware implements. Already listened to and")
    print("                 passed in section 20 (`ab_portable_tuned.wav`).")
    print("  the two differ by the shared noise shape alone: %.2f dB octave rms,"
          % R.por.vs(R.fft)["oe"])
    print("  idle fold %.2f vs %.2f dB. That gap is accepted, and it is *not* the"
          % (R.por.fold, R.fft.fold))
    print("  firmware's to pay -- so the gate below is the portable column.")

    print("\nPart 1 -- the noise table. It is read on the cycle-angular grid, so")
    print("its point count is a crank order (order pts/4) and the Hz it reaches")
    print("scales with rpm, exactly like the wave: order 128 is 5.3 kHz on")
    print("section 21's 2477-6301 rpm clip but 1.9 kHz at the 900 rpm idle.")
    print(HEAD2)
    print(HEAD)
    for cyc, npts in ((4, 512), (4, 1024), (2, 2048), (4, 2048), (8, 2048)):
        b = GEN.build(STRONG, GEN.DEFAULT_SCHEDULE, npts, cyc)
        row("noise %dx%-4d ord %d" % (cyc, npts, npts // 4), rom_bytes(b),
            R.take(b), R)

    print("\nPart 2 -- the wave point-count schedule, at the noise size part 1"
          "\npicks. `fold` is the number that matters at the idle: it is how much"
          "\nof the impulsiveness is cycle-locked, i.e. the cam clatter itself.")
    print(HEAD2)
    print(HEAD)
    rows = {}
    for name in ("flat256", "flat512", "low1024", "low2048"):
        b = GEN.build(STRONG, name, NPTS, NCYC)
        rows[name] = (b, R.take(b))
        row(name, rom_bytes(b), rows[name][1], R)

    print("\nPart 3 -- which C-side substitution costs what (schedule %s)" % PICK)
    b = rows[PICK][0]
    print("  %-36s %12s %10s" % ("", "octave rms", "fold @idle"))
    for label, kw in (("all of them (= the firmware)", {}),
                      ("  float tables instead of int16", {"quant": False}),
                      ("  per-block std instead of constant",
                       {"const_norm": False}),
                      ("  brick-wall LP instead of IIR", {"iir": False}),
                      ("  per-bin noise env (+6.4 kB)",
                       {"shared_env": False}),
                      ("  symmetric Hann instead of periodic",
                       {"win_sym": True})):
        t = R.take(b, **kw)
        print("  %-36s %11.2f %10.2f" % (label, t.vs(R.por)["oe"], t.fold))

    # Not a substitution: a bug that was in this script and in engine_v8.c, found
    # by reading rather than by any number going wrong -- and it made the numbers
    # LOOK better, which is why it is printed instead of described. Reading the
    # rising-half window table periodically crossfades the two noise readers over
    # the last 1/256 of every cycle, i.e. it adds broadband energy locked to the
    # cycle rate: precisely what `imp`/`fold` are built to detect.
    t = R.take(b, win_wrap=True)
    v = t.vs(R.por)
    print("  %-36s %11.2f %10.2f   (imp %+.2f, fold %+.2f vs portable)"
          % ("periodic wrap on the window (the bug)", v["oe"], t.fold,
             v["dimp"], v["dfold"]))

    t = R.take(b, mask_amt=1.0)
    print("  %-36s %11.2f %10.2f   (vs the unmasked prototype)"
          % ("no masking on either side", t.vs(R.por_base)["oe"], t.fold))

    # whatever is left after that line is the mask path, and the only free knob
    # in it is how hard the block-rate gate is smoothed. 8 Hz was a guess.
    print("  the gate smoother (block rate is %.0f Hz, so any of these is"
          " zipper-free):" % (R.fs / BLOCK))
    for fc in (4.0, 8.0, 20.0, 50.0, 200.0):
        t = R.take(b, gate_fc=fc)
        print("    %-34s %11.2f %10.2f"
              % ("%.0f Hz one-pole" % fc, t.vs(R.por)["oe"], t.fold))

    print("\nPart 4 -- octave bands")
    t = rows[PICK][1]
    print("   %-24s" % "band [Hz]"
          + "".join("%8d" % e for e in R.fft.edges[:-1]))
    for nm, v in (("FFT (approved)", R.fft.o), ("portable prototype", R.por.o),
                  ("firmware", t.o)):
        print("   %-24s" % nm + "".join("%8.1f" % q for q in v))
    print("   %-24s" % "firmware - portable"
          + "".join("%+8.1f" % q for q in (t.o - R.por.o)))
    print("   %-24s" % "portable - FFT (accepted)"
          + "".join("%+8.1f" % q for q in (R.por.o - R.fft.o)))

    # ---- the files ----------------------------------------------------------
    save(R.fft.flare, R.fs, "impl_ref_mask1200_x6.wav", ref_rms=R.fft.rms)
    save(R.fft.idle, R.fs, "impl_ref_idle_900.wav", ref_rms=R.fft.idle.std())
    save(R.por.flare, R.fs, "impl_por_mask1200_x6.wav", ref_rms=R.fft.rms)
    for name in ("flat256", PICK):
        save(rows[name][1].flare, R.fs, "impl_%s_mask1200_x6.wav" % name,
             ref_rms=R.fft.rms)
    yi = impl_render(rows[PICK][0], R.tgi, R.rgi, R.fs, drift=DRIFT_RPM)
    save(yi, R.fs, "impl_idle_900.wav", ref_rms=R.fft.idle.std())

    # the rolloff A/B: section 20 fitted 12 dB/oct above order 16 against the
    # *recording*, on a clip that never goes near the idle. It is off by default
    # (see gen_engine_v8_tables.py) and this is the file that says whether that
    # is right by ear.
    GEN.ROLLOFF = 12.0
    broll = GEN.build(STRONG, PICK, NPTS, NCYC)
    GEN.ROLLOFF = 0.0
    troll = R.take(broll)
    print("\n  with section 20's 12 dB/oct rolloff put back, for the A/B:")
    row("roll12 (was fitted)", rom_bytes(broll), troll, R)
    save(troll.flare, R.fs, "impl_roll12_mask1200_x6.wav", ref_rms=R.fft.rms)
    yr = impl_render(broll, R.tgi, R.rgi, R.fs, drift=DRIFT_RPM)
    save(yr, R.fs, "impl_idle_900_roll12.wav", ref_rms=R.fft.idle.std())

    print("\n  wrote impl_ref_{mask1200_x6,idle_900}.wav (FFT, approved),"
          "\n  impl_por_mask1200_x6.wav (portable prototype),"
          "\n  impl_{flat256,%s}_mask1200_x6.wav, impl_idle_900.wav,"
          "\n  impl_roll12_mask1200_x6.wav, impl_idle_900_roll12.wav" % PICK)


if __name__ == "__main__":
    main()
