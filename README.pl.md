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

Pad ma **dwa profile**, wybierane w menuconfig (`APP_GAMEPAD_PROFILE`):

| Profil | Co widzi PC |
|---|---|
| **Xbox (XInput)** — domyślny | Mostek podaje się za bezprzewodowy pad Xbox Series X: deskryptor raportu bajt w bajt z prawdziwego pada oraz PnP ID z VID Microsoftu i PID 0x0B13 (tylko z tej rodziny PID sterownik XInput w Windows sie wiaze). Windows ładuje swój sterownik pada Xbox i udostępnia urządzenie przez **XInput**, czyli widzą go też gry, które nie obsługują DirectInput. |
| generyczny (DirectInput) | 4 osie i 12 przycisków, widoczne w `joy.cpl`. Gry korzystające wyłącznie z XInput takiego pada nie zobaczą. To profil zweryfikowany w Etapie 3, zostaje jako wyjście awaryjne. |

Przełączenie profilu zmienia deskryptor **i** tożsamość urządzenia, więc wymaga usunięcia
pada z listy urządzeń Bluetooth w Windows i sparowania od nowa.

*English version of this document: [`README.md`](README.md).*
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

> **Projekt zawiera załataną kopię komponentu ESP-IDF.** W `firmware/components/esp_hid/`
> leży kopia komponentu `esp_hid` z poprawką błędu, który uniemożliwiał ponowne połączenie
> urządzenia po jego uśpieniu (`services_discovered` nie było zerowane, co przy trzecim
> otwarciu urządzenia psuło stos — `AGENTS.md` §4.27). Kopia jest **przypięta do IDF 5.5.1**;
> po zmianie wersji IDF trzeba ją odtworzyć. Że build używa naszej kopii, a nie wersji z IDF,
> sprawdza `wsl python3 scripts/check_local_esp_hid.py`.

## Budowanie i wgrywanie

Wprost w Windows, bez WSL:

```bat
scripts\build-native-win.bat              REM build
scripts\build-native-win.bat menuconfig   REM konfiguracja
scripts\flash-win.bat COM6                REM wgranie
scripts\monitor-win.bat COM6 30           REM konsola na 30 s, bez resetu płytki
```

Albo build w WSL, wgrywanie z Windows:

```bat
scripts\build-win.bat              REM build w WSL (target esp32c3)
scripts\flash-win.bat COM6         REM wgranie z Windows
scripts\monitor-win.bat COM6 30    REM konsola na 30 s, bez resetu płytki
```

Obie drogi używają **osobnych katalogów build** (`build.esp32c3` dla WSL,
`build.win.esp32c3` dla Windows), bo ścieżki absolutne są w nich inne, a CMake nie zniesie
obu w jednym katalogu. `flash-win.bat` sam znajduje ten, który istnieje.

Ścieżkę do ESP-IDF można nadpisać: `set IDF_WIN=D:\esp\v5.5.1\esp-idf`.

Restart płytki bez otwierania konsoli:

```bat
scripts\reboot-win.bat COM6
```\n
Wyczyszczenie całego flasha razem z kluczami parowania w NVS:

```bat
scripts\erase-win.bat COM6
```

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
4. Na PC: *Ustawienia → Bluetooth → Dodaj urządzenie* → wybierz pada. W profilu Xbox
   rozgłasza się jako **Xbox Wireless Controller**, w generycznym jako **C3 Gamepad**.
5. Sprawdź, czy Windows podpiął właściwy sterownik. W profilu Xbox w `joy.cpl` (Win+R →
   `joy.cpl`) urządzenie ma się nazywać **„Urządzenie wejściowe Bluetooth LE zgodne
   z interfejsem XINPUT"** — wtedy pad idzie przez XInput i widzą go gry. Jeśli zamiast
   tego zobaczysz „Kontroler gier zgodny z HID" albo „6-osiowy 17-przyciskowy pad", to
   podpiął się sterownik generyczny; przyczyny i diagnostyka w `AGENTS.md` §4.32.

Klucze parowania są zapisywane w NVS, więc po restarcie urządzenia łączą się same.

Jeśli w logu pojawi się `esp_hidh_dev_open() did not return within 45 s`, mostek **restartuje się
sam**. To obejście błędu w ESP-IDF, który przy zerwaniu połączenia w trakcie odczytu usług
zawiesza wątek na stałe (szczegóły w `AGENTS.md` §4.23). Po restarcie PC wraca po ~2 s,
a klawiatura i mysz podłączają się same — nie trzeba nic robić.

**Po zmianie deskryptora raportu HID trzeba usunąć pad z listy urządzeń Bluetooth w Windows
i sparować go ponownie** — Windows cache'uje deskryptor per sparowane urządzenie i inaczej
pokaże stary układ osi i przycisków.

## Mapowanie wejść na pada

Wejścia są te same w obu profilach — różni się tylko to, czym są po stronie PC. Kolumna
„Xbox (XInput)" dotyczy profilu domyślnego.

| Wejście | Profil Xbox (XInput) | Profil generyczny |
|---|---|---|
| `W` / `S` / `A` / `D` | lewy analog | lewy analog |
| strzałki | **krzyżak (D-pad)** | nieużywane |
| ruch myszy | prawy analog | prawy analog |
| lewy przycisk myszy | **prawy spust (RT)** | przycisk 1 |
| prawy przycisk myszy | **lewy spust (LT)** | przycisk 2 |
| środkowy przycisk myszy | klik prawej gałki (RS) | przycisk 3 |
| `Spacja` | **A** | przycisk 4 |
| lewy `Shift` | klik lewej gałki (LS) | przycisk 5 |
| lewy `Ctrl` | **B** | przycisk 6 |
| `E` | **X** | przycisk 7 |
| `Q` | **Y** | przycisk 8 |
| `R` | LB | przycisk 9 |
| `F` | RB | przycisk 10 |
| `Tab` | View (dawne Back) | przycisk 11 |
| `Esc` | Menu (dawne Start) | przycisk 12 |

Skos na lewym analogu jest skalowany, żeby nie był szybszy niż ruch w linii prostej.
Prawy analog dostaje przeskalowany przyrost myszy i po zatrzymaniu wraca do środka.

Dwie rzeczy, które warto wiedzieć przy testowaniu profilu Xbox:

- **Spusty są analogowe, nie są przyciskami.** Kliknięcie myszą daje pełne wychylenie
  (1023), ale w siatce przycisków `joy.cpl` nic się nie zaświeci — spusty widać na osi.
  W logu płytki widać je wprost: `xbox: LT=1023 RT=0 …`.
- **Krzyżak działa tylko w profilu Xbox.** Deskryptor pada generycznego nie ma hat
  switcha, a dodanie go wymagałoby sparowania tamtego profilu od nowa.

Przypisanie zmienia się w jednym miejscu: tablica `s_xbox_ctrl` w
[`firmware/main/ble_gamepad.c`](firmware/main/ble_gamepad.c). Wejścia i profil generyczny
zostają wtedy nietknięte, bo `input_mapper` nie wie, który profil jest aktywny.

Czułość myszy reguluje `CONFIG_APP_MOUSE_SCALE_DIV` (większa wartość = mniej czuła).
Domyślne **24** oznacza pełne wychylenie gałki przy średnio 96 zliczeniach myszy na tik
zadania. Jeśli nadal za szybko dobija do maksimum, podnieś do 32 albo 48; jeśli za
sztywno — zejdź do 16. Zmiana wymaga tylko przebudowania i **nie** wymaga parowania pada
od nowa, bo deskryptor się nie zmienia:
`scripts\build-win.bat esp32c3 menuconfig`, sekcja *Mostek HID (konfiguracja aplikacji)*.

Do sprawdzenia samego deskryptora HID, bez klawiatury i myszy, jest
`CONFIG_APP_GAMEPAD_SELFTEST` — pad krąży wtedy analogami i cyklicznie wciska przyciski.
Przy włączonym selfteście mapowanie wejść jest nieaktywne (obie rzeczy pisałyby ten sam
raport).

## Stan projektu

Patrz tabela w [`AGENTS.md`](AGENTS.md#2-stan-projektu) — tam jest rozdzielone to, co
zweryfikowane na sprzęcie, od tego, co tylko się kompiluje.
