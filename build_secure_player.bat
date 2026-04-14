@echo off
setlocal

set "SRC_DIR=%~dp0"
pushd "%SRC_DIR%"

if not exist "SecurePlayer.pro" (
    echo [ERROR] SecurePlayer.pro not found.
    popd
    exit /b 1
)

qmake SecurePlayer.pro -spec win32-g++
if errorlevel 1 (
    echo [ERROR] qmake failed.
    popd
    exit /b 1
)

mingw32-make -f Makefile.Release -j4
if errorlevel 1 (
    echo [ERROR] build failed.
    popd
    exit /b 1
)

if exist "release\\SecureAudioPlayer.exe" (
    powershell -NoProfile -Command "$n=[string]([char]0x676D)+[char]0x5DDE+[char]0x516C+[char]0x4EA4+[char]0x62A5+[char]0x7AD9+[char]0x8BED+[char]0x97F3+[char]0x5E93+'.exe'; Copy-Item -LiteralPath 'release\\SecureAudioPlayer.exe' -Destination (Join-Path 'release' $n) -Force"
)

echo [DONE] build complete under .\release\
popd
exit /b 0
