@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0car-session.ps1" -Step dhcp-start
pause
