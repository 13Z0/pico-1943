#!/usr/bin/env python3
"""Compare two mono 16-bit WAVs of the same run: c1943_host's sound and
ym_compare's ymfm rendering of its YM2203 writes.

    python3 hosttest/wavcompare.py <c1943.wav> <ymfm.wav> [window_ms]

Reports, over windows of window_ms (default 50):
  - the level of each (RMS, dB) and how well the two level curves follow each
    other (correlation of the RMS in dB);
  - for the windows with sound in both, how often the strongest frequency is
    the same (within 2 %, or an octave apart), as a check of pitch, and how
    alike the whole spectra are (cosine similarity of the magnitudes, 1 for
    the same spectrum).
Needs numpy.
"""
import sys
import wave

import numpy as np


def read(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    return rate, data


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ra, a = read(sys.argv[1])
    rb, b = read(sys.argv[2])
    assert ra == rb
    win = int(ra * (float(sys.argv[3]) if len(sys.argv) > 3 else 50) / 1000)
    n = min(len(a), len(b)) // win * win
    a, b = a[:n].reshape(-1, win), b[:n].reshape(-1, win)

    rms_a = np.sqrt((a ** 2).mean(axis=1)) + 1e-9
    rms_b = np.sqrt((b ** 2).mean(axis=1)) + 1e-9
    db_a, db_b = 20 * np.log10(rms_a / 32768), 20 * np.log10(rms_b / 32768)
    loud = (db_a > -60) & (db_b > -60)
    print(f"{a.shape[0]} windows of {win} samples, {loud.sum()} with sound in both")
    print(f"level: c1943 {db_a[loud].mean():.1f} dB, ymfm {db_b[loud].mean():.1f} dB "
          f"(difference {np.mean(db_a[loud] - db_b[loud]):+.1f} dB, spread {np.std(db_a[loud] - db_b[loud]):.1f})")
    print(f"level curves: correlation {np.corrcoef(db_a[loud], db_b[loud])[0, 1]:.3f}")
    peak_a, peak_b = np.abs(a).max(), np.abs(b).max()
    print(f"peak: c1943 {peak_a:.0f}, ymfm {peak_b:.0f}")

    hann = np.hanning(win)
    freqs = np.fft.rfftfreq(win, 1 / ra)
    same = octave = 0
    total = 0
    mism = []
    sims = []
    for i in np.nonzero(loud)[0]:
        fa = np.abs(np.fft.rfft(a[i] * hann))
        fb = np.abs(np.fft.rfft(b[i] * hann))
        fa[:2] = fb[:2] = 0
        sims.append(float(fa @ fb / (np.linalg.norm(fa) * np.linalg.norm(fb) + 1e-9)))
        pa, pb = freqs[fa.argmax()], freqs[fb.argmax()]
        total += 1
        if abs(pa - pb) <= 0.02 * max(pa, pb) + freqs[1]:
            same += 1
        elif abs(pa - 2 * pb) <= 0.02 * pa + freqs[1] or abs(pb - 2 * pa) <= 0.02 * pb + freqs[1]:
            octave += 1
        elif len(mism) < 8:
            mism.append((i * win / ra, pa, pb))
    if total:
        print(f"strongest frequency: same in {100 * same / total:.0f} %, an octave apart in "
              f"{100 * octave / total:.0f} % of {total} windows")
        sims = np.array(sims)
        print(f"spectra: similarity median {np.median(sims):.3f}, 10th percentile {np.percentile(sims, 10):.3f}")
        for t, pa, pb in mism:
            print(f"  at {t:6.2f} s: c1943 {pa:7.1f} Hz, ymfm {pb:7.1f} Hz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
