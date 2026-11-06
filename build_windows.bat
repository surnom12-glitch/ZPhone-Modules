@echo off
setlocal
cd /d "%~dp0"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFFMPEG_ROOT="%CD%\ffmpeg-sdk"
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
echo DLL: build\Release\gmcl_zhphone_win64.dll
