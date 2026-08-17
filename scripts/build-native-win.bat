@echo off
REM Budowanie WPROST W WINDOWS, bez WSL.
REM
REM Dlaczego to istnieje: build-win.bat uruchamia build.sh w WSL, a WSL potrafi sie
REM zawiesic tak, ze samo wpisanie "wsl" wisi bez konca, a "wsl --shutdown" tez nie
REM odpowiada (odblokowanie wymaga wtedy restartu uslugi WSLService z uprawnieniami
REM administratora). Skoro ESP-IDF jest zainstalowany takze po stronie Windows - bo
REM stad wgrywamy firmware - to nie ma powodu, zeby build od WSL zalezal.
REM
REM Uzywa OSOBNEGO katalogu build.win.esp32c3 i osobnego pliku sdkconfig.win.esp32c3,
REM zeby nie mieszac z buildem z WSL: tam sciezki absolutne to /mnt/d/..., a tutaj
REM D:\..., czego CMake w jednym katalogu nie zniesie.
REM
REM Uzycie:
REM   scripts\build-native-win.bat              - build
REM   scripts\build-native-win.bat menuconfig   - konfiguracja
REM   scripts\build-native-win.bat fullclean    - czyszczenie

setlocal
set IDF_DIR=C:\Users\%USERNAME%\esp\v5.5.1\esp-idf
set TARGET=esp32c3
set BUILD_DIR=build.win.%TARGET%
set SDKCONFIG=sdkconfig.win.%TARGET%
set ACTION=%1
if "%ACTION%"=="" set ACTION=build

if not exist "%IDF_DIR%\export.bat" (
    echo BLAD: nie znajduje ESP-IDF w %IDF_DIR%
    echo Popraw IDF_DIR w tym skrypcie.
    exit /b 1
)

pushd "%~dp0..\firmware" || exit /b 1
call "%IDF_DIR%\export.bat" >nul 2>&1

REM Przy pierwszym uruchomieniu trzeba ustawic target - inaczej idf.py nie wie,
REM na co budowac, a przy okazji wczytuje nasze pliki sdkconfig.defaults.
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo == pierwszy build: ustawiam target %TARGET%
    idf.py -B %BUILD_DIR% -D SDKCONFIG=%SDKCONFIG% ^
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.%TARGET%" ^
        set-target %TARGET% || (popd & exit /b 1)
)

idf.py -B %BUILD_DIR% %ACTION%
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
