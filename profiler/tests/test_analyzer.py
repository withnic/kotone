"""Unit tests for WAV decoding, acoustic analysis, DSP optimization, and stdlib fallback."""

from __future__ import annotations

import math
from pathlib import Path

import pytest

from conftest import synthesize_voice, write_wav_file
from kotone_profiler import KotoneProfilerError
from kotone_profiler.analyzer import analyze_audio
from kotone_profiler.optimizer import optimize_profile
from kotone_profiler.wav_io import load_wav_mono


def test_normal_talk_analysis_accuracy(tmp_path: Path) -> None:
    """Verify F0 (130 Hz), noise floor (-60 dBFS), 250 Hz mud cut, and 6.5 kHz De-Esser tuning."""
    samples = synthesize_voice(
        sample_rate=48000,
        voiced_sec=6.5,
        silence_sec=2.5,
        f0_hz=130.0,
        voice_rms_dbfs=-22.0,
        noise_floor_dbfs=-60.0,
        mud_250hz_boost_db=12.0,
        sibilance_burst_amp=0.14,
        sibilance_freq_hz=6500.0,
    )
    wav_path = write_wav_file(
        tmp_path / "talk_130hz.wav", [samples], sample_rate=48000, bits_per_sample=16
    )

    audio = load_wav_mono(wav_path)
    analysis = analyze_audio(audio)

    assert analysis.voiced_duration_sec >= 5.5
    assert -64.0 <= analysis.noise_floor_dbfs <= -56.0
    assert abs(analysis.fundamental_freq_median_hz - 130.0) <= 5.0
    assert analysis.sibilance_ratio > 0.05
    assert 5800.0 <= analysis.sibilance_peak_freq_hz <= 7200.0

    profile = optimize_profile(analysis, profile_name="普段トーク", preset="clear_broadcast")
    dsp = profile["dsp_params"]

    # HPF cutoff = F0 * 0.72 (~93.6 Hz), clamped to [65, 110] Hz
    assert 88.0 <= dsp["hpf"]["cutoff_hz"] <= 99.0
    assert dsp["hpf"]["slope_db_per_oct"] == 24

    # Band 1 (250 Hz) must cut excessive 250 Hz low-mid energy within [-6.0, 0.0] dB
    band_250 = dsp["eq_bands"][1]
    assert band_250["freq_hz"] == 250.0
    assert -6.0 <= band_250["gain_db"] <= -1.5

    # All EQ gains must strictly respect [-6.0, +6.0] dB optimizer limit
    for band in dsp["eq_bands"]:
        assert -6.0 <= band["gain_db"] <= 6.0

    # De-Esser enabled and centered near 6.5 kHz sibilance burst
    assert dsp["deesser"]["enabled"] is True
    assert 5500.0 <= dsp["deesser"]["freq_hz"] <= 7500.0
    assert dsp["deesser"]["max_reduction_db"] >= 6.0
    assert dsp["limiter"]["enabled"] is True
    assert dsp["limiter"]["ceiling_dbfs"] == -1.0


def test_whisper_voice_gain_coordination(tmp_path: Path) -> None:
    """Low-RMS whisper speech must receive coordinated input + makeup lift toward -16 LUFS."""
    samples = synthesize_voice(
        sample_rate=48000,
        voiced_sec=6.0,
        silence_sec=2.0,
        f0_hz=115.0,
        voice_rms_dbfs=-36.0,
        noise_floor_dbfs=-64.0,
    )
    wav_path = write_wav_file(
        tmp_path / "whisper.wav", [samples], sample_rate=48000, bits_per_sample=16
    )

    audio = load_wav_mono(wav_path)
    analysis = analyze_audio(audio)
    profile = optimize_profile(
        analysis, profile_name="囁き声配信", preset="warm_podcast", target_lufs=-16.0
    )

    dsp = profile["dsp_params"]
    total_lift_db = dsp["input_gain_db"] + dsp["compressor"]["makeup_gain_db"]
    assert dsp["input_gain_db"] > 4.0
    assert dsp["compressor"]["makeup_gain_db"] > 6.0
    assert total_lift_db >= 14.0
    assert dsp["limiter"]["ceiling_dbfs"] == -1.0


def test_stdlib_fallback_matches_numpy_path(tmp_path: Path) -> None:
    """Pure Python stdlib fallback (use_numpy=False) must agree closely with NumPy path."""
    samples = synthesize_voice(
        sample_rate=24000,
        voiced_sec=5.5,
        silence_sec=1.5,
        f0_hz=145.0,
        voice_rms_dbfs=-24.0,
        noise_floor_dbfs=-60.0,
        mud_250hz_boost_db=10.0,
        sibilance_burst_amp=0.10,
    )
    wav_path = write_wav_file(
        tmp_path / "fallback_test.wav", [samples], sample_rate=24000, bits_per_sample=16
    )

    audio_np = load_wav_mono(wav_path, use_numpy=True)
    audio_py = load_wav_mono(wav_path, use_numpy=False)
    res_np = analyze_audio(audio_np, use_numpy=True)
    res_py = analyze_audio(audio_py, use_numpy=False)

    assert abs(res_np.voiced_duration_sec - res_py.voiced_duration_sec) <= 0.1
    assert abs(res_np.noise_floor_dbfs - res_py.noise_floor_dbfs) <= 1.0
    assert abs(res_np.mean_rms_dbfs - res_py.mean_rms_dbfs) <= 0.5
    assert abs(res_np.fundamental_freq_median_hz - res_py.fundamental_freq_median_hz) <= 5.0
    assert abs(res_np.sibilance_ratio - res_py.sibilance_ratio) <= 0.05


@pytest.mark.parametrize(
    ("bits", "is_float", "stereo"),
    [
        (16, False, False),
        (16, False, True),
        (24, False, True),
        (32, False, False),
        (32, True, True),
    ],
)
def test_wav_io_all_formats_and_downmix(
    tmp_path: Path, bits: int, is_float: bool, stereo: bool
) -> None:
    """Verify 16/24/32-bit PCM & 32-bit IEEE Float WAV decoding and stereo-to-mono downmix."""
    sr = 48000
    n = 480
    ch0 = [0.5 * math.sin(2.0 * math.pi * 440.0 * i / sr) for i in range(n)]
    channels = [ch0, [x * 0.5 for x in ch0]] if stereo else [ch0]
    expected_peak = 0.375 if stereo else 0.5

    wav_path = write_wav_file(
        tmp_path / f"fmt_{bits}_{'f' if is_float else 'i'}_{'st' if stereo else 'mo'}.wav",
        channels,
        sample_rate=sr,
        bits_per_sample=bits,
        ieee_float=is_float,
    )

    for use_np in (True, False):
        audio = load_wav_mono(wav_path, use_numpy=use_np)
        assert audio.sample_rate == sr
        assert audio.num_frames == n
        peak = max(abs(float(x)) for x in audio.samples)
        assert abs(peak - expected_peak) < 1e-3


def test_error_silence_and_short_speech(tmp_path: Path) -> None:
    """Silence-only and <5s speech WAVs must fail with ERR_INSUFFICIENT_SPEECH (exit 2)."""
    # 1. Pure silence (8 seconds)
    silent_path = write_wav_file(tmp_path / "silence.wav", [[0.0] * (48000 * 8)], sample_rate=48000)
    with pytest.raises(KotoneProfilerError) as exc_sil:
        analyze_audio(load_wav_mono(silent_path))
    assert exc_sil.value.code == "ERR_INSUFFICIENT_SPEECH"
    assert exc_sil.value.exit_code == 2

    # 2. Too short voiced speech (2.0 seconds voiced < 5.0s minimum)
    short_samples = synthesize_voice(sample_rate=48000, voiced_sec=2.0, silence_sec=4.0)
    short_path = write_wav_file(tmp_path / "short.wav", [short_samples], sample_rate=48000)
    with pytest.raises(KotoneProfilerError) as exc_short:
        analyze_audio(load_wav_mono(short_path))
    assert exc_short.value.code == "ERR_INSUFFICIENT_SPEECH"
    assert exc_short.value.exit_code == 2


def test_error_invalid_and_missing_wav(tmp_path: Path) -> None:
    """Corrupted WAV must fail with ERR_INVALID_WAV; missing file with ERR_FILE_NOT_FOUND."""
    bad_path = tmp_path / "corrupt.wav"
    bad_path.write_bytes(b"NOT_A_RIFF_WAV_FILE_1234567890")
    with pytest.raises(KotoneProfilerError) as exc_bad:
        load_wav_mono(bad_path)
    assert exc_bad.value.code == "ERR_INVALID_WAV"
    assert exc_bad.value.exit_code == 1

    with pytest.raises(KotoneProfilerError) as exc_missing:
        load_wav_mono(tmp_path / "nonexistent.wav")
    assert exc_missing.value.code == "ERR_FILE_NOT_FOUND"
    assert exc_missing.value.exit_code == 1
