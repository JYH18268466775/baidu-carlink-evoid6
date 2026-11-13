@echo off
rem Double-click: shows the CarLink step menu. Output is saved in carlink-logs\ next to this file.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0car-session.ps1" %*
pause
