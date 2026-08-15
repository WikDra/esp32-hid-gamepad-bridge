@echo off
REM Uruchamia build w WSL z powloki Windows.
REM
REM   scripts\build-win.bat [target] [dodatkowe argumenty idf.py]
REM   scripts\build-win.bat esp32c3
REM   scripts\build-win.bat esp32c3 menuconfig
setlocal
set TARGET=%1
if "%TARGET%"=="" set TARGET=esp32c3
shift
set EXTRA=
:collect
if "%1"=="" goto run
set EXTRA=%EXTRA% %1
shift
goto collect
:run
wsl -e bash -lc "cd /mnt/d/wysypisko/esp32-hid-gamepad-bridge && ./scripts/build.sh %TARGET%%EXTRA%"
exit /b %ERRORLEVEL%
