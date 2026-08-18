# Ustawienia lokalne (wzór)

Skopiuj ten plik jako `AGENTS.local.md` i wpisz swoje wartości. Prawdziwy `AGENTS.local.md`
jest gitignorowany, więc nie trafi do repozytorium.

Powód istnienia: `AGENTS.md` opisuje **projekt** i ma być użyteczny dla każdego, kto klonuje
repo. Numer portu COM, litera dysku czy nazwa użytkownika to rzeczy zależne od maszyny —
w notatkach projektu tylko przeszkadzają, a przy publikacji wyciekają.

## Ścieżki

| Co | Moja wartość |
|---|---|
| Repo | `D:\...\esp32-hid-gamepad-bridge` |
| ESP-IDF (Windows) | `%USERPROFILE%\esp\v5.5.1\esp-idf` |
| ESP-IDF (WSL) | `~/esp/v5.5.1/esp-idf` |
| Toolchain `addr2line` / `nm` | `~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin/riscv32-esp-elf-{addr2line,nm}` |
| Python z `pyserial` | `%USERPROFILE%\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe` |

Jeśli IDF jest w innym miejscu, skrypty przyjmują nadpisanie:

```bat
set IDF_WIN=D:\esp\v5.5.1\esp-idf
```

## Sprzęt

| Co | Moja wartość |
|---|---|
| Port płytki | `COM6` |
| Klawiatura | model, tryb BLE, sposób wejścia w parowanie |
| Mysz | model, tryb BLE, sposób wejścia w parowanie |
| Adresy BLE urządzeń | z logu, po `OPEN` |

## Uwagi do własnego sprzętu

Miejsce na rzeczy, które dotyczą tylko tego zestawu — np. że dana klawiatura wchodzi
w parowanie przez `Fn` + cyfrę kanału, że mysz ma przełącznik trybu na spodzie, albo że
któryś kabel USB nie przenosi danych.

## Typowe polecenia

```bat
scripts\build-native-win.bat
scripts\flash-win.bat COM6
scripts\monitor-win.bat COM6 45 reset
scripts\erase-win.bat COM6
python scripts\check_local_esp_hid.py
```

Dekodowanie backtrace'u z paniki (przydatne przy diagnozie crashy z §4.21 i §4.26):

```bash
riscv32-esp-elf-addr2line -pfiaC -e firmware/build.esp32c3/hid_gamepad_bridge.elf <adresy>
riscv32-esp-elf-nm -n firmware/build.esp32c3/hid_gamepad_bridge.elf | grep <symbol>
```
