"""Cross-platform quality gate runner for Kotone (fmt / lint / test / check).

Runs clang-format, clang-tidy, ruff, static policy audits (UTF-8/LF, RT safety,
header isolation, locale key parity), C++ ctest, and Python pytest.
Works identically on Windows PowerShell/CMD and Linux/WSL2.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
RUFF_CONFIG = REPO_ROOT / "profiler" / "pyproject.toml"


def _cpp_all_files() -> list[Path]:
    exts = {".hpp", ".h", ".cpp", ".cc"}
    files: list[Path] = []
    for sub in ("plugin/include", "plugin/src", "plugin/tests"):
        root = REPO_ROOT / sub
        if root.exists():
            files.extend(sorted(p for p in root.rglob("*") if p.suffix in exts and p.is_file()))
    return files


def _cpp_tidy_targets() -> list[Path]:
    # Exclude plugin/src/obs/ when libobs is not configured in build/compile_commands.json
    files: list[Path] = []
    for sub in ("plugin/src/dsp", "plugin/src/profile", "plugin/tests"):
        root = REPO_ROOT / sub
        if root.exists():
            files.extend(sorted(root.glob("*.cpp")))
    return files


def _find_tool(name: str) -> list[str] | None:
    exe = shutil.which(name)
    if exe:
        return [exe]

    if sys.platform == "win32":
        win_candidates: list[Path] = []
        if name in {"clang-format", "clang-tidy"}:
            win_candidates.extend(
                [
                    Path(r"C:\Program Files\LLVM\bin") / f"{name}.exe",
                    Path(r"C:\Program Files (x86)\LLVM\bin") / f"{name}.exe",
                ]
            )
        elif name == "ruff":
            home = Path.home()
            win_candidates.extend(
                [
                    home / ".local" / "bin" / "ruff.exe",
                    home / "AppData" / "Roaming" / "uv" / "tools" / "ruff" / "Scripts" / "ruff.exe",
                    home / "AppData" / "Local" / "uv" / "tools" / "ruff" / "Scripts" / "ruff.exe",
                ]
            )
        for cand in win_candidates:
            if cand.is_file():
                return [str(cand)]

    if name == "ruff":
        # Check if `python -m ruff` works
        probe = subprocess.run(
            [sys.executable, "-m", "ruff", "--version"],
            capture_output=True,
            check=False,
        )
        if probe.returncode == 0:
            return [sys.executable, "-m", "ruff"]
    return None


def _run(cmd: list[str], *, cwd: Path = REPO_ROOT) -> int:
    print(f"==> {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, cwd=cwd, check=False)
    return proc.returncode


def _run_static_audits() -> bool:
    print("==> Running static policy audits (UTF-8/LF, RT-safety, includes, locales)")
    ok = True

    # 1. UTF-8 (no BOM) & LF-only check
    scan_targets = [
        REPO_ROOT / "CMakeLists.txt",
        REPO_ROOT / "CMakePresets.json",
        REPO_ROOT / "Makefile",
        REPO_ROOT / ".clang-format",
        REPO_ROOT / ".clang-tidy",
        REPO_ROOT / "profiler" / "pyproject.toml",
    ]
    for sub in ("plugin", "profiler", "scripts", ".github"):
        base = REPO_ROOT / sub
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if not p.is_file():
                continue
            if any(part in {"__pycache__", ".pytest_cache"} for part in p.parts):
                continue
            if p.suffix in {".hpp", ".h", ".cpp", ".py", ".ini", ".yml", ".yaml", ".toml"}:
                scan_targets.append(p)

    for p in sorted(set(scan_targets)):
        if not p.is_file():
            continue
        raw = p.read_bytes()
        rel = p.relative_to(REPO_ROOT)
        if raw.startswith(b"\xef\xbb\xbf"):
            print(f"  [ERROR] UTF-8 BOM found: {rel}")
            ok = False
        if b"\r" in raw:
            print(f"  [ERROR] CRLF line ending found: {rel}")
            ok = False

    # 2. Dependency direction check: no obs.h / windows.h in dsp/ or profile/
    forbidden_inc = re.compile(r'#\s*include\s*[<"](obs\.h|obs-module\.h|windows\.h)[>"]')
    for sub in ("plugin/include", "plugin/src/dsp", "plugin/src/profile"):
        for p in sorted((REPO_ROOT / sub).rglob("*")):
            if p.suffix in {".hpp", ".cpp"} and p.is_file():
                text = p.read_text(encoding="utf-8")
                if forbidden_inc.search(text):
                    print(f"  [ERROR] Forbidden OBS/Windows include in {p.relative_to(REPO_ROOT)}")
                    ok = False

    # 3. Real-time audio safety check in plugin/src/dsp/
    forbidden_rt = re.compile(
        r"(#\s*include\s*<(mutex|fstream|iostream|cstdio)>"
        r"|\b(malloc|calloc|realloc|free)\s*\(|\bstd::vector\b)"
    )
    for p in sorted((REPO_ROOT / "plugin/src/dsp").glob("*.cpp")):
        text = p.read_text(encoding="utf-8")
        # Strip line comments before checking
        code_only = "\n".join(line.split("//", 1)[0] for line in text.splitlines())
        if forbidden_rt.search(code_only):
            print(f"  [ERROR] Real-time unsafe construct in {p.relative_to(REPO_ROOT)}")
            ok = False

    # 4. Locale INI key parity check
    ja_path = REPO_ROOT / "plugin/data/locale/ja-JP.ini"
    en_path = REPO_ROOT / "plugin/data/locale/en-US.ini"
    if ja_path.is_file() and en_path.is_file():

        def _ini_keys(path: Path) -> set[str]:
            return {
                line.split("=", 1)[0].strip()
                for line in path.read_text(encoding="utf-8").splitlines()
                if "=" in line and not line.strip().startswith("#")
            }

        ja_keys = _ini_keys(ja_path)
        en_keys = _ini_keys(en_path)
        if ja_keys != en_keys:
            print(
                f"  [ERROR] Locale mismatch: ja-en={ja_keys - en_keys}, en-ja={en_keys - ja_keys}"
            )
            ok = False
        obs_src = (REPO_ROOT / "plugin/src/obs/plugin_main.cpp").read_text(encoding="utf-8") + (
            REPO_ROOT / "plugin/src/obs/kotone_filter.cpp"
        ).read_text(encoding="utf-8")
        used = set(re.findall(r'"(Kotone\.[A-Za-z0-9_.]+)"', obs_src))
        if used != ja_keys:
            print(
                f"  [ERROR] Locale usage mismatch: missing={used - ja_keys}, "
                f"unused={ja_keys - used}"
            )
            ok = False

    if ok:
        print("  [PASS] All static policy audits passed.")
    return ok


def cmd_fmt(strict: bool) -> int:
    rc = 0

    cf = _find_tool("clang-format")
    cpp_files = [str(p.relative_to(REPO_ROOT)) for p in _cpp_all_files()]
    if cf:
        if _run([*cf, "-i", *cpp_files]) != 0:
            rc = 1
    else:
        msg = (
            "clang-format not found "
            "(install via: winget install LLVM.LLVM / apt install clang-format)"
        )
        print(f"  [{'ERROR' if strict else 'SKIP'}] {msg}")
        if strict:
            rc = 1

    ruff = _find_tool("ruff")
    if ruff:
        ruff_fix = [*ruff, "check", "--config", str(RUFF_CONFIG), "--fix", "profiler", "scripts"]
        if _run(ruff_fix) != 0:
            rc = 1
        if _run([*ruff, "format", "--config", str(RUFF_CONFIG), "profiler", "scripts"]) != 0:
            rc = 1
    else:
        msg = "ruff not found (install via: pip install ruff / uv tool install ruff)"
        print(f"  [{'ERROR' if strict else 'SKIP'}] {msg}")
        if strict:
            rc = 1

    return rc


def cmd_lint(strict: bool) -> int:
    rc = 0

    if not _run_static_audits():
        rc = 1

    # 1. Ruff lint + format check
    ruff = _find_tool("ruff")
    if ruff:
        if _run([*ruff, "check", "--config", str(RUFF_CONFIG), "profiler", "scripts"]) != 0:
            rc = 1
        if (
            _run([*ruff, "format", "--check", "--config", str(RUFF_CONFIG), "profiler", "scripts"])
            != 0
        ):
            rc = 1
    else:
        msg = "ruff not found (install via: pip install ruff / uv tool install ruff)"
        print(f"  [{'ERROR' if strict else 'SKIP'}] {msg}")
        if strict:
            rc = 1

    # 2. clang-format dry-run check
    cf = _find_tool("clang-format")
    cpp_files = [str(p.relative_to(REPO_ROOT)) for p in _cpp_all_files()]
    if cf:
        if _run([*cf, "--dry-run", "--Werror", *cpp_files]) != 0:
            rc = 1
    else:
        msg = (
            "clang-format not found "
            "(install via: winget install LLVM.LLVM / apt install clang-format)"
        )
        print(f"  [{'ERROR' if strict else 'SKIP'}] {msg}")
        if strict:
            rc = 1

    # 3. clang-tidy static analysis (requires build/compile_commands.json)
    ct = _find_tool("clang-tidy")
    compile_db = REPO_ROOT / "build" / "compile_commands.json"
    if ct:
        if not compile_db.is_file():
            print("  [INFO] Generating build/compile_commands.json for clang-tidy...")
            _run(
                [
                    "cmake",
                    "-B",
                    "build",
                    "-DKOTONE_BUILD_OBS_PLUGIN=OFF",
                    "-DKOTONE_BUILD_TESTS=ON",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                ]
            )
        if compile_db.is_file():
            targets = [str(p.relative_to(REPO_ROOT)) for p in _cpp_tidy_targets()]
            if _run([*ct, "-p", "build", *targets]) != 0:
                rc = 1
        else:
            print("  [SKIP] clang-tidy skipped (compile_commands.json unavailable)")
    else:
        msg = (
            "clang-tidy not found (install via: winget install LLVM.LLVM / apt install clang-tidy)"
        )
        print(f"  [{'ERROR' if strict else 'SKIP'}] {msg}")
        if strict:
            rc = 1

    return rc


def cmd_test() -> int:
    if (
        _run(
            [
                "cmake",
                "-B",
                "build",
                "-DKOTONE_BUILD_OBS_PLUGIN=OFF",
                "-DKOTONE_BUILD_TESTS=ON",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            ]
        )
        != 0
    ):
        return 1
    if _run(["cmake", "--build", "build", "--config", "Release"]) != 0:
        return 1
    if _run(["ctest", "--test-dir", "build", "-C", "Release", "--output-on-failure"]) != 0:
        return 1
    if _run([sys.executable, "-m", "pytest", "profiler/tests"]) != 0:
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Kotone project formatter, linter, and test runner."
    )
    parser.add_argument(
        "action",
        choices=("fmt", "lint", "test", "check"),
        help="Action to execute (check runs lint + test).",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail if external lint tools (ruff, clang-format, clang-tidy) are not installed.",
    )
    args = parser.parse_args(argv)

    if args.action == "fmt":
        return cmd_fmt(strict=args.strict)
    if args.action == "lint":
        return cmd_lint(strict=args.strict)
    if args.action == "test":
        return cmd_test()
    if args.action == "check":
        lint_rc = cmd_lint(strict=args.strict)
        test_rc = cmd_test()
        return 0 if (lint_rc == 0 and test_rc == 0) else 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
