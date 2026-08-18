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

REM Sciezke do windowsowej instalacji ESP-IDF mozna nadpisac zmienna IDF_WIN,
REM np.  set IDF_WIN=D:\esp\v5.5.1\esp-idf
if "%IDF_WIN%"=="" set IDF_WIN=%USERPROFILE%\esp\v5.5.1\esp-idf
set BUILD_DIR=%~dp0..\firmware\build.%TARGET%

if not exist "%IDF_WIN%\export.bat" (
    echo BLAD: nie znajduje ESP-IDF w %IDF_WIN%
    echo Ustaw sciezke:  set IDF_WIN=^<katalog esp-idf^>
    exit /b 1
)

REM Build z WSL, a jesli go nie ma - build windowsowy (scripts\build-native-win.bat).
if not exist "%BUILD_DIR%\flash_args" (
    if exist "%~dp0..\firmware\build.win.%TARGET%\flash_args" (
        set BUILD_DIR=%~dp0..\firmware\build.win.%TARGET%
    )
)

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
