"""Tests for lip-noise / breath detection, analysis exclusion, and learned cleaner thresholds."""

from __future__ import annotations

import math
import random
from pathlib import Path

import pytest

from conftest import synthesize_voice, write_wav_file
from kotone_profiler.analyzer import analyze_audio
from kotone_profiler.cleaner import detect_clicks
from kotone_profiler.optimizer import optimize_profile
from kotone_profiler.wav_io import HAS_NUMPY, load_wav_mono

SR = 48000


def _add_clicks(samples: list[float], positions: list[int], amp: float = 0.3) -> list[float]:
    out = list(samples)
    for p in positions:
        out[p] += amp
        out[p + 1] -= amp * 1.4
        out[p + 2] += amp * 0.6
    return out


def _add_breaths(
    samples: list[float], spans: list[tuple[int, int]], level_dbfs: float, seed: int = 5
) -> list[float]:
    """Add inhalation-like high-tilted hiss (first-difference noise) with smooth edges."""
    rng = random.Random(seed)
    out = list(samples)
    # First difference of uniform noise: RMS = a * sqrt(2/3)
    amp = 10.0 ** (level_dbfs / 20.0) / math.sqrt(2.0 / 3.0)
    for a, b in spans:
        prev = 0.0
        fade = int(0.04 * SR)
        for i in range(a, b):
            w = rng.uniform(-1.0, 1.0)
            env = min(1.0, (i - a) / fade, (b - 1 - i) / fade)
            out[i] += amp * env * (w - prev)
            prev = w
    return out


def _speech_with_pauses() -> list[float]:
    """Two 4 s voiced phrases, each padded with 0.6 s of room tone on both sides."""
    phrase = synthesize_voice(sample_rate=SR, voiced_sec=4.0, silence_sec=1.2, seed=1)
    phrase2 = synthesize_voice(sample_rate=SR, voiced_sec=4.0, silence_sec=1.2, seed=2)
    return phrase + phrase2


@pytest.mark.parametrize("use_np", [True, False] if HAS_NUMPY else [False])
def test_click_detector_counts_isolated_clicks(use_np: bool) -> None:
    rng = random.Random(9)
    base = [rng.uniform(-0.002, 0.002) for _ in range(SR * 2)]
    positions = list(range(4800, SR * 2 - 100, 9600))
    noisy = _add_clicks(base, positions)
    found = detect_clicks(noisy, SR, 12.0, use_numpy=use_np)
    assert len(found) == len(positions)
    for f, p in zip(found, positions, strict=True):
        assert abs(f - p) <= int(0.002 * SR)
    # Clean room tone and steady voice produce no clicks
    assert detect_clicks(base, SR, 12.0, use_numpy=use_np) == []
    voice = synthesize_voice(sample_rate=SR, voiced_sec=2.0, silence_sec=0.5)
    assert detect_clicks(voice, SR, 12.0, use_numpy=use_np) == []


def test_clicks_and_breaths_excluded_from_voice_metrics(tmp_path: Path) -> None:
    clean = _speech_with_pauses()
    # Clicks in the pauses / at phrase edges (where lip noise is audible), breaths in the pauses
    # Layout: voice 0.6-4.6 s, pause 4.6-5.8 s, voice 5.8-9.8 s, pause 9.8-10.4 s
    # (breaths leave enough room tone for the 10th-percentile noise floor estimate)
    pause1 = (int(4.9 * SR), int(5.5 * SR))
    pause2 = (int(9.9 * SR), int(10.25 * SR))
    clicks = [int(4.65 * SR), int(4.8 * SR), int(5.6 * SR), int(9.85 * SR), int(10.32 * SR)]
    dirty = _add_breaths(clean, [pause1, pause2], level_dbfs=-42.0)
    dirty = _add_clicks(dirty, clicks, amp=0.45)

    clean_wav = write_wav_file(tmp_path / "clean.wav", [clean], sample_rate=SR)
    dirty_wav = write_wav_file(tmp_path / "dirty.wav", [dirty], sample_rate=SR)
    res_clean = analyze_audio(load_wav_mono(clean_wav))
    res_dirty = analyze_audio(load_wav_mono(dirty_wav))

    cl = res_dirty.cleaner
    assert cl is not None
    assert cl.click_count == len(clicks)
    assert cl.breath_count == 2
    assert cl.breath_duration_sec >= 0.6

    # Without exclusion the clicks (-7 dBFS peaks) would dominate peak / crest factor and the
    # breaths would count as speech. With exclusion the measurements match the clean take.
    assert abs(res_dirty.peak_dbfs - res_clean.peak_dbfs) <= 1.0
    assert abs(res_dirty.crest_factor_db - res_clean.crest_factor_db) <= 1.0
    assert abs(res_dirty.voiced_duration_sec - res_clean.voiced_duration_sec) <= 0.4
    assert abs(res_dirty.mean_rms_dbfs - res_clean.mean_rms_dbfs) <= 0.5

    # Learned thresholds: breath level (-42 dBFS) sits under the ceiling, the voice above it
    assert -42.0 < cl.learned_level_ceiling_dbfs < res_clean.mean_rms_dbfs - 6.0
    assert cl.voice_tilt_p95_db < cl.learned_tilt_threshold_db

    profile = optimize_profile(res_dirty, profile_name="ブレス入り", preset="clear_broadcast")
    breath = profile["dsp_params"]["breath"]
    assert breath["enabled"] is True
    assert breath["level_ceiling_dbfs"] == pytest.approx(cl.learned_level_ceiling_dbfs, abs=0.01)
    assert breath["tilt_threshold_db"] == pytest.approx(cl.learned_tilt_threshold_db, abs=0.01)
    assert profile["dsp_params"]["declick"]["enabled"] is True
    assert profile["schema_version"] == "1.1.0"


def test_cleaner_stdlib_fallback_matches_numpy(tmp_path: Path) -> None:
    if not HAS_NUMPY:
        pytest.skip("NumPy not installed: parity check needs both paths")
    clean = _speech_with_pauses()
    dirty = _add_breaths(clean, [(int(4.6 * SR), int(5.5 * SR))], level_dbfs=-44.0)
    dirty = _add_clicks(dirty, [int(4.7 * SR), int(5.2 * SR)])
    wav = write_wav_file(tmp_path / "fallback.wav", [dirty], sample_rate=SR)
    res_np = analyze_audio(load_wav_mono(wav, use_numpy=True), use_numpy=True)
    res_py = analyze_audio(load_wav_mono(wav, use_numpy=False), use_numpy=False)
    assert res_np.cleaner is not None and res_py.cleaner is not None
    assert res_np.cleaner.click_count == res_py.cleaner.click_count
    assert res_np.cleaner.breath_count == res_py.cleaner.breath_count
    assert res_np.cleaner.learned_level_ceiling_dbfs == pytest.approx(
        res_py.cleaner.learned_level_ceiling_dbfs, abs=0.05
    )
    assert res_np.cleaner.learned_tilt_threshold_db == pytest.approx(
        res_py.cleaner.learned_tilt_threshold_db, abs=0.05
    )


def test_whisper_preset_keeps_cleaner_gentle(tmp_path: Path) -> None:
    samples = synthesize_voice(sample_rate=SR, voiced_sec=6.0, voice_rms_dbfs=-38.0)
    wav = write_wav_file(tmp_path / "whisper.wav", [samples], sample_rate=SR)
    res = analyze_audio(load_wav_mono(wav))
    warm = optimize_profile(res, profile_name="囁き", preset="warm_podcast")
    clear = optimize_profile(res, profile_name="通常", preset="clear_broadcast")
    assert (
        warm["dsp_params"]["breath"]["max_reduction_db"]
        < (clear["dsp_params"]["breath"]["max_reduction_db"])
    )
    assert (
        warm["dsp_params"]["declick"]["max_reduction_db"]
        < (clear["dsp_params"]["declick"]["max_reduction_db"])
    )
