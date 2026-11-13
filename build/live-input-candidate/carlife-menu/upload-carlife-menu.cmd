@echo off
rem Uploads the CarLife iDrive menu files to /dev/shmem/clmenu-*. Build the ELFs first (tools\build-qnx-probes.ps1).
setlocal
set "P=%~dp0..\idrive-hold\package"
set "B=%~dp0..\build"
call :up "%P%\libNbtAad.so" || exit /b 1
call :up "%P%\NbtAad.manifest" || exit /b 1
call :up "%P%\NbtAad.xml" || exit /b 1
call :up "%P%\NbtAad_ALL_Images.zip" || exit /b 1
call :up "%~dp0package\NbtAad_ALL_Texts.zip" || exit /b 1
call :up "%~dp0package\nbt-aad" || exit /b 1
for %%F in (carlife_qnx_usb_capture carlife_qnx_mmr_replay carlife_qnx_most_watch carlife_qnx_cp_purge carlife_qnx_ctl) do call :up "%B%\%%F.elf" || exit /b 1
call :up "%~dp0..\tools\carlife_usb_visible.sh" || exit /b 1
call :up "%~dp0..\tools\carlife_visible_first_frame.sh" || exit /b 1
call :up "%~dp0..\tools\carlife_cp_purge.sh" || exit /b 1
call :up "%~dp0..\src\carlife_qnx_replay_guard.sh" || exit /b 1
call :up "%~dp0tools\clmenu_install.sh" || exit /b 1
call :up "%~dp0tools\clmenu_uninstall.sh" || exit /b 1
echo CarLife menu upload complete: files are /dev/shmem/clmenu-*
exit /b 0

:up
if not exist "%~1" (echo Missing %~1 & exit /b 1)
echo Uploading %~nx1
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969 "cat > /dev/shmem/clmenu-%~nx1" < "%~1"
exit /b %errorlevel%
