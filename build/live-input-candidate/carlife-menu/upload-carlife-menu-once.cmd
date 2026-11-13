@echo off
rem Uploads the CarLife iDrive menu files to /dev/shmem/clmenu-* with ONE ssh login (one password). The car has no tar,
rem so the 17 files are joined into one stream and cut apart again on the car with dd (exact sizes, bs=1).
setlocal enabledelayedexpansion
set "P=%~dp0..\idrive-hold\package"
set "B=%~dp0..\build"
set "T=%TEMP%\clmenu.bin"
set "CP="
set "RC=cd /dev/shmem;"
set "N=0"
call :add "%P%\libNbtAad.so" || exit /b 1
call :add "%P%\NbtAad.manifest" || exit /b 1
call :add "%P%\NbtAad.xml" || exit /b 1
call :add "%P%\NbtAad_ALL_Images.zip" || exit /b 1
call :add "%~dp0package\NbtAad_ALL_Texts.zip" || exit /b 1
call :add "%~dp0package\nbt-aad" || exit /b 1
for %%F in (carlife_qnx_usb_capture carlife_qnx_mmr_replay carlife_qnx_most_watch carlife_qnx_cp_purge carlife_qnx_ctl) do call :add "%B%\%%F.elf" || exit /b 1
call :add "%~dp0..\tools\carlife_usb_visible.sh" || exit /b 1
call :add "%~dp0..\tools\carlife_visible_first_frame.sh" || exit /b 1
call :add "%~dp0..\tools\carlife_cp_purge.sh" || exit /b 1
call :add "%~dp0..\src\carlife_qnx_replay_guard.sh" || exit /b 1
call :add "%~dp0tools\clmenu_install.sh" || exit /b 1
call :add "%~dp0tools\clmenu_uninstall.sh" || exit /b 1
set "CP=!CP:~1!"
copy /b !CP! "%T%" >nul || exit /b 1
set "RC=!RC! ls -l clmenu-*"
echo Uploading %N% files (enter the password once)
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 "!RC!" < "%T%"
exit /b %errorlevel%

:add
if not exist "%~1" (echo Missing %~1 & exit /b 1)
for %%I in ("%~1") do set "SZ=%%~zI"
set "CP=!CP!+%~1"
set "RC=!RC! dd of=clmenu-%~nx1 bs=1 count=!SZ! 2>/dev/null;"
set /a N+=1
exit /b 0
