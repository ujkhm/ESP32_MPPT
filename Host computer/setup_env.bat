@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

echo [setup] Creating Python virtual environment...
if not exist ".venv\Scripts\python.exe" (
    py -3 -m venv .venv
    if errorlevel 1 (
        echo Failed to create virtual environment.
        exit /b 1
    )
)

echo [setup] Installing dependencies...
".venv\Scripts\python.exe" -m pip install --upgrade pip
".venv\Scripts\pip.exe" install -r requirements-dev.txt
if errorlevel 1 (
    echo Failed to install dependencies.
    exit /b 1
)

echo.
echo Environment ready.
echo   Dev run : run_dev.bat
echo   Build   : build.bat
echo.
pause
