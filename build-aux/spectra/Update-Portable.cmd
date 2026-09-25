@echo off
rem Updates a portable OBS-Spectra folder to this version, keeping its settings (see Update-Portable.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update-Portable.ps1" %*
echo.
pause
