@echo off
REM Flashes the firmware using esptool from the Windows ESP-IDF installation.
REM
REM   scripts\flash-win.bat [COM port] [target]
REM   scripts\flash-win.bat COM6 esp32c3
REM
REM MSYSTEM is cleared because ESP-IDF's export.bat refuses to run under Git Bash/MSYS.
setlocal EnableDelayedExpansion
set PORT=%1
if "%PORT%"=="" set PORT=COM6
set TARGET=%2
if "%TARGET%"=="" set TARGET=esp32c3

REM The path to the Windows ESP-IDF installation can be overridden with IDF_WIN,
REM e.g.  set IDF_WIN=D:\esp\v5.5.1\esp-idf
if "%IDF_WIN%"=="" set IDF_WIN=%USERPROFILE%\esp\v5.5.1\esp-idf

if not exist "%IDF_WIN%\export.bat" (
    echo ERROR: no ESP-IDF found in %IDF_WIN%
    echo Set the path:  set IDF_WIN=^<esp-idf directory^>
    exit /b 1
)

REM Two build directories can coexist: build.<target> from WSL and build.win.<target>
REM from a native Windows build. Choosing one by a fixed preference silently flashes a
REM stale image whenever the other one is newer - which cost real debugging time once.
REM So we flash whichever image was built LAST.
set WSL_DIR=%~dp0..\firmware\build.%TARGET%
set WIN_DIR=%~dp0..\firmware\build.win.%TARGET%
set BUILD_DIR=

if exist "%WSL_DIR%\flash_args" if exist "%WIN_DIR%\flash_args" (
    for /f "delims=" %%N in ('powershell -NoProfile -Command "if ((Get-Item '%WIN_DIR%\flash_args').LastWriteTime -ge (Get-Item '%WSL_DIR%\flash_args').LastWriteTime) { 'win' } else { 'wsl' }"') do set NEWER=%%N
    if "!NEWER!"=="win" (
        set BUILD_DIR=%WIN_DIR%
        echo == two builds present, flashing the newer one: build.win.%TARGET%
    ) else (
        set BUILD_DIR=%WSL_DIR%
        echo == two builds present, flashing the newer one: build.%TARGET%
    )
)
if "!BUILD_DIR!"=="" if exist "%WIN_DIR%\flash_args" set BUILD_DIR=%WIN_DIR%
if "!BUILD_DIR!"=="" if exist "%WSL_DIR%\flash_args" set BUILD_DIR=%WSL_DIR%

if "!BUILD_DIR!"=="" (
    echo ERROR: no build found for target %TARGET%
    echo Run scripts\build-native-win.bat  ^(or scripts\build-win.bat %TARGET%^) first
    exit /b 1
)

set MSYSTEM=
call "%IDF_WIN%\export.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: cannot load the ESP-IDF environment
    exit /b 1
)

pushd "!BUILD_DIR!"
python -m esptool --chip %TARGET% --port %PORT% --baud 921600 write_flash @flash_args
set RC=%ERRORLEVEL%
popd
exit /b %RC%
