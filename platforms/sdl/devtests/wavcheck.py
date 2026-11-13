#!/usr/bin/env python3
"""Checks a 16-bit PCM WAV from --audio-file against an expectation. Standard library only.

    wavcheck.py FILE.wav silent|sound

silent: every sample is exactly zero (the app's own output has no noise floor; any hiss heard on
        the device comes after the app, in the codec or the analog path).
sound:  the level reaches -40 dBFS, nothing clips (no sample at full scale), and no single
        sample-to-sample step is larger than half of full scale (a click from a dropout or a
        discontinuity, which a sample or a synth note does not produce at this level).
Prints one line of measurements; exit 0 when the expectation holds, 1 when not, 2 on a bad file.
"""
import math
import struct
import sys
import wave

FULL = 32768.0


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("silent", "sound"):
        print(__doc__.strip())
        return 2
    try:
        with wave.open(sys.argv[1], "rb") as w:
            if w.getsampwidth() != 2:
                print("not 16-bit PCM")
                return 2
            ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
            data = w.readframes(n)
    except (OSError, wave.Error, EOFError) as e:
        print(f"bad wav: {e}")
        return 2
    samples = struct.unpack(f"<{len(data) // 2}h", data)
    if not samples:
        print("empty wav")
        return 2
    peak = max(abs(s) for s in samples)
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    clipped = sum(1 for s in samples if s in (32767, -32768))
    nonzero = sum(1 for s in samples if s)
    step = 0
    for c in range(ch):
        prev = samples[c]
        for s in samples[c::ch]:
            step = max(step, abs(s - prev))
            prev = s

    def db(v):
        return 20 * math.log10(v / FULL) if v > 0 else float("-inf")

    print(f"{n / rate:.2f} s, peak {db(peak):.1f} dBFS, rms {db(rms):.1f} dBFS, "
          f"nonzero {nonzero}, clipped {clipped}, max step {step}")
    if sys.argv[2] == "silent":
        ok = nonzero == 0
    else:
        ok = db(peak) >= -40 and clipped == 0 and step <= FULL / 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
