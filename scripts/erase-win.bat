@echo off
REM Czysci CALY flash ukladu - firmware ORAZ partycje NVS z kluczami parowania.
REM
REM   scripts\erase-win.bat [port COM] [target]
REM   scripts\erase-win.bat COM6 esp32c3
REM
REM Kiedy to jest potrzebne:
REM   - urzadzenia nie chca sie podlaczyc, a podejrzewamy nieaktualne bondy w NVS
REM     (mostek trzyma trzy: klawiatura, mysz, PC),
REM   - chcemy zaczac od czystego stanu przed testem parowania,
REM   - firmware wpadl w petle restartow i chcemy wykluczyc zawartosc NVS.
REM
REM CZEGO SIE SPODZIEWAC PO WYCZYSZCZENIU:
REM   - klawiature i mysz trzeba wprowadzic w tryb parowania jeszcze raz,
REM   - pada trzeba USUNAC z listy urzadzen Bluetooth w Windows i sparowac od nowa,
REM     bo Windows ma zapisany klucz do urzadzenia, ktore go juz nie ma,
REM   - sam flash bez firmware'u nic nie robi: po wyczyszczeniu trzeba wgrac obraz
REM     przez scripts\flash-win.bat.
REM
REM Sciezke do ESP-IDF mozna nadpisac zmienna IDF_WIN.

setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM6
set TARGET=%2
if "%TARGET%"=="" set TARGET=esp32c3

if "%IDF_WIN%"=="" set IDF_WIN=%USERPROFILE%\esp\v5.5.1\esp-idf
if not exist "%IDF_WIN%\export.bat" (
    echo BLAD: nie znajduje ESP-IDF w %IDF_WIN%
    echo Ustaw sciezke:  set IDF_WIN=^<katalog esp-idf^>
    exit /b 1
)

echo.
echo === CZYSZCZENIE CALEGO FLASHA na %PORT% (%TARGET%) ===
echo To usunie firmware ORAZ wszystkie klucze parowania z NVS.
echo Po tej operacji trzeba sparowac klawiature, mysz i pada od nowa.
echo.
choice /C TN /N /M "Kontynuowac? [T/N] "
if errorlevel 2 (
    echo Przerwane, nic nie zmieniono.
    exit /b 1
)

REM MSYSTEM jest czyszczone, bo export.bat z ESP-IDF odmawia startu pod Git Bash/MSYS.
set MSYSTEM=
call "%IDF_WIN%\export.bat" >nul 2>&1
if errorlevel 1 (
    echo BLAD: nie moge zaladowac srodowiska ESP-IDF
    exit /b 1
)

python -m esptool --chip %TARGET% --port %PORT% erase_flash
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo BLAD czyszczenia ^(kod %RC%^). Czy plytka jest podlaczona i port %PORT% wolny?
    exit /b %RC%
)

echo.
echo Flash wyczyszczony. Teraz wgraj firmware:
echo   scripts\flash-win.bat %PORT% %TARGET%
exit /b 0
