@echo off
REM ESP32 console from Windows. Finds the ESP-IDF Python by itself, because the
REM system Python usually lacks pyserial.
REM
REM   scripts\monitor-win.bat [COM port] [seconds] [reset]
REM   scripts\monitor-win.bat COM6 30
REM   scripts\monitor-win.bat COM6 30 reset    <- reboots the board, log from line one
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM6
set SECONDS=%2
if "%SECONDS%"=="" set SECONDS=20
set MODE=%3

set SCRIPT=monitor.py
if /i "%MODE%"=="reset" set SCRIPT=reset_monitor.py

set PY=
for /d %%D in ("%USERPROFILE%\.espressif\python_env\idf*") do (
    if exist "%%D\Scripts\python.exe" set PY=%%D\Scripts\python.exe
)
if "%PY%"=="" (
    echo No ESP-IDF Python found in %USERPROFILE%\.espressif\python_env
    echo Trying the system one - if pyserial is missing, that is why.
    set PY=python
)

"%PY%" "%~dp0%SCRIPT%" %PORT% %SECONDS%
exit /b %ERRORLEVEL%
