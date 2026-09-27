@rem Updates a portable OBS-Spectra folder, keeping its settings (see Update-Portable.ps1).
@rem One line, so cmd never reads this file again after the update has replaced it.
@echo off & powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update-Portable.ps1" %* & echo. & pause & exit /b
