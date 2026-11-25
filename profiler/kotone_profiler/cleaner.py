"""Lip-noise (click) and breath detection shared with the real-time Voice Cleaner stage.

The detectors here mirror ``plugin/src/dsp/voice_cleaner.cpp`` exactly (same FIR click
detector, same RBJ band-pass biquads, same boxcar windows) so that thresholds learned from a
recording behave identically inside the OBS plugin. See docs/02_data_modeling.md Section 4.1.

NumPy is used when available; a pure standard-library fallback produces the same results
(only slower).
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

from .wav_io import HAS_NUMPY

if HAS_NUMPY:
    import numpy as np  # type: ignore

# ---- Shared constants (must match plugin/include/kotone/voice_cleaner.hpp) ----
CLICK_FAST_MS = 0.5
CLICK_SLOW_MS = 20.0
CLICK_DECAY_MS = 1.0
CLICK_DECAY_DB = 6.0
CLICK_HOLD_MS = 2.0
# A click is broadband: its 2nd-difference energy must be close to its plain energy.
# Voiced-speech transients (glottal pulses, onsets) are dominated by low frequencies and sit
# 15-30 dB lower. The ratio of band-limited content scales with (f / fs)^4, hence the
# sample-rate compensation.
CLICK_BROADBAND_DB_AT_48K = -10.0
BREATH_WINDOW_MS = 20.0
BREATH_LOW_HZ = 400.0
BREATH_HIGH_HZ = 4000.0
BREATH_BAND_Q = 0.6
ANALYSIS_HOP_MS = 10.0

# Labeling rules for learning (profiler only)
_VOICE_TILT_MAX_DB = -6.0
_BREATH_MIN_RUN_MS = 100.0
_TILT_MARGIN_DB = 3.0
_LEVEL_CAP_BELOW_VOICE_DB = 6.0
_DEFAULT_HANGOVER_MS = 80.0
_MIN_HANGOVER_MS = 40.0
_MAX_HANGOVER_MS = 150.0


@dataclass(frozen=True)
class CleanerAnalysis:
    """Detected lip-noise / breath statistics and learned Voice Cleaner thresholds."""

    click_count: int
    click_rate_per_min: float
    breath_count: int
    breath_duration_sec: float
    breath_level_p90_dbfs: float | None
    voice_level_p10_dbfs: float
    voice_tilt_p95_db: float
    learned_tilt_threshold_db: float
    learned_level_ceiling_dbfs: float
    learned_hangover_ms: float
    excluded_frame_mask: tuple[bool, ...]  # per analysis frame (10 ms hop)


def bandpass_coeffs(sample_rate: float, freq_hz: float, q: float) -> tuple[float, ...]:
    """RBJ constant 0 dB peak band-pass biquad (b0, b1, b2, a1, a2), normalized by a0."""
    f = max(20.0, min(freq_hz, sample_rate * 0.48))
    w0 = 2.0 * math.pi * f / sample_rate
    alpha = math.sin(w0) / (2.0 * max(q, 0.05))
    a0 = 1.0 + alpha
    return (alpha / a0, 0.0, -alpha / a0, (-2.0 * math.cos(w0)) / a0, (1.0 - alpha) / a0)


def breath_band_freqs(sample_rate: float) -> tuple[float, float]:
    """Low / high detector band centers, kept safely below Nyquist for low sample rates."""
    return (BREATH_LOW_HZ, min(BREATH_HIGH_HZ, sample_rate * 0.3))


def ms_to_samples(ms: float, sample_rate: float) -> int:
    return max(1, int(round(ms * 0.001 * sample_rate)))


def click_broadband_ratio(sample_rate: float) -> float:
    """Linear power ratio threshold for (2nd-difference energy) / (plain energy)."""
    db = CLICK_BROADBAND_DB_AT_48K - 40.0 * math.log10(max(sample_rate, 8000.0) / 48000.0)
    return 10.0 ** (db / 10.0)


def _biquad_filter_stdlib(x: Sequence[float], c: tuple[float, ...]) -> list[float]:
    b0, b1, b2, a1, a2 = c
    z1 = 0.0
    z2 = 0.0
    out = [0.0] * len(x)
    for i, xi in enumerate(x):
        y = b0 * xi + z1
        z1 = b1 * xi - a1 * y + z2
        z2 = b2 * xi - a2 * y
        out[i] = y
    return out


def _biquad_filter_numpy(x, c: tuple[float, ...]):
    """Exact IIR filtering via the biquad's frequency response (zero-padded to avoid wrap)."""
    b0, b1, b2, a1, a2 = c
    n = len(x)
    nfft = 1
    while nfft < n + 8192:
        nfft <<= 1
    spec = np.fft.rfft(x, n=nfft)
    z = np.exp(-1j * np.pi * np.arange(len(spec)) / (len(spec) - 1))
    h = (b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z)
    return np.fft.irfft(spec * h, n=nfft)[:n]


def _trailing_mean(sq_cumsum, idx, win: int):
    """Mean of squared samples over [idx - win + 1, idx] from a prefix-sum array (numpy)."""
    lo = np.maximum(idx + 1 - win, 0)
    return (sq_cumsum[idx + 1] - sq_cumsum[lo]) / float(win)


def detect_clicks(
    samples: Sequence[float], sample_rate: int, sensitivity_db: float, *, use_numpy: bool = True
) -> list[int]:
    """Return sample indices of detected lip-noise clicks (mirrors the C++ detector).

    e[n] = (x[n] - 2x[n-1] + x[n-2])^2; fast = trailing 0.5 ms mean, slow = 20 ms mean that
    ends right before the fast window. A click at c requires fast[c] > slow[c] * sens, a
    broadband burst (fast[c] > plain-signal fast energy * broadband ratio) and the burst to
    decay by 6 dB within 1 ms (fast[c + 1 ms] < fast[c] / 4).
    """
    sr = float(sample_rate)
    lf = ms_to_samples(CLICK_FAST_MS, sr)
    ls = ms_to_samples(CLICK_SLOW_MS, sr)
    ld = ms_to_samples(CLICK_DECAY_MS, sr)
    hold = ms_to_samples(CLICK_HOLD_MS, sr)
    sens = 10.0 ** (sensitivity_db / 10.0)
    decay = 10.0 ** (-CLICK_DECAY_DB / 10.0)
    broad = click_broadband_ratio(sr)
    n = len(samples)
    if n < lf + ls + ld + 3:
        return []

    clicks: list[int] = []
    if use_numpy and HAS_NUMPY:
        x = np.asarray(samples, dtype=np.float64)
        d = np.zeros(n, dtype=np.float64)
        d[2:] = x[2:] - 2.0 * x[1:-1] + x[:-2]
        cs = np.concatenate(([0.0], np.cumsum(d * d)))
        cx = np.concatenate(([0.0], np.cumsum(x * x)))
        c = np.arange(lf + ls, n - ld)
        fast = (cs[c + 1] - cs[c + 1 - lf]) / float(lf)
        fast_x = (cx[c + 1] - cx[c + 1 - lf]) / float(lf)
        slow = (cs[c + 1 - lf] - cs[c + 1 - lf - ls]) / float(ls)
        fast_next = (cs[c + 1 + ld] - cs[c + 1 + ld - lf]) / float(lf)
        hit = (
            (fast > slow * sens)
            & (fast > 1e-14)
            & (fast > fast_x * broad)
            & (fast_next < fast * decay)
        )
        last = -(10**12)
        for ci in c[hit].tolist():
            if ci - last > hold:
                clicks.append(int(ci))
            last = ci
        return clicks

    x_list = [float(v) for v in samples]
    e = [0.0, 0.0] + [(x_list[i] - 2.0 * x_list[i - 1] + x_list[i - 2]) ** 2 for i in range(2, n)]
    cs_list = [0.0] * (n + 1)
    cx_list = [0.0] * (n + 1)
    acc = 0.0
    acc_x = 0.0
    for i, v in enumerate(e):
        acc += v
        cs_list[i + 1] = acc
        acc_x += x_list[i] * x_list[i]
        cx_list[i + 1] = acc_x
    last = -(10**12)
    inv_lf = 1.0 / float(lf)
    inv_ls = 1.0 / float(ls)
    for ci in range(lf + ls, n - ld):
        fast = (cs_list[ci + 1] - cs_list[ci + 1 - lf]) * inv_lf
        if fast <= 1e-14:
            continue
        slow = (cs_list[ci + 1 - lf] - cs_list[ci + 1 - lf - ls]) * inv_ls
        if fast <= slow * sens:
            continue
        fast_x = (cx_list[ci + 1] - cx_list[ci + 1 - lf]) * inv_lf
        if fast <= fast_x * broad:
            continue
        fast_next = (cs_list[ci + 1 + ld] - cs_list[ci + 1 + ld - lf]) * inv_lf
        if fast_next < fast * decay:
            if ci - last > hold:
                clicks.append(ci)
            last = ci
    return clicks


def breath_features(
    samples: Sequence[float],
    sample_rate: int,
    hop_len: int,
    num_frames: int,
    frame_len: int,
    *,
    use_numpy: bool = True,
) -> tuple[list[float], list[float]]:
    """Per analysis frame: (level_db, tilt_db) from trailing 20 ms boxcar windows.

    Evaluated at the last sample of each analysis frame, exactly as the C++ detector sees it.
    level = 10log10(mean x^2), tilt = 10log10(mean high^2 / mean low^2).
    """
    sr = float(sample_rate)
    win = ms_to_samples(BREATH_WINDOW_MS, sr)
    f_lo, f_hi = breath_band_freqs(sr)
    c_lo = bandpass_coeffs(sr, f_lo, BREATH_BAND_Q)
    c_hi = bandpass_coeffs(sr, f_hi, BREATH_BAND_Q)
    n = len(samples)

    if use_numpy and HAS_NUMPY:
        x = np.asarray(samples, dtype=np.float64)
        lo = _biquad_filter_numpy(x, c_lo)
        hi = _biquad_filter_numpy(x, c_hi)
        idx = np.minimum(np.arange(num_frames) * hop_len + frame_len - 1, n - 1)
        cs_x = np.concatenate(([0.0], np.cumsum(x * x)))
        cs_l = np.concatenate(([0.0], np.cumsum(lo * lo)))
        cs_h = np.concatenate(([0.0], np.cumsum(hi * hi)))
        e_x = _trailing_mean(cs_x, idx, win)
        e_l = _trailing_mean(cs_l, idx, win)
        e_h = _trailing_mean(cs_h, idx, win)
        level = 10.0 * np.log10(np.maximum(e_x, 1e-24))
        tilt = 10.0 * np.log10(np.maximum(e_h, 1e-24) / np.maximum(e_l, 1e-24))
        return level.tolist(), tilt.tolist()

    x_list = [float(v) for v in samples]
    lo_l = _biquad_filter_stdlib(x_list, c_lo)
    hi_l = _biquad_filter_stdlib(x_list, c_hi)

    def prefix(vals: list[float]) -> list[float]:
        out = [0.0] * (len(vals) + 1)
        acc = 0.0
        for i, v in enumerate(vals):
            acc += v * v
            out[i + 1] = acc
        return out

    cs_x_l, cs_lo_l, cs_hi_l = prefix(x_list), prefix(lo_l), prefix(hi_l)
    levels: list[float] = []
    tilts: list[float] = []
    for fi in range(num_frames):
        i = min(fi * hop_len + frame_len - 1, n - 1)
        a = max(i + 1 - win, 0)
        ex = (cs_x_l[i + 1] - cs_x_l[a]) / win
        el = (cs_lo_l[i + 1] - cs_lo_l[a]) / win
        eh = (cs_hi_l[i + 1] - cs_hi_l[a]) / win
        levels.append(10.0 * math.log10(max(ex, 1e-24)))
        tilts.append(10.0 * math.log10(max(eh, 1e-24) / max(el, 1e-24)))
    return levels, tilts


def _percentile(vals: Sequence[float], q: float) -> float:
    s = sorted(vals)
    if not s:
        return 0.0
    pos = max(0.0, min(1.0, q)) * (len(s) - 1)
    lo = int(math.floor(pos))
    hi = min(len(s) - 1, lo + 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def analyze_cleaner(
    samples: Sequence[float],
    sample_rate: int,
    *,
    hop_len: int,
    frame_len: int,
    num_frames: int,
    active_mask: Sequence[bool],
    noise_floor_dbfs: float,
    click_sensitivity_db: float = 12.0,
    use_numpy: bool = True,
) -> CleanerAnalysis:
    """Detect clicks and breaths, learn per-speaker thresholds, and mark frames to exclude."""
    sr = float(sample_rate)
    duration_min = max(len(samples) / sr / 60.0, 1e-9)

    clicks = detect_clicks(samples, sample_rate, click_sensitivity_db, use_numpy=use_numpy)
    excluded = [False] * num_frames
    guard = ms_to_samples(CLICK_FAST_MS + CLICK_HOLD_MS, sr)
    for c in clicks:
        f0 = max(0, (c - guard - frame_len) // hop_len + 1)
        f1 = min(num_frames - 1, (c + guard) // hop_len)
        for fi in range(f0, f1 + 1):
            excluded[fi] = True

    level, tilt = breath_features(
        samples, sample_rate, hop_len, num_frames, frame_len, use_numpy=use_numpy
    )

    voice_idx = [i for i in range(num_frames) if active_mask[i] and tilt[i] <= _VOICE_TILT_MAX_DB]
    if voice_idx:
        voice_tilt_p95 = _percentile([tilt[i] for i in voice_idx], 0.95)
        voice_levels = [level[i] for i in voice_idx]
        voice_level_p10 = _percentile(voice_levels, 0.10)
        voice_level_p50 = _percentile(voice_levels, 0.50)
    else:
        voice_tilt_p95 = -10.0
        voice_level_p10 = noise_floor_dbfs + 20.0
        voice_level_p50 = noise_floor_dbfs + 26.0

    tilt_threshold = max(-6.0, min(6.0, voice_tilt_p95 + _TILT_MARGIN_DB))
    # Breaths can be as loud as quiet speech tails, so tilt is the primary separator and the
    # level cap only guards against suppressing normal-level speech.
    level_cap = voice_level_p50 - _LEVEL_CAP_BELOW_VOICE_DB

    candidate = [
        bool(active_mask[i]) and tilt[i] > tilt_threshold and level[i] < level_cap
        for i in range(num_frames)
    ]
    min_run = max(1, int(round(_BREATH_MIN_RUN_MS / ANALYSIS_HOP_MS)))
    breath_runs: list[tuple[int, int]] = []
    i = 0
    while i < num_frames:
        if candidate[i]:
            j = i
            while j < num_frames and candidate[j]:
                j += 1
            if j - i >= min_run:
                breath_runs.append((i, j))
            i = j
        else:
            i += 1

    breath_levels: list[float] = []
    for a, b in breath_runs:
        for fi in range(a, b):
            excluded[fi] = True
            breath_levels.append(level[fi])

    floor_guard = noise_floor_dbfs + 6.0
    if breath_levels:
        breath_p90: float | None = _percentile(breath_levels, 0.90)
        ceiling = breath_p90 + 3.0
    else:
        breath_p90 = None
        ceiling = level_cap - 4.0
    ceiling = min(ceiling, level_cap)
    ceiling = max(ceiling, floor_guard)
    ceiling = max(-80.0, min(-12.0, ceiling))

    # Hangover: unvoiced consonant tails right after voiced speech (e.g. devoiced Japanese
    # "-su") look breath-like for a few tens of ms. Protect the speaker's typical tail length.
    is_voice = [bool(active_mask[i]) and tilt[i] <= _VOICE_TILT_MAX_DB for i in range(num_frames)]
    breathy_rt = [tilt[i] > tilt_threshold and level[i] < ceiling for i in range(num_frames)]
    tail_ms: list[float] = []
    i = 1
    while i < num_frames:
        if breathy_rt[i] and not breathy_rt[i - 1] and any(is_voice[max(0, i - 3) : i]):
            j = i
            while j < num_frames and breathy_rt[j]:
                j += 1
            if j - i < min_run:
                tail_ms.append((j - i) * ANALYSIS_HOP_MS)
            i = j
        else:
            i += 1
    hangover_ms = _percentile(tail_ms, 0.90) + ANALYSIS_HOP_MS if tail_ms else _DEFAULT_HANGOVER_MS
    hangover_ms = max(_MIN_HANGOVER_MS, min(_MAX_HANGOVER_MS, hangover_ms))

    breath_sec = sum(b - a for a, b in breath_runs) * hop_len / sr
    return CleanerAnalysis(
        click_count=len(clicks),
        click_rate_per_min=len(clicks) / duration_min,
        breath_count=len(breath_runs),
        breath_duration_sec=breath_sec,
        breath_level_p90_dbfs=breath_p90,
        voice_level_p10_dbfs=voice_level_p10,
        voice_tilt_p95_db=voice_tilt_p95,
        learned_tilt_threshold_db=tilt_threshold,
        learned_level_ceiling_dbfs=ceiling,
        learned_hangover_ms=hangover_ms,
        excluded_frame_mask=tuple(excluded),
    )
