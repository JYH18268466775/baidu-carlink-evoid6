@echo off
setlocal
rem Upload only the three changed files. Existing 30-second launcher and guard are required.
call :upload build\carlife_qnx_usb_capture.elf || exit /b 1
call :upload build\carlife_qnx_mmr_replay.elf || exit /b 1
call :upload tools\carlife_visible_first_frame.sh || exit /b 1
echo Wide display candidate upload complete.
exit /b 0
:upload
if not exist "%~dp0%~1" exit /b 1
for %%F in ("%~dp0%~1") do if %%~zF EQU 0 exit /b 1
echo Uploading %~nx1
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 "cat > /dev/shmem/%~nx1" < "%~dp0%~1"
exit /b %errorlevel%
