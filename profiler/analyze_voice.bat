@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "PYTHONUTF8=1"

if "%~1"=="" (
    echo ============================================================
    echo   Kotone Voice Profiler - Drag and Drop Launcher
    echo ============================================================
    echo.
    echo Please drag and drop a recorded .wav file onto this .bat file.
    echo.
    pause
    exit /b 1
)

rem Release ZIP ships an embeddable Python in .\python (no install needed).
rem Source checkouts fall back to the Python on PATH.
if exist "%SCRIPT_DIR%python\python.exe" (
    "%SCRIPT_DIR%python\python.exe" -X utf8 -m kotone_profiler wizard "%~1"
) else (
    set "PYTHONPATH=%SCRIPT_DIR%;%PYTHONPATH%"
    python -m kotone_profiler wizard "%~1"
)
echo.
pause
