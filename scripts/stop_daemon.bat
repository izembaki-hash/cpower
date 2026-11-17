@echo off
setlocal
cd /d "%~dp0.."
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage_interactive_daemon.ps1" -Action Stop
pause
