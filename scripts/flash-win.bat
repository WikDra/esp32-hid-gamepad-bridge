@echo off
REM Wgrywa firmware zbudowany w WSL, uzywajac esptool z windowsowej instalacji ESP-IDF.
REM
REM   scripts\flash-win.bat [port COM] [target]
REM   scripts\flash-win.bat COM6 esp32c3
REM
REM MSYSTEM jest czyszczone, bo export.bat z ESP-IDF odmawia startu pod Git Bash/MSYS.
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM6
set TARGET=%2
if "%TARGET%"=="" set TARGET=esp32c3

set IDF_WIN=C:\Users\1thew\esp\v5.5.1\esp-idf
set BUILD_DIR=%~dp0..\firmware\build.%TARGET%

if not exist "%BUILD_DIR%\flash_args" (
    echo Brak katalogu build %BUILD_DIR% - najpierw: scripts\build-win.bat %TARGET%
    exit /b 1
)

set MSYSTEM=
call "%IDF_WIN%\export.bat" >nul 2>&1
if errorlevel 1 exit /b 1

pushd "%BUILD_DIR%"
python -m esptool --chip %TARGET% --port %PORT% --baud 921600 write_flash @flash_args
set RC=%ERRORLEVEL%
popd
exit /b %RC%
