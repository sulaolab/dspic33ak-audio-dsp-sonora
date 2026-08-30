import os
import wave

import numpy as np


def source_dir():
    """Directory holding the source recordings.

    There is deliberately NO default.  The recordings are not in this repository
    -- they are large, and they are a customer's -- so any default would be a
    path on one particular machine, which is what this replaces.  Point
    ASTM_V8_WAVDIR at wherever you keep them.
    """
    d = os.environ.get("ASTM_V8_WAVDIR")
    if not d:
        raise SystemExit(
            "ASTM_V8_WAVDIR is not set.  Point it at the directory holding "
            "the source recordings; they are not part of this repository.")
    if not os.path.isdir(d):
        raise SystemExit("ASTM_V8_WAVDIR is not a directory: %s" % d)
    return d


def read_wav(path):
    """Return (float64 [-1,1] samples of shape (n, ch), fs)."""
    w = wave.open(path)
    ch, sw, fs, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw == 3:
        a = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / 8388608.0
    elif sw == 2:
        v = np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
    elif sw == 4:
        v = np.frombuffer(raw, dtype='<i4').astype(np.float64) / 2147483648.0
    else:
        raise SystemExit("unsupported sample width %d" % sw)
    return v.reshape(-1, ch), fs


def write_wav(path, x, fs, sampwidth=2):
    """x: float array, (n,) mono or (n, ch); clipped to [-1, 1)."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1:
        x = x[:, None]
    ch = x.shape[1]
    x = np.clip(x, -1.0, 0.999969)
    if sampwidth == 2:
        raw = (x * 32767.0).astype("<i2").tobytes()
    elif sampwidth == 3:
        v = (x * 8388607.0).astype(np.int32).ravel()
        b = np.empty((v.size, 3), dtype=np.uint8)
        b[:, 0] = v & 0xFF
        b[:, 1] = (v >> 8) & 0xFF
        b[:, 2] = (v >> 16) & 0xFF
        raw = b.tobytes()
    else:
        raise SystemExit("sampwidth %d" % sampwidth)
    w = wave.open(path, "wb")
    w.setnchannels(ch)
    w.setsampwidth(sampwidth)
    w.setframerate(int(fs))
    w.writeframes(raw)
    w.close()
