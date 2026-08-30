"""Step 30: the two idle candidates differ by ONE number -- the noise ratio.

Measured in the tables, not guessed:

  corr(wave @750 rpm, wave @2125 rpm) = 1.000000

i.e. the extrapolated idle bin *is* the 2125 rpm waveform. So `idle_extrap_900`
and `idle_measured_900` have the same coherent waveform, the same phases and the
same noise *shape*; at matched rms the only thing that differs is how much
residual noise sits under it:

  noise / coherent, extrapolated bin  +65.7 dB   (trend fit evaluated at idle)
  noise / coherent, measured bin      +42.9 dB
  difference                          +22.8 dB

That also explains a26's octave measurement -- a near-constant +3.3 dB above
40 Hz, no change of shape.

Which is right cannot be decided from the data: the coherent trend says the
level falls 18.2 dB from 2125 to 750 rpm while the noise trend says the residual
*rises* 4.6 dB, and the recording that would arbitrate it is the noisy idle clip
we already refused to measure. But the owner's two reports point in opposite
directions -- the burble behind the extrapolated tail "sounds real" (that burble
is this noise) while the quieter measured idle was preferred at matched level --
so the answer is somewhere between, and it is one knob wide.

Writes, idle at 900 rpm with the adopted wobble (drift 40 rpm, jitter 0.018):
  idle_900_noise+0.wav   the measured ratio (= idle_measured_900)
  idle_900_noise+6.wav
  idle_900_noise+12.wav
  idle_900_noise+18.wav
  idle_900_noise+23.wav  the extrapolated ratio (= idle_extrap_900)
All rms-matched, so this is a timbre judgement and not a loudness one.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a20_portable_synth import octaves  # noqa: E402
from a22_idle_normal_model import measured_only  # noqa: E402

OUT = os.path.join(HERE, "out")
RPM, DRIFT, JITTER, DUR = 900.0, 40.0, 0.018, 8.0
STEPS = (0.0, 6.0, 12.0, 18.0, 22.8)
RNG = np.random.default_rng(3009)


def main():
    tab = EngineTables(os.path.join(OUT, measured_only()))
    fs = tab.fs
    tg = np.arange(0.0, DUR, 0.002)
    d = np.cumsum(RNG.normal(0.0, 1.0, len(tg)))
    d -= d.mean()
    d *= DRIFT / (np.abs(d).max() + 1e-12)
    rg = RPM + d

    ref = None
    print("idle at %.0f rpm, drift +-%.0f rpm, jitter %.3f, all rms-matched"
          % (RPM, DRIFT, JITTER))
    print("  noise    A-ish balance: octave bands re the 40 Hz band [dB]")
    for db in STEPS:
        y, _, _ = synth(rg, tg, tab, fs, noise_scale=10 ** (db / 20.0),
                        jitter=JITTER)
        k = int(0.05 * fs)
        y[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
        if ref is None:
            ref = y.std()
        y = y * (ref / (y.std() + 1e-12))
        name = "idle_900_noise+%g.wav" % round(db)
        write_wav(os.path.join(OUT, name), np.clip(y, -0.99, 0.99), fs, 3)
        o, edges = octaves(y, fs)
        print("  %+5.0f dB  " % db + "".join("%7.1f" % v for v in (o - o[0])))
    print("           " + "".join("%7d" % e for e in edges[:-1]) + "   Hz")
    print("\n  +0 dB is today's `idle_measured_900`, +23 dB is today's"
          " `idle_extrap_900`.\n  Pick the point where the burble is present but"
          " the hiss is not; that number\n  is a constant in the table, so it"
          " costs nothing on the target.")


if __name__ == "__main__":
    main()
