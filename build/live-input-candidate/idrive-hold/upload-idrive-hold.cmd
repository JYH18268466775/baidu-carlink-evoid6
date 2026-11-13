@echo off
setlocal
call :upload package\libNbtAad.so || exit /b 1
call :upload package\NbtAad.manifest || exit /b 1
call :upload package\NbtAad.xml || exit /b 1
call :upload package\NbtAad_ALL_Images.zip || exit /b 1
call :upload package\NbtAad_ALL_Texts.zip || exit /b 1
call :upload package\nbt-aad || exit /b 1
call :upload tools\idhold_install.sh || exit /b 1
call :upload tools\idhold_uninstall.sh || exit /b 1
echo iDrive hold upload complete: files are /dev/shmem/idhold-*
exit /b 0

:upload
if not exist "%~dp0%~1" exit /b 1
echo Uploading %~nx1
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 "cat > /dev/shmem/idhold-%~nx1" < "%~dp0%~1"
exit /b %errorlevel%
