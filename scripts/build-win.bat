@echo off
REM Runs the build in WSL from a Windows shell.
REM
REM   scripts\build-win.bat [target] [extra idf.py arguments]
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
REM The repo path is derived from this script's location and translated to the WSL
REM convention with wslpath - so the script works regardless of where the repo was
REM cloned and on which drive.
for /f "usebackq delims=" %%p in (`wsl -e wslpath -a "%~dp0.."`) do set REPO_WSL=%%p
if "%REPO_WSL%"=="" (
    echo ERROR: cannot determine the repo path in WSL ^(is WSL running?^)
    exit /b 1
)
wsl -e bash -lc "cd '%REPO_WSL%' && ./scripts/build.sh %TARGET%%EXTRA%"
exit /b %ERRORLEVEL%
