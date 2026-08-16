# esp32-hid-gamepad-bridge

Mostek BLE na ESP32-C3 SuperMini: **klawiatura i mysz Bluetooth → pad (BLE HID Gamepad) widziany przez PC**.

```
AULA F99 Pro    (BLE HID keyboard) ─┐
                                    ├─→ ESP32-C3 SuperMini ─→ BLE HID Gamepad ─→ PC (Windows)
AJAZZ AJ159 Pro (BLE HID mouse)   ──┘      (2× central + 1× peripheral)
```

Projekt eksperymentalny (proof-of-concept). Celem jest sprawdzenie, czy jeden ESP32-C3 utrzyma
jednocześnie dwa połączenia jako **central** (odbiór z klawiatury i myszy) oraz jedno jako
**peripheral** (wystawienie pada do PC).

To **nie jest** emulacja pada Xbox ani XInput — PC widzi generyczny pad HID (DirectInput,
`joy.cpl`). Gry korzystające wyłącznie z XInput go nie zobaczą.

Notatki techniczne, ustalenia i pułapki: [`AGENTS.md`](AGENTS.md).

## Sprzęt

| Element | Uwagi |
|---|---|
| ESP32-C3 SuperMini | 4 MB flash, **brak PSRAM**, natywne USB (USB Serial/JTAG) — nie ma mostka USB-UART |
| AULA F99 Pro | klawiatura tri-mode; używamy trybu BLE 5.0 |
| AJAZZ AJ159 Pro | mysz tri-mode; używamy trybu BLE 5.0 |

Płytka podłączona do PC jednym kablem USB-C — służy jednocześnie do zasilania, wgrywania
firmware i konsoli. Żadnego dodatkowego okablowania nie trzeba.

## Wymagania po stronie PC

- **ESP-IDF v5.5.1** (nie starszy — patrz `AGENTS.md`, sekcja o `GATTC_AUTO_PAIR`).
- Build w WSL (`~/esp/v5.5.1/esp-idf`), wgrywanie i konsola z Windows (`C:\Users\<user>\esp\v5.5.1\esp-idf`).
- Python z `pyserial` na Windows (do `scripts/monitor.py`). Systemowy Python go zwykle nie ma —
  najprościej użyć tego z ESP-IDF:
  `C:\Users\<user>\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe`.

## Budowanie i wgrywanie

```bat
scripts\build-win.bat              REM build w WSL (target esp32c3)
scripts\flash-win.bat COM6         REM wgranie z Windows
scripts\monitor-win.bat COM6 30    REM konsola na 30 s, bez resetu płytki
```

`build-win.bat` to tylko opakowanie na `scripts/build.sh`, który uruchamia się w WSL.
Binarki trafiają do `firmware/build.esp32c3/`.

Reset płytki, gdy trzeba zobaczyć log od pierwszej linii:

```bat
scripts\monitor-win.bat COM6 30 reset
```

`monitor-win.bat` sam znajduje pythona z ESP-IDF. Jeśli wolisz wywołać skrypty wprost:

```bat
python scripts\monitor.py COM6 30
python scripts\reset_monitor.py COM6 30
```

Uwaga: przy natywnym USB linie DTR/RTS sterują resetem i bootloaderem, dlatego `monitor.py`
otwiera port z DTR/RTS wyłączonymi (nie restartuje układu), a `reset_monitor.py` restartuje
świadomie.

## Parowanie

Kolejność ma znaczenie — najpierw wejścia, potem PC:

1. Wgraj firmware i zostaw płytkę zasilaną.
2. Wprowadź klawiaturę w tryb parowania BLE (AULA F99 Pro: `Fn` + **cyfra** kanału BT,
   przytrzymane ~3 s — dioda kanału zaczyna mrugać szybko). Mostek skanuje w pętli
   i połączy się sam.

   Windows w tym momencie też zobaczy klawiaturę i zaproponuje jej sparowanie —
   **odrzuć to okno**. Jeśli klawiatura sparuje się z Windows, połączy się tam, a nie
   z mostkiem.
3. To samo z myszą (AJAZZ AJ159 Pro: przełącznik trybu na BT, przycisk parowania).
4. Na PC: *Ustawienia → Bluetooth → Dodaj urządzenie* → wybierz **C3 Gamepad**.
5. Sprawdź w `joy.cpl` (Win+R → `joy.cpl` → *Właściwości*), czy pad reaguje.

Klucze parowania są zapisywane w NVS, więc po restarcie urządzenia łączą się same.

Jeśli w logu pojawi się `esp_hidh_dev_open() nie wrocilo w 45 s`, mostek **restartuje się
sam**. To obejście błędu w ESP-IDF, który przy zerwaniu połączenia w trakcie odczytu usług
zawiesza wątek na stałe (szczegóły w `AGENTS.md` §4.23). Po restarcie PC wraca po ~2 s,
a klawiatura i mysz podłączają się same — nie trzeba nic robić.

**Po zmianie deskryptora raportu HID trzeba usunąć pad z listy urządzeń Bluetooth w Windows
i sparować go ponownie** — Windows cache'uje deskryptor per sparowane urządzenie i inaczej
pokaże stary układ osi i przycisków.

## Mapowanie wejść na pada

| Wejście | Wyjście na padzie |
|---|---|
| `W` / `S` / `A` / `D` | lewy analog (skos skalowany, żeby nie był szybszy) |
| ruch myszy | prawy analog (przyrost skalowany; po zatrzymaniu gałka wraca do środka) |
| lewy / prawy / środkowy przycisk myszy | przycisk 1 / 2 / 3 |
| `Spacja` | przycisk 4 |
| lewy `Shift` / lewy `Ctrl` | przycisk 5 / 6 |
| `E` / `Q` / `R` / `F` / `Tab` / `Esc` | przycisk 7 / 8 / 9 / 10 / 11 / 12 |

Czułość myszy reguluje `CONFIG_APP_MOUSE_SCALE_DIV` (większa wartość = mniej czuła).
Zmiana wymaga przebudowania: `scripts\build-win.bat esp32c3 menuconfig`, sekcja
*Mostek HID (konfiguracja aplikacji)*.

Do sprawdzenia samego deskryptora HID, bez klawiatury i myszy, jest
`CONFIG_APP_GAMEPAD_SELFTEST` — pad krąży wtedy analogami i cyklicznie wciska przyciski.
Przy włączonym selfteście mapowanie wejść jest nieaktywne (obie rzeczy pisałyby ten sam
raport).

## Stan projektu

Patrz tabela w [`AGENTS.md`](AGENTS.md#2-stan-projektu) — tam jest rozdzielone to, co
zweryfikowane na sprzęcie, od tego, co tylko się kompiluje.
