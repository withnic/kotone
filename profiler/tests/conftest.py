"""Shared pytest fixtures and deterministic synthetic WAV generators."""

from __future__ import annotations

import math
import random
import struct
import sys
from collections.abc import Sequence
from pathlib import Path

PROFILER_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PROFILER_ROOT.parent
if str(PROFILER_ROOT) not in sys.path:
    sys.path.insert(0, str(PROFILER_ROOT))


def write_wav_file(
    path: Path,
    samples_per_channel: Sequence[Sequence[float]],
    sample_rate: int = 48000,
    bits_per_sample: int = 16,
    ieee_float: bool = False,
) -> Path:
    """Write a standard RIFF/WAVE file in 16/24/32-bit PCM or 32-bit IEEE Float."""
    num_channels = len(samples_per_channel)
    assert num_channels >= 1
    num_frames = len(samples_per_channel[0])
    bytes_per_sample = bits_per_sample // 8
    block_align = num_channels * bytes_per_sample
    byte_rate = sample_rate * block_align
    audio_format = 3 if ieee_float else 1

    raw = bytearray()
    for i in range(num_frames):
        for ch in range(num_channels):
            val = float(samples_per_channel[ch][i])
            if ieee_float:
                raw.extend(struct.pack("<f", val))
            else:
                clamped = max(-1.0, min(1.0, val))
                if bits_per_sample == 16:
                    ival = max(-32768, min(32767, int(round(clamped * 32767.0))))
                    raw.extend(struct.pack("<h", ival))
                elif bits_per_sample == 24:
                    ival = max(-8388608, min(8388607, int(round(clamped * 8388607.0))))
                    if ival < 0:
                        ival += 0x1000000
                    raw.append(ival & 0xFF)
                    raw.append((ival >> 8) & 0xFF)
                    raw.append((ival >> 16) & 0xFF)
                elif bits_per_sample == 32:
                    ival = max(-2147483648, min(2147483647, int(round(clamped * 2147483647.0))))
                    raw.extend(struct.pack("<i", ival))
                else:
                    raise ValueError(f"Unsupported bit depth: {bits_per_sample}")

    data_size = len(raw)
    fmt_chunk = struct.pack(
        "<4sIHHIIHH",
        b"fmt ",
        16,
        audio_format,
        num_channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
    )
    data_header = struct.pack("<4sI", b"data", data_size)
    riff_size = 4 + len(fmt_chunk) + len(data_header) + data_size
    riff_header = struct.pack("<4sI4s", b"RIFF", riff_size, b"WAVE")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(riff_header + fmt_chunk + data_header + bytes(raw))
    return path


def synthesize_voice(
    *,
    sample_rate: int = 48000,
    voiced_sec: float = 6.0,
    silence_sec: float = 2.0,
    f0_hz: float = 130.0,
    voice_rms_dbfs: float = -22.0,
    noise_floor_dbfs: float = -60.0,
    mud_250hz_boost_db: float = 0.0,
    sibilance_burst_amp: float = 0.0,
    sibilance_freq_hz: float = 6500.0,
    seed: int = 42,
) -> list[float]:
    """Generate a deterministic synthetic voice with harmonics, noise floor, mud, and sibilance."""
    rng = random.Random(seed)
    n_voiced = int(round(voiced_sec * sample_rate))
    n_silence = int(round(silence_sec * sample_rate))
    n_total = n_voiced + n_silence

    noise_rms = 10.0 ** (noise_floor_dbfs / 20.0)
    # Uniform noise in [-a, a] has RMS = a / sqrt(3) -> a = noise_rms * sqrt(3)
    noise_amp = noise_rms * math.sqrt(3.0)

    # Harmonic spectrum approximating speech rolloff (~1/n^1.1) up to 4.5 kHz
    harmonics: list[tuple[float, float]] = []
    h = 1
    while h * f0_hz <= min(4800.0, sample_rate * 0.45):
        freq = h * f0_hz
        amp = 1.0 / (h**1.1)
        if mud_250hz_boost_db != 0.0 and 220.0 <= freq <= 290.0:
            amp *= 10.0 ** (mud_250hz_boost_db / 20.0)
        harmonics.append((freq, amp))
        h += 1

    # Normalize harmonic sum RMS to target voice_rms_dbfs
    raw_rms = math.sqrt(sum(0.5 * a * a for _, a in harmonics))
    target_rms = 10.0 ** (voice_rms_dbfs / 20.0)
    voice_scale = target_rms / max(raw_rms, 1e-12)
    two_pi_over_sr = 2.0 * math.pi / float(sample_rate)

    # Interleave speech & silence (leading silence + speech + trailing silence)
    lead_sil = n_silence // 2
    samples = [0.0] * n_total
    for i in range(n_total):
        s = rng.uniform(-noise_amp, noise_amp)
        if lead_sil <= i < lead_sil + n_voiced:
            t_idx = i - lead_sil
            # Speech tone
            v = 0.0
            for freq, amp in harmonics:
                v += amp * math.sin(two_pi_over_sr * freq * t_idx)
            s += v * voice_scale

            # Periodic 150ms sibilance bursts every 1.0s
            if sibilance_burst_amp > 0.0:
                pos_in_sec = (t_idx % sample_rate) / float(sample_rate)
                if 0.65 <= pos_in_sec < 0.80:
                    s += sibilance_burst_amp * math.sin(two_pi_over_sr * sibilance_freq_hz * t_idx)

        samples[i] = max(-0.999, min(0.999, s))
    return samples
