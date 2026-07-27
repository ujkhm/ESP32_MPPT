@echo off
setlocal

cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
    echo Virtual environment not found. Run setup_env.bat first.
    exit /b 1
)

echo [build] Packaging ESP32_MPPT_Host.exe ...
".venv\Scripts\pyinstaller.exe" --noconfirm --clean esp32_mppt_host.spec
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

if exist "dist\ESP32_MPPT_Host.exe" (
    copy /Y "dist\ESP32_MPPT_Host.exe" "ESP32_MPPT_Host.exe" >nul
    echo [build] Output: %CD%\ESP32_MPPT_Host.exe
) else (
    echo Build finished but exe was not found in dist\
    exit /b 1
)

echo Done.
pause
