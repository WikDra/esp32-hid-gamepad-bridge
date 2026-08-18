@echo off
REM Erases the ENTIRE flash - firmware AND the NVS partition with the pairing keys.
REM
REM   scripts\erase-win.bat [COM port] [target]
REM   scripts\erase-win.bat COM6 esp32c3
REM
REM When this is needed:
REM   - devices refuse to connect and stale bonds in NVS are suspected
REM     (the bridge keeps three: keyboard, mouse, PC),
REM   - you want a clean slate before a pairing test,
REM   - the firmware is in a reboot loop and NVS contents need ruling out.
REM
REM WHAT TO EXPECT AFTERWARDS:
REM   - the keyboard and mouse have to be put into pairing mode again,
REM   - the pad must be REMOVED from the Windows Bluetooth device list and paired again,
REM     because Windows holds a key for a device that no longer has one,
REM   - an empty flash does nothing on its own: after erasing, the image has to be
REM     flashed with scripts\flash-win.bat.
REM
REM The ESP-IDF path can be overridden with the IDF_WIN variable.

setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM6
set TARGET=%2
if "%TARGET%"=="" set TARGET=esp32c3

if "%IDF_WIN%"=="" set IDF_WIN=%USERPROFILE%\esp\v5.5.1\esp-idf
if not exist "%IDF_WIN%\export.bat" (
    echo ERROR: no ESP-IDF found in %IDF_WIN%
    echo Set the path:  set IDF_WIN=^<esp-idf directory^>
    exit /b 1
)

echo.
echo === ERASING THE ENTIRE FLASH on %PORT% (%TARGET%) ===
echo This removes the firmware AND every pairing key from NVS.
echo Afterwards the keyboard, mouse and pad all have to be paired again.
echo.
choice /C YN /N /M "Continue? [Y/N] "
if errorlevel 2 (
    echo Aborted, nothing was changed.
    exit /b 1
)

REM MSYSTEM is cleared because ESP-IDF's export.bat refuses to run under Git Bash/MSYS.
set MSYSTEM=
call "%IDF_WIN%\export.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: cannot load the ESP-IDF environment
    exit /b 1
)

python -m esptool --chip %TARGET% --port %PORT% erase_flash
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo ERASE FAILED ^(code %RC%^). Is the board connected and port %PORT% free?
    exit /b %RC%
)

echo.
echo Flash erased. Now flash the firmware:
echo   scripts\flash-win.bat %PORT% %TARGET%
exit /b 0
