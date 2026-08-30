"""Step 24: the start-up sequence -- starter, catch, flare, settle to idle.

The owner describes a real behaviour: right after the starter the engine flares up
briefly and drops back. Reproducing it needs a *timing* and an *RPM* curve.

The recording gives one of those and not the other. Three independent trackers
were tried on `astm_v8_cell_motor+idling.wav` and none of them locks:
  comb tracker (a04)      no lock at all, parks at the search floor
  order-4 ridge (a15)     wanders 1740-2726 rpm, which is not even plausible
  envelope ridge (a23)    score 4.2 dB, ridge wandering 30-55 Hz with no structure
The reason is visible in `out/envtrack_*.png`: the strongest lines (~200 Hz and
~520 Hz) are present at constant frequency during cranking *and* during idle, so
they are background, and what is left of the engine is too weak to track.

So this step measures what the recording *does* support -- the level envelope and
the spectral balance, which give the event times -- and takes the RPM values as a
design choice, rendered in several variants to be picked by ear.

Outputs:
  startup_<peak>rpm.wav   starter sample + catch + flare to <peak> + settle
  startup_timing.png      the measured envelope with the detected events
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from scipy import signal, ndimage  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav, source_dir  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a19_starter import adpcm_roundtrip, resample_to  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")
SRC = os.path.join(WAVDIR, "astm_v8_cell_motor+idling.wav")
STARTER = os.path.join(source_dir(), "astm_v8_cell_motor.wav")
STARTER_RATE = 12000            # owner's choice: 12 kHz ADPCM4 in external flash
IDLE_RPM = 900.0                # chosen by ear in the a22 listening test
IDLE_DRIFT = 40.0               # owner: wobble ON ("the design is old, it is not steady")
# First set (1300/1600/1900) was judged too low -- the owner's read is ~3000 rpm.
PEAKS = (2200.0, 2600.0, 3000.0, 3400.0)
RNG = np.random.default_rng(24601)


def band_energy(m, fs, lo, hi, win=0.020):
    b, a = signal.butter(4, [lo / (fs / 2), min(hi / (fs / 2), 0.99)], "band")
    e = signal.filtfilt(b, a, m) ** 2
    n = int(win * fs)
    return np.sqrt(ndimage.uniform_filter1d(e, n))


def measure_events():
    x, fs = read_wav(SRC)
    m = x.mean(axis=1)
    m -= m.mean()
    t = np.arange(len(m)) / fs
    lo = band_energy(m, fs, 40, 300, 0.030)
    mid = band_energy(m, fs, 300, 2000, 0.030)
    tot = band_energy(m, fs, 40, 8000, 0.030)
    ldb = 20 * np.log10(tot + 1e-9)
    ratio = 20 * np.log10(lo / (mid + 1e-12))

    print("--- measured level / balance (what the recording does support) ---")
    print("   t[s]   level[dBFS]   low/mid[dB]")
    for i in range(0, len(t), int(0.10 * fs)):
        print("  %5.2f     %7.1f       %+7.1f" % (t[i], ldb[i], ratio[i]))

    # When the engine catches: the owner's own cut of the starter ends exactly
    # there, and that is a better measurement than any detector on this recording.
    # (A gradient detector was tried and pinned itself to the edge of its search
    # window at 0.200 s, which is inside the cranking.)
    if os.path.exists(STARTER):
        xs, fss = read_wav(STARTER)
        i_catch = int(len(xs) / fss * fs)
        print("\n  catch time taken from the length of the starter-only cut")
    else:
        key = ndimage.uniform_filter1d(ldb + ratio, int(0.05 * fs))
        i_catch = int(np.argmax((np.gradient(key) * fs)[int(0.4 * fs):int(2.0 * fs)])
                      + 0.4 * fs)
    i_peak = int(np.argmax(ldb[i_catch:i_catch + int(1.0 * fs)]) + i_catch)
    settled = np.median(ldb[int(3.2 * fs):])
    above = np.where(ldb[i_peak:] > settled + 3.0)[0]
    i_settle = i_peak + (above[-1] if len(above) else int(0.5 * fs))
    print("\n  engine catches at   t = %.3f s" % (i_catch / fs))
    print("  level peaks at      t = %.3f s  (%.0f ms after the catch, %+.1f dB"
          " above the settled level)"
          % (i_peak / fs, 1000 * (i_peak - i_catch) / fs, ldb[i_peak] - settled))
    print("  back within 3 dB    t = %.3f s  (%.0f ms of flare in total)"
          % (i_settle / fs, 1000 * (i_settle - i_catch) / fs))
    print("  settled level       %.1f dBFS" % settled)
    # the flare is a level event as much as an RPM event (+14 dB here), so keep
    # the measured envelope, smoothed enough to lose the per-firing ripple but
    # not the flare shape, as a gain curve relative to the settled level
    lsm = ndimage.uniform_filter1d(ldb, int(0.060 * fs))
    return dict(fs=fs, t=t, ldb=ldb, ratio=np.clip(ratio, -60, 60),
                t_catch=i_catch / fs, t_peak=i_peak / fs, t_settle=i_settle / fs,
                flare_db=float(ldb[i_peak] - settled),
                level_gain=10 ** ((lsm - settled) / 20.0))


def starter_asset():
    path = STARTER if os.path.exists(STARTER) else SRC
    x, fs = read_wav(path)
    s = x.mean(axis=1)
    s -= s.mean()
    if path == SRC:
        s = s[:int(0.70 * fs)]
    lo = resample_to(s, fs, STARTER_RATE)
    g = 0.9 / (np.abs(lo).max() + 1e-12)
    dec = adpcm_roundtrip(np.round(lo * g * 32767).astype(np.int16)) / g
    print("\n  starter asset: %.3f s at %d Hz IMA-ADPCM4 = %.1f kB"
          % (len(lo) / STARTER_RATE, STARTER_RATE, len(lo) / 2 / 1024.0))
    return resample_to(dec, STARTER_RATE, fs), fs


def main():
    ev = measure_events()
    fs = ev["fs"]
    st, _ = starter_asset()
    tab = EngineTables(os.path.join(OUT, "tables_v3_measured.npz"))
    print("  engine tables: %d bins, %.0f..%.0f rpm (measured only)"
          % (len(tab.rpm), tab.rpm[0], tab.rpm[-1]))

    t_crank = len(st) / fs
    flare_up = ev["t_peak"] - ev["t_catch"]
    flare_dn = ev["t_settle"] - ev["t_peak"]
    print("\n  sequence: starter %.2f s -> catch -> flare up %.0f ms -> decay %.0f ms"
          " -> idle %.0f rpm" % (t_crank, 1000 * flare_up, 1000 * flare_dn, IDLE_RPM))

    print("\n  peak[rpm]  model's own rise   measured rise   gain applied")
    for peak in PEAKS:
        keys = [(0.0, IDLE_RPM * 0.55),               # barely turning, under the sample
                (t_crank - 0.05, IDLE_RPM * 0.75),
                (t_crank, IDLE_RPM * 0.9),
                (t_crank + flare_up, peak),
                (t_crank + flare_up + flare_dn * 0.45, IDLE_RPM * 1.10),
                (t_crank + flare_up + flare_dn, IDLE_RPM),
                (t_crank + flare_up + flare_dn + 2.5, IDLE_RPM)]
        tk = np.array([k[0] for k in keys])
        rk = np.array([k[1] for k in keys])
        tg = np.arange(0.0, tk[-1], 0.002)
        rg = np.interp(tg, tk, rk)
        # owner: the idle wobbles. Fade the drift in once the flare has settled,
        # so it does not fight the flare shape.
        drift = np.cumsum(RNG.normal(0.0, 1.0, len(tg)))
        drift -= drift.mean()
        drift *= IDLE_DRIFT / (np.abs(drift).max() + 1e-12)
        rg = rg + drift * np.clip((tg - ev["t_settle"]) / 0.5, 0.0, 1.0)
        eng, _, _ = synth(rg, tg, tab, fs, jitter=0.008)

        # the engine is inaudible under the starter, then fades in as it catches
        n = len(eng)
        fade = np.clip((np.arange(n) / fs - (t_crank - 0.10)) / 0.20, 0.0, 1.0)
        # Do NOT simply multiply by the measured envelope: the wavetables carry
        # their own per-bin gain, so a flare to 3000 rpm already raises the level
        # on its own. Applying the measured +14 dB on top would count it twice.
        # Measure what the model does by itself, then apply only the difference.
        tn = np.arange(n) / fs
        rms = np.sqrt(ndimage.uniform_filter1d(eng ** 2, int(0.060 * fs)))
        own_db = 20 * np.log10(rms + 1e-12)
        own_db -= np.median(own_db[int((tk[-1] - 1.5) * fs):])   # re settled
        want_db = 20 * np.log10(np.interp(tn, ev["t"], ev["level_gain"],
                                          left=1.0, right=1.0) + 1e-12)
        want_db = np.where(tn > ev["t_settle"], 0.0, want_db)
        add_db = np.clip(want_db - own_db, -6.0, ev["flare_db"])
        add_db = np.where(tn > ev["t_settle"], 0.0, add_db)
        i_pk = int(round(ev["t_peak"] * fs))
        print("    %5.0f        %+6.1f dB        %+6.1f dB       %+6.1f dB"
              % (peak, own_db[i_pk], ev["flare_db"], add_db[i_pk]))
        eng = eng * fade * 10 ** (add_db / 20.0)
        # the starter stops once the engine has caught
        stv = np.zeros(n)
        k = min(len(st), n)
        stv[:k] = st[:k]
        rel = np.clip(1.0 - (np.arange(n) / fs - t_crank) / 0.12, 0.0, 1.0)
        stv *= rel
        # match the starter to the engine's settled level, then apply the measured
        # flare in level on top of the wavetable's own gain curve
        eng_rms = eng[int((tk[-1] - 1.5) * fs):].std() + 1e-12
        stv *= (eng_rms / (stv[:k].std() + 1e-12)) * 0.9
        y = eng + stv
        y /= np.abs(y).max() + 1e-9
        write_wav(os.path.join(OUT, "startup_%.0frpm.wav" % peak), y * 0.9, fs, 3)
        print("   startup_%.0frpm.wav: %.2f s, flare peak %.0f rpm" % (peak, len(y) / fs, peak))

    fig, ax = plt.subplots(2, 1, figsize=(13, 6), constrained_layout=True)
    ax[0].plot(ev["t"], ev["ldb"], lw=0.8)
    for k, c, lab in (("t_catch", "g", "catch"), ("t_peak", "r", "level peak"),
                      ("t_settle", "b", "settled")):
        ax[0].axvline(ev[k], color=c, lw=1.0, label="%s %.3f s" % (lab, ev[k]))
    ax[0].legend(fontsize=8)
    ax[0].set_ylabel("level [dBFS]")
    ax[0].set_title("astm_v8_cell_motor+idling: measured level, with the detected events")
    ax[1].plot(ev["t"], ev["ratio"], lw=0.8, color="tab:purple")
    ax[1].set_ylabel("low/mid balance [dB]")
    ax[1].set_xlabel("time [s]")
    ax[1].grid(alpha=0.3)
    fig.savefig(os.path.join(OUT, "startup_timing.png"), dpi=85)
    plt.close(fig)
    print("\nwrote out/startup_*.wav and out/startup_timing.png")


if __name__ == "__main__":
    main()
