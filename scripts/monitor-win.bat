@echo off
REM Konsola ESP32 z Windows. Sam znajduje pythona z ESP-IDF, bo systemowy python
REM zwykle nie ma pyserial.
REM
REM   scripts\monitor-win.bat [port COM] [sekundy] [reset]
REM   scripts\monitor-win.bat COM6 30
REM   scripts\monitor-win.bat COM6 30 reset    <- restartuje plytke, log od pierwszej linii
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
    echo Nie znalazlem pythona z ESP-IDF w %USERPROFILE%\.espressif\python_env
    echo Probuje systemowego - jesli zabraknie pyserial, to jest przyczyna.
    set PY=python
)

"%PY%" "%~dp0%SCRIPT%" %PORT% %SECONDS%
exit /b %ERRORLEVEL%
