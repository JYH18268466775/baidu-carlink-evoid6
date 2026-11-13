@echo off
setlocal
call :upload build\carlife_qnx_most_watch.elf || exit /b 1
echo CarLife MOST watch upload complete.
exit /b 0

:upload
if not exist "%~dp0%~1" exit /b 1
echo Uploading %~nx1
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 "cat > /dev/shmem/%~nx1" < "%~dp0%~1"
exit /b %errorlevel%
