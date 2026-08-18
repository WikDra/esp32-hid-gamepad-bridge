@echo off
REM Builds DIRECTLY ON WINDOWS, without WSL.
REM
REM Why this exists: build-win.bat runs build.sh in WSL, and WSL can wedge itself so
REM badly that typing "wsl" alone hangs forever and "wsl --shutdown" does not respond
REM either (unwedging then needs a restart of the WSLService service with
REM administrator rights). Since ESP-IDF is installed on the Windows side anyway -
REM that is where we flash from - there is no reason for the build to depend on WSL.
REM
REM It uses a SEPARATE build.win.esp32c3 directory and a separate sdkconfig.win.esp32c3,
REM so as not to clash with the WSL build: there the absolute paths are /mnt/..., here
REM they are Windows paths, and CMake will not tolerate both in one directory.
REM
REM Usage:
REM   scripts\build-native-win.bat              - build
REM   scripts\build-native-win.bat menuconfig   - configure
REM   scripts\build-native-win.bat fullclean    - clean

setlocal
REM The ESP-IDF path can be overridden with IDF_WIN, e.g.
REM   set IDF_WIN=D:\esp\v5.5.1\esp-idf
if "%IDF_WIN%"=="" set IDF_WIN=%USERPROFILE%\esp\v5.5.1\esp-idf
set IDF_DIR=%IDF_WIN%
set TARGET=esp32c3
set BUILD_DIR=build.win.%TARGET%
set SDKCONFIG=sdkconfig.win.%TARGET%
set ACTION=%1
if "%ACTION%"=="" set ACTION=build

if not exist "%IDF_DIR%\export.bat" (
    echo ERROR: no ESP-IDF found in %IDF_DIR%
    echo Set IDF_WIN or fix IDF_DIR in this script.
    exit /b 1
)

pushd "%~dp0..\firmware" || exit /b 1
call "%IDF_DIR%\export.bat" >nul 2>&1

REM On the first run the target has to be set - otherwise idf.py does not know what
REM to build for, and this is also what loads our sdkconfig.defaults files.
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo == first build: setting target %TARGET%
    idf.py -B %BUILD_DIR% -D SDKCONFIG=%SDKCONFIG% ^
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.%TARGET%" ^
        set-target %TARGET% || (popd & exit /b 1)
)

idf.py -B %BUILD_DIR% %ACTION%
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
