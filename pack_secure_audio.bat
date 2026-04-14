@echo off
setlocal

set "AA_DIR=%~dp0"
if "%AA_DIR:~-1%"=="\" set "AA_DIR=%AA_DIR:~0,-1%"
set "MODE_ARG="
if /I "%~1"=="packonly" set "MODE_ARG=-PackOnly"

echo [INFO] aa dir: "%AA_DIR%"
if defined MODE_ARG (
    echo [INFO] mode: pack only
) else (
    echo [INFO] mode: full installer
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%AA_DIR%\\make_install_package.ps1" -AaDir "%AA_DIR%" %MODE_ARG%
if errorlevel 1 (
    echo [ERROR] process failed.
    exit /b 1
)

if defined MODE_ARG (
    echo [DONE] pack finished.
) else (
    echo [DONE] installer finished.
)
exit /b 0
