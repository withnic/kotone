"""DSP parameter optimizer and Kotone VoiceProfile JSON schema generator.

Strictly adheres to docs/02_data_modeling.md Sections 2.1, 2.2, and 5.2.
"""

from __future__ import annotations

import datetime
import math
from typing import Any

from .analyzer import EQ_CENTER_FREQS_HZ, AnalysisResult

VALID_PRESETS = ("clear_broadcast", "warm_podcast", "natural_flat")

# Target 1/3-octave relative speech curves (dB relative to 500/1500 Hz mid anchor)
_PRESET_TARGET_CURVES: dict[str, dict[float, float]] = {
    "clear_broadcast": {
        120.0: 1.5,
        250.0: 0.0,
        500.0: 0.5,
        1500.0: -0.5,
        3200.0: -2.0,
        5000.0: -5.5,
        7200.0: -9.5,
        11000.0: -12.5,
    },
    "warm_podcast": {
        120.0: 4.0,
        250.0: 1.0,
        500.0: 1.0,
        1500.0: -1.0,
        3200.0: -3.5,
        5000.0: -7.0,
        7200.0: -11.0,
        11000.0: -14.5,
    },
    "natural_flat": {
        120.0: 2.5,
        250.0: 1.0,
        500.0: 0.5,
        1500.0: -0.5,
        3200.0: -3.5,
        5000.0: -6.5,
        7200.0: -10.0,
        11000.0: -14.0,
    },
}

_DEFAULT_EQ_QS: dict[float, float] = {
    120.0: 0.707,
    250.0: 2.0,
    500.0: 1.8,
    1500.0: 1.4,
    3200.0: 1.6,
    5000.0: 2.0,
    7200.0: 3.5,
    11000.0: 0.707,
}


def _clamp(value: float, lo: float, hi: float, fallback: float) -> float:
    if not math.isfinite(value):
        return fallback
    return max(lo, min(hi, value))


def _round_num(value: float, digits: int = 2) -> float:
    if not math.isfinite(value):
        return 0.0
    v = round(value, digits)
    return 0.0 if abs(v) < 1e-9 else v


def _truncate_profile_name(name: str, max_chars: int = 64) -> str:
    cleaned = name.strip()
    if not cleaned:
        return "Custom Profile"
    return cleaned[:max_chars]


def optimize_profile(
    analysis: AnalysisResult,
    *,
    profile_name: str,
    preset: str = "clear_broadcast",
    target_lufs: float = -16.0,
) -> dict[str, Any]:
    """Generate a validated, schema-1.0.0 VoiceProfile dictionary from AnalysisResult."""
    if preset not in VALID_PRESETS:
        raise ValueError(f"Unsupported preset '{preset}'. Choose from: {', '.join(VALID_PRESETS)}")

    sr = int(_clamp(float(analysis.sample_rate_hz), 8000.0, 192000.0, 48000.0))
    target_lufs_safe = _clamp(target_lufs, -30.0, -8.0, -16.0)
    now_iso = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")

    # 1. HPF cutoff from F0 median: F0 * 0.72 clamped to [65 Hz, 110 Hz]
    hpf_cutoff_hz = _clamp(analysis.fundamental_freq_median_hz * 0.72, 65.0, 110.0, 80.0)
    hpf_cutoff_hz = _clamp(hpf_cutoff_hz, 20.0, min(300.0, sr * 0.45), 80.0)

    # 2. 8-Band PEQ auto-fitting (60% deviation scaling, clamped to [-6.0, +6.0] dB)
    target_curve = _PRESET_TARGET_CURVES[preset]
    eq_bands: list[dict[str, Any]] = []
    for idx, fc in enumerate(EQ_CENTER_FREQS_HZ):
        band_type = "low_shelf" if idx == 0 else ("high_shelf" if idx == 7 else "peaking")
        measured_rel = analysis.band_rel_db.get(fc, -120.0)
        target_rel = target_curve[fc]

        if measured_rel <= -40.0:
            # Avoid boosting empty/noise-only frequency bands
            gain_db = 0.0
        else:
            deviation = target_rel - measured_rel
            gain_db = deviation * 0.60
            # Guard against boosting very weak high-frequency noise
            if measured_rel < -25.0 and gain_db > 0.0:
                gain_db *= 0.25
            gain_db = _clamp(gain_db, -6.0, 6.0, 0.0)

        enabled = abs(gain_db) >= 0.25
        eq_bands.append(
            {
                "index": idx,
                "enabled": enabled,
                "type": band_type,
                "freq_hz": _round_num(_clamp(fc, 20.0, min(20000.0, sr * 0.48), 1000.0), 1),
                "gain_db": _round_num(gain_db if enabled else 0.0, 2),
                "q": _round_num(_clamp(_DEFAULT_EQ_QS[fc], 0.1, 18.0, 1.0), 3),
            }
        )

    # 3. Input gain + Compressor + Makeup gain coordination
    # Ensures both whisper voices (low RMS) and loud voices converge safely near target_lufs.
    mean_rms = _clamp(analysis.mean_rms_dbfs, -65.0, -3.0, -24.0)
    noise_floor = _clamp(analysis.noise_floor_dbfs, -96.0, -20.0, -60.0)
    crest = _clamp(analysis.crest_factor_db, 4.0, 35.0, 16.0)

    # Bring working pre-compressor RMS toward -22 dBFS while respecting noise-floor headroom
    if mean_rms < -22.0:
        desired_in_gain = -22.0 - mean_rms
        # Allow lifting whisper voice while keeping noise floor below ~-38 dBFS
        noise_safe_cap = max(0.0, -38.0 - noise_floor)
        input_gain_db = min(desired_in_gain * 0.65, noise_safe_cap, 16.0)
    elif mean_rms > -16.0:
        input_gain_db = max(-12.0, -18.0 - mean_rms)
    else:
        input_gain_db = 0.0
    input_gain_db = _clamp(input_gain_db, -24.0, 24.0, 0.0)

    working_rms = mean_rms + input_gain_db

    # Compressor ratio and ballistics tuned to crest factor and preset
    if preset == "warm_podcast":
        base_ratio = 3.0 + _clamp((crest - 14.0) * 0.12, 0.0, 1.5, 0.0)
        knee_db = 8.0
        attack_ms = 7.0
        release_ms = 120.0
    elif preset == "natural_flat":
        base_ratio = 2.0 + _clamp((crest - 15.0) * 0.08, 0.0, 1.0, 0.0)
        knee_db = 8.0
        attack_ms = 12.0
        release_ms = 140.0
    else:  # clear_broadcast
        base_ratio = 3.0 + _clamp((crest - 13.0) * 0.14, 0.0, 2.0, 0.0)
        knee_db = 6.0
        attack_ms = 6.0
        release_ms = 100.0

    comp_ratio = _clamp(base_ratio, 1.0, 20.0, 3.0)
    comp_threshold = _clamp(working_rms - 2.0, -45.0, -14.0, -22.0)

    # Estimate steady gain reduction + transient compression loss for makeup allocation
    est_over_db = max(0.0, (working_rms + 3.0) - comp_threshold)
    est_gr_db = est_over_db * (1.0 - 1.0 / comp_ratio)
    needed_makeup = (target_lufs_safe - working_rms) + est_gr_db * 0.75
    comp_makeup = _clamp(needed_makeup, -12.0, 24.0, 4.0)

    # 4. De-Esser optimization from sibilance ratio and peak frequency
    sib_ratio = _clamp(analysis.sibilance_ratio, 0.0, 1.0, 0.05)
    sib_freq = _clamp(analysis.sibilance_peak_freq_hz, 3000.0, min(12000.0, sr * 0.48), 6500.0)
    # Post-input-gain sibilance peak determines De-Esser threshold
    post_in_sib_peak = analysis.sibilance_peak_dbfs + input_gain_db
    strong_sibilance = sib_ratio >= 0.06 or post_in_sib_peak >= -30.0
    deesser_enabled = strong_sibilance or preset in ("clear_broadcast", "warm_podcast")
    if strong_sibilance:
        deesser_threshold = _clamp(post_in_sib_peak - 6.0, -42.0, -14.0, -24.0)
        deesser_max_red = _clamp(5.0 + sib_ratio * 25.0, 6.0, 12.0, 8.0)
    else:
        deesser_threshold = _clamp(working_rms - 2.0, -36.0, -16.0, -24.0)
        deesser_max_red = 4.5 if preset != "natural_flat" else 3.0

    # 5. Brickwall safety limiter (always active at -1.0 dBFS ceiling)
    ceiling_dbfs = -1.0

    return {
        "schema_version": "1.0.0",
        "profile_name": _truncate_profile_name(profile_name),
        "created_at": now_iso,
        "updated_at": now_iso,
        "target_preset": preset,
        "ui_overrides": {
            "mix_percent": 100.0,
            "output_makeup_trim_db": 0.0,
            "limiter_ceiling_dbfs": ceiling_dbfs,
        },
        "analysis_metrics": {
            "sample_rate_hz": sr,
            "voiced_duration_sec": _round_num(analysis.voiced_duration_sec, 2),
            "noise_floor_dbfs": _round_num(analysis.noise_floor_dbfs, 2),
            "mean_rms_dbfs": _round_num(analysis.mean_rms_dbfs, 2),
            "peak_dbfs": _round_num(analysis.peak_dbfs, 2),
            "crest_factor_db": _round_num(analysis.crest_factor_db, 2),
            "fundamental_freq_median_hz": _round_num(analysis.fundamental_freq_median_hz, 2),
            "sibilance_ratio": _round_num(analysis.sibilance_ratio, 4),
        },
        "dsp_params": {
            "input_gain_db": _round_num(input_gain_db, 2),
            "hpf": {
                "enabled": True,
                "cutoff_hz": _round_num(hpf_cutoff_hz, 2),
                "slope_db_per_oct": 24,
            },
            "eq_bands": eq_bands,
            "deesser": {
                "enabled": bool(deesser_enabled),
                "freq_hz": _round_num(sib_freq, 1),
                "bandwidth_q": 2.6,
                "threshold_dbfs": _round_num(deesser_threshold, 2),
                "max_reduction_db": _round_num(deesser_max_red, 2),
                "attack_ms": 1.0,
                "release_ms": 40.0,
            },
            "compressor": {
                "enabled": True,
                "threshold_dbfs": _round_num(comp_threshold, 2),
                "ratio": _round_num(comp_ratio, 2),
                "knee_db": _round_num(knee_db, 2),
                "attack_ms": _round_num(attack_ms, 2),
                "release_ms": _round_num(release_ms, 2),
                "makeup_gain_db": _round_num(comp_makeup, 2),
            },
            "limiter": {
                "enabled": True,
                "ceiling_dbfs": ceiling_dbfs,
                "lookahead_ms": 2.0,
                "release_ms": 50.0,
            },
        },
    }
