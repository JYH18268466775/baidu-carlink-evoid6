@echo off
setlocal
call :upload ..\verified-live-baseline\build\carlife_qnx_usb_capture.elf || exit /b 1
call :upload ..\verified-live-baseline\build\carlife_qnx_mmr_replay.elf || exit /b 1
call :upload carlife_usb_visible.sh || exit /b 1
call :upload carlife_visible_first_frame.sh || exit /b 1
call :upload ..\verified-live-baseline\src\carlife_qnx_replay_guard.sh || exit /b 1
echo CarLife unplug test upload complete.
exit /b 0

:upload
if not exist "%~dp0%~1" exit /b 1
echo Uploading %~nx1
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 "cat > /dev/shmem/%~nx1" < "%~dp0%~1"
exit /b %errorlevel%
