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

REM The console needs a Python with pyserial, which the system one usually lacks. Two
REM installer layouts have to be covered: install.bat creates
REM   %IDF_TOOLS_PATH%\python_env\idf<x.y>_py<a.b>_env
REM while EIM creates
REM   %IDF_TOOLS_PATH%\python\<x.y>\venv
REM Nothing here needs idf.py, only the interpreter, so no environment activation.
set PY=
if defined IDF_PYTHON_ENV_PATH (
    if exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" set PY=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe
)
if "%PY%"=="" (
    for /d %%D in ("%USERPROFILE%\.espressif\python_env\idf*") do (
        if exist "%%D\Scripts\python.exe" set PY=%%D\Scripts\python.exe
    )
)
if "%PY%"=="" if defined IDF_TOOLS_PATH (
    for /d %%D in ("%IDF_TOOLS_PATH%\python\*") do (
        if exist "%%D\venv\Scripts\python.exe" set PY=%%D\venv\Scripts\python.exe
    )
)
if "%PY%"=="" (
    for /d %%D in ("C:\Espressif\tools\python\*") do (
        if exist "%%D\venv\Scripts\python.exe" set PY=%%D\venv\Scripts\python.exe
    )
)
if "%PY%"=="" (
    echo No ESP-IDF Python found. Looked in IDF_PYTHON_ENV_PATH,
    echo   %USERPROFILE%\.espressif\python_env  and  ^<IDF_TOOLS_PATH^>\python\*\venv
    echo Trying the system one - if pyserial is missing, that is why.
    set PY=python
)

echo == python: %PY%
echo == port %PORT%, %SECONDS% s, console baud 115200
"%PY%" "%~dp0%SCRIPT%" %PORT% %SECONDS%
exit /b %ERRORLEVEL%
