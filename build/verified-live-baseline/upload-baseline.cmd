@echo off
setlocal
call :upload build\carlife_qnx_usb_capture.elf || exit /b 1
call :upload build\carlife_qnx_mmr_replay.elf || exit /b 1
call :upload tools\carlife_usb_visible.sh || exit /b 1
call :upload tools\carlife_visible_first_frame.sh || exit /b 1
call :upload src\carlife_qnx_replay_guard.sh || exit /b 1
echo CarLife baseline upload complete.
exit /b 0

:upload
echo Uploading %~nx1
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 "cat > /dev/shmem/%~nx1" < "%~dp0%~1"
exit /b %errorlevel%
