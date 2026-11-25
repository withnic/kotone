@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "PYTHONPATH=%SCRIPT_DIR%;%PYTHONPATH%"
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

python -m kotone_profiler wizard "%~1"
echo.
pause
