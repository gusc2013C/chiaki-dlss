@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-NVIDIA-Runtime.ps1" %*
if errorlevel 1 (
    echo NVIDIA runtime setup failed.
    pause
    exit /b 1
)
pause
