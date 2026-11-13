@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\.."
"D:\MATLAB\toolbox\sldrt\clang\win64\clang.exe" --target=x86_64-pc-windows-msvc -std=c11 -Wall -Wextra -Werror tools\tests\replay-duration-host.c -o build\live-30s-candidate\build\obj\replay-duration-host.exe || exit /b 1
build\live-30s-candidate\build\obj\replay-duration-host.exe
