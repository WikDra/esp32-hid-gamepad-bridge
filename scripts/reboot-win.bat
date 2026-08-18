@echo off
REM Reboots the board and exits - no log reading.
REM
REM   scripts\reboot-win.bat [COM port]
REM   scripts\reboot-win.bat COM7
REM
REM Why this exists separately from monitor-win.bat: the console over native USB was once
REM seen to go mute after hours of idling while the firmware itself kept running (the pad
REM stayed paired). A deliberate reboot brings the log back, and sometimes that is all you
REM want - without opening a monitor session.
REM
REM On boards with native USB the RTS line drives CHIP_EN, so a short pulse resets the
REM chip. That is the same mechanism scripts\reset_monitor.py uses; here we just do the
REM pulse and quit.

setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM6

set PY=
for /d %%D in ("%USERPROFILE%\.espressif\python_env\idf*") do (
    if exist "%%D\Scripts\python.exe" set PY=%%D\Scripts\python.exe
)
if "%PY%"=="" (
    echo No ESP-IDF Python found in %USERPROFILE%\.espressif\python_env
    echo Trying the system one - if pyserial is missing, that is why.
    set PY=python
)

"%PY%" -c "import sys, time, serial; p=sys.argv[1]; s=serial.Serial(p, 115200, timeout=0.5); s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False); s.close(); print('reboot pulse sent on ' + p)" %PORT%
exit /b %ERRORLEVEL%
