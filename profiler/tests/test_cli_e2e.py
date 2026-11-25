"""End-to-end integration tests: Python kotone-profiler CLI -> C++ kotone_tests --verify-profile."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import PROFILER_ROOT, REPO_ROOT, synthesize_voice, write_wav_file


def _find_cpp_verifier() -> Path:
    candidates = [
        REPO_ROOT / "build" / "kotone_tests.exe",
        REPO_ROOT / "build" / "Release" / "kotone_tests.exe",
        REPO_ROOT / "build" / "kotone_tests",
        REPO_ROOT / "build" / "Release" / "kotone_tests",
    ]
    for path in candidates:
        if path.is_file():
            return path
    pytest.fail(
        f"C++ kotone_tests binary not found in {REPO_ROOT / 'build'}. "
        "Build Phase 1 before running E2E tests."
    )


def _run_profiler_cli(args: list[str]) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    existing_py = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = (
        f"{PROFILER_ROOT}{os.pathsep}{existing_py}" if existing_py else str(PROFILER_ROOT)
    )
    env["PYTHONUTF8"] = "1"
    return subprocess.run(
        [sys.executable, "-m", "kotone_profiler", *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        env=env,
        check=False,
    )


@pytest.mark.parametrize(
    ("profile_name", "preset", "voice_rms_dbfs", "use_output_dir"),
    [
        ("普段トーク", "clear_broadcast", -22.0, False),
        ("ゲーム配信", "natural_flat", -15.0, False),
        ("囁き声配信", "warm_podcast", -35.0, True),
    ],
)
def test_cli_japanese_profile_cpp_e2e(
    tmp_path: Path,
    profile_name: str,
    preset: str,
    voice_rms_dbfs: float,
    use_output_dir: bool,
) -> None:
    """Generate Japanese-named profile JSON via CLI and verify loading & DSP processing in C++."""
    cpp_bin = _find_cpp_verifier()

    samples = synthesize_voice(
        sample_rate=48000,
        voiced_sec=6.0,
        silence_sec=2.0,
        f0_hz=125.0,
        voice_rms_dbfs=voice_rms_dbfs,
        noise_floor_dbfs=-62.0,
        mud_250hz_boost_db=8.0,
        sibilance_burst_amp=0.10,
    )
    wav_path = write_wav_file(tmp_path / f"{profile_name}.wav", [samples], sample_rate=48000)

    out_dir = tmp_path / "profiles"
    if use_output_dir:
        cli_args = [
            "analyze",
            "--input",
            str(wav_path),
            "--name",
            profile_name,
            "--preset",
            preset,
            "--output-dir",
            str(out_dir),
        ]
        expected_json = out_dir / f"{profile_name}.json"
    else:
        expected_json = out_dir / f"{profile_name}.json"
        cli_args = [
            "analyze",
            "--input",
            str(wav_path),
            "--name",
            profile_name,
            "--preset",
            preset,
            "--output",
            str(expected_json),
        ]

    cli_proc = _run_profiler_cli(cli_args)
    assert cli_proc.returncode == 0, (
        f"CLI failed: stdout={cli_proc.stdout}\nstderr={cli_proc.stderr}"
    )
    assert expected_json.is_file(), f"Expected profile JSON not created: {expected_json}"

    # Verify UTF-8 (no BOM) and LF-only formatting
    raw_bytes = expected_json.read_bytes()
    assert not raw_bytes.startswith(b"\xef\xbb\xbf")
    assert b"\r" not in raw_bytes

    data = json.loads(raw_bytes.decode("utf-8"))
    assert data["schema_version"] == "1.0.0"
    assert data["profile_name"] == profile_name
    assert data["target_preset"] == preset
    assert len(data["dsp_params"]["eq_bands"]) == 8
    assert data["ui_overrides"]["mix_percent"] == 100.0
    assert data["dsp_params"]["limiter"]["ceiling_dbfs"] == -1.0

    # Cross-language E2E verification with C++ kotone_tests --verify-profile
    cpp_proc = subprocess.run(
        [str(cpp_bin), "--verify-profile", str(expected_json)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    assert cpp_proc.returncode == 0, (
        f"C++ verifier failed for '{profile_name}': "
        f"stdout={cpp_proc.stdout}\nstderr={cpp_proc.stderr}"
    )
    assert "[VERIFY PASS]" in cpp_proc.stdout


def test_cli_exit_codes_on_error_inputs(tmp_path: Path) -> None:
    """Verify CLI exit code 2 for insufficient speech and exit code 1 for invalid WAV."""
    # Exit code 2: short speech (< 5s)
    short_samples = synthesize_voice(sample_rate=48000, voiced_sec=1.5, silence_sec=2.0)
    short_wav = write_wav_file(tmp_path / "too_short.wav", [short_samples], sample_rate=48000)
    proc_short = _run_profiler_cli(
        [
            "analyze",
            "--input",
            str(short_wav),
            "--name",
            "短すぎ",
            "--output",
            str(tmp_path / "out1.json"),
        ]
    )
    assert proc_short.returncode == 2
    assert "ERR_INSUFFICIENT_SPEECH" in proc_short.stderr

    # Exit code 1: corrupted WAV
    bad_wav = tmp_path / "broken.wav"
    bad_wav.write_bytes(b"BROKEN_HEADER")
    proc_bad = _run_profiler_cli(
        [
            "analyze",
            "--input",
            str(bad_wav),
            "--name",
            "破損",
            "--output",
            str(tmp_path / "out2.json"),
        ]
    )
    assert proc_bad.returncode == 1
    assert "ERR_INVALID_WAV" in proc_bad.stderr


def test_cli_wizard_interactive_flow(tmp_path: Path) -> None:
    """Verify drag-and-drop wizard mode reads Japanese input and generates valid JSON."""
    samples = synthesize_voice(sample_rate=48000, voiced_sec=5.5, silence_sec=1.5)
    wav_path = write_wav_file(tmp_path / "sample.wav", [samples], sample_rate=48000)
    out_dir = tmp_path / "obs_profiles"

    env = os.environ.copy()
    existing_py = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = (
        f"{PROFILER_ROOT}{os.pathsep}{existing_py}" if existing_py else str(PROFILER_ROOT)
    )
    env["PYTHONUTF8"] = "1"
    proc = subprocess.run(
        [sys.executable, "-m", "kotone_profiler", "wizard", str(wav_path), "-d", str(out_dir)],
        input="雑談テスト\n2\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        env=env,
        check=False,
    )
    assert proc.returncode == 0, f"wizard failed: {proc.stdout}\n{proc.stderr}"
    out_json = out_dir / "雑談テスト.json"
    assert out_json.is_file()
    data = json.loads(out_json.read_text(encoding="utf-8"))
    assert data["profile_name"] == "雑談テスト"
    assert data["target_preset"] == "warm_podcast"
