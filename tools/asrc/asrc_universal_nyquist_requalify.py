#!/usr/bin/env python3
"""Re-qualify all 45 down-conversion pairs against the WORST IN-BAND UNWANTED COMPONENT,
i.e. front-end alias AND rear-resampler phase-interpolation spur together.

WHY THIS EXISTS SEPARATELY FROM asrc_universal_nyquist_{model,planner,classify}.py.
Those rank pairs by FRONT-END ALIAS, which was the whole specification until the
44.1 -> 16 kHz hardware measurement showed the alias is not always the worst thing in
the band: alias -114.46 dBc, rear spur -105.7 dBc.  The adopted spec is

    protected band  0 .. 0.8125 x FN_out
    passband        +/-0.10 dB
    product         every unwanted component <= -100 dBc   (alias AND rear spur)
    engineering     <= -105 dBc, a DESIGN MARGIN and not a product limit

so a pair's spectral performance is max(alias, spur).  This driver composes the two
existing authorities rather than editing either: the alias/passband/CPU numbers come
from asrc_universal_nyquist_model, the spur from asrc_rear_spur_model.  The 45-pair
outputs of the older tools therefore do not move under this work.

VERDICT VOCABULARY (owner, 2026-09-15, study SS 30.2).  -105 dBc is design margin, not
specification, so failing it is not a product failure and must not be reported as one:

    PASS          worst case over the allowed clock offset <= -105 dBc
    MARGIN WATCH  -105 < worst <= -100 dBc   (product PASS, internal 5 dB margin MISS)
    FAIL          worst > -100 dBc

THE READ IS COLLISION-AWARE (owner, study SS 30.3).  The earlier revision of this driver
read the spur only at PREDICTED sideband positions, at one capture length, on a coarse
detune grid, and that is NOT an upper bound: at 160/147 + 97.5 ppm the k=1 minus and k=2
plus sidebands land 2.7 Hz apart and their power sums 2.7 dB above either order alone
(study SS 29.4).  Seven conditions are therefore all applied at once, per owner:

  1  the WHOLE protected band is searched, not the predicted positions
  2  a COLLISION SEARCH locates the offsets where two spur orders fall inside one main
     lobe -- analytically, from the closed-form sideband positions, so it costs nothing
  3  amplitudes are MAIN-LOBE POWER SUMS, so scalloping cannot flatter a reading and two
     nearly-coincident lines are summed rather than split
  4  the detune grid is DENSE, and refined around every coarse maximum and every
     predicted collision
  5  TWO CAPTURE LENGTHS (2048, the instrument's own, and 8192) -- a collision beats, so
     its reading depends on the analysis length and the worse of the two is taken
  6  the SIMULATOR adjudicates; the closed form only screens and is reported as a check
  7  the sweep spans the ALLOWED CLOCK OFFSET, +/-200 ppm by default

ONE SWEEP SERVES EVERY PAIR THAT SHARES A REAR RATIO.  With the tone pinned to the
protected edge the whole read depends on the rear STEP alone: fs_in = step x fs_out, the
tone sits at 0.203125/step of the rear input rate, and every frequency in the read is a
fraction of fs_out.  So the read is done once per distinct rear ratio, normalised to
fs_out = 1, and scaled back per pair -- verified bit-identical against the absolute-rate
reader.  That is what makes 7 conditions affordable: 45 pairs share 15 spur-bearing rear
ratios, so EVERY CANDIDATE of every pair is simulator-backed, not only the chosen chain.

THE TWO MECHANISMS ARE NOT DOUBLE COUNTED.  The model's alias figure already includes
the rear prototype's stopband (it multiplies every image branch by the rear response),
so it covers "input images that fold into the protected band", the rear's own imaging
included.  The spur is a different mechanism at a different frequency -- linear
interpolation between stored phase rows -- and the `cont` arm was verified to show
nothing at its sidebands (study SS 27.3).

CONTROLS CARRIED FROM THE MECHANISM PROOF.  Each pair's worst point is re-read at
-20 dBFS: the spur is signal-proportional, so its dBc must not move.  If it improves by
about 19 dB the reading is a fixed floor (Q31 quantisation / the rear filter's own
imaging), which is a real in-band component but a different mechanism with a different
cure, so it is named in the limiting-mechanism column instead of being called a spur.

CHOICE RULE -- A PRODUCT PLANNER, NOT A SPECTRUM MAXIMISER (owner, 2026-09-15, revised).
The first revision of this driver chose the cheapest chain reaching -105 dBc, and that is
wrong in a way the 44.1 -> 16 kHz row exposed: it abandoned a composition that is already
Hard-RT PASS on hardware (-103.39 dBc, i.e. product PASS with a thin internal margin) for a
129-tap direct LPF that reaches -109.90 dBc at 86.5 % of the 16-channel front-end budget --
spectrally better, and not buildable in the product.  A product planner must never trade a
chain that satisfies the product gate AND fits the CPU budget for one that satisfies neither.
So the order is:

  1  the PRODUCT spectral gate, -100 dBc, on the worst case over the allowed clock offset
  2  the CPU / Hard-RT budget -- taken as the best TIER a candidate can reach
     (16 ch within budget > 12 ch only > over), and never given up for step 3
  3  within that tier, prefer the -105 dBc internal margin
  4  within that, the cheapest in MACs, preferring a stage order that already ships

CPU is therefore a CONSTRAINT here and a REPORTED FLAG in the verdict columns: the
classification stays spectral (PASS / MARGIN WATCH / FAIL), because a pair's spectral
performance is a property of the pair, while what fits is a property of the profile.  The
pair's BEST ACHIEVABLE overall is reported twice -- unconstrained, and restricted to chains
inside the 16-channel budget -- so the cost of the internal margin is visible per pair
instead of being spent silently.
"""
from __future__ import annotations

import argparse
import csv
import dataclasses
import fractions
import json
import math
import pathlib
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import asrc_universal_nyquist_model as U      # noqa: E402
import asrc_rear_spur_model as S              # noqa: E402
import asrc_q31_resampler_model as Q          # noqa: E402
import asrc_asbuilt_sizing as A               # noqa: E402

RATES = (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000)

# THE TWO MODES (owner, 2026-09-15).  They are not two precisions of one number -- they answer
# different questions, and Phase F-2 happened because one mode number was read in the other
# column.  So the mode is stamped on the first line of the log, on the table header, and in a
# column of every CSV and Markdown table this driver writes.
#
#   QUALIFICATION  the AS-BUILT chains: the shipped coefficient sets where a route publishes one
#                  (asrc_universal_nyquist_model.current_chain, whose tap counts and band edges
#                  come from the generator constants), plus chains sized by the as-built rule
#                  (asrc_asbuilt_sizing) where the uniform spec can be held.  Asks: DOES THE
#                  IMAGE MEET THE SPEC.  This is the product authority, so it is the default.
#   EXPERIMENTAL   the optimizer search space: build_candidates x GROW_SCALES, every stage
#                  sized against what it alone folds in.  Asks: HOW CHEAP COULD THIS BE.  A
#                  future optimization candidate, never a product verdict.
MODE_QUALIFICATION = 'qualification'
MODE_EXPERIMENTAL = 'experimental'
MODE_LABEL = {MODE_QUALIFICATION: 'QUALIFICATION / AS-BUILT',
              MODE_EXPERIMENTAL: 'EXPERIMENTAL OPTIMIZER'}
MODE = MODE_QUALIFICATION

# THE SPECIFICATION PROFILE (owner, 2026-09-15, Phase H SS0/SS2).  A SECOND AXIS, ORTHOGONAL TO
# THE MODE, and the two must never be collapsed: the MODE says which QUESTION a run answers
# (is the image qualified / how cheap could it be), the PROFILE says which SPECIFICATION the
# coefficient set was signed off against.  The same physical pair legitimately has two different
# coefficient sets, one per profile, and the owner decision is that neither replaces the other.
#
#   LEGACY     the shipped coefficient set and the PER-RATE specification it was signed off
#              against.  asrc_coeff_provenance.py shows 15 of the 19 shipped sets are of this
#              kind: their design passband edge predates the uniform 0.8125 x FN rule and sits
#              BELOW it (asrc_universal_passband_decompose.py measures the deficit per stage).
#              A legacy row is not a defect and its past sign-off is not invalidated -- the
#              owner decision is explicitly "do not silently rewrite an existing profile".
#   UNIVERSAL  the uniform specification applied to EVERY supported row: protected band
#              0 .. 0.8125 x FN_out, passband +/-0.10 dB, every unwanted component <= -100 dBc.
#              A shipped set is eligible for this profile only if it MEETS the uniform passband;
#              where it does not, the row migrates to a chain sized by the as-built rule under
#              the uniform spec (asrc_asbuilt_sizing), and that migration is reported per row
#              in the `universal_migration` / `legacy_chain` columns rather than being silent.
#
# The profile is stamped in the same four places the mode is (log first line, table header, a
# column of every CSV, the Markdown header), for the same reason.
PROFILE_LEGACY = 'legacy'
PROFILE_UNIVERSAL = 'universal'
PROFILE_LABEL = {PROFILE_LEGACY: 'LEGACY / per-rate sign-off',
                 PROFILE_UNIVERSAL: 'UNIVERSAL / uniform spec on every row'}
PROFILE = PROFILE_LEGACY

PROTECT_RATIO = 0.8125          # owner-fixed
PASSBAND_LIMIT_DB = 0.10        # owner-fixed
GUARANTEE_DB = -100.0           # product
TARGET_DB = -105.0              # engineering margin, NOT a product limit
LAW_BAND_DB = 1.5               # the closed form's stated screening band
PROP_TOL_DB = 1.0               # a signal-proportional component holds its dBc to this

# THE TWO CPU CONSTANTS, KEPT APART ON PURPOSE (owner, 2026-09-15).
# Now that the CPU tier is step 2 of the choice rule, the constant behind it decides which chains
# a pair is even allowed, so calling either of these "the calibration" would hide a real choice:
#
#   1.90 cyc/MAC  CONSERVATIVE COMPOSED-CHAIN MODEL.  Its provenance is the shipping composed
#                 96 -> 32 kHz front end, whose two stages measured 1.75 and 2.09 cyc/MAC
#                 (asrc_universal_nyquist_model.CYC_PER_MAC) -- i.e. it already carries the ring
#                 push, intermediate buffer, phase schedule and s24-left scatter that a COMPOSED
#                 chain pays.  Applying it to a SINGLE-STAGE chain therefore charges composition
#                 overhead that chain does not have.
#   1.42 cyc/MAC  MEASURED SINGLE-STAGE DENSE-Q31 ANCHOR: the 1:1 LPF row at 140 us / 28.0 %
#                 (study SS 16), i.e. calibration factor 0.75 against the model above.  The study
#                 already quotes both (SS 21.1: "53.6 % @1.90 / 40.1 % @1.42"); this driver
#                 regressed on that by quoting one, and does not any more.
#   1.012 cyc/MAC the pure kernel slope, an absolute FLOOR and not a chain estimate.
#
# The default stays the conservative one, so the planner cannot be accused of buying margin with
# an optimistic constant -- but every pair reports its tier under BOTH, and the summary says how
# many pairs change tier, which is what separates "architecturally impossible at 16 ch" from
# "excluded by the conservative model".
CYC_MODEL_CONSERVATIVE = 1.90
CYC_MEASURED_SINGLE_STAGE = 1.42

LOBE_BINS = 8                   # the main-lobe power sum, +/- this many bins around the peak
# THE GUARDS ARE LOBE_BINS + A WHOLE MAIN LOBE, AND THAT IS NOT WHAT THE SS 29.4 PROBES USED.
# Kaiser-24's first null is at 7.94 bins (measured, not assumed) and its sidelobes are -193 dB, so
# +/-LOBE_BINS = +/-8 is exactly one main lobe -- which is why the instrument sums that span
# (analyze_meas_capture_alias.mainlobe_amplitude_dbfs, half=8) and why leakage beyond it is
# negligible.  It also means a component's power-sum window reaches 8 bins outward, so the ARGMAX
# has to stay 8 + 8 bins clear of the tone and of DC or the sum swallows THEIR main lobes.  The
# probes guarded 10 bins, i.e. the argmax was kept outside the tone's lobe but the summing window
# was not, and that is precisely how 441/160 read -10.90 dBc with its own -20 dBFS control at
# -117.08, and how 49/40 read -77.70 dBc from bin 826 of a tone at bin 832.  The probes' own four
# readings are unaffected -- their peaks sit hundreds of bins from either edge -- so section 29's
# numbers stand; this only removes readings those probes never had to make.
GUARD_BINS_TONE = LOBE_BINS + 10.0
GUARD_BINS_DC = LOBE_BINS + 10.0

ANCHOR = dict(rear_in=22050.0, fs_out=16000.0, tone=6468.0, phases=128,
              measured_dbc=-105.79, host_dbc=-105.78, step_bits=0x3FB0668C)
# THE ANCHOR IS EVALUATED AT ITS OWN L, ALWAYS -- never at the L being priced.  It was captured
# from an L=128 image, so reading it at another L compares the model against hardware that does
# not exist and fails by however much that L is worth: at L=256 the frozen step reads -117.73 dBc
# against the measured -105.79, i.e. -11.94 dB, which is the -12.04 dB law and not a defect.
# The anchor's job is to show the MODEL reproduces hardware; the sweep's job is to price a
# geometry.  Conflating the two would make every L != 128 run self-report a STOP.
# The hardware figure is a PEAK-BIN read of the capture (study SS 24.5: "-106.8 dBFS =
# -105.8 dBc"), so the peak-bin arm is the one held to 0.10 dB.  The collision-aware
# convention (whole band, main-lobe power sum) is reported beside it and held to 0.30 dB:
# it reads +0.16 dB differently on the same capture by construction, and quoting the two
# interchangeably across the convention change is exactly the mistake SS 29.2 warns about.
ANCHOR_TOL_PEAK_DB = 0.10
ANCHOR_TOL_POWERSUM_DB = 0.30


# --------------------------------------------------------------------- L, in one place ---
def set_phases(phases: int) -> None:
    """Point every authority at the same L.

    L lives in THREE modules and all three must move together or the two columns stop
    describing the same resampler:

      Q.L      sizes the Q31 coefficient table and the phase lookup (the arithmetic arm)
      chk.L    builds the rear prototype's phase rows, which is how
      U.L_REAR charges the REAR FILTER's own stopband inside the alias column

    S.rear_spur_dbc reads Q.L for the closed form.  U caches the interleaved prototype, so
    that cache is dropped.  Nothing here changes the firmware: L=128 is what ships, and a
    different L is a design question this driver only prices.
    """
    p = int(phases)
    if p < 8 or (p & (p - 1)) != 0:
        sys.exit("--phases must be a power of two >= 8 (got %d)" % p)
    Q.L = p
    U.chk.L = p
    U.L_REAR = p
    U._REAR_PROTO = None
    _ROWS_CACHE.clear()
    _READ_CACHE.clear()


# --------------------------------------------------------------- rear-ratio algebra ---
def ratio_terms(rear_in: float, fs_out: float):
    """(a, q) of the rear step in lowest terms, from EXACT rationals.

    Every catalogue rate is an integer number of Hz and the only non-integer rear rate a
    chain can produce is a 2/3 stage (fs_in * 2 / 3), so Fraction reproduces the ratio
    exactly instead of rediscovering it from a float.
    """
    f = fractions.Fraction(rear_in).limit_denominator(10 ** 6) / \
        fractions.Fraction(fs_out).limit_denominator(10 ** 6)
    return f.numerator, f.denominator


def exact_cancellation(a: int, q: int, phases: int | None = None) -> bool:
    """(L*a) mod q == 0: the inter-row weight never moves, so there is no modulation and
    no spur.  This is what asrc_app_config.h calls "an inter-phase blend weight of
    exactly zero" for 48 -> 48 / 48 -> 32 / 48 -> 24 / 44.1 -> 44.1."""
    return ((Q.L if phases is None else int(phases)) * a) % q == 0


def carrier_hz_exact(a: int, q: int, fs_out: float) -> float:
    return ((Q.L * a) % q) / q * fs_out


# ------------------------------------------------------------------ the simulated read ---
_ROWS_CACHE: dict[tuple, np.ndarray] = {}
_READ_CACHE: dict[tuple, tuple] = {}


def _rows_q31() -> np.ndarray:
    """The Q31 table the image would carry at the current geometry.

    Cached because it is rebuilt from the design formula on every call in the model
    modules, and the sweep calls the read tens of thousands of times.
    """
    key = (Q.L, Q.M, Q.FC, Q.BETA)
    if key not in _ROWS_CACHE:
        _ROWS_CACHE[key] = Q.poly_build_q31_shipped()
    return _ROWS_CACHE[key]


def _read(step: float, fs_out: float, tone_hz: float, protect_hz: float, nout: int,
          level_dbfs: float):
    """Worst unwanted component anywhere in 0..protect_hz, by main-lobe power sum.

    Only the q31 arm is run: it is what the target emits, and the lin64/cont arms exist to
    attribute a mechanism, which study SS 27.3 already did.

    NO NOTCH ON THE FUNDAMENTAL, DELIBERATELY.  Kaiser-24's sidelobes are -193 dB, so once the
    guards keep the summing window a whole main lobe clear of the tone there is nothing of the
    tone left to remove, and the reading stays in exactly the convention the hardware anchor and
    the SS 29.4 probes were read in.  (A projection was tried and is not merely unnecessary:
    getting its conjugate the wrong way round subtracts the mirror component and silently leaves
    the tone in place, which reads as a pass because every reference number reproduces.)
    """
    fs_in = float(step) * float(fs_out)
    n_in = Q.MH + int(math.ceil(float(step) * (nout + 2))) + Q.M + 8
    x = S.tone_slots(tone_hz, fs_in, n_in, level_dbfs, bits=31)
    ps, wbs, ws = S.phase_walk(step, 0.0, nout)
    y = np.asarray(S.run_q31(x, ps, wbs, ws, _rows_q31()), dtype=np.float64)
    w = np.kaiser(nout, 24.0)
    X = np.fft.rfft(y / 256.0 / S.FULL_SCALE * w)
    freqs = np.fft.rfftfreq(nout, 1.0 / fs_out)
    mag = np.abs(X) * 2.0 / w.sum()
    bin_hz = fs_out / nout
    keep = (freqs <= min(protect_hz, tone_hz) - GUARD_BINS_TONE * bin_hz) \
        & (freqs > GUARD_BINS_DC * bin_hz)
    if not keep.any():
        return None, None
    k = int(np.argmax(np.where(keep, mag, 0.0)))
    lo, hi = max(0, k - LOBE_BINS), min(X.size, k + LOBE_BINS + 1)
    amp = math.sqrt(4.0 * float(np.sum(np.abs(X[lo:hi]) ** 2))
                    / (nout * float(np.sum(w * w))))
    return 20.0 * math.log10(max(amp, 1e-300)) - level_dbfs, float(freqs[k])


def attribute(step: float, nout: int, level_dbfs: float = -1.0):
    """Which mechanism owns the worst component -- SS 29.4's control 1, at every ratio.

    Three arms over one and the same phase bookkeeping, read at the SAME bin by the same
    main-lobe power sum:

      cont   float64 prototype at the exact read phase, NO row grid  -> the rear filter's own
             imaging and its decimation alias.  At step > 1 the prototype's cutoff is
             0.465 x fs_in, i.e. ABOVE fs_out/2, so the rear stage does not band-limit its own
             decimation; whatever this arm shows is that, and asrc_universal_nyquist_model's
             alias column already charges the rear response, so it must not be called a spur.
      lin64  float64 rows, linear inter-row blend                    -> + phase interpolation
      q31    the shipping path                                       -> + Q31 arithmetic

    Returns (dBc per arm, frequency / fs_out).  Run once per rear ratio at its worst offset,
    not per read: `cont` rebuilds a float64 prototype row per output sample and is expensive.
    """
    edge = PROTECT_RATIO * 0.5
    r = S.evaluate(step, 1.0, edge, level_dbfs, nout, 0.0, edge,
                   arms=('q31', 'lin64', 'cont'), input_bits=31)
    freqs = r['freqs']
    w = np.kaiser(nout, 24.0)
    bin_w = 1.0 / nout
    keep = (freqs <= edge - GUARD_BINS_TONE * bin_w) & (freqs > GUARD_BINS_DC * bin_w)
    k = int(np.argmax(np.where(keep, r['mag']['q31'], 0.0)))
    lo, hi = max(0, k - LOBE_BINS), k + LOBE_BINS + 1
    out = {}
    for arm in ('q31', 'lin64', 'cont'):
        X = np.fft.rfft(np.asarray(r['y'][arm], dtype=np.float64) / 256.0 / S.FULL_SCALE * w)
        amp = math.sqrt(4.0 * float(np.sum(np.abs(X[lo:min(hi, X.size)]) ** 2))
                        / (nout * float(np.sum(w * w))))
        out[arm] = 20.0 * math.log10(max(amp, 1e-300)) - level_dbfs
    return out, float(freqs[k])


def read_worst(step: float, nout: int, level_dbfs: float = -1.0):
    """The same read, NORMALISED to fs_out = 1 with the tone at the protected edge.

    Returns (dBc, f / fs_out).  Depends only on (step, nout, level, geometry), which is
    why one sweep answers every pair sharing a rear ratio -- verified bit-identical
    against `_read` at absolute rates for the four readings SS 29 was stopped on.
    """
    key = (float(step), int(nout), float(level_dbfs), Q.L, Q.M, Q.FC, Q.BETA)
    hit = _READ_CACHE.get(key)
    if hit is None:
        edge = PROTECT_RATIO * 0.5
        hit = _read(step, 1.0, edge, edge, nout, level_dbfs)
        _READ_CACHE[key] = hit
    return hit


# ------------------------------------------------------------------- collision search ---
def sideband_positions(step: float, kmax: int) -> np.ndarray:
    """Folded sideband positions of the protected-edge tone, in units of fs_out.

    d = ((L * step * k) mod 1) is the interpolation error's carrier (asrc_rear_spur_model),
    so order k puts lines at |edge +/- k*d| folded into [0, 0.5].  Closed form, no
    simulation: this is what makes the collision search free.
    """
    edge = PROTECT_RATIO * 0.5
    st = float(np.float32(step))
    out = []
    for k in range(1, kmax + 1):
        d = (st * Q.L * k) % 1.0
        for sgn in (-1.0, 1.0):
            f = abs(edge + sgn * d) % 1.0
            out.append(1.0 - f if f > 0.5 else f)
    return np.sort(np.asarray(out, dtype=np.float64))


def collision_offsets(base: float, ppm_max: float, nout: int, kmax: int = 4,
                      grid_ppm: float = 0.1, keep: int = 6):
    """Detune offsets where two DIFFERENT spur orders land inside one main lobe.

    A collision is why the 160/147 family is not bounded by its own predicted-position
    reading: two orders inside one lobe sum in power, so the pair reads worse than either
    order and the reading beats with the analysis length.  Scanned analytically over the
    allowed offset, then the minima are returned for the simulator to refine.
    """
    lobe = float(LOBE_BINS) / float(nout)      # normalised main-lobe half width
    ppm = np.arange(-ppm_max, ppm_max + 0.5 * grid_ppm, grid_ppm)
    sep = np.empty(ppm.size, dtype=np.float64)
    for i, p in enumerate(ppm):
        f = sideband_positions(base * (1.0 + float(p) * 1.0e-6), kmax)
        d = np.diff(f)
        sep[i] = float(d.min()) if d.size else 1.0
    hit = sep < lobe
    out = []
    i = 0
    while i < hit.size:
        if not hit[i]:
            i += 1
            continue
        j = i
        while j + 1 < hit.size and hit[j + 1]:
            j += 1
        k = i + int(np.argmin(sep[i:j + 1]))
        out.append((float(sep[k]), float(ppm[k])))
        i = j + 1
    out.sort()
    return [p for _s, p in out[:keep]]


# ------------------------------------------------------------------------ ratio sweep ---
@dataclasses.dataclass
class Sweep:
    a: int
    q: int
    step: float
    worst_dbc: float | None
    worst_f_norm: float | None
    worst_ppm: float | None
    worst_nout: int | None
    exact_dbc: float | None
    prop_dbc: float | None          # the worst point re-read at -20 dBFS
    arms: dict | None               # q31 / lin64 / cont at the worst point
    collisions: int
    reads: int
    note: str

    @property
    def proportional(self) -> bool | None:
        if self.worst_dbc is None or self.prop_dbc is None:
            return None
        return abs(self.prop_dbc - self.worst_dbc) <= PROP_TOL_DB

    @property
    def owner(self) -> str | None:
        """Which mechanism the worst component belongs to, from the three arms."""
        if self.worst_dbc is None or not self.arms:
            return None
        q31, lin, cont = self.arms['q31'], self.arms['lin64'], self.arms['cont']
        if cont > q31 - 3.0:
            return "rear filter imaging/decimation alias"
        if lin > cont + 6.0:
            return "phase interpolation"
        if q31 > lin + 6.0:
            return "Q31 arithmetic"
        return "unattributed"


def sweep_ratio(a: int, q: int, nout_a: int, nout_b: int, ppm_max: float, ppm_step: float,
                ntop: int, ncoll: int, level: float = -1.0, kmax: int = 4) -> Sweep:
    """The 7-condition read for one rear ratio: coarse dense grid, then refinement around
    every coarse maximum AND every predicted collision, at two capture lengths."""
    base = float(a) / float(q)
    if exact_cancellation(a, q):
        return Sweep(a, q, base, None, None, None, None, None, None, None, 0, 0,
                     "exact cancellation ((L*a) mod q == 0): no modulation, no spur")
    reads = 0
    grid = list(np.arange(-ppm_max, ppm_max + 0.5 * ppm_step, ppm_step))
    if not any(abs(p) < 1.0e-12 for p in grid):
        grid.append(0.0)
    scan = []
    for p in grid:
        v, f = read_worst(base * (1.0 + float(p) * 1.0e-6), nout_a, level)
        reads += 1
        if v is not None:
            scan.append((v, float(p), f, nout_a))
    if not scan:
        return Sweep(a, q, base, None, None, None, None, None, None, None, 0, reads,
                     "no component resolvable at any offset")
    scan.sort(key=lambda t: -t[0])
    best = scan[0]

    # Coarse maxima, kept apart so one peak is not refined ntop times.
    peaks: list[float] = []
    for _v, p, _f, _n in scan:
        if all(abs(p - x) > 3.0 for x in peaks):
            peaks.append(p)
        if len(peaks) >= ntop:
            break
    coll = collision_offsets(base, ppm_max, nout_a, kmax=kmax, keep=ncoll)
    focus = list(peaks) + [c for c in coll if all(abs(c - p) > 0.5 for p in peaks)]

    for p0 in focus:
        for p in np.linspace(p0 - 1.0, p0 + 1.0, 21):
            for n in (nout_a, nout_b):
                v, f = read_worst(base * (1.0 + float(p) * 1.0e-6), n, level)
                reads += 1
                if v is not None and v > best[0]:
                    best = (v, float(p), f, n)
    # The coarse scan only ran at nout_a; give its strongest points the second length too.
    for _v, p, _f, _n in scan[:30]:
        v, f = read_worst(base * (1.0 + p * 1.0e-6), nout_b, level)
        reads += 1
        if v is not None and v > best[0]:
            best = (v, p, f, nout_b)

    ex = None
    for n in (nout_a, nout_b):
        v, _f = read_worst(base, n, level)
        reads += 1
        if v is not None and (ex is None or v > ex):
            ex = v
    prop, _f = read_worst(base * (1.0 + best[1] * 1.0e-6), best[3], -20.0)
    reads += 1
    arms, _at = attribute(base * (1.0 + best[1] * 1.0e-6), best[3])
    note = "worst at %+g ppm, n=%d" % (best[1], best[3])
    if coll:
        note += ", %d collision offset(s) probed" % len(coll)
    return Sweep(a, q, base, best[0], best[2], best[1], best[3], ex, prop, arms,
                 len(coll), reads, note)


# ------------------------------------------------------------------------ candidates ---
# The cascades the as-built rule is asked for, in qualification mode.  Only shapes the FIRMWARE
# could represent: integer /2 and /3 stages, at most three of them, then one 1:1 stage.  The
# rational 2/3 stage is deliberately absent -- a 2/3 stage followed by a 1:1 stage has no shipping
# precedent, which is exactly the kind of chain the experimental mode is for.
ASBUILT_COMBOS = ((), (2,), (3,), (2, 2), (2, 3), (3, 2), (2, 2, 2), (2, 2, 3), (2, 3, 2))


def shipped_combo(fs_in: int, fs_out: int, fp: float):
    """The DECIMATION FACTORS OF THE SHIPPED STRUCTURE, IN ITS OWN ORDER, or None.

    Read off the shipped chain the model builds -- not factored out of the ratio and not
    re-ordered -- because the order is a shipping fact (96 -> 32 kHz is /2 then 2/3, and the
    study has already been bitten once by inventing the other order).  A shipped route with a
    rational stage in it has no decimators-only re-spec, so it returns None and the pair is
    left to the 1:1-cascade sizing.
    """
    try:
        ship, tag = U.current_chain(int(fs_in), int(fs_out), fp)
    except Exception:                                              # noqa: BLE001
        return None
    if ship is None or tag == 'direct' or not ship.stages:
        return None
    if any(s.kind == 'rat' for s in ship.stages):
        return None
    if len(ship.stages) == 1 and ship.stages[0].d == 1:            # a 1:1 prototype row
        return None
    return tuple(int(s.d) for s in ship.stages)


def candidates_qualification(fs_in: float, fs_out: float, fp: float,
                            profile: str = PROFILE_LEGACY):
    """AS-BUILT candidates: the image first, then what the as-built rule would size.

    Two origins, and they are kept distinct because only one of them is in an image:

      shipped       the route publishes this chain today.  Tap counts and band edges come from
                    the generator constants via U.current_chain, so this row IS the image.
                    Note its specification may be a LEGACY per-rate one -- see
                    asrc_coeff_provenance.py; 15 of 19 shipped sets are.
      asbuilt-sized what asrc_asbuilt_sizing would produce for this pair under the UNIFORM spec
                    if a row were added.  Not in any image, but sized by the rule the generator
                    uses, so it is the honest answer to "what would qualification cost here".
      universal-    the SHIPPED STRUCTURE with its band edges re-specified to the uniform spec
      respec        and the smallest odd tap count that then clears the design gate.  Offered
                    ONLY in the universal profile, because it exists only to answer "what is
                    the CHEAPEST migration", and it is the minimum-topology-change answer: same
                    stage count, same decimation factors, same order, new band edges.  Where
                    the shipped structure decimates all the way to Fs_out it does not exist at
                    all (asrc_asbuilt_sizing.size_decimator_only says why), and that absence is
                    the finding, not a gap.
    """
    out = []
    try:
        ship, tag = U.current_chain(int(fs_in), int(fs_out), fp)
    except Exception:                                             # noqa: BLE001
        ship, tag = None, 'direct'
    if ship is not None and tag != 'direct' and ship.stages:
        out.append((ship, 1.0, False, 'shipped'))
        if profile == PROFILE_UNIVERSAL:
            combo = shipped_combo(int(fs_in), int(fs_out), fp)
            if combo:
                ch, _per = A.size_decimator_only(float(fs_in), combo, float(fs_out))
                if ch is not None:
                    out.append((ch, 1.0, False, 'universal-respec'))
    for combo in ASBUILT_COMBOS:
        d = 1
        for c in combo:
            d *= c
        if fs_in / d < fs_out:
            continue
        ch, _per = A.size_cascade(float(fs_in), combo, float(fs_out))
        if ch is not None:
            out.append((ch, 1.0, False, 'asbuilt-sized'))

    # NO FRONT END, WHERE THE INPUT CANNOT CARRY ANYTHING THAT FOLDS INTO THE PROTECTED BAND.
    # This is not a relaxation and it is not the old `direct` fall-through: it is admitted only
    # when the INPUT NYQUIST is already at or below the uniform fold edge,
    #
    #     fs_in / 2  <=  Fs_out - Fp
    #
    # in which case there is no input frequency whose image can land in [0, Fp] and an
    # anti-alias stage would have nothing to remove.  It is the three near-unity pairs
    # (12 -> 11.025, 24 -> 22.05, 48 -> 44.1 kHz, all ratio 1.0884) and nothing else, and they
    # need it: the as-built rule's 1:1 stage must reach Fstop = 0.59375 x Fs_out and therefore
    # has to run at 1.1875 x Fs_out or more, which at ratio 1.0884 it cannot -- so those pairs
    # had NO candidate at all and dropped out of the 45, which is how a supportable pair went
    # missing instead of being reported.  The rear resampler still has to hold the passband and
    # the spur, and it is evaluated for both here like any other chain.
    if fs_in * 0.5 <= (fs_out - fp) + 1e-9:
        out.append((U.Chain("no front end (input Nyquist %.4g kHz <= fold edge %.4g kHz)"
                            % (fs_in / 2000.0, (fs_out - fp) / 1000.0),
                            "direct", [], float(fs_in), float(fs_out)),
                    1.0, False, 'no-front-end-needed'))
    return out


def candidates_for(fs_in: float, fs_out: float, fp: float, mode: str = MODE_QUALIFICATION,
                   profile: str = PROFILE_LEGACY):
    """The candidate set for one pair, in one mode.

    QUALIFICATION delegates to candidates_qualification().  EXPERIMENTAL is the original search
    space: the model families, plus `2/3 -> 1:1 LPF`, which build_candidates cannot emit
    because it always puts a rational stage last.  It is the only way to reach the
    intermediate rear rate fs_in*2/3, and that rate is a real point on the spur/CPU
    trade (study SS 28.1), so leaving it out would hide a ceiling.  It is FLAGGED: no
    shipping chain uses that stage order (96 -> 32 is /2 then 2/3), so adopting it is a
    firmware-representation decision, not a free choice.
    """
    if mode == MODE_QUALIFICATION:
        return candidates_qualification(fs_in, fs_out, fp, profile)
    out = []
    sb_final = fs_out - fp
    for scale in U.GROW_SCALES:
        for ch in U.build_candidates(fs_in, fs_out, fp, scale=scale):
            out.append((ch, scale, False, 'experimental'))
        r23 = fs_in * 2.0 / 3.0
        if r23 > fs_out and (r23 - fp) > fp and sb_final > fp:
            s1 = U.make_stage("rat", fs_in, fp, r23 - fp, L=2, M=3, scale=scale)
            s2 = U.make_stage("lpf", r23, fp, sb_final, d=1, scale=scale)
            out.append((U.Chain("A2 2/3 + LPF@%.4gk" % (r23 / 1000.0), "rat+A2",
                                [s1, s2], r23, fs_out), scale, True, 'experimental'))
    return out


def limiting_mechanism(alias_db: float, spur: Sweep | None) -> str:
    """What sets this pair's number, named from evidence rather than from which column won.

    The rear column is not automatically "the spur": the three arms say whether the worst
    component is phase interpolation, the rear filter's own imaging (which the alias column
    already charges, so calling it a spur would mis-attribute it), or Q31 arithmetic.  And the
    -20 dBFS control says whether it is signal-proportional at all -- a floor does not improve
    with L, so at large L this distinction decides whether more phases buy anything.
    """
    if spur is None or spur.worst_dbc is None:
        return "front-end alias (no rear spur: exact cancellation)"
    if alias_db > spur.worst_dbc + 0.5:
        return "front-end alias"
    tag = "rear: " + (spur.owner or "unattributed")
    if spur.proportional is False:
        tag += ", NOT signal-proportional (a floor, not a spur)"
    if abs(alias_db - spur.worst_dbc) <= 0.5:
        tag += "; alias within 0.5 dB"
    return tag


# FRONT-END CPU BUDGETS, from asrc_universal_nyquist_model (already containing the 5 %
# engineering reserve): P1 = 16 ch at 48 kHz, P2 = 12 ch at 96 kHz.  A pair can be
# spectrally PASS and still not implementable, so the budget is reported as its own flag
# and NEVER folded into the verdict -- the classification here is spectral only.
def load_at(macs_per_sec: float, channels: int, cyc: float) -> float:
    return 100.0 * cyc * channels * macs_per_sec / U.CPU_HZ


def cpu_flag_at(macs_per_sec: float, cyc: float) -> str:
    if load_at(macs_per_sec, 16, cyc) <= U.BUDGET_P1_PCT:
        return "ok"
    if load_at(macs_per_sec, 12, cyc) <= U.BUDGET_P2_PCT:
        return "12ch-only"
    return "OVER"


def cpu_flag(macs_per_sec: float) -> str:
    """The tier the PLANNER selects on, at whatever constant this run was given."""
    return cpu_flag_at(macs_per_sec, U.CYC_PER_MAC)


# U.current_plan TABULATES ONLY 96 kHz AND 48 kHz SOURCES.  Every other source rate falls through
# to (1, 1, "direct"), which means "not in the shipping table" and NOT "hardware ran it with no
# front end".  Reading that fall-through as evidence would manufacture hardware backing for rows
# that have none -- so the tabulated sources are named here, and rows whose evidence comes from
# somewhere else are listed explicitly WITH their citation, because they cannot be derived.
SHIPPING_PLAN_SOURCES = (96000, 48000, 44100, 32000)
# 44100 and 32000 joined on 2026-09-15 (Phase G): U.current_plan now models the two
# APP_ASRC_LPF1TO1_PROTOTYPE rows, which the board is running, so a chain matching one of them
# is a chain hardware ran.  It is NOT part of the 81-cell matrix, and hardware_evidence says so
# for those rows rather than borrowing the matrix citation.
HW_MEASURED_ROWS = {
    # pair: (rear input rate that was measured, what to report)
    (44100, 16000): (22050.0, "PASS measured, rear 22.05 -> 16 k (study SS 24.5 / SS 25.1)"),
}


def hardware_evidence(fs_in: int, fs_out: int, chain, fp: float) -> str:
    """What HARDWARE says about this pair -- kept in its own column, never merged with the model.

    Derived from the tree, not asserted: U.current_plan / U.current_chain reproduce the shipping
    front-end table that the 81-cell rate-pair qualification was taken on, and
    U.QUAL_FENCE_DIRECT is the A1 fence's own list of direct-path pairs this image is NOT
    qualified for.  So the only claim made here is whether the chain the planner chose is THE SAME
    chain hardware ran -- which for most pairs it is not, and saying so is the point.
    """
    key = (int(fs_in), int(fs_out))
    if key in HW_MEASURED_ROWS:
        rear, what = HW_MEASURED_ROWS[key]
        return what if abs(chain.rear_in - rear) < 1.0 else \
            "HW measured a different rear rate (%.4g k) -- selected chain NOT measured" \
            % (rear / 1000.0)
    if key in U.QUAL_FENCE_DIRECT:
        return "A1 fence: direct path not qualified"
    if int(fs_in) not in SHIPPING_PLAN_SOURCES:
        return "no shipping plan for this source rate -- not measured"
    try:
        ship, tag = U.current_chain(int(fs_in), int(fs_out), fp)   # (Chain, tag)
    except Exception as exc:                                       # noqa: BLE001
        return "not measured (no shipping chain: %s)" % type(exc).__name__
    if ship is None or tag == "direct":
        return "shipping table has no front end for this pair -- not measured"
    if ship.describe() != chain.describe():
        return "shipping chain differs (%s) -- selected chain NOT measured" % tag
    # WHICH hardware evidence, named per row.  The 81-cell matrix was taken on the production
    # front-end table; the two prototype rows are a separate, later first light and must not
    # borrow its citation (Phase G).
    if int(fs_in) in (44100, 32000):
        return "same as shipping chain (APP_ASRC_LPF1TO1_PROTOTYPE row, not the 81-cell matrix)"
    return "same as shipping chain (81-cell HW matrix)"


def classify(overall_db: float) -> str:
    """Owner vocabulary, study SS 30.2.  -105 dBc is design margin, so missing it is
    MARGIN WATCH -- product PASS with the internal 5 dB margin unmet -- never FAIL."""
    if overall_db <= TARGET_DB:
        return "PASS"
    if overall_db <= GUARANTEE_DB:
        return "MARGIN WATCH"
    return "FAIL"


# ----------------------------------------------------------------------------- anchor ---
def check_anchor(verbose: bool = True) -> bool:
    """Reproduce the 44.1 -> 16 kHz hardware anchor before anything else runs.

    FOUR readings, because they answer different questions and SS 29.2 was written after
    two of them were once quoted as the same number:

      1  the frozen float32 step the board actually ran, PEAK-BIN at the predicted
         sideband -- the convention the hardware figure was read in, held to 0.10 dB;
      2  the same step under the COLLISION-AWARE convention (whole band, main-lobe power
         sum) -- the convention this driver's 45-pair sweep uses.  It reads a little
         differently on the same data by construction, so it is held to a looser band and
         the difference is printed rather than hidden;
      3  the NOMINAL exact rational, which must be CONSERVATIVE: at an exact ratio the
         error sequence and the tone are commensurate and add coherently in one bin, so
         it lands ~1.1 dB worse than the frozen step;
      4  the worst over the allowed offset, which is what the sweep reports.
    """
    a = ANCHOR
    requested = Q.L
    if Q.L != a['phases']:
        set_phases(a['phases'])                 # the geometry the capture was taken on
    fp_edge = PROTECT_RATIO * a['fs_out'] / 2.0
    tone = a['tone']
    frozen = float(np.frombuffer(int(a['step_bits']).to_bytes(4, 'little'),
                                 dtype=np.float32)[0])
    nominal = a['rear_in'] / a['fs_out']

    # Arm 1: peak bin restricted to the predicted sideband positions.
    r = S.evaluate(frozen, a['fs_out'], tone, -1.0, 2048, 0.0, tone,
                   arms=('q31',), input_bits=31)
    freqs, mag = r['freqs'], r['mag']['q31']
    bin_hz = a['fs_out'] / 2048.0
    win = max(3.0 * bin_hz, 20.0)
    near = np.zeros(freqs.size, dtype=bool)
    for _k, _s, f in S.spur_freqs(tone, frozen, a['fs_out'], kmax=2):
        near |= np.abs(freqs - f) <= win
    near &= (np.abs(freqs - tone) > 8.0 * bin_hz) & (freqs > 8.0 * bin_hz)
    peak = S.db(mag[int(np.argmax(np.where(near, mag, 0.0)))]) + 1.0
    d_peak = peak - a['measured_dbc']

    # Arm 2: the collision-aware convention on the same step.
    ps, ps_at = _read(frozen, a['fs_out'], tone, fp_edge, 2048, -1.0)
    d_ps = ps - a['measured_dbc']

    # Arms 3 and 4, in the sweep's own convention.
    ex, _ = _read(nominal, a['fs_out'], tone, fp_edge, 2048, -1.0)
    sw = sweep_ratio(*ratio_terms(a['rear_in'], a['fs_out']), 2048, 8192, 200.0, 1.0, 5, 6)

    ok_peak = abs(d_peak) <= ANCHOR_TOL_PEAK_DB
    ok_ps = abs(d_ps) <= ANCHOR_TOL_POWERSUM_DB
    ok_cons = ex >= a['measured_dbc'] - 0.10 and (ex - a['measured_dbc']) <= 3.0
    if verbose:
        print("ANCHOR 44.1 -> 16 kHz (rear %.0f -> %.0f Hz, capture tone %.0f Hz), L = %d"
              % (a['rear_in'], a['fs_out'], tone, Q.L))
        print("  hardware measured, peak bin              %+8.2f dBc  (study SS 24.5)"
              % a['measured_dbc'])
        print("  frozen step 0x%08X, peak@predicted    %+8.2f dBc   delta %+.2f dB -> %s"
              % (a['step_bits'], peak, d_peak, "PASS" if ok_peak else "FAIL"))
        print("  frozen step, collision-aware convention  %+8.2f dBc at %7.1f Hz  "
              "delta %+.2f dB -> %s"
              % (ps, ps_at, d_ps, "PASS" if ok_ps else "FAIL"))
        print("  nominal exact %-11s              %+8.2f dBc   delta %+.2f dB -> %s"
              % (str(fractions.Fraction(*ratio_terms(a['rear_in'], a['fs_out']))), ex,
                 ex - a['measured_dbc'],
                 "conservative, as required" if ok_cons else "NOT CONSERVATIVE"))
        if sw.worst_dbc is not None:
            print("  worst over +/-200 ppm, edge tone         %+8.2f dBc at %7.1f Hz  (%s)"
                  % (sw.worst_dbc, sw.worst_f_norm * a['fs_out'], sw.note))
        print("  anchor: %s" % ("reproduced" if (ok_peak and ok_ps and ok_cons)
                                else "NOT REPRODUCED -- STOP"))
        if requested != a['phases']:
            print("  (checked at L = %d, the geometry the capture was taken on; the sweep below "
                  "runs at L = %d)" % (a['phases'], requested))
        print()
    if requested != Q.L:
        set_phases(requested)
    return ok_peak and ok_ps and ok_cons


# ------------------------------------------------------------------------------ sweep ---
def enumerate_pairs(only: str | None):
    pairs = [(hi, lo) for hi in RATES for lo in RATES if hi > lo]
    if only:
        want = {tuple(int(v) for v in p.split(':')) for p in only.split(',')}
        pairs = [p for p in pairs if p in want]
    return pairs


def collect_candidates(pairs, mode: str = MODE_QUALIFICATION, verbose: bool = True,
                       profile: str = PROFILE_LEGACY):
    """Alias / passband / CPU for every candidate of every pair, plus the set of rear
    ratios the simulator will have to read.

    THE PASSBAND FILTER IS MODE-DEPENDENT, and that is deliberate.  In EXPERIMENTAL mode a
    candidate that misses the passband target is discarded: the mode is searching, and taps
    cannot make a lowpass flatter past its edge.  In QUALIFICATION mode a SHIPPED chain is
    never discarded -- discarding the image is how a row goes missing instead of failing.  It
    is kept, its passband error is carried in pb_fail, and the verdict reports it.
    """
    out = {}
    ratios = {}
    t0 = time.time()
    for fs_in, fs_out in pairs:
        fp = PROTECT_RATIO * fs_out / 2.0
        rows = []
        for ch, scale, flagged, origin in candidates_for(float(fs_in), float(fs_out), fp,
                                                         mode, profile):
            if U.step_verdict(ch.step) != "ok":
                continue
            r = U.evaluate(ch, float(fs_in), float(fs_out), fp, q31=True)
            pb_fail = r.passband_err_db > PASSBAND_LIMIT_DB
            if pb_fail and not (mode == MODE_QUALIFICATION and origin == 'shipped'):
                continue
            a, q = ratio_terms(ch.rear_in, float(fs_out))
            law = None if exact_cancellation(a, q) \
                else S.rear_spur_dbc(fp, ch.rear_in, Q.L)
            rows.append(dict(ch=ch, scale=scale, flagged=flagged, res=r, origin=origin,
                             pb_fail=pb_fail,
                             alias=r.worst_alias_db, law=law, a=a, q=q))
            ratios[(a, q)] = None
        out[(fs_in, fs_out)] = rows
    if verbose:
        print("candidates: %d pairs, %d distinct rear ratios, %.1f s"
              % (len(out), len(ratios), time.time() - t0))
    return out, ratios


def _cache_key(args) -> str:
    """Everything the simulated sweep depends on.  L, M, fc and beta are the geometry; the rest is
    the read.  The chain-selection rule and the CPU constant are deliberately NOT in here -- that
    is the whole value of the cache: re-selecting under another CPU constant or another priority
    order is then instant instead of another 13 minutes of simulation."""
    return "L%d_M%d_fc%g_b%g_n%d-%d_ppm%g-%g_top%d_coll%d" % (
        Q.L, Q.M, Q.FC, Q.BETA, args.nout, args.nout2, args.ppm_max, args.ppm_step,
        args.ntop, args.ncoll)


def load_cache(path: pathlib.Path, args):
    if path is None or not path.exists():
        return {}
    blob = json.loads(path.read_text(encoding='utf-8'))
    if blob.get('key') != _cache_key(args):
        print("  sweep cache %s is for a different geometry/read (%s) -- ignored"
              % (path, blob.get('key')))
        return {}
    out = {}
    for rec in blob['sweeps']:
        out[(rec['a'], rec['q'])] = Sweep(**rec)
    return out


def save_cache(path: pathlib.Path, args, ratios) -> None:
    if path is None:
        return
    path.write_text(json.dumps(
        {'key': _cache_key(args),
         'sweeps': [dataclasses.asdict(v) for v in ratios.values() if v is not None]},
        indent=1), encoding='utf-8')
    print("  sweep cache -> %s" % path)


def run_sweeps(ratios, args, verbose: bool = True):
    t0 = time.time()
    cached = load_cache(args.sweep_cache, args)
    order = sorted(ratios, key=lambda k: k[0] / k[1])
    for i, (a, q) in enumerate(order, 1):
        t1 = time.time()
        if (a, q) in cached:
            ratios[(a, q)] = cached[(a, q)]
            if verbose:
                print("  [%2d/%2d] %7d/%-7d step %9.6f  from cache: %s"
                      % (i, len(order), a, q, a / q,
                         "no spur" if cached[(a, q)].worst_dbc is None
                         else "%+.2f dBc" % cached[(a, q)].worst_dbc))
            continue
        sw = sweep_ratio(a, q, args.nout, args.nout2, args.ppm_max, args.ppm_step,
                         args.ntop, args.ncoll)
        ratios[(a, q)] = sw
        if verbose:
            if sw.worst_dbc is None:
                print("  [%2d/%2d] %7d/%-7d step %9.6f  %s"
                      % (i, len(order), a, q, a / q, sw.note))
            else:
                print("  [%2d/%2d] %7d/%-7d step %9.6f  worst %+8.2f dBc @ %.5f  %-46s"
                      % (i, len(order), a, q, a / q, sw.worst_dbc, sw.worst_f_norm, sw.note))
                print("           arms q31 %+8.2f  lin64 %+8.2f  cont %+8.2f -> %-38s "
                      "-20 dBFS %+8.2f (%s)  reads %5d  %.0f s"
                      % (sw.arms['q31'], sw.arms['lin64'], sw.arms['cont'], sw.owner,
                         sw.prop_dbc,
                         "proportional" if sw.proportional else "FLOOR",
                         sw.reads, time.time() - t1))
    if verbose:
        print("  sweeps done in %.0f s" % (time.time() - t0))
    save_cache(args.sweep_cache, args, ratios)
    return ratios


CPU_TIER = {'ok': 0, '12ch-only': 1, 'OVER': 2}


def pick_chain(rows, ratios, fs_out: float, mode: str = MODE_QUALIFICATION,
               profile: str = PROFILE_LEGACY):
    """The owner four-level product-planner order, applied to max(alias, SIMULATED spur).

    IN QUALIFICATION MODE THERE IS NOTHING TO CHOOSE when the route publishes a chain: the
    image is the image, and selecting a cheaper alternative in its place is what turned the
    45-pair table into a description of something that was never built (F-C).  The chosen row
    is then the shipped one, and the alternatives stay visible as `best` / `best_ok`.

    THE PROFILE CHANGES WHAT "the image" MEANS, and only that (Phase H SS2):

      LEGACY     the shipped chain is the pick, uniform-passband failure and all.  This is the
                 pre-Phase-H behaviour and it is the authority for what is signed off TODAY.
      UNIVERSAL  a shipped chain is the pick only if it MEETS the uniform passband.  Where it
                 does not, the pair migrates: the same four-level order is run over the
                 candidates that DO hold the uniform spec (as-built sized, i.e. sized by the
                 rule the generator uses), and the row is stamped `universal_migration` with
                 the legacy chain it replaces kept in its own column.  A row is never dropped
                 for failing -- if NO candidate holds the uniform passband the pair is picked
                 on the best available reading and flagged `no_uniform_candidate`, because a
                 row that goes missing is how a defect stops being counted.

    Step 2 is what makes this a product planner: the CPU tier is chosen BEFORE the internal
    margin is considered, so a chain that clears -100 dBc inside the 16-channel budget is
    never dropped in favour of one that clears -105 dBc outside it.  A tier is used rather
    than a boolean because some wide pairs have no candidate inside the 16-channel budget at
    all; the planner then states the best tier that exists instead of silently pretending
    one does.
    """
    for x in rows:
        sw = ratios[(x['a'], x['q'])]
        x['sweep'] = sw
        x['spur'] = None if sw.worst_dbc is None else sw.worst_dbc
        x['overall'] = x['alias'] if x['spur'] is None else max(x['alias'], x['spur'])
        x['cpu'] = cpu_flag(x['ch'].macs_per_sec)
        x['tier'] = CPU_TIER[x['cpu']]

    def key(x):
        return (x['flagged'], x['ch'].macs_per_sec)

    shipped = [x for x in rows if x.get('origin') == 'shipped']
    legacy_pick = shipped[0] if shipped else None
    if mode == MODE_QUALIFICATION and shipped \
            and (profile == PROFILE_LEGACY or not shipped[0]['pb_fail']):
        pick = shipped[0]
        best = min(rows, key=lambda x: (x['overall'], key(x)))
        ok16 = [x for x in rows if x['tier'] == 0]
        best_ok = min(ok16, key=lambda x: (x['overall'], key(x))) if ok16 else None
        pick['universal_migration'] = False
        pick['no_uniform_candidate'] = bool(pick['pb_fail'])
        pick['legacy_pick'] = legacy_pick
        return pick, best, best_ok

    # UNIVERSAL profile, qualification mode, shipped set does not hold the uniform passband:
    # choose among the candidates that DO.  collect_candidates already dropped every
    # non-shipped candidate whose passband misses, so the eligible pool is simply
    # "everything except the shipped row that was kept as evidence".
    migrating = (mode == MODE_QUALIFICATION and profile == PROFILE_UNIVERSAL
                 and shipped and shipped[0]['pb_fail'])
    pool_rows = [x for x in rows if not x['pb_fail']] if migrating else rows
    no_uniform = migrating and not pool_rows
    if no_uniform:
        pool_rows = rows

    # 1  product spectral gate.  If nothing clears it the pair FAILs whatever is chosen, so
    #    the best available reading is reported rather than an arbitrary survivor.
    rows = pool_rows
    passing = [x for x in rows if x['overall'] <= GUARANTEE_DB]
    if not passing:
        pick = min(rows, key=lambda x: (x['overall'], key(x)))
    else:
        # 2  best CPU tier that clears the product gate -- not negotiable against step 3
        tier = min(x['tier'] for x in passing)
        pool = [x for x in passing if x['tier'] == tier]
        # 3  prefer the internal -105 dBc margin INSIDE that tier
        margin = [x for x in pool if x['overall'] <= TARGET_DB]
        # 4  cheapest, shipping stage order first
        pick = min(margin or pool, key=key)

    best = min(rows, key=lambda x: (x['overall'], key(x)))
    ok16 = [x for x in rows if x['tier'] == 0]
    best_ok = min(ok16, key=lambda x: (x['overall'], key(x))) if ok16 else None
    pick['universal_migration'] = bool(migrating and not no_uniform)
    pick['no_uniform_candidate'] = bool(no_uniform)
    pick['legacy_pick'] = legacy_pick
    return pick, best, best_ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--phases', type=int, default=Q.L,
                    help='ASRC_POLY_L for this run (default %(default)s, the shipping value)')
    ap.add_argument('--nout', type=int, default=2048,
                    help='primary capture length (matches the hardware MEAS capture)')
    ap.add_argument('--nout2', type=int, default=8192,
                    help='second capture length; a collision beats, so it reads differently')
    ap.add_argument('--ppm-max', type=float, default=200.0,
                    help='allowed clock offset swept, +/- ppm (default %(default)s)')
    ap.add_argument('--ppm-step', type=float, default=1.0,
                    help='coarse detune grid step in ppm (default %(default)s)')
    ap.add_argument('--ntop', type=int, default=5,
                    help='coarse maxima refined per ratio')
    ap.add_argument('--ncoll', type=int, default=6,
                    help='predicted collision offsets refined per ratio')
    ap.add_argument('--cyc-per-mac', type=float, default=CYC_MODEL_CONSERVATIVE,
                    help='cycles per MAC the CPU TIER is judged on (default %(default)s, the '
                         'conservative composed-chain model; ' + str(CYC_MEASURED_SINGLE_STAGE)
                         + ' is the measured single-stage dense-Q31 anchor). Both are reported '
                           'per pair whatever this is set to.')
    ap.add_argument('--sweep-cache', type=pathlib.Path, default=None,
                    help='JSON of the per-ratio simulated sweeps: reused when the geometry and '
                         'read match, so re-selecting under another CPU constant or priority '
                         'order costs seconds instead of another full simulation')
    ap.add_argument('--hold-chains', type=pathlib.Path, default=None,
                    help='CSV from a previous run: reuse ITS chosen chains instead of '
                         're-selecting, so a change of L is read on the same front ends')
    ap.add_argument('--csv', type=pathlib.Path, default=None)
    ap.add_argument('--md', type=pathlib.Path, default=None)
    ap.add_argument('--mode', choices=(MODE_QUALIFICATION, MODE_EXPERIMENTAL),
                    default=MODE_QUALIFICATION,
                    help='which question this run answers (default %(default)s -- the product '
                         'authority).  qualification = the as-built chains, does the IMAGE meet '
                         'the spec.  experimental = the optimizer search space, how cheap COULD '
                         'it be.  Never read one mode number in the other mode column.')
    ap.add_argument('--profile', choices=(PROFILE_LEGACY, PROFILE_UNIVERSAL),
                    default=PROFILE_LEGACY,
                    help='which SPECIFICATION the coefficient sets are held to (default '
                         '%(default)s).  legacy = the shipped per-rate sign-off, reported as it '
                         'is.  universal = the uniform 0.8125 x FN / +/-0.10 dB / -100 dBc spec '
                         'on every row; a shipped set that misses the uniform passband MIGRATES '
                         'to an as-built-sized chain and the migration is a reported column.  '
                         'Orthogonal to --mode: the mode is the question, the profile is the spec.')
    ap.add_argument('--only', type=str, default=None,
                    help='comma-separated fs_in:fs_out pairs in Hz, for spot checks')
    args = ap.parse_args()

    set_phases(args.phases)
    global MODE, PROFILE
    MODE = args.mode
    PROFILE = args.profile
    print("PLANNER MODE = %s (%s) -- %s"
          % (MODE.upper(), MODE_LABEL[MODE],
             'product authority: does the IMAGE meet the spec'
             if MODE == MODE_QUALIFICATION
             else 'NOT a product verdict: how cheap COULD a chain be'))
    print("SPEC PROFILE = %s (%s) -- %s"
          % (PROFILE.upper(), PROFILE_LABEL[PROFILE],
             'shipped sets reported against their own per-rate sign-off; no migration'
             if PROFILE == PROFILE_LEGACY
             else 'uniform spec on every row; shipped sets that miss it migrate to as-built '
                  'sizing'))
    if MODE == MODE_EXPERIMENTAL and PROFILE == PROFILE_UNIVERSAL:
        print("  NOTE: the experimental search space is sized under the uniform spec already, so "
              "the profile\n        changes nothing here -- it is stamped so the column cannot be "
              "read as a product verdict.")
    U.CYC_PER_MAC = float(args.cyc_per_mac)
    print("CPU tier judged at %.3f cyc/MAC (%s); every pair also reports its tier at %.2f and "
          "%.2f, and HARDWARE evidence is a separate column"
          % (U.CYC_PER_MAC,
             "conservative composed-chain model" if U.CYC_PER_MAC == CYC_MODEL_CONSERVATIVE
             else "measured single-stage anchor" if U.CYC_PER_MAC == CYC_MEASURED_SINGLE_STAGE
             else "caller-supplied",
             CYC_MODEL_CONSERVATIVE, CYC_MEASURED_SINGLE_STAGE))
    print("L = %d phases, M = %d taps, fc = %g, beta = %g   "
          "protected band 0..%.4g x FN, passband +/-%.2f dB"
          % (Q.L, Q.M, Q.FC, Q.BETA, PROTECT_RATIO, PASSBAND_LIMIT_DB))
    print("verdicts: PASS <= %.0f dBc   MARGIN WATCH %.0f < x <= %.0f   FAIL > %.0f  "
          "(-105 is design margin, not specification)"
          % (TARGET_DB, TARGET_DB, GUARANTEE_DB, GUARANTEE_DB))
    print()
    anchor_ok = check_anchor()

    held = {}
    if args.hold_chains:
        with args.hold_chains.open(newline='', encoding='utf-8') as fh:
            for row in csv.DictReader(fh):
                held[(int(row['fs_in_hz']), int(row['fs_out_hz']))] = row['chain']
        print("holding %d chosen chains from %s" % (len(held), args.hold_chains))
        print()

    pairs = enumerate_pairs(args.only)
    cand, ratios = collect_candidates(pairs, MODE, profile=PROFILE)
    print("simulating %d rear ratios (one sweep serves every pair that shares it):"
          % len(ratios))
    run_sweeps(ratios, args)
    print()

    results = []
    for fs_in, fs_out in pairs:
        rows = cand[(fs_in, fs_out)]
        if not rows:
            print("  %6d -> %6d : NO CANDIDATE meets the passband/ring constraints"
                  % (fs_in, fs_out))
            continue
        pick, best, best_ok = pick_chain(rows, ratios, float(fs_out), MODE, PROFILE)
        if (fs_in, fs_out) in held:
            match = [x for x in rows if x['ch'].describe() == held[(fs_in, fs_out)]]
            if match:
                pick = match[0]
            else:
                print("  %6d -> %6d : held chain %r is not a candidate here -- re-selected"
                      % (fs_in, fs_out, held[(fs_in, fs_out)]))
        sw = pick['sweep']
        results.append(dict(
            fs_in=float(fs_in), fs_out=float(fs_out), fp=PROTECT_RATIO * fs_out / 2.0,
            pick=pick, best=best, best_ok=best_ok, a=pick['a'], q=pick['q'],
            d_hz=carrier_hz_exact(pick['a'], pick['q'], float(fs_out)),
            spur=pick['spur'],
            spur_at=None if sw.worst_f_norm is None else sw.worst_f_norm * fs_out,
            spur_ppm=sw.worst_ppm, spur_exact=sw.exact_dbc, spur_nout=sw.worst_nout,
            prop=sw.proportional, owner=sw.owner, arms=sw.arms, note=sw.note,
            # Did step 2 cost step 3?  True when the chosen chain misses the internal margin
            # while some chain in a WORSE CPU tier would have reached it.  This is exactly the
            # trade the revised rule refuses to make silently, so it is a reported column.
            margin_costs_cpu=(pick['overall'] > TARGET_DB
                              and any(x['overall'] <= TARGET_DB and x['tier'] > pick['tier']
                                      for x in rows)),
            law=pick['law'],
            law_delta=None if (pick['spur'] is None or pick['law'] is None)
            else pick['spur'] - pick['law'],
            overall=pick['overall'], cls=classify(pick['overall']),
            # THE THREE GATES, COUNTED SEPARATELY AND NEVER PRE-MERGED (Phase H SS6/SS10).  A
            # single QUALIFIED boolean cannot say whether a row is one coefficient re-spec away
            # from support or blocked by the architecture, and that difference is the whole
            # question this phase exists to answer.
            passband_err=pick['res'].passband_err_db,
            passband_pass=pick['res'].passband_err_db <= PASSBAND_LIMIT_DB,
            alias_pass=pick['overall'] <= GUARANTEE_DB,
            product_pass=(pick['res'].passband_err_db <= PASSBAND_LIMIT_DB
                          and pick['overall'] <= GUARANTEE_DB),
            internal_green=(pick['res'].passband_err_db <= PASSBAND_LIMIT_DB
                            and pick['overall'] <= TARGET_DB),
            universal_migration=bool(pick.get('universal_migration')),
            no_uniform_candidate=bool(pick.get('no_uniform_candidate')),
            legacy_chain=('' if pick.get('legacy_pick') is None
                          else pick['legacy_pick']['ch'].describe()),
            legacy_passband_err=(None if pick.get('legacy_pick') is None
                                 else pick['legacy_pick']['res'].passband_err_db),
            cpu=cpu_flag(pick['ch'].macs_per_sec),
            cpu_cons=cpu_flag_at(pick['ch'].macs_per_sec, CYC_MODEL_CONSERVATIVE),
            cpu_meas=cpu_flag_at(pick['ch'].macs_per_sec, CYC_MEASURED_SINGLE_STAGE),
            hw=hardware_evidence(fs_in, fs_out, pick['ch'], PROTECT_RATIO * fs_out / 2.0),
            mech=limiting_mechanism(pick['alias'], sw)))

    hdr = ("%7s %7s %9s %-28s %6s %8s %8s %8s %7s %9s %6s %-12s %-14s %s"
           % ("Fs_in", "Fs_out", "step", "chain [%s/%s]" % (MODE, PROFILE), "q", "alias", "law",
              "spur", "ppm", "OVERALL", "cpu16", "verdict", "origin",
              "cpu / limiting mechanism"))
    print(hdr)
    print("-" * len(hdr))
    for r in sorted(results, key=lambda x: -x['overall']):
        p = r['pick']
        print("%7.0f %7.0f %9.4f %-28s %6d %8.2f %8s %8s %7s %9.2f %5.1f%% %-12s %-14s %-9s "
              "%s%s%s"
              % (r['fs_in'], r['fs_out'], p['ch'].step, p['ch'].describe()[:28], r['q'],
                 p['alias'],
                 "-" if r['law'] is None else "%.2f" % r['law'],
                 "none" if r['spur'] is None else "%.2f" % r['spur'],
                 "-" if r['spur_ppm'] is None else "%+.1f" % r['spur_ppm'],
                 r['overall'], U.load_percent(p['ch'].macs_per_sec, 16),
                 r['cls'], p.get('origin', MODE), "cpu:" + r['cpu'], r['mech'],
                 ("  [PASSBAND %.4f dB > %.2f -- kept because it is the image]"
                  % (p['res'].passband_err_db, PASSBAND_LIMIT_DB)) if p.get('pb_fail') else "",
                 ("  [UNIVERSAL MIGRATION from %s, %.4f dB]"
                  % (r['legacy_chain'], r['legacy_passband_err']))
                 if r['universal_migration'] else
                 ("  [NO uniform-passband candidate exists for this pair]"
                  if r['no_uniform_candidate'] else "")))
        if p['flagged']:
            print("%56s[stage order has no shipping precedent]" % "")

    n = {'PASS': 0, 'MARGIN WATCH': 0, 'FAIL': 0}
    c = {'ok': 0, '12ch-only': 0, 'OVER': 0}
    for r in results:
        n[r['cls']] += 1
        c[r['cpu']] += 1
    print()
    print("pairs evaluated %d   PASS %d   MARGIN WATCH %d   FAIL %d"
          % (len(results), n['PASS'], n['MARGIN WATCH'], n['FAIL']))
    # THE THREE GATES, SEPARATELY (Phase H SS6).  Reported before any combined figure so the
    # combined one cannot be read as the only one.
    npb = sum(1 for r in results if r['passband_pass'])
    nal = sum(1 for r in results if r['alias_pass'])
    npr = sum(1 for r in results if r['product_pass'])
    ngr = sum(1 for r in results if r['internal_green'])
    print("UNIFORM_PASSBAND pass %d/%d   ALIAS pass %d/%d   PRODUCT pass (both) %d/%d   "
          "internal GREEN (both, -105) %d/%d"
          % (npb, len(results), nal, len(results), npr, len(results), ngr, len(results)))
    pbf = [r for r in results if not r['passband_pass']]
    if pbf:
        print("  pairs missing the uniform passband: %s"
              % ", ".join("%.4g->%.4g k (%.4f dB)"
                          % (r['fs_in'] / 1000.0, r['fs_out'] / 1000.0, r['passband_err'])
                          for r in sorted(pbf, key=lambda x: -x['passband_err'])))
    mig = [r for r in results if r['universal_migration']]
    if mig:
        print("UNIVERSAL MIGRATION -- %d pair(s) whose shipped set misses the uniform passband "
              "and whose universal profile therefore uses a different chain:" % len(mig))
        for r in sorted(mig, key=lambda x: (x['fs_in'], -x['fs_out'])):
            print("    %7.4g -> %-7.4g  legacy %-30s %7.4f dB  ->  universal %-30s %7.4f dB   "
                  "%s  cpu16 %5.1f %% (%s)"
                  % (r['fs_in'] / 1000.0, r['fs_out'] / 1000.0, r['legacy_chain'],
                     r['legacy_passband_err'], r['pick']['ch'].describe(), r['passband_err'],
                     r['cls'], U.load_percent(r['pick']['ch'].macs_per_sec, 16), r['cpu']))
    nouni = [r for r in results if r['no_uniform_candidate']]
    if nouni:
        print("NO CANDIDATE holds the uniform passband on %d pair(s) -- these are the only rows "
              "the uniform spec cannot reach: %s"
              % (len(nouni), ", ".join("%.4g->%.4g k" % (r['fs_in'] / 1000.0,
                                                        r['fs_out'] / 1000.0) for r in nouni)))
    print("front-end CPU budget of the chosen chain: within 16 ch %d   12 ch only %d   OVER %d"
          % (c['ok'], c['12ch-only'], c['OVER']))
    moved = [r for r in results if r['cpu_cons'] != r['cpu_meas']]
    print("CPU tier of the chosen chain under the two constants: %.2f -> %.2f cyc/MAC moves %d "
          "pair(s)%s" % (CYC_MODEL_CONSERVATIVE, CYC_MEASURED_SINGLE_STAGE, len(moved),
                         ":" if moved else " -- so the tiers here are not an artefact of the "
                                          "conservative constant"))
    for r in moved:
        print("    %7.4g -> %-7.4g  %-10s -> %-10s   (16 ch %.1f %% -> %.1f %%)   %s"
              % (r['fs_in'] / 1000.0, r['fs_out'] / 1000.0, r['cpu_cons'], r['cpu_meas'],
                 load_at(r['pick']['ch'].macs_per_sec, 16, CYC_MODEL_CONSERVATIVE),
                 load_at(r['pick']['ch'].macs_per_sec, 16, CYC_MEASURED_SINGLE_STAGE),
                 r['cls']))
    print("HARDWARE evidence for the SELECTED chain (model tier is a separate column):")
    hw = {}
    for r in results:
        hw[r['hw']] = hw.get(r['hw'], 0) + 1
    for k, v in sorted(hw.items(), key=lambda kv: -kv[1]):
        print("    %-48s %d" % (k, v))
    prod = [r for r in results if r['cls'] != 'FAIL' and r['cpu'] == 'ok']
    print("PRODUCT-VIABLE (product gate met AND inside the 16 ch budget): %d of %d   "
          "-- of those, %d also hold the -105 dBc internal margin"
          % (len(prod), len(results), sum(1 for r in prod if r['cls'] == 'PASS')))
    traded = [r for r in results if r['margin_costs_cpu']]
    if traded:
        print("the -105 dBc margin was DECLINED to keep the CPU tier on %d pair(s) (owner rule "
              "step 2 over step 3): %s"
              % (len(traded), ", ".join("%.4g->%.4g k" % (r['fs_in'] / 1000.0, r['fs_out'] / 1000.0)
                                        for r in traded)))

    big = [r['law_delta'] for r in results if r['law_delta'] is not None and r['q'] >= 32]
    small = [(r['fs_in'], r['fs_out'], r['q'], r['law_delta'])
             for r in results if r['law_delta'] is not None and r['q'] < 32]
    if big:
        print("closed form vs simulator, q >= 32 : simulator worse by up to %.2f dB, better by "
              "up to %.2f dB, over %d pairs" % (max(big), -min(big), len(big)))
        print("                                    (the law screens ONE order at ONE frequency; "
              "this read takes the worst of the")
        print("                                    whole band over the whole offset, so worse "
              "is the expected direction)")
    seen = set()
    for fi, fo, q, d in small:
        if q in seen:
            continue
        seen.add(q)
        print("closed form vs simulator, q = %-3d : %6.0f -> %6.0f  %+.2f dB "
              "(law is not a bound here -- simulator decides)" % (q, fi, fo, d))
    print("exact cancellation (no rear spur at all): %d pairs"
          % sum(1 for r in results if r['spur'] is None))
    own = {}
    for r in results:
        own[r['owner'] or 'no rear component'] = own.get(r['owner'] or 'no rear component', 0) + 1
    print("rear component attributed by the three arms: %s"
          % ", ".join("%s %d" % (k, v) for k, v in sorted(own.items(), key=lambda kv: -kv[1])))
    floors = [r for r in results if r['prop'] is False]
    if floors:
        print("worst component NOT signal-proportional (an in-band floor, not the spur): "
              "%d pairs -- %s" % (len(floors), ", ".join("%.0f->%.0f" % (r['fs_in'], r['fs_out'])
                                                         for r in floors)))
    mw = [r for r in results if r['cls'] == 'MARGIN WATCH']
    if mw:
        print()
        print("MARGIN WATCH (product PASS, internal 5 dB margin MISS), by rear ratio:")
        fam = {}
        for r in mw:
            fam.setdefault((r['a'], r['q']), []).append(r)
        for (a, q), rs in sorted(fam.items(), key=lambda kv: -max(x['overall'] for x in kv[1])):
            print("  %6d/%-6d  %+7.2f dBc  %d pair(s): %s"
                  % (a, q, max(x['overall'] for x in rs), len(rs),
                     ", ".join("%.4g->%.4g k" % (x['fs_in'] / 1000.0, x['fs_out'] / 1000.0)
                               for x in rs)))

    worst = max(results, key=lambda x: x['overall']) if results else None
    if worst:
        print()
        print("WORST pair: %.0f -> %.0f Hz   rear step %.4f   overall %+.2f dBc   %s   [%s]"
              % (worst['fs_in'], worst['fs_out'], worst['pick']['ch'].step,
                 worst['overall'], worst['mech'], worst['cls']))

    stops = []
    if not anchor_ok:
        stops.append("the 44.1 -> 16 kHz anchor was not reproduced")
    if n['FAIL']:
        stops.append("%d pair(s) exceed the -100 dBc product guarantee" % n['FAIL'])
    # A UNIFORM-PASSBAND MISS IS A STOP ONLY IN THE UNIVERSAL PROFILE.  In the legacy profile it
    # is the expected reading -- the shipped set was signed off against a per-rate spec and
    # saying so is the point of that profile, not a failure of this run.
    if PROFILE == PROFILE_UNIVERSAL and nouni:
        stops.append("%d pair(s) have NO candidate meeting the uniform passband" % len(nouni))
    # ASYMMETRIC ON PURPOSE.  Under the collision-aware read the simulator is EXPECTED to land
    # worse than the closed form: the law predicts ONE order at ONE frequency, while the read
    # takes the worst of the whole band over the whole offset range, where collisions, higher
    # orders and floors all add to it.  The alarming direction is the other one -- a simulator
    # reading BETTER than the law by more than the band would mean the sweep failed to find the
    # sideband the law says is there.
    missed = [r for r in results if r['law_delta'] is not None and r['q'] >= 32
              and r['law_delta'] < -LAW_BAND_DB]
    if missed:
        stops.append("%d pair(s) with q >= 32 read BETTER than the closed form by more than "
                     "%.1f dB -- the sweep may have missed their sideband"
                     % (len(missed), LAW_BAND_DB))
    print()
    print("STOP conditions: %s" % ("; ".join(stops) if stops else "none triggered"))

    if args.csv:
        with args.csv.open('w', newline='', encoding='utf-8') as fh:
            w = csv.writer(fh)
            w.writerow(['planner_mode', 'spec_profile', 'chain_origin',
                        'universal_migration', 'no_uniform_candidate',
                        'legacy_chain', 'legacy_passband_err_db',
                        'uniform_passband_pass', 'alias_pass', 'product_pass',
                        'internal_green',
                        'cpu_pct_16ch_1p90', 'cpu_pct_16ch_1p42',
                        'cpu_pct_12ch_1p90', 'cpu_pct_12ch_1p42',
                        'cpu_tier_1p90', 'cpu_tier_1p42',
                        'fs_in_hz', 'fs_out_hz', 'phases_L', 'protected_edge_hz', 'chain',
                        'rear_in_hz', 'rear_step', 'ratio_a', 'ratio_q', 'd_hz', 'taps',
                        'macs_per_sec', 'cpu_pct_16ch', 'cpu_pct_12ch', 'passband_err_db',
                        'front_end_alias_dbc', 'rear_spur_law_dbc', 'rear_spur_sim_dbc',
                        'rear_spur_exact_ratio_dbc', 'rear_spur_worst_ppm',
                        'rear_spur_worst_hz', 'rear_spur_worst_nout',
                        'rear_arm_q31_dbc', 'rear_arm_lin64_dbc', 'rear_arm_cont_dbc',
                        'rear_component_owner',
                        'signal_proportional', 'law_minus_sim_db', 'overall_dbc',
                        'verdict', 'cpu_budget', 'limiting_mechanism',
                        'best_achievable_overall_dbc', 'best_achievable_chain',
                        'best_within_16ch_overall_dbc', 'best_within_16ch_chain',
                        'margin_would_cost_cpu_tier',
                        'stage_order_precedent', 'spur_note'])
            for r in sorted(results, key=lambda x: (x['fs_in'], -x['fs_out'])):
                p, b, bo = r['pick'], r['best'], r['best_ok']
                w.writerow([MODE, PROFILE, p.get('origin', ''),
                            'yes' if r['universal_migration'] else 'no',
                            'yes' if r['no_uniform_candidate'] else 'no',
                            r['legacy_chain'],
                            '' if r['legacy_passband_err'] is None
                            else '%.4f' % r['legacy_passband_err'],
                            'yes' if r['passband_pass'] else 'NO',
                            'yes' if r['alias_pass'] else 'NO',
                            'yes' if r['product_pass'] else 'NO',
                            'yes' if r['internal_green'] else 'no',
                            '%.2f' % load_at(p['ch'].macs_per_sec, 16, CYC_MODEL_CONSERVATIVE),
                            '%.2f' % load_at(p['ch'].macs_per_sec, 16,
                                             CYC_MEASURED_SINGLE_STAGE),
                            '%.2f' % load_at(p['ch'].macs_per_sec, 12, CYC_MODEL_CONSERVATIVE),
                            '%.2f' % load_at(p['ch'].macs_per_sec, 12,
                                             CYC_MEASURED_SINGLE_STAGE),
                            r['cpu_cons'], r['cpu_meas'],
                            int(r['fs_in']), int(r['fs_out']), Q.L, '%.4f' % r['fp'],
                            p['ch'].describe(), '%.4f' % p['ch'].rear_in,
                            '%.6f' % p['ch'].step, r['a'], r['q'], '%.3f' % r['d_hz'],
                            '|'.join(str(t) for t in p['ch'].taps_list),
                            int(p['ch'].macs_per_sec),
                            '%.2f' % U.load_percent(p['ch'].macs_per_sec, 16),
                            '%.2f' % U.load_percent(p['ch'].macs_per_sec, 12),
                            '%.4f' % p['res'].passband_err_db,
                            '%.2f' % p['alias'],
                            '' if r['law'] is None else '%.2f' % r['law'],
                            '' if r['spur'] is None else '%.2f' % r['spur'],
                            '' if r['spur_exact'] is None else '%.2f' % r['spur_exact'],
                            '' if r['spur_ppm'] is None else '%g' % r['spur_ppm'],
                            '' if r['spur_at'] is None else '%.2f' % r['spur_at'],
                            '' if r['spur_nout'] is None else int(r['spur_nout']),
                            '' if not r['arms'] else '%.2f' % r['arms']['q31'],
                            '' if not r['arms'] else '%.2f' % r['arms']['lin64'],
                            '' if not r['arms'] else '%.2f' % r['arms']['cont'],
                            r['owner'] or '',
                            '' if r['prop'] is None else ('yes' if r['prop'] else 'NO'),
                            '' if r['law_delta'] is None else '%.2f' % r['law_delta'],
                            '%.2f' % r['overall'], r['cls'], r['cpu'], r['mech'],
                            '%.2f' % b['overall'], b['ch'].describe(),
                            '' if bo is None else '%.2f' % bo['overall'],
                            '' if bo is None else bo['ch'].describe(),
                            'yes' if r['margin_costs_cpu'] else 'no',
                            'none (2/3 before 1:1)' if p['flagged'] else 'shipping',
                            r['note']])
        print("csv  -> %s" % args.csv)

    if args.md:
        L = []
        L.append("**PLANNER MODE = %s (%s).**  %s\n"
                 % (MODE.upper(), MODE_LABEL[MODE],
                    "Product authority: does the IMAGE meet the spec."
                    if MODE == MODE_QUALIFICATION
                    else "NOT a product verdict -- a search for a cheaper chain."))
        L.append("**SPEC PROFILE = %s (%s).**  %s\n"
                 % (PROFILE.upper(), PROFILE_LABEL[PROFILE],
                    "Shipped sets are reported against their own per-rate sign-off."
                    if PROFILE == PROFILE_LEGACY
                    else "Uniform spec on every row; a shipped set that misses it MIGRATES and "
                         "the migration is a column."))
        L.append("L = %d phases.  PASS <= %.0f dBc / MARGIN WATCH %.0f..%.0f / FAIL > %.0f "
                 "(-105 dBc is design margin, not specification).\n"
                 % (Q.L, TARGET_DB, TARGET_DB, GUARANTEE_DB, GUARANTEE_DB))
        L.append("The three gates are separate columns on purpose: `pb` is the uniform "
                 "passband, `alias` the -100 dBc spectral gate, `prod` both together.\n")
        L.append("| mode | profile | origin | mig | Fs_in | Fs_out | rear step | chain | q | "
                 "alias dBc | spur sim | worst ppm | **overall** | pb dB | pb | alias | prod | "
                 "cpu16 @1.90 | cpu16 @1.42 | cpu budget | verdict | limiting mechanism |")
        L.append("|---|---|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---|---|---|"
                 "---:|---:|---|---|---|")
        for r in sorted(results, key=lambda x: (x['fs_in'], -x['fs_out'])):
            p = r['pick']
            L.append("| %s | %s | %s | %s | %.0f | %.0f | %.4f | `%s` | %d | %.2f | %s | %s | "
                     "**%.2f** | %.4f | %s | %s | %s | %.1f %% | %.1f %% | %s | %s | %s |"
                     % (MODE, PROFILE, p.get('origin', ''),
                        "**yes**" if r['universal_migration'] else "no",
                        r['fs_in'], r['fs_out'], p['ch'].step, p['ch'].describe(), r['q'],
                        p['alias'],
                        "none" if r['spur'] is None else "%.2f" % r['spur'],
                        "-" if r['spur_ppm'] is None else "%+g" % r['spur_ppm'],
                        r['overall'], r['passband_err'],
                        "PASS" if r['passband_pass'] else "**FAIL**",
                        "PASS" if r['alias_pass'] else "**FAIL**",
                        "PASS" if r['product_pass'] else "**FAIL**",
                        load_at(p['ch'].macs_per_sec, 16, CYC_MODEL_CONSERVATIVE),
                        load_at(p['ch'].macs_per_sec, 16, CYC_MEASURED_SINGLE_STAGE),
                        r['cpu'], r['cls'], r['mech']))
        args.md.write_text("\n".join(L) + "\n", encoding='utf-8')
        print("md   -> %s" % args.md)

    return 0 if not stops else 3


if __name__ == '__main__':
    raise SystemExit(main())
