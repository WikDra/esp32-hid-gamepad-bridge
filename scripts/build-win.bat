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
REM Sciezke do repo wyliczamy z polozenia tego skryptu i tlumaczymy na konwencje WSL
REM przez wslpath - zeby skrypt dzialal niezaleznie od tego, gdzie repo zostalo
REM sklonowane i na ktorym dysku.
for /f "usebackq delims=" %%p in (`wsl -e wslpath -a "%~dp0.."`) do set REPO_WSL=%%p
if "%REPO_WSL%"=="" (
    echo BLAD: nie moge ustalic sciezki repo w WSL ^(czy WSL dziala?^)
    exit /b 1
)
wsl -e bash -lc "cd '%REPO_WSL%' && ./scripts/build.sh %TARGET%%EXTRA%"
exit /b %ERRORLEVEL%
