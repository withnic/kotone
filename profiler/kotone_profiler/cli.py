"""Command-line interface for kotone-profiler."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import sys
from collections.abc import Sequence
from pathlib import Path

from . import KotoneProfilerError, __version__
from .analyzer import AnalysisResult, analyze_audio
from .optimizer import VALID_PRESETS, optimize_profile
from .wav_io import load_wav_mono

_FORBIDDEN_CHARS = set('\\/:*?"<>|')
_RESERVED_WIN_NAMES = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    *(f"COM{i}" for i in range(1, 10)),
    *(f"LPT{i}" for i in range(1, 10)),
}


def sanitize_filename(name: str) -> str:
    """Sanitize a profile name into a safe UTF-8 filename stem matching C++ rules."""
    chars = ["_" if (ord(c) < 0x20 or c in _FORBIDDEN_CHARS) else c for c in name]
    cleaned = "".join(chars).strip(" \t.")[:64].rstrip(" \t.")
    if not cleaned:
        return "custom_profile"
    if cleaned.upper() in _RESERVED_WIN_NAMES:
        cleaned = "_" + cleaned
    return cleaned


def _atomic_write_json(target_path: Path, payload: dict) -> None:
    target_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = target_path.with_name(target_path.name + ".tmp")
    text = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
    try:
        with tmp_path.open("w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp_path, target_path)
    except OSError as exc:
        if tmp_path.exists():
            with contextlib.suppress(OSError):
                tmp_path.unlink()
        raise KotoneProfilerError(
            "ERR_PROFILE_SAVE_FAILED", f"Failed to write profile JSON: {exc}", exit_code=1
        ) from exc


def _resolve_output_path(
    input_path: Path,
    profile_name: str,
    output_file: str | None,
    output_dir: str | None,
) -> Path:
    if output_file:
        return Path(output_file)
    stem = sanitize_filename(profile_name)
    if output_dir:
        return Path(output_dir) / f"{stem}.json"
    return input_path.with_name(f"{stem}.json")


def _default_obs_profiles_dir() -> Path:
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA")
        if appdata:
            return Path(appdata) / "obs-studio" / "plugin_config" / "kotone" / "profiles"
    return Path.home() / ".config" / "obs-studio" / "plugin_config" / "kotone" / "profiles"


def _cleaner_summary(analysis: AnalysisResult) -> str:
    cl = analysis.cleaner
    if cl is None:
        return ""
    return (
        f"リップノイズ {cl.click_count} 回 ({cl.click_rate_per_min:.0f} 回/分) / "
        f"ブレス {cl.breath_count} 回 ({cl.breath_duration_sec:.1f}s) を検出\n"
        "  (声の解析からは除外し、配信中はプラグインが自動で抑えます)"
    )


def _run_wizard(wav_input: str, output_dir_override: str | None = None) -> int:
    input_path = Path(wav_input)
    if not input_path.is_file():
        print(f"ERR_FILE_NOT_FOUND: WAV file not found: {input_path}", file=sys.stderr)
        return 1

    print("=" * 60)
    print("  琴音 (Kotone) 声質プロファイル自動生成ウィザード")
    print("=" * 60)
    print(f"入力 WAV: {input_path}")

    default_name = input_path.stem
    try:
        entered_name = input(f"プロファイル名を入力 [Enter で '{default_name}']: ").strip()
    except EOFError:
        entered_name = ""
    profile_name = entered_name or default_name

    print("\nプリセットを選択してください:")
    print("  1. clear_broadcast  (標準・クリア配信向け / デフォルト)")
    print("  2. warm_podcast     (温かみ重視・囁き声 / ASMR 向け)")
    print("  3. natural_flat     (自然なフラット補正・ゲーム配信向け)")
    preset_map = {
        "1": "clear_broadcast",
        "2": "warm_podcast",
        "3": "natural_flat",
        "": "clear_broadcast",
    }
    try:
        choice = input("番号を入力 [1-3, Enter で 1]: ").strip()
    except EOFError:
        choice = "1"
    preset = preset_map.get(choice, "clear_broadcast")

    out_dir = Path(output_dir_override) if output_dir_override else _default_obs_profiles_dir()
    out_path = _resolve_output_path(input_path, profile_name, None, str(out_dir))

    print("\n解析中...")
    try:
        audio = load_wav_mono(input_path)
        analysis = analyze_audio(audio)
        profile = optimize_profile(
            analysis,
            profile_name=profile_name,
            preset=preset,
            target_lufs=-16.0,
        )
        _atomic_write_json(out_path, profile)
    except KotoneProfilerError as exc:
        print(str(exc), file=sys.stderr)
        return exc.exit_code
    except Exception as exc:  # pragma: no cover - defensive fallback
        print(f"ERR_UNEXPECTED: {exc}", file=sys.stderr)
        return 1

    print(
        f"\n[完了] プロファイル '{profile['profile_name']}' を保存しました！\n"
        f"  保存先: {out_path}\n"
        f"  解析結果: 有声={analysis.voiced_duration_sec:.1f}s / "
        f"基本周波数 F0={analysis.fundamental_freq_median_hz:.1f}Hz / "
        f"平均音量={analysis.mean_rms_dbfs:.1f}dBFS / "
        f"ノイズフロア={analysis.noise_floor_dbfs:.1f}dBFS\n"
        f"  {_cleaner_summary(analysis)}\n\n"
        "OBS Studio の「琴音 (Kotone) 声質フィルタ」で\n"
        "「プロファイル一覧を再スキャン」を押して選択してください。"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="kotone-profiler",
        description="Kotone (琴音) offline voice analyzer and OBS DSP profile generator.",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")

    subparsers = parser.add_subparsers(dest="command", required=True)
    analyze_p = subparsers.add_parser(
        "analyze",
        help="Analyze a dry voice WAV recording and generate a Kotone voice profile JSON.",
    )
    analyze_p.add_argument(
        "--input",
        "-i",
        required=True,
        help="Path to input WAV file (16/24/32-bit PCM or 32-bit IEEE Float).",
    )
    analyze_p.add_argument(
        "--name",
        "-n",
        default=None,
        help="Human-readable profile name (Japanese/Unicode supported).",
    )
    analyze_p.add_argument(
        "--output",
        "-o",
        default=None,
        help="Explicit output JSON file path.",
    )
    analyze_p.add_argument(
        "--output-dir",
        "-d",
        default=None,
        help="Output directory where <sanitized_name>.json will be written.",
    )
    analyze_p.add_argument(
        "--preset",
        "-p",
        choices=VALID_PRESETS,
        default="clear_broadcast",
        help="Target voice preset curve (default: clear_broadcast).",
    )
    analyze_p.add_argument(
        "--target-lufs",
        type=float,
        default=-16.0,
        help="Target loudness in LUFS (default: -16.0).",
    )

    wizard_p = subparsers.add_parser(
        "wizard",
        help="Interactive wizard for drag-and-drop WAV profiling into OBS profiles folder.",
    )
    wizard_p.add_argument("input", help="Path to input WAV file.")
    wizard_p.add_argument(
        "--output-dir",
        "-d",
        default=None,
        help="Optional custom output directory (defaults to OBS kotone profiles folder).",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.command == "wizard":
        return _run_wizard(args.input, args.output_dir)

    if args.command == "analyze":
        input_path = Path(args.input)
        profile_name = args.name if args.name is not None and args.name.strip() else input_path.stem
        out_path = _resolve_output_path(input_path, profile_name, args.output, args.output_dir)

        try:
            audio = load_wav_mono(input_path)
            analysis = analyze_audio(audio)
            profile = optimize_profile(
                analysis,
                profile_name=profile_name,
                preset=args.preset,
                target_lufs=args.target_lufs,
            )
            _atomic_write_json(out_path, profile)
        except KotoneProfilerError as exc:
            print(str(exc), file=sys.stderr)
            return exc.exit_code
        except Exception as exc:  # pragma: no cover - defensive fallback
            print(f"ERR_UNEXPECTED: {exc}", file=sys.stderr)
            return 1

        print(
            f"[OK] Saved profile '{profile['profile_name']}' -> {out_path} "
            f"(voiced={analysis.voiced_duration_sec:.1f}s, "
            f"F0={analysis.fundamental_freq_median_hz:.1f}Hz, "
            f"RMS={analysis.mean_rms_dbfs:.1f}dBFS, "
            f"noise={analysis.noise_floor_dbfs:.1f}dBFS"
            + (
                f", clicks={analysis.cleaner.click_count}, breaths={analysis.cleaner.breath_count}"
                if analysis.cleaner is not None
                else ""
            )
            + ")"
        )
        return 0

    return 1
