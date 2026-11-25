"""Write a short synthetic voice WAV (stdlib only) for smoke-testing packaged profilers.

Usage: python make_smoke_wav.py <out.wav>
"""

from __future__ import annotations

import math
import random
import struct
import sys
import wave

SR = 48000


def main() -> int:
    out = sys.argv[1]
    rng = random.Random(1)
    frames = bytearray()
    total = SR * 10
    for n in range(total):
        t = n / SR
        # 1 s room tone, 8 s of syllable-like voiced phrases, 1 s room tone
        voiced = 1.0 <= t < 9.0
        x = rng.uniform(-1.0, 1.0) * 0.001
        if voiced:
            env = 0.5 - 0.5 * math.cos(2.0 * math.pi * 3.0 * (t - 1.0))
            f0 = 130.0
            v = sum(math.sin(2.0 * math.pi * f0 * k * t) / k for k in range(1, 12))
            x += 0.08 * env * v
        frames += struct.pack("<h", max(-32768, min(32767, int(x * 32767))))
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(frames))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
