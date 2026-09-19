# Mostek BLE HID: klawiatura + mysz → pad na ESP32-C3

Notatki projektu i plan pracy dla kolejnego agenta. Stan na 2026-08-15.

Repo: `https://github.com/WikDra/esp32-hid-gamepad-bridge`.

> **Uwaga o cytowanych logach.** Notatki są po polsku, ale **kod, komunikaty logu
> i opisy opcji w menuconfig zostały przetłumaczone na angielski** przy przygotowaniu
> repozytorium do upublicznienia. Fragmenty logów w tym pliku są zapisem tego, co
> faktycznie wyszło z płytki **przed** tym tłumaczeniem, więc brzmienie komunikatów
> może się różnić od obecnego (np. `wejscia 2` → `inputs 2`,
> `zapis CCCD` → `CCCD write`). Świadomie ich nie przepisałem: to dowody, a nie
> dokumentacja. Same liczby, adresy i kody błędów pozostają aktualne.
Instrukcja dla człowieka: [`README.pl.md`](README.pl.md) (po polsku) albo
[`README.md`](README.md) (po angielsku, główne). Pochodzenie materiału zewnętrznego
i licencje: [`THIRD-PARTY.md`](THIRD-PARTY.md). Ustawienia zależne od maszyny:
`AGENTS.local.md` (wzór w [`AGENTS.local.example.md`](AGENTS.local.example.md)).

## 1. Cel

Jeden ESP32-C3 SuperMini pełni jednocześnie dwie role BLE:

- **2× central / GATT client** — odbiera raporty HID z klawiatury AULA F99 Pro i myszy
  AJAZZ AJ159 Pro (profil HOGP, usługa 0x1812),
- **1× peripheral / GATT server** — wystawia PC własną usługę HID 0x1812 z deskryptorem
  raportu pada.

Wejścia są mapowane na osie i przyciski pada, więc z punktu widzenia PC klawiatura i mysz
wyglądają jak jeden kontroler.

Pad ma dwa profile, wybierane w menuconfig (`APP_GAMEPAD_PROFILE`):

- **Xbox (XInput)** — mostek podaje się za bezprzewodowy pad Xbox Series X (PID 0x0B13):
  deskryptor raportu bajt w bajt taki jak w prawdziwym padzie i PnP ID z VID Microsoftu. Windows ładuje wtedy
  swój sterownik pada Xbox i udostępnia urządzenie przez XInput (§4.30, §4.31).
- **generyczny (DirectInput)** — 4 osie `int8` i 12 przycisków, widoczne w `joy.cpl`.
  Profil zweryfikowany w Etapie 3, zostaje jako wyjście awaryjne.

## 2. Stan projektu

Zrobione i **zweryfikowane na sprzęcie** (ESP32-C3 na COM6):

| Element | Status |
|---|---|
| Szkielet ESP-IDF, target esp32c3, IDF v5.5.1 | build OK, 203 728 B, `0x145430 bytes (87%) free` w partycji |
| Boot na płytce | `Project name: hid_gamepad_bridge`, `ESP-IDF: v5.5.1-dirty`, `chip revision: v0.4`, bez crashy |
| Konsola po USB Serial/JTAG | log widoczny na COM6, heartbeat `alive 50 s` |
| Brak PSRAM potwierdzony przez firmware | `flash 4096 kB, PSRAM brak` |
| Punkt wyjścia pamięci (bez BLE) | `free 319976 B`, największy blok `180224 B` |
| Skrypty build (WSL) / flash + monitor (Windows) | `build.sh`, `flash-win.bat`, `monitor-win.bat` — wszystkie użyte w praktyce |
| NimBLE startuje na C3 (central) | `stack gotowy (own_addr_type=0)`, `GAP procedure initiated: discovery` |
| Skaner widzi otoczenie i scala pakiety ADV | `skan: 2 urzadzen`, m.in. `e2:bb:9e:80:49:54 rssi=-55 'L3250 Series'` |
| Koszt pamięciowy NimBLE | `heap przed BLE: free 277988 B` → po starcie stacku `heap 207188 B`, czyli ~70 kB |
| Rozmiar firmware z BLE | `0x92a60` B (601 kB), `61%` partycji wolne |
| Usługa HID pada zarejestrowana w GATT | `usluga HID zarejestrowana, Report Map 56 B, raport 6 B`, `charakterystyka Report ma handle 21` |
| Pad rozgłasza się | `rozglaszam jako 'C3 Gamepad' (appearance 0x03c4)` |
| **Obie role naraz na jednym C3** | w jednym logu: `rozglaszam jako 'C3 Gamepad'` + `skan: 2 urzadzen` w pętli, bez crashy |
| Pamięć z oboma rolami aktywnymi | `heap 197712 B (min 197712 B)` — zapas ~190 kB |
| **Windows paruje pada i widzi go w `joy.cpl`** | `PC podlaczony, conn_handle=1`, `szyfrowanie/parowanie: status=0`, `PC wlaczyl notyfikacje raportu`, `MTU=256`; właściciel potwierdził, że w `joy.cpl` widać sekwencję testową |
| Bond pada przeżywa restart C3 | po `reset` PC łączy się sam po 2,1 s, bez akcji użytkownika — czyli `NVS_PERSIST=y` działa |
| Skaner znajduje klawiaturę | `HID ed:c7:b1:bb:83:01 type=1 rssi=-32 appearance=0x03c1 'AULA-F99Pro'` — adres **Random**, appearance keyboard |
| Połączenie z klawiaturą się ustanawia | `GAP procedure initiated: connect` → `Connection established` |
| **Klawiatura podłączona i wysyła raporty** | `OPEN c8:d7:b2:79:50:5f 'AULA-F99Pro 5.0 ' vid=0x3554 pid=0xfa07`, potem `KBD map=0 id=1 len=8 [00 00 14 …]` przy pisaniu |
| Tabela raportów klawiatury odczytana | 10 raportów, maska usage `0x63`, w tym własny raport myszy `id=5` (§4.16) |
| Klasyfikacja urządzenia (kbd/mouse) | `wejscia 1 (kbd=1 mouse=1)` — poprawiona, liczona z listy raportów (§4.15) |
| **Rekonekcja po wybudzeniu klawiatury** | `DIR_IND BOND` bez nazwy i appearance → połączenie bez akcji użytkownika (§4.20) |
| Bond klawiatury zapisany w NVS | `bondow w NVS: 2` (PC + klawiatura) po restarcie C3 |
| Margines stosu `hid_scan` | `po esp_hidh_dev_open zostalo 5588 B stosu` z 8192 B, czyli szczyt ~2604 B (§4.11) |
| Pamięć z padem + klawiaturą | `heap 192100 B (min 191148 B)` |
| **Etap 3 end-to-end: klawiatura → pad → PC** | zweryfikowane w `joy.cpl`; w logu zgadza się każde mapowanie: `04`→`L(-127,0)`, `07`→`L(127,0)`, `1a`→`L(0,-127)`, `16`→`L(0,127)`, `2c`→`btn=0x008`, `02`(LShift)→`0x010`, `01`(LCtrl)→`0x020`, `08`→`0x040`, `14`→`0x080`, `15`→`0x100`, `2b`→`0x400`, `29`→`0x800` |
| Klawisze niezmapowane są ignorowane | `04` (LAlt) i `35` (`` ` ``) nie zmieniają raportu pada |
| Pamięć z padem + klawiaturą + mapperem | `heap 188692 B (min 187720 B)` |
| **Mysz AJAZZ AJ159 Pro podłączona** | `OPEN f4:ee:25:36:c8:75 'AJ159 PRO' vid=0x3151 pid=0x402c`, `appearance=0x03c2`, 11 raportów |
| **Układ raportu myszy rozstrzygnięty danymi** | `map=0 id=5 len=7`, osie int16 little-endian — patrz §4.10 |
| Kółko myszy | `[00 00 00 00 00 ff 00]` → −1, `[… 01 00]` → +1, czyli `d[5]` jako `int8` |
| **TRZY jednoczesne połączenia BLE na jednym C3** | `podlaczone …, razem 2/2 urzadzen` + `pad gotowy`; `wejscia 2 (kbd=1 mouse=1)`. To było główne ryzyko PoC — **rozstrzygnięte** |
| Pamięć z trzema połączeniami | `heap 186960 B (min 185096 B)` — zapas ~180 kB |
| Bateria obu urządzeń czytana | mysz `bateria 83%`, klawiatura `bateria 100%` |
| **Mysz → prawy analog działa end-to-end** | właściciel potwierdził w `joy.cpl`: lewo/prawo to oś Z, góra/dół to obrót Z (zgodne z deskryptorem, §4.24) |
| Kółko i przyciski myszy | `btn=0x001` / `0x002` w raporcie pada, kółko czytane jako `int8` |
| Crash w timerze GAP przy łączeniu z myszą | wystąpił **cztery razy**, `Load access fault`. **PRZYCZYNA ZNALEZIONA I NAPRAWIONA (§4.28):** NimBLE indeksuje `g_max_tx_time[]` uchwytem połączenia, a wymiaruje liczbą połączeń — zapis dla uchwytu 4 trafiał w głowę listy GAP. Limit podniesiony 3 → 9 |
| **Blokada `esp_hidh_dev_open()` na zawsze** | znaleziona i obsłużona — realny błąd, ale **nie** przyczyna objawu z rekonekcją (§4.23) |
| **Brak `ESP_HIDH_CLOSE_EVENT` na NimBLE** | to była przyczyna „mysz po zaśnięciu nie wraca": tablica trzymała odłączone urządzenie 145 s, licznik `2/2` blokował skanowanie (§4.25) |
| **Cykl uśpienie → powrót myszy przechodzi** | w logu: `wejscie odlaczone f4:ee:… reason=531` → `zasoby odlaczonego urzadzenia zwolnione` → `wejscia 1` → `skan` → `kandydat f4:ee:…` → ponowne połączenie. Wykrywanie rozłączeń z GAP działa |
| Drugi crash: skok w pulę procedur GATT | `Instruction access fault`, `MEPC=0x3fc98678` leży w `ble_gattc_proc_mem` (§4.26). `GATT_MAX_PROCS` 4 → 12 **nie pomogło** |
| **PRZYCZYNA znaleziona: `services_discovered` w IDF nigdy nie jest zerowane** | licznik rośnie przez cały czas życia firmware'u, a `svc_disced()` pisze po tablicy 10 elementów na stosie wołającego. Trzecie otwarcie urządzenia (czyli powrót po uśpieniu) niszczy ramkę stosu — §4.27 |
| **Łatka §4.27 potwierdzona na sprzęcie** | w jednym logu: otwarcie 1 (mysz), 2 (klawiatura, `razem 2/2`), rozłączenie myszy, `zasoby odlaczonego urzadzenia zwolnione`, **otwarcie 3 (mysz) bez crashu**, znowu `razem 2/2`, potem kolejny cykl rozłączenia. Wcześniej trzecie otwarcie padało niezawodnie |
| Brak wycieku między cyklami | `heap 190480 B` z dwoma urządzeniami, `191448 B` po rozłączeniu, `min 180912 B` przez 115 s i kilka cykli |
| Zabezpieczenie z §4.23 zadziałało w praktyce | gdy mysz zasnęła w trakcie odkrywania usług klawiatury, link padł (`reason=520`), `esp_hidh_dev_open()` zawisło i firmware zrobił kontrolowany restart zamiast zawisnąć na stałe |
| **Naprawa §4.28 potwierdzona na sprzęcie** | `polaczenie nawiazane, conn_handle=4` → `mtu update event; conn_handle=4 mtu=247` → **bez paniki**, 180 s pracy, `razem 2/2 urzadzen`. To była niezawodna recepta na crash |
| **Mysz milczała po ponownym połączeniu** | przyczyna: `esp_hidh` nigdy nie inicjuje szyfrowania, a HOGP tego wymaga (§4.29). Naprawione, potwierdzone logiem: `szyfrowanie: conn_handle=3 status=0 \| enc=1 bond=1`, `zapis CCCD: status=0` ×6, `MOU map=0 id=5 len=7 [00 35 00 7e 00 00 00]` |
| **Naprawa §4.29 potwierdzona end-to-end: mysz → prawy analog** | oba linki zaszyfrowane (`enc=1 bond=1` dla `conn_handle=3` i `4`), wszystkie `zapis CCCD: status=0`, raporty `MOU … dx=-18 dy=-2` → `pad: R(-15,0)`, przyciski myszy `btn=0x002` |
| Pełne mapowanie klawiatury potwierdzone ponownie | `1a`→`L(0,-127)`, `04`→`L(-127,0)`, `16`→`L(0,127)`, `07`→`L(127,0)`, skos `16`+`07`→`L(90,90)` |
| **Dwa kolejne cykle uśpienie → powrót myszy, bez crashu** | `reason=531` → `zasoby odlaczonego urzadzenia zwolnione` → skan → `kandydat` → `conn_handle=4` → `razem 2/2`; w jednym przebiegu 180 s wyszło pięć otwarć urządzeń |
| Brak wycieku pamięci przez 180 s i kilka cykli | `heap 188856 B` z dwoma urządzeniami, `189816 B` po rozłączeniu, `min 179248 B` |
| **XInput działa — Etap 4 osiągnięty** | po zmianie tożsamości na PID `0x0B13` (§4.32) `joy.cpl` pokazuje urządzenie jako **„Urządzenie wejściowe Bluetooth LE zgodne z interfejsem XINPUT"**. Właściciel potwierdził: **Rocket League, Apex Legends i Steam obsługują pada** |
| **Rozstrzygający dowód: Windows przysyła wibracje** | `raport wyjsciowy id=3 (8 B): 0f 00 00 00 00 ff 00 eb` — polecenie rumble wysyła wyłącznie sterownik pada Xbox, nie zwykła obsługa HID. Pierwszy bajt `0x0f` to „DC Enable Actuators" ze wszystkimi czterema silnikami |
| Krzyżak ze strzałek | zaimplementowany w profilu Xbox (hat switch 1–8, przeciwne kierunki znoszą się); w profilu generycznym nieaktywny, bo tamten deskryptor nie ma hat switcha |
| Czułość myszy dobrana | `CONFIG_APP_MOUSE_SCALE_DIV` 8 → **24** (3× mniej czuła) po zgłoszeniu, że gałka zbyt szybko dobija do maksimum |
| **Interwały połączeń zmierzone i wyciśnięte do maksimum** | pad → PC **7,5 ms (133 Hz)**, wejścia **15 ms (66 Hz)** wobec 45 ms na starcie. 15 ms to udowodniony sufit kontrolera C3 w roli centrala — sześć hipotez wykluczonych osobnymi pomiarami (§4.33) |
| Tożsamość odczytana z systemu | `HID\{00001812-…}_Dev_VID&02045e_PID&02fd_REV&0408` przy pierwszym podejściu — dowód, że PnP ID dociera do Windows bezbłędnie i że problemem był wyłącznie **wybór PID** (§4.32) |
| Naprawa | lokalna kopia komponentu `esp_hid` z jednolinijkową łatką + kontrole granic. Potwierdzone, że build bierze naszą kopię (`check_local_esp_hid.py`) i że łatka jest w binarce. **Weryfikacja cyklu uśpienia na sprzęcie do zrobienia** |
| **Dorobek śledztwa z §4.35 scalony do `main` i przejechany na C3** | po scaleniu: `roles: hid_host=on gamepad=on`, `TX power level: 15`, klawiatura i mysz **wracają same** z bondów (`OPEN d8:78:… 'AULA-F99Pro 5.0 '` z `enc=1 bond=1`, potem `f4:ee:…`), `inputs 2 (kbd=1 mouse=1)`, oba z maską `0x63`, w logu lecą `MOU … [00 1b 00 f5 ff 00 00]` i `KBD … [00 00 06 …]`. Heap 190 588 B |
| **Cały łańcuch na scalonym `main`: wejścia → pad → PC** | `pad ready` plus mapowanie w logu: `pad: L(0,-127)` z klawisza, `pad: R(127,83)` z myszy, skos `L(-90,-90)` ze skalowaniem. Pad łączy się z PC sam z bondu — pierwsze podejrzenie, że wymaga ponownego sparowania, było błędne; w Windows był po prostu zapamiętany inny komputer |
| **XInput na scalonym `main`** | właściciel potwierdził, że **test pada w Steam pokazuje wejścia poprawnie** — czyli po scaleniu 29 commitów ze śledztwa §4.35 profil Xbox nadal działa i nic w deskryptorze ani w tożsamości nie ucierpiało |
| **Wersja USB: pad XInput na ESP32-S3 SuperMini związany przez Windows** | `USB\VID_045E&PID_028E\08FEC93` → **`Service=xusb22`**, nazwa „Kontroler konsoli Xbox 360 dla systemu Windows”. `xusb22` to sterownik XInput, więc **Windows wiąże XUSB przy JEDNYM zadeklarowanym interfejsie** — to była jedyna otwarta niewiadoma §4.37 i jest zamknięta. Firmware w logu: `pad ready`, `heap 371 088 B` przez 6 min bez zmiany |
| Płytka użyta do tego testu | ESP32-S3 (QFN56) rev v0.2, 4 MB flash (XMC), **2 MB PSRAM quad** (AP_3v3), MAC `90:da:72:49:a3:28`. Odczytane przez `esptool chip-id`; PSRAM nie jest w tym projekcie włączane |
| **Rozstrzygający dowód dla pada USB: XInput z nami rozmawia w obie strony** | `XInputGetState` widzi go w **slocie 0** (`CONNECTED`), a `XInputSetState` dociera do urządzenia: `0xffff,0x0000` → `rumble from host: left=255 right=0`, `0x0000,0xffff` → `left=0 right=255`, `0x4000,0x4000` → `left=64 right=64`, zerowanie → `left=0 right=0`. Trzy **różne** wartości i trzy zgodne linie w tej samej kolejności, ze skalowaniem 16→8 bitów zgodnym z przewidywaniem. Dowodzi wiązania sterownika, obecności w slocie XInput **oraz** działania endpointu OUT (`scripts/xinput_rumble.py`) |

| **Wersja USB: cały łańcuch na dwóch S3 SuperMini** | mierzone z dwóch stron jednocześnie. Płytka wejść: `usb ifaces 4 (kbd=1 mouse=1)`, `KBD report len=8 [00 00 1a 00]` (0x1a = `w`), `MOU report len=7 [00 ff ff 07]`, `link: sent 19884 frames (dropped 0)`. Pad w PC przez `XInputGetState`: `L=(0,32767)` po `w`, `L=(-32767,0)` po `a`, a ruch myszą daje **977 zmian stanu w 18 s** z gładkim opadaniem `R=(516,0) → (258,0) → (0,0)`, czyli filtrem z §4.22. W systemie **jeden** kontroler XInput, slot 0, więc pomiar nie może dotyczyć innego urządzenia |
| Hub i dongle na sprzęcie | zasilany hub USB 3.0 z dwoma dongle'ami 2,4 GHz obsłużony poprawnie; heap płytki wejść `347 404 B (min 346 116 B)` stabilny przez ~24 min |
| **Wersja USB domknięta: test pada w Steam przechodzi w całości** | potwierdzenie właściciela (nie log): **test kontrolera w Steam pokazuje wszystko poprawnie** — osie, spusty, przyciski i krzyżak. To zamyka krok 5 planu z §4.37, czyli cały plan. Tym samym mostek działa w dwóch niezależnych wariantach transportu: BLE (§4.31, §4.32) i USB |
| **Panel konfiguracyjny po Wi-Fi działa w całości** | zmierzone z komputera: strona 32 462 B HTTP 200, `/api/state` bez hasła **401**, z hasłem pełny stan; 20 domyślnych przypisań zgodnych z tablicą; przycinanie `div_x` 9999→512 i `anti_dz` 200→80; sanityzacja nazwy `a"<b>c\d;e` → `a__b_c_d_e`; profile zapisane i odczytane z NVS **przez restart** (`apex`, div 96/72, tau 60); eksport 707 B. Szczegóły i pułapki w §4.41 |
| **Wi-Fi na żądanie nie kosztuje tempa** | licznik `ticks` w oknie z `uptime_ms`: **1000,0 Hz** przy podniesionym AP i odpytywanym panelu (6105 przebiegów w 6,105 s) oraz **1000 Hz** w trybie klienta (okno 8,191 s). Heap 242–248 kB wolne, czyli radio z serwerem kosztuje ~125 kB |
| **OTA i wycofanie zweryfikowane** | trzy obiegi aktualizacji, każdy `HTTP 200` i każdy wstał. Wycofanie udowodnione obrazem z `abort()`, kryptograficznie poprawnym: przyjęty, uruchomiony, panika, bootloader wrócił do poprzedniego bez niczyjego udziału — po 87 s panel odpowiada z `pad_ready=True` |
| **Dołączenie do sieci domowej** | `net=station`, `http://192.168.1.32`, `sta_ssid='PLAY_Swiatlowod_D4C0'`. Lista DHCP routera go **nie pokazywała** — znaleziony skanem LAN po MAC `90:da:72:49:a3:28` (klient używa bazowego MAC, AP inkrementowanego) |
| **Passthrough na skrót działa w obie strony** | `Ctrl+Alt+G` na klawiaturze przełącza tożsamość USB układu pada. Odczytane z drzewa urządzeń: w passthrough `USB\VID_303A&PID_4004` z `Service=HidUsb` i dwiema kolekcjami — `COL01` → `kbdhid`, `COL02` → `mouhid`, a pad `045E:028E` **nieobecny** i XInput nie widzi nic w żadnym slocie; po powtórnym skrócie wraca `USB\VID_045E&PID_028E` z `Service=xusb22` i slot 0 `CONNECTED`. Szczegóły projektowe w §4.39 |

**Zbadane, jeszcze nieskompilowane** (wyniki analizy z 2026-08-15, szczegóły w §4):

| Ustalenie | Źródło |
|---|---|
| Płytka na COM6 to ESP32-C3 z natywnym USB | `usbipd list`: `2-1 303a:1001 USB JTAG/serial debug unit (COM6)` |
| `esp_hidh` (NimBLE) działa z AULA F99 Pro | działający projekt OpenLara na ESP32-S3 |
| `CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR` wymaga IDF ≥ 5.4.3 | grep po 4 instalacjach IDF w WSL |
| `esp_hidd` (NimBLE) **nie da się** użyć razem z `esp_hidh` bez łatki | kod IDF 5.4.2 i 5.5.1 |

## 3. Środowisko

| Co | Gdzie |
|---|---|
| Build (bez WSL) | Windows, `%USERPROFILE%\esp\v5.5.1\esp-idf`, `scripts\build-native-win.bat` |
| Build (WSL) | `~/esp/v5.5.1/esp-idf`, `scripts/build.sh` przez `scripts\build-win.bat` |
| Flash + konsola | Windows, `scripts\flash-win.bat`, `scripts\monitor-win.bat` |
| Płytka | ESP32-C3 z natywnym USB, VID:PID `303a:1001` (USB JTAG/serial debug unit) |

Ścieżkę do windowsowej instalacji IDF nadpisuje się zmienną `IDF_WIN`, np.
`set IDF_WIN=D:\esp\v5.5.1\esp-idf`. Numer portu COM podaje się skryptom argumentem.

**IDF musi być ≥ 5.4.3** — patrz §4.1. Wybrane v5.5.1.

Konkretne ścieżki, numer portu i inne rzeczy zależne od maszyny **nie należą do tego pliku** —
idą do `AGENTS.local.md` (gitignorowany, wzór w `AGENTS.local.example.md`).

Dwie drogi budowania istnieją z powodu praktycznego: WSL potrafi zawiesić się tak, że samo
wywołanie `wsl` wisi bez końca, a `wsl --shutdown` nie odpowiada — odblokowanie wymaga wtedy
restartu usługi `WSLService` z uprawnieniami administratora albo restartu systemu. Skoro
ESP-IDF jest po stronie Windows i tak potrzebny do wgrywania, build nie ma powodu zależeć
od WSL. Obie drogi używają **osobnych katalogów build** (`build.esp32c3` i
`build.win.esp32c3`), bo ścieżki absolutne są w nich różne, a CMake nie zniesie obu w jednym.

Płytka ma natywne USB, więc konsola idzie przez USB Serial/JTAG:
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` + `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`.
Przy natywnym USB linie DTR/RTS sterują resetem i bootloaderem, dlatego `monitor.py`
otwiera port z wyłączonymi DTR/RTS, a `reset_monitor.py` resetuje świadomie.

## 4. Ustalenia i pułapki

### 4.1 `CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR` nie istnieje przed IDF 5.4.3

Klawiatura AULA F99 Pro nie odda raportów HID bez szyfrowanego połączenia. Przy próbie
czytania charakterystyk niezaszyfnowanym połączeniem GATT zwraca błąd ATT „insufficient
security" (`status=14`). Opcja `BT_NIMBLE_GATTC_AUTO_PAIR` sprawia, że NimBLE w takiej
sytuacji sam inicjuje parowanie i powtarza operację.

Sprawdzone w instalacjach IDF w WSL (grep po `components/bt/host/nimble/Kconfig.in`):

| Wersja | Opcja obecna |
|---|---|
| v5.3.1 | nie |
| v5.4.2 | **nie** |
| v5.4.3 | tak |
| v5.5.1 | tak |

Ważne: projekt przekaźnika buduje się na **v5.4.2**, więc nie można stąd wprost skopiować
środowiska. W OpenLarze `sdkconfig` ma `CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR=y` — czyli tamten
build używał IDF nowszego niż v5.4.2, mimo że repo przekaźnika wskazuje na v5.4.2.

### 4.2 `esp_hidd` + `esp_hidh` na NimBLE nie współistnieją (kod IDF)

Chodzi o `components/esp_hid/src/nimble_hidd.c` i `nimble_hidh.c`. Sprawdzone w 5.4.2 i 5.5.1
— identycznie w obu.

**Problem 1 — globalne `ble_hs_cfg`.** Oba pliki nadpisują te same wskaźniki:

```
nimble_hidd.c:701   ble_hs_cfg.reset_cb = nimble_host_reset;
nimble_hidd.c:702   ble_hs_cfg.sync_cb  = nimble_host_synced;
nimble_hidh.c:896   ble_hs_cfg.reset_cb = nimble_host_reset;
nimble_hidh.c:897   ble_hs_cfg.sync_cb  = nimble_host_synced;
```

Wygrywa ten zainicjalizowany później. Wersje z hosta są puste (tylko log), a `sync_cb`
device'a wysyła `ESP_HIDD_START_EVENT` — czyli przy inicjalizacji hosta na końcu pad nigdy
nie zacznie rozgłaszać.

**Problem 2 (poważniejszy) — brak sprawdzenia roli połączenia.** `nimble_hidd` rejestruje
globalny `ble_gap_event_listener`, a ten na `BLE_GAP_EVENT_CONNECT` robi bezwarunkowo:

```c
s_dev->connected = true;
s_dev->conn_id = event->connect.conn_handle;
```

Nie ma sprawdzenia `desc.role`. W naszej aplikacji central łączy się z klawiaturą i myszą, więc
`esp_hidd` złapie handle **tych** połączeń i będzie próbował wysyłać raporty pada do
klawiatury. To nie jest do obejścia z zewnątrz — `s_dev` jest `static` w `nimble_hidd.c`.

**Decyzja (skorygowana 2026-08-15, patrz §4.8): peryferial HID budujemy na `ble_svc_hid`
z NimBLE, nie na `esp_hidd` i nie od zera.** Pierwotnie planowaliśmy pisać całą usługę
GATT ręcznie; okazało się, że NimBLE ma gotową usługę HID, która nie ma żadnej z wad
`nimble_hidd.c`.

Rozważona alternatywa: skopiować `nimble_hidd.c` do projektu jako lokalny komponent i dodać
`if (desc.role != BLE_GAP_ROLE_SLAVE) return 0;`. Mniej pisania, ale utrzymujemy forka pliku
z IDF (25 kB) i nadal zostaje Problem 1. Odrzucone.

**Sprostowanie (2026-08-16):** argument „nie forkujemy komponentu" przestał obowiązywać.
Po znalezieniu §4.27 — udowodnionego, jednolinijkowego błędu w `nimble_hidh.c`, który
uniemożliwiał powrót urządzenia po uśpieniu — komponent `esp_hid` **jest** skopiowany do
`firmware/components/esp_hid/` i załatany. Decyzja o samodzielnym peryferialu na
`ble_svc_hid` zostaje mimo to słuszna: tamta strona nie sprawiła ani jednego problemu.

### 4.8 `CONFIG_BT_NIMBLE_HID_SERVICE` gatuje też **hosta**, nie tylko serwer

Nazwa opcji („Human Interface Device service") sugeruje rzecz wyłącznie serwerową. W
rzeczywistości w `components/esp_hid/src/nimble_hidh.c` **cały plik** jest w
`#if CONFIG_BT_NIMBLE_HID_SERVICE ... #endif` (linie 35 i 966 w v5.5.1). Bez tej opcji
projekt kompiluje się, ale linker wywala:

```
undefined reference to `esp_ble_hidh_init'
undefined reference to `esp_ble_hidh_dev_open'
```

Czyli opcja musi być `y` nawet dla czystego centrala. Sprawdzone na własnym błędzie budowania.

Włączenie jej **nie rejestruje** samo z siebie żadnej usługi w GATT — `ble_svc_hid_init()`
jest wołane tylko z `nimble_hidd.c:175`, a `nimble_hidd` startuje wyłącznie przez
`esp_hidd_dev_init()`, którego nie używamy. Sprawdzone grepem po `components/bt` i
`components/esp_hid`: to jedyne wywołanie w całym IDF.

### 4.9 `ble_svc_hid` z NimBLE jest gotową usługą HID i nie ma wad `esp_hidd`

`components/bt/host/nimble/nimble/nimble/host/services/hid/` — usługa HID prosto z NimBLE.
API jest małe:

```c
void ble_svc_hid_init(void);
int  ble_svc_hid_add(struct ble_svc_hid_params params);
void ble_svc_hid_reset(void);
```

`struct ble_svc_hid_params` przyjmuje Report Map, listę charakterystyk Report (z typem
i report ID), HID Information, Control Point, Protocol Mode oraz opcjonalne boot reporty.
Kluczowe: ten plik **nie rejestruje globalnego `ble_gap_event_listener`** i **nie dotyka
`ble_hs_cfg`** — oba problemy z §4.2 siedzą w warstwie `esp_hidd`, nie w samej usłudze.
Zostaje nam do napisania tylko advertising i wysyłka notyfikacji, gdzie sami trzymamy
handle połączenia i sprawdzamy `desc.role`.

Uwaga na rozmiar: `ble_svc_hid_add()` bierze strukturę **przez wartość**, a w środku jest
`report_map[512]` i `rpts[MAX_REPORTS]` po 256 B każdy. Przy domyślnych
`MAX_INSTANCES=2` / `MAX_RPTS=3` to ~1,3 kB kopiowane na stos wołającego. Dlatego
`sdkconfig.defaults` ustawia oba na 1.

### 4.10 Układ raportu myszy nie jest potwierdzony

### 4.10 Układ raportu myszy AJ159 Pro — potwierdzony danymi

Hipoteza z OpenLary (boot protocol, 8-bitowe `data[1]`/`data[2]`) jest **błędna** dla tej
myszy. Rzeczywisty raport to `map=0 id=5`, **7 bajtów**:

| Bajt | Znaczenie |
|---|---|
| `d[0]` | przyciski: bit 0 lewy, bit 1 prawy, bit 2 środkowy |
| `d[1..2]` | X, `int16` little-endian |
| `d[3..4]` | Y, `int16` little-endian |
| `d[5]` | kółko, `int8` |
| `d[6]` | zawsze `0` w naszych próbach (prawdopodobnie kółko poziome) |

Rozstrzygający był ten pakiet przy powolnym ruchu w lewo:

```
MOU map=0 id=5 len=7 [00 ff ff 01 00 00 00]
  btn=0x00 | 8-bit(  -1,  -1) 12-bit(   -1,   31) 16-bit(    -1,     1)
```

Odczyt 16-bitowy daje `(-1, +1)`. Odczyt 8-bitowy daje `(-1, -1)`, bo bierze za Y **górny
bajt X-a** — a dla ujemnego X ten bajt to `ff`, więc 8-bitowa interpretacja zawsze zgłasza
wtedy fałszywy ruch w górę. Wariant 12-bitowy dawał absurdy (`31` przy ruchu o jedno
zliczenie).

Drugie potwierdzenie: przy ruchu **wyłącznie w pionie** bajty X były zerowe
(`[00 00 00 14 00 00 00]`), a odczyt 8-bitowy pokazywał wtedy `Y=0` mimo realnego ruchu.

Kółko: `[00 00 00 00 00 ff 00]` → −1 (w dół), `[… 01 00]` → +1 (w górę).

AJ159 deklaruje też krótszy wariant `id=5 len=3`, czyli klasyczny boot protocol
z 8-bitowymi osiami — dlatego gałąź 8-bitowa w kodzie zostaje, a wybór idzie po długości
raportu (`len >= 5` → 16-bit). Raport myszy AULI to `id=5 len=6`, czyli też 16-bit, tylko
bez ostatniego bajtu.

### 4.21 Crash w `ble_gap_update_next_exp` — pula wpisów aktualizacji ma domyślnie 1 element

Przy **pierwszej** próbie połączenia z myszą (klawiatura już podłączona, PC też):

```
NimBLE: Connection established
NimBLE: mtu update event; conn_handle=4 cid=4 mtu=247
Guru Meditation Error: Core 0 panic'ed (Load access fault). Exception was unhandled.
MEPC : 0x42013eae   MCAUSE : 0x00000005   MTVAL : 0x00000858
```

`addr2line` na ELF-ie z tego builda:

```
0x42013eae  ble_gap_update_next_exp   ble_gap.c:1460   <- SLIST_FOREACH po wpisach
0x42014b54  ble_gap_update_timer      ble_gap.c:3074
0x42014922  ble_gap_master_timer      ble_gap.c:2980
0x4201507e  ble_gap_timer             ble_gap.c:3132
0x4200e698  ble_hs_timer_exp          ble_hs.c:450
```

Linia 1460 to `ticks = entry->exp_os_ticks - now;` w pętli po liście
`ble_gap_update_entries`. `MTVAL=0x858` to adres odczytu, czyli `entry` był wskaźnikiem
śmieciem — lista zawierała zwisający wpis. To jest **błąd wewnątrz NimBLE**, nie w naszym
kodzie: nigdzie nie dotykamy `ble_gap_update_entries` ani nie wołamy
`ble_gap_update_params()`.

**Pierwsza hipoteza (pula wpisów) była BŁĘDNA.** Pula ma w ESP-IDF rozmiar 1:

```
esp_nimble_cfg.h:698   #define MYNEWT_VAL_BLE_GAP_MAX_PENDING_CONN_PARAM_UPDATE (1)
```

Powiększyliśmy ją do 4 definicją globalną w `firmware/CMakeLists.txt` (opcji nie ma
w Kconfig, ale nagłówek używa `#ifndef`; sprawdzone, że definicja trafia do kompilacji
`ble_gap.c`). **Crash wrócił z identycznym backtrace'em co do linii.** Wpis pozostaje
w `CMakeLists.txt`, bo pula 1 przy trzech linkach i tak jest za mała, ale to nie była
przyczyna.

Co wiadomo po drugim wystąpieniu:

- Backtrace jest **identyczny** w obu przypadkach, do numeru linii.
- Adres feralnego wskaźnika to `entry = 0x848` (`MTVAL=0x858` to `entry->exp_os_ticks`).
  `0x848` **nie jest adresem RAM** (DRAM zaczyna się od `0x3FC80000`) — to mały int.
  Czyli ktoś **nadpisał** pamięć listy, a nie pomylił się w logice listy.
- Każde `ble_gap_update_entry_free()` w `ble_gap.c` jest poprzedzone
  `ble_gap_update_entry_remove()`, poza ścieżką błędu w `ble_gap_update_params()`, gdzie
  wpis nie był jeszcze wstawiony. Czyli nie ma oczywistego „free bez remove".
- Głowa listy (`ble_gap_update_entries` @ `0x3fc97a24`) **nie leży obok** pamięci puli
  (`ble_gap_update_entry_mem` @ `0x3fc987a4`) — 3,5 kB odstępu, więc przepełnienie puli
  nie trafiłoby wprost w głowę.

Korelacja z logów, cztery obserwacje drugiego połączenia centralnego:

| Pierwsze urządzenie | Drugie urządzenie | MTU drugiego | Wynik |
|---|---|---|---|
| mysz | klawiatura | 23 | OK |
| mysz | klawiatura | 23 | OK |
| klawiatura | **mysz** | **247** | **crash** |
| klawiatura | **mysz** | **247** | **crash** |

W obu crashach ostatnią linią przed paniką było `mtu update event; conn_handle=4 mtu=247`.
Ta korelacja okazała się kluczem: **§4.28 wyjaśnia ją do końca i jest naprawą.** Wynikające
z niej doraźne obejście („budzić mysz jako pierwszą") nie jest już potrzebne.

**Narzędzie, które przy okazji powstało.** Crash pokazuje ofiarę, nie sprawcę. Odwrócenie
tego: sprzętowy watchpoint na zapis do głowy listy — panika leci wtedy w momencie psucia
struktury, z backtrace'em winowajcy. Włącza się przez `CONFIG_APP_DEBUG_WATCH_ADDR`
(patrz `firmware/sdkconfig.local.example`). Ostatecznie przyczynę udało się ustalić bez
niego, z samej analizy adresów w `nm`, ale narzędzie zostaje — przy następnej takiej
zagadce oszczędzi wiele godzin.

Uwaga praktyczna z uruchamiania watchpointa: musi być uzbrajany **po** synchronizacji
stacku. Wcześniej łapał legalny zapis z `ble_gap_init()` (`ble_gap.c:9113`, `SLIST_INIT`),
co dawało pętlę restartów już przy starcie.

Sprawdzone, że adres nie przesuwa się po włączeniu samego watchpointa
(binarka rośnie z `0x92780` do `0x92900`, ale BSS komponentu `bt` zostaje na miejscu),
więc nie ma problemu „jajko i kura" z odczytem adresu.

Konsekwencja dla §4.18: `hci_err=0x212` na `ocf=0x0013` **nie jest** nieszkodliwe, jak tam
początkowo zapisano. Wpis poprawiony.

### 4.22 Mysz raportuje rzadziej, niż chodzi zadanie pada

Z logu: raporty AJ159 Pro przychodzą co ~40–50 ms (~20–25 Hz), a zadanie mapujące chodzi
100 Hz. Przy prostym przeliczeniu „przyrost z tiku → oś" trzy na cztery tiki widzą zero,
więc gałka skacze między wychyleniem a środkiem ~20 razy na sekundę. Ponieważ raport pada
idzie tylko na zmianie stanu, PC dostawał serię naprzemiennych `R(0,x)` i `R(0,0)`.

Dlatego `input_mapper.c` liczy **średnią kroczącą** przyrostu ze stałą czasową 8 tików
(80 ms), w arytmetyce stałoprzecinkowej ×256. Przy równym ruchu gałka trzyma stabilne
wychylenie proporcjonalne do prędkości myszy, a po zatrzymaniu wraca do środka w ~80 ms.
Dzielenie całkowitoliczbowe nie dochodzi do zera, więc przy zerowym przyroście i resztce
poniżej 1 zliczenia na tik wartość jest zerowana wprost.

Kalibracja z pomiaru: spokojny ruch dawał ~40–60 zliczeń na raport, czyli ~11 zliczeń na
tik. `CONFIG_APP_MOUSE_SCALE_DIV=8` oznacza pełne wychylenie przy 32 zliczeniach na tik,
czyli spokojny ruch to ~1/3 zakresu. **Odczucie na sprzęcie jeszcze niesprawdzone.**

### 4.11 `esp_hidh_dev_open()` potrzebuje grubego stosu w **swoim** zadaniu

Objaw na sprzęcie (2026-08-16), dokładnie w chwili połączenia z klawiaturą:

```
hid_host:  HID ed:c7:b1:bb:83:01 type=1 rssi=-32 appearance=0x03c1 'AULA-F99Pro'
NimBLE:    Connection established
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
Detected in task "hid_scan"
Stack pointer: 0x3fcace20   Stack bounds: 0x3fcaceb8 - 0x3fcadeb0
```

Wskaźnik stosu **poniżej** dolnej granicy, czyli przepełnienie. Powód: `esp_hidh_dev_open()`
jest w pełni blokujące i wykonuje `read_device_services()` **w zadaniu wołającego**, a ta
funkcja trzyma na stosie trzy tablice jednocześnie:

```
nimble_hidh.c   struct ble_gatt_svc service_result[10];
nimble_hidh.c     struct ble_gatt_chr char_result[20];
nimble_hidh.c       struct ble_gatt_dsc descr_result[20];
```

plus `esp_hid_parse_report_map()`. Przykład `esp_hid_host` z IDF daje swojemu zadaniu
`6 * 1024` — i to jest właściwa kalibracja, nie 4 kB.

Dwie poprawki, obie potrzebne:

1. `hid_scan` dostaje **8192 B** zamiast 4096 B.
2. `try_connect_candidates()` i `log_scan_results()` **nie kopiują już całej tablicy
   kandydatów na stos**. `sizeof(candidate_t) * 24` to ~1,5 kB, a pierwsza wersja trzymała
   tę kopię przez cały czas trwania blokującego `esp_hidh_dev_open()`. Teraz jest
   `candidate_get(idx, &jeden)`.

Po każdej próbie połączenia logujemy `uxTaskGetStackHighWaterMark()`, żeby margines był
widoczny w logu, a nie zgadywany.

### 4.12 Logi INFO z NimBLE zalewają konsolę przy aktywnym padzie

Na poziomie INFO NimBLE drukuje dwie linie plus pustą przy **każdej** notyfikacji:

```
I (44106) NimBLE: GATT procedure initiated: notify;
I (44106) NimBLE: att_handle=21
```

Przy padzie wysyłającym ~16 raportów/s daje to ~3000 linii w 60-sekundowym logu i własne
komunikaty stają się niewidoczne. `CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y` to usuwa,
zachowując błędy.

### 4.13 `hci_err=0x21A` przy łączeniu z klawiaturą jest nieszkodliwe

```
NimBLE: ogf=0x08, ocf=0x0022, hci_err=0x21A : BLE_ERR_UNSUPP_REM_FEATURE
```

`OGF 0x08 / OCF 0x0022` to `HCI_LE_Set_Data_Length`. AULA F99 Pro nie wspiera LE Data Packet
Length Extension, więc kontroler odpowiada „unsupported remote feature". NimBLE to obsługuje
i jedzie dalej z domyślną długością pakietu. Nie ma związku z crashem z §4.11 — ta linia
pojawia się po prostu w tym samym momencie.

### 4.14 Windows widzi klawiaturę w trybie parowania i proponuje ją sparować

Gdy AULA F99 Pro rozgłasza się w trybie parowania, Windows też ją widzi i wyskakuje z
propozycją sparowania. **Nie klikać tego** — jeśli klawiatura sparuje się z Windows, połączy
się tam, a nie z mostkiem, i C3 przestanie ją widzieć w skanie. Propozycję trzeba odrzucić
i zostawić klawiaturę w trybie parowania dla C3.

### 4.15 `esp_hidh_dev_usage_get()` zwraca zawsze `GENERIC` na ścieżce NimBLE

Objaw: klawiatura podłączona i wysyłająca raporty, ale w logu `wejscia 1 (kbd=0 mouse=0)`
i `usage=GENERIC`. Powód siedzi w `nimble_hidh.c`:

- `esp_ble_hidh_dev_open()` (linia 929 w v5.5.1) ustawia na sztywno
  `dev->ble.appearance = ESP_HID_APPEARANCE_GENERIC`, a późniejsze
  `if (dev->ble.appearance == 0) dev->ble.appearance = map->appearance;` już nie zadziała,
  bo pole nie jest zerem,
- **`dev->usage` nie jest ustawiane nigdzie w całym pliku** (grep po `dev->usage`: zero
  trafień w `nimble_hidh.c`), a `esp_hidh_dev_usage_get()` zwraca właśnie to pole.

Poprawnie ustawiane jest `report->usage` — per raport, na podstawie Report Map. Dlatego:

- klasyfikację urządzenia bierzemy z `esp_hidh_dev_reports_get()`, sumując bitowo `usage`
  wszystkich raportów typu INPUT (wartości `ESP_HID_USAGE_*` są flagami bitowymi, więc OR
  daje gotową maskę),
- `usage` w `ESP_HIDH_INPUT_EVENT` jest wiarygodne i po nim rozdzielamy raporty.

### 4.16 AULA F99 Pro: dziesięć raportów, w tym własny raport myszy

Tabela raportów odczytana z urządzenia (log z płytki, 2026-08-16):

```
map=0 id=1  typ=OUTPUT usage=KEYBOARD len=1
map=0 id=1  typ=INPUT  usage=KEYBOARD len=8      <- ten dostajemy w praktyce
map=0 id=2  typ=INPUT  usage=KEYBOARD len=8
map=0 id=2  typ=INPUT  usage=KEYBOARD len=20     <- prawdopodobnie NKRO (bitmapa)
map=0 id=3  typ=INPUT  usage=CCONTROL len=2      <- klawisze multimedialne
map=0 id=4  typ=INPUT  usage=GENERIC  len=1      <- system control
map=0 id=5  typ=INPUT  usage=MOUSE    len=6      <- warstwa myszy w klawiaturze
map=0 id=19 typ=INPUT  usage=VENDOR   len=19
map=0 id=19 typ=OUTPUT usage=VENDOR   len=19
```

Maska usage z raportów INPUT wychodzi `0x63` = KEYBOARD | MOUSE | CCONTROL | VENDOR.

Dwa wnioski:

1. **`mouse=1` w logu nie znaczy „mysz podłączona"** — sama klawiatura wystawia raport myszy
   (`id=5`, 6 B), bo ma warstwę Fn z emulacją myszy. Flaga mówi „są dostępne raporty myszy",
   nie „jest fizyczna mysz". Przy testach myszy AJ159 Pro trzeba patrzeć na liczbę urządzeń,
   nie na tę flagę.
2. Raport `id=5 usage=MOUSE len=6` jest zgodny z hipotezą 16-bitowych osi
   (1 B przyciski + 2 B X + 2 B Y + 1 B kółko = 6 B), ale nadal **nie jest to potwierdzone
   danymi** — trzeba zobaczyć surowe bajty przy realnym ruchu.

Przy okazji: raport `id=19 usage=VENDOR` przychodzi zaraz po połączeniu jako
`RAW len=19 [0a 01 00 04 05 64 01 …]`. `64` = 100 dziesiętnie, a chwilę wcześniej jest
`bateria 100%`, więc to najprawdopodobniej ramka statusu urządzenia. Ignorujemy ją.

### 4.17 AULA F99 Pro wysyła serie `ErrorRollOver` po każdym naciśnięciu

Log przy pisaniu (`14` = `q`, `1a` = `w`, `08` = `e`):

```
KBD map=0 id=1 len=8 [00 00 14 00 00 00 00 00]
KBD map=0 id=1 len=8 [00 00 00 00 00 00 00 00]
KBD map=0 id=1 (rollover) len=8 [00 00 01 00 00 00 00 00]   <- trzy razy, ~130 ms
KBD map=0 id=1 len=8 [00 00 00 00 00 00 00 00]
```

`0x01` w pozycji keycodu to **`ErrorRollOver`** z tabeli USB HID Keyboard/Keypad
(0x01 ErrorRollOver, 0x02 POSTFail, 0x03 ErrorUndefined) — nie klawisz. Potwierdzone, że
przychodzi na **tym samym** `map=0 id=1` co prawdziwe klawisze, czyli to nie jest inny raport
źle zinterpretowany. Bez filtra pad dostawałby fantomowy przycisk po każdym naciśnięciu.

`handle_keyboard_report()` zeruje keycody 0x01–0x03, a raport złożony wyłącznie z nich
(bez modyfikatorów) ignoruje w całości — nadpisanie stanu zerami zgubiłoby klawisz
naprawdę trzymany.

### 4.18 Nieszkodliwe błędy w logu przy połączeniu z klawiaturą

Trzy rzeczy, które wyglądają na awarię, a nią nie są:

| Linia | Co to |
|---|---|
| `Read complete; status=14` | `BLE_HS_EDONE` — koniec odczytu, nie błąd |
| `Subscribe complete; status=259` / `269` | **Nie dotyczy subskrypcji.** To wynik `register_for_notify()`, które pisze `{1,0}` do uchwytu **wartości** charakterystyki, a nie do CCCD — ATT 0x03 (Write Not Permitted) jest tam normalny. Prawdziwa subskrypcja to `zapis CCCD` (§4.29) |
| `ogf=0x08, ocf=0x0013, hci_err=0x212` | `HCI_LE_Connection_Update` odrzucony przez kontroler. Podejrzewany o związek z crashem z §4.21 — **niesłusznie**, przyczyną było §4.28. Połączenie działa dalej z dotychczasowymi parametrami |
| `raport nieobslugiwany: usage=GENERIC map=0 id=5` z `RAW len=7` | raport myszy, który przyszedł **w trakcie** odkrywania usług, przed sparsowaniem Report Map — `usage` jest wtedy jeszcze nieznane. Trafia w to tylko okno otwierania urządzenia, w którym mapper i tak wstrzymuje raporty pada |
| **konsola milczy, choć firmware działa** | zaobserwowane raz na C6 po ~2 h bezczynności: `monitor-win.bat` nie pokazywał **nic**, a pad pozostawał sparowany z Windows. Po świadomym resecie log wrócił i heartbeat szedł równo przez 70 s ze stabilnym heapem, więc firmware żył cały czas — zamilkła sama konsola po natywnym USB. **Przyczyna nieudowodniona** (podejrzenie: host przestał odbierać z endpointu CDC i bufor się zatkał). Praktyczny wniosek: przy dłuższych przebiegach bez nadzoru zaczynać sesję od `monitor-win.bat <port> <s> reset`, a ciszy nie brać za awarię firmware'u |

### 4.19 Beacon Swift Pair z Windows w wynikach skanu

W każdym skanie widać urządzenie z rotującym adresem Random, silnym sygnałem i bez nazwy:

```
32:29:75:e0:50:b0 type=1 rssi=-39 NONCONN appearance=0x0000 '?'
   adv len=16 [1e ff 06 00 01 09 20 22 65 78 c0 16 c1 11 bd 14]
```

`ff` to Manufacturer Specific Data, `06 00` to company ID **0x0006 = Microsoft** — to beacon
Swift Pair / CDP samego PC. Typ `NONCONN_IND` oznacza rozgłoszenie **nierozłączalne**, więc
próba połączenia skończyłaby się 30-sekundowym timeoutem w blokującym `esp_hidh_dev_open()`.
`try_connect_candidates()` przepuszcza teraz tylko `ADV_IND` i `DIR_IND`.

To był początkowo podejrzany o bycie klawiaturą (rotujący adres, mocny sygnał). Nie jest —
klawiatura po prostu zasypia i przestaje rozgłaszać.

### 4.20 Klawiatura wracająca ze snu nie rozgłasza UUID 0x1812

Potwierdzone na sprzęcie: po wybudzeniu AULA F99 Pro rozgłasza się tak:

```
HID c8:d7:b2:79:50:5f type=1 rssi=-36 DIR_IND BOND appearance=0x0000 '?'
```

Ani nazwy, ani appearance, ani UUID usługi HID. Kwalifikacja oparta wyłącznie na treści
pakietu ADV (§4.4) **przepuściłaby ten przypadek**. Dlatego kandydatem jest też:

- adres obecny na liście bondów w NVS (`ble_store_util_bonded_peers()`),
- rozgłoszenie kierunkowe `BLE_HCI_ADV_RPT_EVTYPE_DIR_IND` (urządzenie celuje w konkretny host).

Adres w bondzie to adres tożsamości, a w skanie może przyjść z innym oznaczeniem typu,
dlatego porównujemy same bajty adresu, bez `type`.

### 4.23 `esp_hidh_dev_open()` może zablokować się na zawsze (błąd w IDF)

**To był powód, dla którego mysz po zaśnięciu nie wracała bez restartu C3.** Objaw w logu
nie jest oczywisty: po nieudanej próbie połączenia **przestają pojawiać się linie `skan:`**,
a firmware dalej działa i obsługuje już podłączone urządzenie.

Przebieg z płytki:

```
Connection established                      <- klawiatura, conn_handle=4
Read complete; status=7 conn_handle=4       <- 7 = BLE_HS_ENOTCONN
disconnect; reason=520                      <- 0x208 = HCI 0x08, Connection Timeout
...i nic wiecej: zero linii "skan:" do konca logu...
```

Rozstrzygające jest to, czego **nie ma**: linia `po esp_hidh_dev_open zostalo N B stosu`
leci bezwarunkowo po wywołaniu, a dla tej próby jej nie było. Czyli wywołanie nie wróciło.

Przyczyna w `components/esp_hid/src/nimble_hidh.c`:

```c
nimble_hidh.c:49    static inline void WAIT_CB(void)
                    { xSemaphoreTake(s_ble_hidh_cb_semaphore, portMAX_DELAY); }

nimble_hidh.c:353   rc = ble_gattc_disc_all_chrs(...);
                    WAIT_CB();          /* rc NIE jest sprawdzane */
```

Czekanie jest bez timeoutu, a kod nie sprawdza, czy operacja GATT w ogóle wystartowała.
Gdy link padnie w trakcie odkrywania usług, kolejne wywołania `ble_gattc_*` zwracają
`BLE_HS_ENOTCONN` **synchronicznie**, żaden callback nie przyjdzie i nie ma kto oddać
semafora. To samo dotyczy `WAIT_CB()` w liniach 314 i 467.

Skutek: zadanie wołające umiera na stałe. Ponieważ to ono skanuje, mostek przestaje szukać
urządzeń — więc nic już się nie podłączy, mimo że reszta firmware'u działa.

**Rozwiązanie:** `esp_hidh_dev_open()` jest teraz wołane w osobnym, jednorazowym zadaniu
`hid_open` (8 kB), a `hid_scan` czeka na wynik z limitem 45 s (samo `ble_gap_connect`
w środku ma 30 s timeoutu, więc limit nie łapie zwykłych niepowodzeń).

Dodatkowo **wykrywamy zawieszenie wcześniej**: listener rozłączeń sprawdza, czy padł link
do urządzenia, które właśnie otwieramy. Jeśli tak, otwarcie nie ma jak się udać — kolejne
operacje GATT zwrócą `ENOTCONN` synchronicznie. Dajemy wtedy jeszcze 2 s (otwarcie mogło być
na ostatniej prostej) i restartujemy. Na sprzęcie widzieliśmy przypadek, w którym mysz zasnęła
w trakcie odkrywania usług klawiatury: link padł z `reason=520`, a bez tej ścieżki mostek
czekał **36 s** bezczynnie, zanim zadziałał ogólny limit.

Po przekroczeniu limitu robimy **kontrolowany restart**. Dlaczego nie coś delikatniejszego:

- zawieszonego zadania nie da się bezpiecznie usunąć — siedzi na prywatnym semaforze
  `esp_hidh`, którego z zewnątrz nie widać,
- kolejna próba otwarcia konkurowałaby z nim o **ten sam** semafor, więc callback obudziłby
  zawieszone zadanie, a nie nowe — i zawiesiłoby się też,
- bondy są w NVS, więc restart jest tani: PC wraca po ~0,5–2 s, klawiatura i mysz same się
  podłączają.

Dodatkowo zmniejszamy szansę na samo zerwanie linku: po udanym otwarciu urządzenia jest
**3 s przerwy** przed próbą podłączenia następnego. Zerwanie z logu nastąpiło 1,5 s po
podłączeniu myszy, gdy odkrywanie usług klawiatury konkurowało o antenę ze świeżo
zasubskrybowanymi notyfikacjami myszy i raportami pada.

Docelowo (poza zakresem PoC): zrezygnować z `esp_hidh` i napisać klienta HOGP wprost na
NimBLE GATT, tak jak stronę peryferialną zrobiliśmy na `ble_svc_hid`. Ten komponent ma już
w naszych notatkach cztery osobne wady: §4.2, §4.11, §4.15 i tę.

### 4.24 Dlaczego prawy analog widać w `joy.cpl` jako „oś Z" i „obrót Z"

To nie jest błąd, tylko konsekwencja deskryptora. `s_report_map` deklaruje cztery osie:
`X` (0x30), `Y` (0x31), `Z` (0x32) i `Rz` (0x35). X/Y to lewy analog, Z/Rz prawy — taki
układ mają typowe pady DirectInput, więc gry go rozpoznają. `joy.cpl` rysuje krzyżykiem
tylko pierwszą parę (X/Y), a Z i Rz pokazuje jako dwa osobne suwaki. Dlatego ruch myszy
w poziomie widać na suwaku „oś Z", a w pionie na „obrót Z".

Gdyby to kiedyś zmieniać (np. na `Rx`/`Ry`), trzeba pamiętać, że **każda zmiana
`s_report_map` wymaga usunięcia pada z listy urządzeń Bluetooth w Windows i sparowania
od nowa** (§4.7).

### 4.25 `ESP_HIDH_CLOSE_EVENT` nigdy nie przychodzi na ścieżce NimBLE

**To była prawdziwa przyczyna „mysz po zaśnięciu nie wraca bez restartu C3."** §4.23 opisuje
inny, realny błąd, który też występował — ale po jego naprawie objaw został.

Log z płytki (180 s, oba urządzenia podłączone poprawnie):

```
I (33680) podlaczone (maska usage 0x63), razem 2/2 urzadzen
I (35520) NimBLE: disconnect; reason=531        <- 0x213, mysz sama zamknela link
I (74410) KBD map=0 id=1 len=8 [00 00 08 …]    <- klawiatura dalej dziala
I (180240) wejscia 2 | [0] f4:ee:…c8:75 | [1] c8:d7:…50:5f
```

Mysz odpadła w 35 s, a tablica urządzeń trzymała ją jeszcze 145 s później. Nie ma linii
`CLOSE`. Skutek: licznik został na `2/2`, a `scan_task` skanuje tylko wtedy, gdy urządzeń
jest mniej niż limit — więc przestał szukać i mysz nie miała jak wrócić.

Przyczyna w `nimble_hidh.c`:

```c
nimble_hidh.c:686   dev = esp_hidh_dev_get_by_conn_id(event->disconnect.conn.conn_handle);
nimble_hidh.c:687   if (!dev->connected) {
                        dev->status = event->disconnect.reason;
                        dev->ble.conn_id = -1;      /* cicho, bez zdarzenia */
                    } else {
                        dev->connected = false;
                        ...
                        esp_event_post_to(... ESP_HIDH_CLOSE_EVENT ...);
                    }
```

Zdarzenie leci tylko w gałęzi `else`. A `connected` w całym `nimble_hidh.c` występuje
**dokładnie dwa razy**: w tym warunku i przy ustawianiu na `false`. **Nigdzie nie jest
ustawiane na `true`** (sprawdzone grepem `connected = true`: zero trafień). Struktura jest
zerowana przy alokacji, więc warunek zawsze wybiera gałąź cichą i `CLOSE_EVENT` nie
przychodzi nigdy.

Druga pułapka w tym samym miejscu: **`esp_hidh_dev_free()` z publicznego API jest puste**:

```c
esp_hidh.c   esp_err_t esp_hidh_dev_free(esp_hidh_dev_t *dev) { return ESP_OK; }
```

Zasoby zwalnia `esp_hidh_dev_free_inner()` (zadeklarowane w publicznym
`include/esp_private/esp_hidh_private.h`), wołane z wewnętrznego handlera `CLOSE_EVENT`.
Skoro to zdarzenie nie przychodzi, każdy cykl uśpienia zostawiałby wpis w liście
`s_esp_hidh_devices` i jego bufory na stercie.

**Rozwiązanie:** rozłączenie wykrywamy sami, z globalnego zdarzenia GAP
(`ble_gap_event_listener_register`). Na `BLE_GAP_EVENT_DISCONNECT` szukamy adresu peera
(`peer_id_addr`, a jeśli nie pasuje, to `peer_ota_addr`) w **naszej** tablicy wejść; jeśli
jest, zdejmujemy go i oddajemy `esp_hidh_dev_free_inner()` w zadaniu skanującym — nie
w wątku hosta NimBLE, bo `free_inner` bierze muteks listy urządzeń.

Listener jest globalny, więc widzi też rozłączenie PC. To nieszkodliwe, bo dopasowujemy
adres do własnej tablicy wejść, w której PC nie występuje. Tym samym z założenia unikamy
błędu, na którym wykłada się `nimble_hidd` (§4.2, brak sprawdzenia, czyje to połączenie).

Obsługa `ESP_HIDH_CLOSE_EVENT` zostaje w kodzie — jest poprawna i zadziała, gdyby IDF
kiedyś to naprawiło. Podwójne zwolnienie nie grozi, bo `device_take_by_addr()` zdejmuje
wpis tylko raz.

To **piąta** udokumentowana wada `esp_hidh` na ścieżce NimBLE (po §4.2, §4.11, §4.15,
§4.23) i najsilniejszy argument, żeby poza PoC napisać klienta HOGP wprost na NimBLE GATT.

### 4.26 Drugi crash: skok w `ble_gattc_proc_mem` przy ponownym otwieraniu myszy

Inna sygnatura niż §4.21, ale ten sam moment — otwieranie urządzenia HID. Wystąpił po
poprawnym cyklu: mysz odpadła (`wejscie odlaczone … reason=531`), zasoby zwolnione, skaner
znalazł ją ponownie i w trakcie odkrywania usług:

```
Guru Meditation Error: Core 0 panic'ed (Instruction access fault).
MEPC : 0x3fc98678   RA : 0x3fc98678   MCAUSE : 0x00000001   MTVAL : 0x3fc98678
S0/FP: 0x3fc98678   S5 : 0x3fc98678   S6 : 0x3fc98678   S11 : 0x3fc98678
```

`Instruction access fault` z `MEPC` w obszarze DRAM to próba **wykonania danych**, czyli
skok przez zepsuty wskaźnik. To, że `RA` i cztery rejestry zachowywane przez wywoływanego
mają **tę samą** wartość, wskazuje na ramkę stosu odtworzoną z nadpisanej pamięci.

`addr2line` na ELF-ie z tego builda (`37d0aee95`, zgodny z logiem):

```
0x42008926  open_task            ble_hid_host.c:637   <- nasze zadanie
0x4200a706  esp_hidh_dev_open    esp_hidh.c:204
0x42009ff8  unlock_devices       esp_hidh.c:41
```

Rozstrzygające jest to, **czym jest sam adres** `0x3fc98678`. Z tablicy symboli
(`riscv32-esp-elf-nm -n`):

```
3fc9861c b ble_gattc_proc_pool
3fc98638 b ble_gattc_proc_mem      <- 0x3fc98678 lezy tutaj (+0x40)
3fc98738 b ble_l2cap_sig_proc_pool
3fc98754 b ble_l2cap_sig_proc_mem
3fc98788 b ble_gap_update_entry_pool
3fc987a4 b ble_gap_update_entry_mem
```

Czyli w `RA` wylądował wskaźnik **wewnątrz puli procedur klienta GATT**. Dodatkowo widać, że
obszar zepsuty w §4.21 (`ble_gap_update_entry_*`) leży w tej samej okolicy BSS, kilkadziesiąt
bajtów dalej. Oba crashe dotyczą więc wewnętrznych pul NimBLE alokowanych obok siebie.

**Zmiana:** `CONFIG_BT_NIMBLE_GATT_MAX_PROCS` z 4 na 12. Domyślne 4 jest wymiarowane pod
jeden link, a u nas `esp_hidh` wykonuje pełne odkrywanie usług (dziesiątki procedur GATT:
discover services, discover chars, discover descriptors, odczyt Report Map, siedem
subskrypcji CCCD) na jednym urządzeniu, podczas gdy drugie strumieniuje raporty, a pad
notyfikuje PC.

**Druga poprawka, w naszym kodzie:** zadanie mapujące **wstrzymuje notyfikacje pada** na
czas otwierania urządzenia (`ble_hid_host_is_opening()`). To najcięższy moment dla stacku
i oba crashe wypadły dokładnie wtedy — nie ma powodu dokładać do tego ~16 raportów pada na
sekundę. Na wejściu w przerwę idzie jeden raport zerowy, żeby PC nie został z wychyloną
gałką.

**To są hipotezy, nie dowód.** Za pierwszą stoi mocna przesłanka (feralny adres leży
dokładnie w tej puli), za drugą tylko korelacja czasowa. Trop, którego jeszcze nie
wykorzystaliśmy: `MYNEWT_VAL_BLE_L2CAP_SIG_MAX_PROCS` wychodzi w naszej konfiguracji na
**1** (bo `EATT_CHAN_NUM` i `L2CAP_COC_MAX_NUM` są zerowe), a to pula procedur, którymi peery
proszą o zmianę parametrów połączenia — czyli mechanizmu, który w logu bez przerwy zgłasza
`hci_err=0x212`. Tej wartości **nie da się** nadpisać przez `-D`, bo definicja w
`esp_nimble_cfg.h` nie ma osłony `#ifndef`; trzeba by podnieść
`CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM`, co przy okazji włącza nieużywane kanały L2CAP CoC.

### 4.27 PRZYCZYNA crashy przy powrocie urządzenia: `services_discovered` nigdy nie jest zerowane

To jest **udowodniona przyczyna**, nie hipoteza. Wszystkie poprzednie próby (§4.21, §4.26)
celowały w objawy.

W `components/esp_hid/src/nimble_hidh.c`:

```c
44:  static int services_discovered;                      /* globalne */
176: memcpy(service_result + services_discovered, service, sizeof(struct ble_gatt_svc));
177: services_discovered++;                               /* bez kontroli granicy */
311: struct ble_gatt_svc service_result[10];               /* NA STOSIE WOLAJACEGO */
319: dcount = services_discovered;  /* fatal if services are more than 10 */
502: dscs_discovered = 0;                                 /* resetowane */
508: chrs_discovered = 0;                                 /* resetowane */
```

`chrs_discovered` i `dscs_discovered` są zerowane. **`services_discovered` nie jest zerowane
nigdzie w całym pliku** (grep: cztery trafienia, żadne nie jest przypisaniem zera). Licznik
rośnie więc monotonicznie przez cały czas życia firmware'u, a callback `svc_disced()` pisze
pod `service_result + services_discovered`, gdzie `service_result` to tablica **10 elementów
na stosie zadania wołającego** `esp_hidh_dev_open()`.

Arytmetyka zgadza się z obserwacjami. Każde z naszych urządzeń wystawia 5–6 usług
(GAP 0x1800, GATT 0x1801, DIS 0x180A, BAS 0x180F, HID 0x1812, czasem vendor):

| Otwarcie | Zapisywane indeksy | Wynik |
|---|---|---|
| 1. (mysz) | 0–5 | OK |
| 2. (klawiatura) | 6–11 | częściowo za tablicą, ale trafia w nieużywane wyrównanie |
| 3. (powrót myszy po uśpieniu) | 12–17 | **niszczy ramkę stosu** |

Dlatego dwa pierwsze urządzenia łączyły się **zawsze**, a padało dopiero przy trzecim
otwarciu — czyli dokładnie w momencie, w którym urządzenie wraca po uśpieniu. I dlatego
żadna zmiana konfiguracji nie mogła pomóc.

Jak to zostało ustalone: crash wystąpił trzy razy z **prawie identycznym zestawem
rejestrów** (`T1=0x01020600`, `S3=0x180a0610`, `S9=0x180f0610`, `T5=0x01010600`), co wyklucza
przypadkową korupcję. Rozczytanie tych wartości jako par 16-bitowych daje
`0x180A`/`0x180F`/`0x1812` oraz zakresy uchwytów GATT — czyli zawartość `struct ble_gatt_svc`.
To wskazało wprost na tablicę `service_result[]`.

**Naprawa: lokalna kopia komponentu w `firmware/components/esp_hid/`.** ESP-IDF pozwala
nadpisać własny komponent, wstawiając komponent o tej samej nazwie do projektu. Łatka to
w istocie jedna linia:

```c
services_discovered = 0;                 /* przed ble_gattc_disc_all_svcs() */
```

Dodatkowo dołożone są kontrole granic we wszystkich trzech callbackach odkrywania
(`BLE_HIDH_MAX_SERVICES/CHRS/DSCS`), bo samo zerowanie nie chroni przed urządzeniem, które
wystawia więcej usług niż rozmiar tablicy — oryginał ma tam tylko komentarz „fatal if
services are more than 10" i żadnego testu.

Wszystkie zmiany są opatrzone komentarzem `LOKALNA LATKA`, więc `diff` względem IDF jest
czytelny. Kopia jest przypięta do **IDF 5.5.1** — po zmianie wersji IDF trzeba ją odtworzyć.

Weryfikacja, że build bierze naszą kopię, a nie wersję z IDF (bez tego objaw wróciłby cicho):

```
wsl python3 scripts/check_local_esp_hid.py
strings -a firmware/build.esp32c3/hid_gamepad_bridge.bin | grep 'za duzo uslug'
```

Czego to **nie** wyjaśnia: crash z §4.21 miał inną sygnaturę i wypadł w wątku hosta NimBLE,
przy czytaniu listy w BSS komponentu `bt`. Zapis poza `service_result` idzie w górę stosu
zadania otwierającego i teoretycznie może zajść dalej, ale nie ma na to dowodu. Jeśli §4.21
wróci po tej naprawie, będzie to osobny problem.

Zmiany wprowadzone wcześniej na podstawie **błędnych** hipotez zostają, bo są nieszkodliwe
i dają zapas przy trzech linkach, ale trzeba wiedzieć, że nie one naprawiły objaw:
`MYNEWT_VAL_BLE_GAP_MAX_PENDING_CONN_PARAM_UPDATE=4` (§4.21) oraz
`CONFIG_BT_NIMBLE_GATT_MAX_PROCS=12` (§4.26).

### 4.28 PRZYCZYNA crashu z §4.21: NimBLE indeksuje tablice `conn_handle`, a wymiaruje je liczbą połączeń

Druga **udowodniona** przyczyna, tym razem arytmetycznie co do bajtu. Rozstrzyga zagadkę
z §4.21, nad którą trzy próby konfiguracyjne przeszły bez efektu.

W `components/bt/host/nimble/nimble/nimble/host/src/ble_gap.c`:

```c
323: uint16_t g_max_tx_time[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];
324: uint16_t g_max_rx_time[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];
325: uint16_t g_max_tx_octets[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];
326: uint16_t g_max_rx_octets[MYNEWT_VAL(BLE_MAX_CONNECTIONS) + 1];
...
2963: g_max_tx_time[conn_handle] = event.data_len_chg.max_tx_time;
1619: g_max_tx_time[conn_handle] = 0;                 /* to samo przy rozlaczeniu */
```

Tablice są **indeksowane uchwytem połączenia**, a wymiarowane **liczbą** połączeń. To dwie
różne rzeczy: `conn_handle` nadaje kontroler i nie jest to mały indeks od zera. W naszym
systemie uchwyty to `1` (PC), `3` (pierwsze wejście) i `4` (drugie wejście).

Przy `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3` tablica miała 4 elementy, czyli poprawne indeksy
0–3. Zapis dla uchwytu `4` wychodził **dokładnie jeden element za koniec**:

```
g_max_tx_time         @ 0x3fc97a24,  4 * 2 B = 8 B  ->  0x3fc97a24..0x3fc97a2b
g_max_tx_time[4]        0x3fc97a24 + 8 = 0x3fc97a2c
ble_gap_update_entries@ 0x3fc97a2c                  <- glowa listy GAP
```

Adresy odczytane z `nm` na ELF-ie z feralnego builda. Wpisywana wartość to wynegocjowany
maksymalny czas transmisji: `0x848` = 2120 µs, czyli **maksimum ze specyfikacji Bluetooth**.
I dokładnie taką wartość widzieliśmy jako wskaźnik-śmieć: w crashu `A4 = 0x00000848`,
a `MTVAL = 0x858` = `0x848 + 0x10`, gdzie `0x10` to offset pola `exp_os_ticks`
w `struct ble_gap_update_entry`.

To wyjaśnia **wszystkie** wcześniejsze obserwacje, w szczególności tę, której nie umiałem
wytłumaczyć — dlaczego kolejność podłączania miała znaczenie:

| Drugie urządzenie (uchwyt 4) | LE Data Length Extension | `data_len_chg` | Wynik |
|---|---|---|---|
| klawiatura AULA F99 Pro | **nie wspiera** (`hci_err=0x21A`) | nie przychodzi | brak zapisu, OK |
| mysz AJAZZ AJ159 Pro | wspiera (MTU 247) | przychodzi | zapis poza tablicę, **crash** |

Klawiatura nie wspiera rozszerzenia długości pakietu, więc kontroler nigdy nie zgłasza dla
niej zmiany długości danych i felerny zapis się nie wykonuje. Mysz wspiera — i wtedy padało.

**Naprawa: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` z 3 na 9** (9 to maksimum dla C3). Tablice mają
teraz po 10 elementów, więc pokrywają każdy uchwyt, jaki kontroler z `BT_CTRL_BLE_MAX_ACT=6`
może wydać. Potwierdzone w `nm`: odstęp między tablicami wzrósł z 8 B do 20 B, a
`ble_gap_update_entries` przeniosło się o ponad 4 kB dalej, więc nie jest już sąsiadem.

Koszt: `heap przed BLE` spadł z 272 kB do 269,5 kB, a po starcie stacku heap wynosi
190824 B wobec 192440 B — czyli ~1,6 kB za sześć dodatkowych slotów. Tanio.

Dodatkowo logujemy uchwyt każdego nawiązanego połączenia
(`polaczenie nawiazane, conn_handle=1 (limit tablic w NimBLE: 9)`), żeby było widać, w jakim
zakresie kontroler je wydaje. Gdyby kiedyś przekroczył limit, objaw z §4.21 wróciłby.

Uwaga metodologiczna: to jest **złagodzenie przez wymiarowanie**, nie usunięcie błędu. Zapis
poza zakres nadal by nastąpił, gdyby uchwyt przekroczył 9. Prawidłowa poprawka to kontrola
granicy w `ble_gap.c`, ale komponentu `bt` nie da się rozsądnie sforkować (zawiera też
prekompilowane biblioteki kontrolera), inaczej niż `esp_hid` z §4.27.

### 4.29 `esp_hidh` nigdy nie inicjuje szyfrowania — mysz podłączona, ale milczy

Objaw: mysz łączy się, odkrywanie usług przechodzi w całości, przychodzą notyfikacje
baterii — i **ani jednego raportu HID** przez 145 s. Na myszy mruga dioda parowania.
Klawiatura na tym samym mostku działa bez zarzutu.

Profil HOGP wymaga, żeby central **zaszyfrował link** przed korzystaniem z usługi HID.
W `components/esp_hid` nie ma ani jednego wywołania `ble_gap_security_initiate()`
(grep po `security_initiate`: zero trafień). Komponent liczy wyłącznie na to, że
`CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR` zareaguje na odmowę odczytu charakterystyki.

Dla klawiatury to działa **przez przypadek**: AULA F99 Pro odmawia odczytu bez
uwierzytelnienia, więc AUTO_PAIR wchodzi i link zostaje zaszyfrowany (to jest §4.1).
AJ159 Pro pozwala czytać bez szyfrowania, więc nic tego nie wywołuje — link zostaje jawny.
Mysz wtedy nie wysyła raportów HID, bo przez jawny link nie wolno, ale usługa baterii
szyfrowania nie wymaga i notyfikuje normalnie. Stąd mylący obraz: „podłączona, dane
płyną, a nie reaguje".

Dlaczego wcześniej działało: przy pierwszym parowaniu użytkownik wciska przycisk na myszy,
co wymusza SMP i link jest szyfrowany. Objaw wychodzi dopiero przy **ponownym** połączeniu
z gotowym bondem, gdzie już nic szyfrowania nie inicjuje.

**Naprawa:** w naszym listenerze GAP, na `BLE_GAP_EVENT_CONNECT` dla roli MASTER,
wołamy `ble_gap_security_initiate()`. Przy istniejącym bondzie to samo szyfrowanie
kluczem z NVS, bez bondu — parowanie. Rola jest sprawdzana, więc połączenia pada z PC
to nie dotyczy (tam szyfrowanie inicjuje Windows).

Potwierdzenie na sprzęcie, w jednym logu:

```
szyfrowanie: conn_handle=3 status=0 | enc=1 auth=0 bond=1     <- tej linii nie bylo NIGDY wczesniej
zapis CCCD: status=0 conn_handle=3 attr_handle=52 (i 48, 44, 40, 36, 32)
MOU map=0 id=5 len=7 [00 35 00 7e 00 00 00]                  <- raport ruchu
```

**Druga zmiana, na wypadek nieaktualnego bondu:**
`CONFIG_BT_NIMBLE_HANDLE_REPEAT_PAIRING_DELETION=y`. Gdy urządzenie zapomni bondu,
próbuje parować się od nowa, a NimBLE pyta wtedy o decyzję **funkcję zwrotną połączenia**
(`ble_gap_repeat_pairing_event()` → `ble_gap_call_conn_event_cb()`). Dla linków centralnych
trzyma ją `esp_hidh`, który tego zdarzenia nie obsługuje, a globalny listener nie pomoże,
bo jego wynik jest ignorowany. Ta opcja każe stackowi samemu usunąć nieaktualny bond
i powtórzyć parowanie.

#### Pułapka diagnostyczna: `Subscribe complete; status=259` to NIE błąd subskrypcji

Zmyliło mnie na starcie, więc warto to zapamiętać. `attach_report_listeners()` robi na
każdy raport **dwie** operacje:

```c
register_for_notify(dev->ble.conn_id, report->handle);   /* zapis {1,0} w uchwyt WARTOSCI */
if (report->ccc_handle)
    write_char_descr(..., report->ccc_handle, ...);      /* PRAWDZIWA subskrypcja */
```

Pierwsza pisze do uchwytu **wartości** charakterystyki raportu, a nie do CCCD — więc
ATT 0x03 (Write Not Permitted) jest tam całkowicie normalny i nic nie znaczy. To właśnie
ten zapis drukuje `Subscribe complete`. Prawdziwa subskrypcja leci przez `on_write()`,
które w oryginale loguje na **DEBUG**, czyli w praktyce niewidocznie.

W naszej kopii komponentu ten log jest podniesiony do INFO i nazwany `zapis CCCD`,
a dodatkowo logujemy `subskrypcja: id=… ccc_handle=…` oraz ostrzeżenie, gdy raport INPUT
zostaje pominięty. Widać wtedy od razu, że AJ159 Pro ma dwa raporty INPUT
w trybie BOOT (`protocol_mode=0`), które są pomijane **słusznie**, bo używamy trybu Report.

### 4.30 Dwie usługi z NimBLE, których nie da się użyć do udawania pada Xbox

Żeby Windows załadował sterownik pada Xbox i udostępnił urządzenie przez XInput,
muszą się zgadzać **dwie** rzeczy: tożsamość (PnP ID) i deskryptor raportu. Obie gotowe
usługi z NimBLE okazały się do tego nieprzydatne, każda z innego powodu.

**`ble_svc_dis` nie potrafi ustawić źródła VID na USB.** PnP ID (charakterystyka 0x2A50)
ma 7 bajtów: źródło VID, VID, PID, wersja. Źródło `0x01` znaczy „rejestr Bluetooth SIG",
`0x02` — „rejestr USB Implementers Forum". Pad Xbox ma VID `0x045E`, czyli numer **USB**
Microsoftu, więc źródło musi być `0x02`. A `ble_svc_dis.c` wpisuje pierwszy bajt na sztywno:

```c
case BLE_SVC_DIS_CHR_UUID16_PNP_ID:
    info = ble_svc_dis_data.pnp_id;
    uint8_t flag = 0x01;                       /* zrodlo VID, ZASZYTE */
    os_mbuf_append(ctxt->om, &flag, sizeof flag);
    break;
...
os_mbuf_append(ctxt->om, info, strlen(info));  /* i nasze 6 bajtow jako STRING */
```

Wartość jest więc doklejana jako łańcuch znaków (domyślnie `"000000"`, czyli sześć znaków
ASCII `0x30`), a źródła nie da się zmienić z zewnątrz.

**`ble_svc_hid` obcina Report Map do 255 bajtów.** Bufor ma 512 B, ale długość jest
trzymana w `uint8_t` — i tym polem usługa karmi hosta:

```c
ble_svc_hid.h:99    uint8_t report_map[REPORT_MAP_SIZE];   /* 512 */
ble_svc_hid.h:102   uint8_t report_map_len;                /* <- osiem bitow */
ble_svc_hid.c:467   os_mbuf_append(ctxt->om, &hid_instances[i].report_map,
                                   hid_instances[i].report_map_len);
```

Deskryptor pada Xbox ma **334 B**, więc Windows dostałby 78 bajtów (334 modulo 256) —
obcięty, bezsensowny opis. Złapał to kompilator przy przypisaniu:
`conversion from 'unsigned int' to 'uint8_t' changes value from '334' to '78'`.

Komponentu `bt` nie da się sforkować tak jak `esp_hid` (§4.27) — zawiera prekompilowane
biblioteki kontrolera. Dlatego **obie usługi są w projekcie napisane wprost na GATT**
w `ble_gamepad.c`: HID 0x1812 (Report Map, HID Information, Control Point, Protocol Mode,
charakterystyki Report z deskryptorami Report Reference) oraz DIS 0x180A (PnP ID,
Manufacturer Name). Razem to ~150 linii definicji i dwa callbacki dostępu.

Dwie korzyści poza samą możliwością:

- uchwyty charakterystyk dostajemy wprost przez `val_handle`, więc zniknęło zgadywanie
  przez `ble_gatts_find_chr()`, które przy kilku charakterystykach o tym samym UUID 0x2A4D
  zwraca po prostu pierwszą,
- widzimy zapisy hosta do raportu wyjściowego, co jest **testem rozstrzygającym**: jeśli
  Windows wysyła polecenia wibracji, to obsługuje nas swoim sterownikiem pada, a nie jako
  zwykłe HID.

Usługa `ble_svc_dis` z NimBLE nie jest w naszym buildzie rejestrowana (`ble_svc_dis_init()`
woła wyłącznie `nimble_hidd.c`, którego nie używamy), więc nie ma ryzyka dwóch usług 0x180A.
Opcji `CONFIG_BT_NIMBLE_HID_SERVICE` nie można wyłączyć, bo gatuje też hosta `esp_hidh`
(§4.8) — zostaje włączona, a jej struktury po prostu nie używamy.

### 4.31 Profil Xbox: skąd deskryptor i jak mapowane są wejścia

Deskryptor pochodzi z projektu **Mystfit/ESP32-BLE-CompositeHID** (licencja MIT), który
odczytał go z prawdziwego pada Xbox One S (model 1708, firmware sprzed 2021). Nie jest
przepisany ręcznie — generuje go `scripts/gen_xbox_report_map.py` do
`firmware/main/xbox_report_map.h`, bo 334 bajty przepisywane z komentarzy to gwarantowana
literówka, której potem szukalibyśmy w zachowaniu Windows, a nie w kodzie. Skrypt
rozwiązuje nazwane identyfikatory raportów, sprawdza bilans kolekcji HID i liczy sumę
kontrolną (`sha256 df1c86a6…`). Wygenerowany nagłówek jest w repo, więc do zwykłego
budowania klon repo referencyjnego nie jest potrzebny.

Tożsamość: źródło VID `0x02` (USB), VID `0x045E` (Microsoft), PID `0x02FD` (pad Xbox
One S), wersja `0x0408`. Nazwa rozgłaszana: `Xbox Wireless Controller`.

Raport wejściowy ma **16 bajtów**, wszystko little-endian:

| Bajty | Zawartość |
|---|---|
| 0–1, 2–3 | X, Y — lewy analog, 16 bitów **bez znaku**, środek 32768 |
| 4–5, 6–7 | Z, Rz — prawy analog |
| 8–9 | hamulec = lewy spust, 10 bitów (0–1023) + 6 bitów dopełnienia |
| 10–11 | gaz = prawy spust, 10 bitów |
| 12 | hat switch (0 = wyśrodkowany, 1–8 kierunki) + 4 bity dopełnienia |
| 13–14 | 15 przycisków + 1 bit dopełnienia |
| 15 | przycisk Share + 7 bitów dopełnienia |

Maski przycisków mają **dziury** — bity 2, 5, 8 i 9 są puste. Tak jest w prawdziwym padzie
i nie wygładzamy tego, bo celem jest zgodność:
`A=0x0001 B=0x0002 X=0x0008 Y=0x0010 LB=0x0040 RB=0x0080 View=0x0400 Menu=0x0800
Guide=0x1000 LS=0x2000 RS=0x4000`.

Mapowanie wejść zostało bez zmian — `input_mapper` nadal produkuje te same 12 przycisków
i cztery osie `int8`, a tłumaczenie na kontrolki Xbox siedzi w `ble_gamepad.c` (tablica
`s_xbox_ctrl`). Dzięki temu mapper nie musi wiedzieć, który profil jest aktywny, a zmiana
przypisania to jedna tablica:

| Nasz przycisk | Wejście | Kontrolka Xbox |
|---|---|---|
| 1 | lewy przycisk myszy | prawy spust (RT) |
| 2 | prawy przycisk myszy | lewy spust (LT) |
| 3 | środkowy przycisk myszy | klik prawej gałki (RS) |
| 4 | Spacja | A |
| 5 | lewy Shift | klik lewej gałki (LS) |
| 6 | lewy Ctrl | B |
| 7 / 8 | E / Q | X / Y |
| 9 / 10 | R / F | LB / RB |
| 11 / 12 | Tab / Esc | View / Menu |

Wybór jest mój, kierowany tym, jak te klawisze działają w grach: lewy przycisk myszy to
strzał (prawy spust), prawy to celowanie (lewy spust), Shift to sprint (klik gałki).
Osie 8-bitowe są skalowane do 16 bitów tak, że zero wypada dokładnie na 32768, a końce
zakresu na 0 i 65535.

Krzyżak jest zasilany **klawiszami strzałek**. `input_mapper` składa z nich bitmapę
(`GAMEPAD_DPAD_*`), a `ble_gamepad.c` zamienia ją na wartość hat switcha przez tablicę
16-elementową: 0 = wyśrodkowany, 1–8 zgodnie z ruchem wskazówek zegara od góry.
Przeciwne kierunki wciśnięte naraz **znoszą się** — bez tego wynik zależałby od
kolejności sprawdzania warunków. W profilu generycznym krzyżak jest nieaktywny, bo tamten
deskryptor nie ma hat switcha.

Profil wybiera się w menuconfig (`APP_GAMEPAD_PROFILE`); generyczny pad DirectInput
zostaje jako wyjście awaryjne, bo to on jest zweryfikowany w Etapie 3. **Przełączenie
profilu zmienia deskryptor i tożsamość, więc wymaga usunięcia pada z listy urządzeń
Bluetooth w Windows i sparowania od nowa** (§4.7).

### 4.32 Windows wiąże sterownik XInput tylko z PID 0x0B13 i 0x0B20–0x0B27

Pierwsze podejście do Etapu 4 udawało pada **Xbox One S (model 1708, PID 0x02FD)** — ten
model ma w projekcie referencyjnym najlepiej opisany deskryptor. Windows sparował
urządzenie, przeczytał deskryptor i **podpiął generyczny sterownik**: w `joy.cpl` pojawił
się „6-osiowy 17-przyciskowy pad", a Apex Legends, Rocket League i Steam pada nie widziały
w ogóle. Liczby się zgadzały (4 osie + 2 spusty = 6 osi; 15 przycisków + AC Back +
AC Home = 17), czyli deskryptor był sparsowany poprawnie — po prostu nie uruchomiło to
XInput.

Rozstrzygające było sprawdzenie, **jaki identyfikator sprzętowy nadał nam Windows**:

```
PS> Get-PnpDevice -Class HIDClass | ... DEVPKEY_Device_HardwareIds
HID\{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02045e_PID&02fd_REV&0408
```

Czyli tożsamość dotarła **idealnie**: źródło `02` (USB), VID `045e`, PID `02fd`. Problem był
gdzie indziej — w tym, z czym Windows umie się z tym powiązać. Sterownik siedzi
w `C:\Windows\INF\xinputhid.inf`:

```
Btle_Bus.DeviceDesc = "Bluetooth LE XINPUT compatible input device"
...
%Btle_Bus.DeviceDesc%=Btle_Bus, BTHLEDevice\{...1812...}_Dev_VID&02045e_PID&0b13
%Btle_Bus.DeviceDesc%=Btle_Bus, BTHLEDevice\{...1812...}_Dev_VID&02045e_PID&0b20
   ... 0b21, 0b22, 0b23, 0b24, 0b25, 0b26, 0b27
```

**Dopasowanie idzie tylko po VID i PID; `REV` w nim nie występuje.** Lista obejmuje
wyłącznie rodzinę `0x0Bxx`, czyli pady z ery Series X/S. `0x02FD` (One S) jej nie ma —
i dlatego pad z tamtą tożsamością nigdy nie dostanie XInput po BLE, niezależnie od
deskryptora.

Wniosek praktyczny: **udajemy pada Xbox Series X (model 1914): PID `0x0B13`, wersja
`0x0509`**, z deskryptorem `XboxOneS_1914_HIDDescriptor` (283 B, dwa raporty: `0x01` INPUT
16 B i `0x03` OUTPUT 8 B). Wcześniejszy wybór modelu 1708 był podyktowany komentarzem
w projekcie referencyjnym o obsłudze wibracji na starszych jądrach Linuksa — dla Windows
liczy się dokładnie odwrotna rzecz.

Metodologicznie warto to zapamiętać: przy udawaniu urządzenia **nie ma sensu zgadywać, czy
system nas rozpoznał** — wystarczy odczytać nadany identyfikator sprzętowy i sprawdzić go
w plikach INF. To dwa polecenia i daje odpowiedź pewną zamiast prób.

Uwaga: deskryptor 1914 deklaruje **dwa** raporty, nie cztery jak 1708 (nie ma osobnego
raportu z przyciskiem Guide ani z poziomem baterii). Dlatego identyfikatory i długości
raportów nie są w kodzie wpisane liczbami — `scripts/gen_xbox_report_map.py` liczy je
z sumy bitów pól w deskryptorze i emituje tablicę `xbox_reports[]`, a
`register_hid_service()` sprawdza jej układ przy starcie. Generator potwierdził przy okazji
niezależnie, że raport wejściowy ma 16 bajtów — czyli tyle, ile wyszło z ręcznej analizy
w §4.31.

### 4.33 Dlaczego mysz raportowała tylko ~20–25 razy na sekundę

To nie ograniczenie myszy, tylko **interwał połączenia BLE**. Peryferial może wysłać
notyfikację wyłącznie w zdarzeniu połączenia, więc interwał jest sztywnym górnym limitem
częstotliwości raportów — niezależnie od tego, jak szybko urządzenie próbkuje wewnętrznie.

Kto go ustala: central, przy wywołaniu `ble_gap_connect()`. A `esp_hidh` podaje tam `NULL`:

```c
nimble_hidh.c:991   ret = ble_gap_connect(own_addr_type, &addr, 30000, NULL,
                                          esp_hidh_gattc_event_handler, NULL);
```

więc obowiązują domyślne z NimBLE:

```c
ble_gap.h:113   #define BLE_GAP_INITIAL_CONN_ITVL_MIN   BLE_GAP_CONN_ITVL_MS(30)
ble_gap.h:116   #define BLE_GAP_INITIAL_CONN_ITVL_MAX   BLE_GAP_CONN_ITVL_MS(50)
```

30–50 ms to 20–33 Hz i dokładnie tyle mierzyliśmy w §4.22 (raporty co ~40–50 ms). Zgodność
jest na tyle dokładna, że nie ma tu miejsca na inną hipotezę.

Dla porównania: link do PC ma `itvl=12`, czyli **15 ms** — bo tam centralem jest Windows
i on negocjuje sensowną wartość. Widać to w logu przy szyfrowaniu.

**Poprawka:** po otwarciu urządzenia wołamy `ble_gap_update_params()` z żądaniem 11,25–15 ms
(`request_fast_interval()` w `ble_hid_host.c`). Trzy decyzje projektowe warte zapisania:

- **Dopiero po otwarciu**, nie zaraz po połączeniu. Odkrywanie usług to najcięższy moment
  dla stacku i oba historyczne crashe wypadły właśnie wtedy (§4.21, §4.26); zagęszczanie
  zdarzeń połączenia w chwili, gdy raportów jeszcze nie ma, byłoby ryzykiem bez zysku.
- **Docelowy interwał jest w Kconfig** (`APP_INPUT_CONN_ITVL`), bo to kompromis, nie
  stała: krótszy interwał to więcej zdarzeń połączenia w tej samej sekundzie, także
  pustych, a na jednej antenie mamy trzy linki plus okresowy skan. Pierwsze podejście
  dawało 15 ms (66 Hz); po zgłoszeniu, że AJ159 Pro obsługuje **125 Hz po BT**, zejście
  do **7,5 ms** — minimum ze specyfikacji BLE, 133 Hz. Gdyby pojawiła się niestabilność,
  wartość podnosi się jedną opcją, bez grzebania w kodzie.
- **Jeśli urządzenie już ma krótki interwał, nie ruszamy go.**

Nierozstrzygnięty wątek obok: `hci_err=0x212` na `ocf=0x0013` (LE Connection Update)
pojawia się w logu stale, jeszcze zanim sami cokolwiek aktualizujemy. Ktoś inicjuje zmianę
parametrów i kontroler ją odrzuca. Sprawdzone, że nie chodzi o `min_ce_len`/`max_ce_len` —
domyślne są `0x0000`.

#### Rozstrzygnięcie: kontroler C3 przy trzech linkach nie zejdzie poniżej 15 ms

Pierwsze żądanie 7,5 ms zwróciło `rc=530`, czyli `0x212` — **dokładnie ten sam błąd, który
od początku widzieliśmy w logu jako `hci_err=0x212` na `ocf=0x0013`**. To był jeden problem,
nie dwa: kontroler odrzuca aktualizacje parametrów połączenia.

Zamiast zgadywać, który parametr mu nie pasuje, firmware przeprowadził eksperyment sam —
sześć zestawów po kolei, każdy z wynikiem w logu. Wynik pierwszej serii:

| Próba | Wynik |
|---|---|
| 7,5–10 ms | odrzucona, HCI 0x12 |
| 7,5–10 ms **z `ce_len`** | odrzucona, HCI 0x12 |
| 7,5–10 ms **z timeoutem nadzoru 4 s** | odrzucona, HCI 0x12 |
| 15–20 ms | **przyjęta**, link dostał 20 ms |

To wyklucza `ce_len` i timeout nadzoru — problemem jest **sam interwał**. Druga obserwacja
z tej serii: przy podaniu zakresu kontroler wybrał jego **górną** granicę (poprosiliśmy
15–20 ms, dostaliśmy 20 ms). Dlatego prosimy teraz o konkretną wartość (`min == max`).

Druga seria, drabinką od najkrótszej, dała **identyczny wynik dla obu urządzeń**
(klawiatura `conn_handle=3`, mysz `conn_handle=4`):

```
interwal 6 (7.50 ms) odrzucony: rc=530 (HCI 0x12)
interwal 8 (10.00 ms) odrzucony: rc=530 (HCI 0x12)
interwal 10 (12.50 ms) odrzucony: rc=530 (HCI 0x12)
PRZYJETE: interwal 12 (15.00 ms = 66 Hz)
parametry linku 3: status=0 itvl=12 (15.00 ms) latency=0 timeout=256
```

Czyli **15 ms to podłoga tego kontrolera przy trzech aktywnych linkach**. Efekt końcowy:
45 ms → 15 ms, z 22 Hz na 66 Hz, czyli trzykrotnie. Deklarowanych przez AJ159 Pro 125 Hz
**nie da się osiągnąć** — to ograniczenie kontrolera, nie naszego kodu.

Czego nie wiem: dokładnej reguły, którą stosuje kontroler. Zwraca „Invalid HCI Command
Parameters", co sugeruje walidację wartości, a nie brak zasobów — ale 12,5 ms jest poprawną
wartością w rozumieniu specyfikacji, więc to walidacja **kontekstowa**, zależna od tego, co
już jest zestawione. Warto zauważyć, że link do PC też pracuje na 15 ms (wybrał go Windows),
więc możliwe, że kontroler wymaga wspólnej siatki dla wszystkich linków.

**Tani eksperyment, gdyby ktoś chciał to domknąć:** wyłączyć Bluetooth w PC (znika trzeci
link) i sprawdzić, czy wtedy 7,5 ms przechodzi. Jeśli tak, ograniczeniem jest liczba linków
i wspólna siatka; jeśli nie, to sztywny limit kontrolera. Firmware sam zaloguje wynik, bo
drabinka jest w kodzie na stałe.

Drabinka zostaje włączona także dlatego, że nie wpisuje wyniku na sztywno: gdyby przyszła
wersja IDF albo inna liczba linków dopuszczała krótszy interwał, firmware sam go weźmie.

#### Link do PC: 7,5 ms, czyli 133 Hz — i sprostowanie

Napisałem wcześniej, że „prawdziwy pad Xbox po Bluetooth pracuje na tych samych 15 ms".
**To była ekstrapolacja z naszego linku, nie pomiar, i była błędna.** Właściciel zwrócił
uwagę, że pad Xbox obsługuje po BT 125 Hz — i miał rację.

Na tym linku centralem jest Windows, więc nie narzucamy parametrów; peryferial może tylko
poprosić (`ble_gap_update_params()` przechodzi wtedy na LL Connection Parameters Request
albo na L2CAP). Dorobiona prośba (`negotiate_pad_interval()` w `ble_gamepad.c`) pokazała
trzy rzeczy:

1. **Windows daje 7,5 ms.** Zmierzone: `link do PC: interwal 6 (7.50 ms = 133 Hz)`.
   Wcześniej w logach było 15 ms, potem 10 ms — wartość zmieniała się po ponownym
   sparowaniu, więc nie była żadną stałą, tylko wynikiem negocjacji.
2. **Odpowiedź jest asynchroniczna.** `rc == 0` z `ble_gap_update_params()` znaczy tylko
   „wysłano"; wynik przychodzi jako `BLE_GAP_EVENT_CONN_UPDATE` ze statusem. Dlatego po
   każdej próbie odczytujemy `conn_itvl` i patrzymy, co faktycznie jest.
3. **Prośba wysłana za wcześnie wraca z kolizją.** Pierwsze podejście, wysłane 10 ms po
   `PC wlaczyl notyfikacje raportu`, dało `status=554`, czyli HCI `0x2A`
   (Different Transaction Collision) — Windows kończył jeszcze swoje procedury. Stąd
   1,5 s odczekania przed pierwszą próbą i po trzy podejścia na każdą wartość.

Ważne rozróżnienie, które log teraz robi wprost: `nasza strona nie wyslala prosby`
(odmówił nasz host albo kontroler C3, jeszcze przed eterem) kontra
`Windows nie skrocil interwalu` (prośba poszła, decyzja była po drugiej stronie).

Konsekwencja dla całości: **najwęższym ogniwem nie jest już pad, a wejścia.** Pad idzie
z PC na 7,5 ms (133 Hz), a klawiatura i mysz na 15 ms (66 Hz), bo tam limit narzuca nasz
kontroler. Stabilność sprawdzona: 100 s z padem na 7,5 ms, bez rozłączeń, heap bez zmian.

Otwarte pytanie (hipoteza właściciela): skoro link do PC pracuje teraz na 7,5 ms, czy
kontroler nadal odrzuci 7,5 ms dla myszy? Jeśli jego reguła dotyczyła najdłuższego
interwału w systemie albo wspólnej siatki, to teraz sytuacja jest inna.

#### Odpowiedź: nie, i to zawęża podejrzanych

Zmierzone przy padzie pracującym na 7,5 ms:

```
link do PC: interwal 6 (7.50 ms = 133 Hz)
link 3: interwal 36 (45 ms), latency=0, timeout nadzoru=256 (2560 ms)
interwal 6 (7.50 ms) odrzucony: rc=530 (HCI 0x12)
interwal 8 (10.00 ms) odrzucony: rc=530 (HCI 0x12)
interwal 10 (12.50 ms) odrzucony: rc=530 (HCI 0x12)
PRZYJETE: interwal 12 (15.00 ms = 66 Hz)
```

Hipoteza **odrzucona**: skrócenie linku pada nic nie odblokowało. Ale ten sam log wyklucza
przy okazji dwie inne rzeczy i zostawia jedną **mocną obserwację**:

- **To nie brak przepustowości radia.** Kontroler w tym samym momencie utrzymuje link
  7,5 ms w roli peryferiala (pad). Fizycznie 7,5 ms działa.
- **To nie liczba linków.** Odrzucenie wypadło przy `razem 1/2`, czyli przy **jednym**
  linku centralnym. Wcześniejsza hipoteza „3 linki × 5 ms = 15 ms" upada.
- **Asymetria rola-zależna:** kontroler *utrzymuje* 7,5 ms, gdy interwał narzuca peer
  (my jesteśmy peryferialem), ale *odmawia zainicjowania* czegokolwiek poniżej 15 ms, gdy
  sam jest centralem. Odmowa jest natychmiastowa i synchroniczna (`rc` z
  `ble_gap_update_params()`), czyli pochodzi z walidacji komendy HCI, a nie z negocjacji
  z urządzeniem.

Metodologicznie warto zapamiętać, że pierwszy przebieg tego testu **nic nie sprawdził**:
domyślne `APP_INPUT_CONN_ITVL` było już ustawione na 12, więc drabinka poprosiła od razu
o 15 ms i dostała, nie próbując krótszych wartości. Dopiero ustawienie 6 dało odpowiedź.

Ostatni niesprawdzony podejrzany: **skaner**. Przy `1/2` urządzeń mostek dalej skanuje,
a skan rezerwuje czas radia. Dlatego `scan_task` ponawia próbę raz, po zatrzymaniu
skanowania (czyli po osiągnięciu limitu urządzeń).

#### Sprawa zamknięta: 15 ms to sufit tego kontrolera w roli centrala

Test przy **zatrzymanym skanerze**, obu urządzeniach podłączonych i padzie na 7,5 ms:

```
skan zatrzymany (limit urzadzen) - ponawiam probe skrocenia interwalu
link 3: interwal 12 (15 ms), latency=0, timeout nadzoru=256 (2560 ms)
interwal 6 (7.50 ms) odrzucony: rc=530 (HCI 0x12)
interwal 8 (10.00 ms) odrzucony: rc=530 (HCI 0x12)
interwal 10 (12.50 ms) odrzucony: rc=530 (HCI 0x12)
zaden interwal nie przeszedl - zostaje 15 ms
```

To samo dla linku 4. Skaner **też nie jest przyczyną**. Wyczerpaliśmy w ten sposób wszystkie
sensowne podejrzenia — każde odrzucone osobnym pomiarem, nie rozumowaniem:

| Podejrzany | Jak wykluczony |
|---|---|
| przepustowość radia | kontroler równolegle utrzymuje link 7,5 ms w roli peryferiala (pad) |
| liczba linków | odrzucenie także przy **jednym** linku centralnym (`razem 1/2`) |
| najdłuższy interwał / wspólna siatka | pad zszedł na 7,5 ms, wejścia nadal odrzucane |
| `min_ce_len` / `max_ce_len` | ten sam interwał z ustawionym `ce_len` też odrzucony |
| timeout nadzoru | ten sam interwał z timeoutem 4 s też odrzucony |
| skaner | próba po zatrzymaniu skanowania, z 3 s na uspokojenie radia — odrzucona |
| opcja w Kconfig kontrolera | w `components/bt/Kconfig` nie ma **żadnej** opcji dotyczącej interwału |

Wniosek: kontroler ESP32-C3 (biblioteka z IDF 5.5.1) **nie inicjuje interwałów krótszych
niż 15 ms w roli centrala**, i nie da się tego przestawić z zewnątrz. Odmowa jest
natychmiastowa i synchroniczna, czyli pochodzi z walidacji komendy HCI w kontrolerze.

Stan końcowy łańcucha: **wejścia 15 ms (66 Hz), pad → PC 7,5 ms (133 Hz)**. Wejścia są więc
teraz najwęższym ogniwem, ale 45 ms → 15 ms to i tak trzykrotna poprawa względem stanu
wyjściowego.

`APP_INPUT_CONN_ITVL` wraca do 12, żeby nie generować trzech odrzuceń przy każdym
podłączeniu. Drabinka zostaje w kodzie — kto chce sprawdzić, czy nowsza wersja IDF to
poluzowała, ustawia 6 i czyta log.

#### Potwierdzenie na S3: 15 ms to sufit RODZINY kontrolerów, nie jednej płytki

Ten sam pomiar powtórzony na ESP32-S3 (płytka ESP Thread BR, H2 wyczyszczony, żeby nie
konkurował), z `APP_INPUT_CONN_ITVL=6`, czyli z drabinką startującą od 7,5 ms:

```
link 4 (mysz): interval 6 (7.50 ms)  rejected: rc=530 (HCI 0x12)
               interval 8 (10.00 ms) rejected: rc=530 (HCI 0x12)
               interval 10 (12.50 ms) rejected: rc=530 (HCI 0x12)
               ACCEPTED: interval 12 (15.00 ms = 66 Hz), latency=0
link 3 (klawiatura): interval on link 3 is already 6 (7 ms), leaving it
```

Identycznie jak na C3, do kodu błędu włącznie. Ponieważ S3 dzieli z C3 **bibliotekę
kontrolera i przestrzeń opcji** (§4.35), wniosek z §4.33 wolno teraz sformułować szerzej:
15 ms to sufit **starej rodziny kontrolerów** w roli centrala, a nie cecha jednej płytki.

Zarazem link klawiatury pracuje na **7,5 ms** — bo o to prosi sama klawiatura. Dowodem jest
`latency=48` w jego parametrach: my zawsze prosimy o `latency = 0`, więc ta wartość mogła
przyjść tylko od peera. To domyka obraz z §4.33: kontroler **utrzymuje** krótki interwał
narzucony przez peera i **odmawia zainicjowania** go sam.

Praktyczny wniosek dla mostka: mysz dostaje 66 Hz na C3 i na S3, a klawiatura tyle, ile sobie
wynegocjuje.

#### Drabinka interwałów raportowała nieprawdę — naprawione

Przy pierwszym przebiegu na S3 drabinka ogłosiła `no interval was accepted - keeping 50 ms`,
podczas gdy link **właśnie ustawił się na 7,5 ms**. Dwa błędy w jednym miejscu:

- **`BLE_HS_EALREADY` (rc=2) traktowane jako odrzucenie.** To nie odrzucenie, a informacja
  „procedura aktualizacji już trwa" — bardzo często taka, którą zaczął peer. Stara wersja
  wystrzeliwała wszystkie sześć wartości w tę samą milisekundę i sześć razy dostawała `rc=2`,
  co w logu wyglądało jak sześć odmów kontrolera. Teraz czekamy 400 ms i ponawiamy, do
  czterech podejść.
- **`rc == 0` traktowane jako „przyjęte".** Odpowiedź jest asynchroniczna
  (`BLE_GAP_EVENT_CONN_UPDATE`) — to samo ustalenie, które §4.33 zapisał dla strony pada,
  ale w tej pętli nigdy go nie zastosowano. Teraz po wysłaniu prośby odczytujemy link przez
  1,5 s i dopiero zmierzona wartość decyduje.

Na koniec drabinka loguje zawsze **rzeczywisty** interwał linku, więc nie da się już
przeczytać z logu wniosku sprzecznego z faktem. Uwaga metodologiczna ta sama, co przy
`APP_DEBUG_SCAN_ONLY`: to już drugi raz, gdy nasze narzędzie pomiarowe kłamało w sposób
spójny i dlatego wiarygodny.

Zastrzeżenie do §4.33: tamte pomiary na C3 zapisywały `rc=530`, czyli **prawdziwe** odmowy
kontrolera (HCI 0x12), a nie `rc=2`, więc wniosek o suficie 15 ms nie był zbudowany na tej
wadzie i pozostaje w mocy.

### 4.37 Wersja USB: dwa ESP32-S3, pad XInput po USB (branch `s3-usb`)

```
klawiatura USB ─┐                                    ┌─ pad Xbox 360 (XInput) ─→ PC
                ├─→ hub ─→ S3 #1 (USB host) ─→ UART ─→ S3 #2 (USB device) ┘
mysz USB ───────┘
```

Kod jest napisany i **wszystkie konfiguracje się budują**, ale na sprzęcie nie był jeszcze
uruchomiony — właściciel nie ma w tej chwili dwóch S3. Ta sekcja zawiera ustalenia z
researchu, decyzje projektowe i plan testu, żeby uruchomienie nie wymagało dopisywania kodu.

#### Rozstrzygające ustalenie: po USB udajemy pada Xbox **360**, nie Series X

To jedyna rzecz, która zmieniła założenia. Sprawdzone tą samą metodą, która rozstrzygnęła
§4.32 — czytaniem plików INF w tym Windows, a nie zgadywaniem:

```
C:\Windows\INF\xusb22.inf
  %XUSB22.DeviceName.Wired%=CC_Install, USB\Vid_045E&Pid_028E   <- Xbox 360 wired
  %XUSB22.DeviceName%=CC_Install,       USB\Vid_045E&Pid_0719   <- odbiornik 360 wireless
  %XUSB22.DeviceName.Jump%=CC_Install,  USB\Vid_045E&Pid_028F   <- play and charge
```

Dopasowanie idzie po **samym VID/PID**, bez klasy interfejsu. Trzy wnioski:

1. **Deskryptor z wersji BLE jest tu bezużyteczny.** `xinputhid.inf`, który wiąże nasz pad
   BLE, ma **zero** wpisów `USB\` (policzone: 0). Urządzenie USB HID z PID `0x0B13` dostanie
   więc sterownik generyczny i żadnego XInput. **XInput po USB to w ogóle nie HID**: to
   interfejs vendorowy `0xFF / 0x5D / 0x01`, dwa endpointy interrupt i brak Report Map.
2. **Series X po USB to protokół GIP** (`xboxgip.inf`, „Xbox Game Input Protocol Driver"),
   zupełnie inny i znacznie większy stack. Emulacja pada 360 daje ten sam efekt dla gier —
   Windows wystawia zwykłego kontrolera XInput.
3. Tożsamość: **VID `0x045E`, PID `0x028E`**, `bcdDevice 0x0114`.

#### Protokół, wzięty z dwóch niezależnych źródeł

Deskryptory i format raportów pochodzą z przechwytu Wireshark **prawdziwego** pada
(partsnotincluded.com) i są zgodne z tym, na co dopasowuje się sterownik `xpad` w Linuksie
(`bInterfaceClass 0xFF`, `bInterfaceSubClass 93`, `bInterfaceProtocol 1`).

Raport wejściowy, 20 B na EP `0x81` co 4 ms (250 Hz):

| Bajt | Zawartość |
|---|---|
| 0, 1 | typ `0x00`, długość `0x14` |
| 2 | krzyżak (bity 0–3: góra, dół, lewo, prawo), Start, Back, L3, R3 |
| 3 | LB, RB, Guide, —, A, B, X, Y |
| 4, 5 | lewy i prawy spust, `uint8` 0–255 |
| 6–13 | gałki, `int16` LE, X przed Y, **północ-wschód dodatni** |
| 14–19 | nieużywane |

Wyjściowo: `0x00 0x08 … [3]=lewy silnik [4]=prawy` (rumble) oraz `0x01 0x03 [2]=wzór` (LED).

**Uwaga na znak osi Y.** Prawdziwy pad ma dodatni kierunek w górę, a nasz mapper produkuje
dodatni w dół (konwencja ekranowa, z myszy i z WASD). `xpad` w Linuksie neguje Y właśnie
z tego powodu. U nas ta jedna inwersja siedzi w `axis_to_xbox()` i nigdzie więcej — to
klasyczne miejsce na błąd „gałka odwrócona".

#### Jak to jest zrobione w TinyUSB

XInput nie jest żadną klasą, którą TinyUSB zna, więc:

- **cały deskryptor konfiguracji podajemy sami** — `esp_tinyusb` 1.7 przyjmuje
  `configuration_descriptor` i `device_descriptor` w `tinyusb_config_t`,
- **własna klasa** wchodzi przez `usbd_app_driver_get_cb()`, który w TinyUSB jest symbolem
  **słabym** (`TU_ATTR_WEAK` w `usbd.c`) — to udokumentowany sposób dodania klasy bez
  forkowania stacku, więc nie powtarzamy historii z §4.30, gdzie musieliśmy pisać usługi
  GATT ręcznie, bo gotowych nie dało się użyć.

Zadeklarowany jest **tylko interfejs 0**. Prawdziwy pad ma cztery (headset, nieznany,
Xbox Security Method), ale żaden nie jest potrzebny do związania XInput, a sam security
method nie jest zaimplementowany po stronie sterownika Windows.

**Deskryptor vendorowy `0x21` (17 B) jest przepisany dosłownie i to nie jest zabobon.**
Powtarza adresy endpointów i ich rozmiary, a źródło, które go rozebrało, stwierdziło wprost:
zmiana numeru endpointu w deskryptorze endpointu **bez** zmiany go tutaj sprawia, że sterownik
Windows przestaje rozmawiać z urządzeniem.

**Jedno świadome odstępstwo:** prawdziwy pad podaje `bMaxPacketSize0 = 8`, my podajemy 64,
bo tyle ma kontrolny endpoint TinyUSB (`CFG_TUD_ENDPOINT0_SIZE`) i deskryptor musi się
zgadzać ze sprzętem. Dopasowanie w INF idzie po VID/PID, więc jest to bezpieczne; gdyby
kiedyś przeszkadzało, wymuszamy `CFG_TUD_ENDPOINT0_SIZE` na 8.

#### Huby: sprawdzone, że IDF 5.5.1 to potrafi

To była druga rzecz, która mogła wywrócić plan („mysz i klawiatura przez huba"). W
`components/usb/Kconfig` w 5.5.1 jest `CONFIG_USB_HOST_HUBS_SUPPORTED` (domyślnie `n`) oraz
`CONFIG_USB_HOST_HUB_MULTI_LEVEL` dla hubów łączonych kaskadowo. **Nie jest to opcja za flagą
eksperymentalną** — za `IDF_EXPERIMENTAL_FEATURES` schowana jest tylko liczba prób resetu
portu. Własny przykład ESP-IDF `usb/host/hid` włącza huby w swoim
`sdkconfig.defaults`, co jest najlepszym dowodem, że to zamierzony scenariusz.

Bez tej opcji host obsługuje **dokładnie jedno** urządzenie podłączone bezpośrednio, czyli
klawiatura i mysz naraz nie są możliwe.

#### Sprzęt: dwie rzeczy do wiedzenia przed testem

- **Konsola musi zejść z USB Serial/JTAG na UART, na obu układach.** Na S3 peryferium
  USB Serial/JTAG i USB-OTG są podłączone do **tych samych pinów GPIO19/20** i tylko jedno
  może nimi sterować. TinyUSB (albo host USB) zabiera te piny, więc konsola po USB
  **zamilknie** — a objaw jest zwodniczy, wygląda jak martwy firmware. Na
  **ESP32-S3-DevKitC-1** nic nie tracimy: ta płytka ma drugie gniazdo USB idące przez mostek
  USB-UART do UART0. To ta sama pułapka, która kosztowała rundę diagnozy na H2-DevKitM
  (§4.35), tylko z innej strony.
- **Host musi zasilić urządzenia (VBUS).** Gniazdo devkitu samo z siebie nie poda 5 V
  urządzeniom, dlatego zalecany jest **hub z własnym zasilaniem** — co przy okazji zdejmuje
  pytanie, czy klawiatura z myszą nie przekroczą tego, co płytka mogłaby dać.

#### Co zostało wspólne z wersją BLE, a co nowe

Szew okazał się czterosymbolowy. `input_mapper` **pobiera** stan i **wysyła** raport, więc
oba końce są teraz makrami wybieranymi w czasie kompilacji (`IO_TAKE_STATE`, `IO_INPUT_BUSY`,
`IO_PAD_SEND`). Logika mapowania, krzywa myszy, filtr `ErrorRollOver` i tablica przypisań
klawiszy **nie są duplikowane** — wersja USB używa dokładnie tych samych.

Nowe pliki: `usb_pad.c` (urządzenie XInput), `usb_hid_host.c` (host + HID),
`input_state.c` (akumulator stanu dla buildów bez BLE), `gamepad_state.h` i `input_state.h`
(typy wyjęte ze nagłówków BLE, żeby build USB nie musiał ich włączać).

`chip_link` dorobił **ramkę klawiatury** (`0x03`: modyfikatory + 6 kodów, stan absolutny,
więc zgubiona ramka sama się naprawia) oraz keepalive z **maską obecności** zamiast jednego
bitu myszy. Podział BLE zachowuje się jak dotąd, bo tam obecność myszy nadal liczy się
z tablicy urządzeń.

Warianty: jeden target, dwie role, więc skrypty przyjmują teraz **nazwę wariantu** i trzymają
osobne katalogi build oraz osobny `sdkconfig`:

```bat
scripts\build-native-win.bat esp32s3 s3pad
scripts\flash-win.bat COM<n> esp32s3 s3pad
scripts\build-native-win.bat esp32s3 s3input
scripts\flash-win.bat COM<m> esp32s3 s3input
```

#### Co już zweryfikowane bez sprzętu

Deskryptor jest sprawdzany skryptem **na zbudowanej binarce**, nie na źródle — czyli na tym,
co faktycznie pójdzie na drut:

```
python scripts/check_xinput_descriptor.py firmware/build.win.esp32s3.s3pad

  configuration descriptor: 49 B (wTotalLength says 49)
  interface 0: class FF subclass 5D protocol 01, 2 endpoints - OK
  vendor 0x21: IN 0x81/20 B, OUT 0x01/8 B, consistent with the endpoints - OK
  endpoint 0x81: interrupt, 32 B, every 4 ms - OK
  endpoint 0x01: interrupt, 32 B, every 8 ms - OK
  device: VID 0x045E PID 0x028E (Xbox 360 wired) - matches xusb22.inf
  OK: interface 0 is byte-for-byte the real controller's
```

Skrypt porównuje blok interfejsu 0 **bajt w bajt** z przechwytem prawdziwego pada i sprawdza
spójność wewnętrzną (długości, zgodność endpointów z deskryptorem `0x21`). Powstał z tego
samego powodu co `gen_xbox_report_map.py`: 40 bajtów przepisywanych ręcznie to gwarantowana
literówka, której potem szuka się w zachowaniu Windows, a nie w kodzie.

Zbudowane bez ostrzeżeń: `s3pad` (284 kB), `s3input` (339 kB) oraz — jako test regresji —
`esp32c3`, `esp32s3`, `esp32c6`, `esp32h2` w rolach BLE. Wersja USB **nie rusza** platformy
odniesienia: na C3 komponenty USB nie są nawet zaciągane (reguły `rules:` w
`idf_component.yml` po targecie), bo C3 nie ma peryferium USB OTG i build by padł.

#### Plan testu na sprzęcie

1. ~~**Sam pad, bez drugiego układu.**~~ **PRZEJECHANE.** `s3pad` na ESP32-S3 SuperMini:
   `USB\VID_045E&PID_028E\08FEC93` z `Service=xusb22`, w logu `pad ready`, heap 371 088 B
   niezmienny. Bez drugiego układu pad jest nieruchomy — tak ma być, wejść nie ma.
2. ~~**Rozstrzygający dowód XInput.**~~ **PRZEJECHANE, i mocniej niż planowano.** Zamiast
   szukać gry, która wibruje, `scripts/xinput_rumble.py` woła XInput wprost: `XInputGetState`
   pokazuje pada w **slocie 0**, a `XInputSetState` z trzema **różnymi** parami wartości daje
   w logu trzy zgodne linie w tej samej kolejności:
   `0xffff,0x0000` → `rumble from host: left=255 right=0`; `0x0000,0xffff` → `left=0 right=255`;
   `0x4000,0x4000` → `left=64 right=64`; zerowanie → `left=0 right=0`. Skalowanie 16→8 bitów
   zgodne z przewidywaniem. Polecenia wibracji wysyła **wyłącznie** sterownik pada, więc to
   dowodzi zarazem wiązania sterownika, obecności w slocie XInput i działania endpointu OUT.
3. ~~**Sam host, bez pada.**~~ **PRZEJECHANE.** `s3input` z zasilanym hubem i dwoma dongle'ami
   2,4 GHz: `usb ifaces 4 (kbd=1 mouse=1)`, `KBD report len=8 [00 00 1a 00]`,
   `MOU report len=7 [00 ff ff 07]`, heap 347 404 B stabilny. Interfejsy boot potwierdzone
   wcześniej z drzewa urządzeń Windows (§4.38).
4. ~~**Drut.**~~ **PRZEJECHANE.** `s3input` TX = GPIO4 → `s3pad` RX = GPIO5, wspólna masa; piny
   podane jawnie w wariantach, bo domyślne z Kconfig opisują płytkę BR (§4.38). Nadajnik
   raportuje `sent 19884 frames (dropped 0)`, a dowodem na drugi koniec są ruszające się osie
   pada. Rozpiska: [`docs/WIRING-USB.pl.md`](docs/WIRING-USB.pl.md).
5. ~~**Całość.**~~ **PRZEJECHANE.** Ruch myszy → prawa gałka i WASD → lewa zmierzone
   (`L=(0,32767)` po `w`, `L=(-32767,0)` po `a`, 977 zmian stanu przy kręceniu myszą), a resztę
   domyka potwierdzenie właściciela: **test kontrolera w Steam pokazuje wszystko poprawnie** —
   osie, spusty, przyciski i krzyżak. Cały plan zamknięty.
4. **Drut.** Połączyć UART: TX hosta → RX pada, wspólna masa. Piny są już ustawione jawnie
   w obu wariantach (`s3input` TX = GPIO4, `s3pad` RX = GPIO5, czyli piny z listwy
   18-pinowej) — patrz §4.38 i [`docs/WIRING-USB.pl.md`](docs/WIRING-USB.pl.md).
   Host ma raportować `sent N frames (dropped 0)`, pad `received N frames (CRC errors 0)` i
   `peer serves: mouse keyboard`.
5. **Całość.** Ruch myszy → prawa gałka, WASD → lewa, klawisze i przyciski zgodnie z tabelą
   mapowania, która jest wspólna z wersją BLE.

Czego **nie** wiedzieliśmy do tego testu: czy Windows zwiąże XUSB przy zadeklarowanym jednym
interfejsie zamiast czterech. **ROZSTRZYGNIĘTE NA SPRZĘCIE: zwiąże.** Wgrany `s3pad` zgłasza się
jako `USB\VID_045E&PID_028E\08FEC93` z `Service=xusb22` i nazwą „Kontroler konsoli Xbox 360 dla
systemu Windows”, czyli dostaje sterownik XInput, a nie generyczny. Deskryptor interfejsu 0 jest
identyczny z prawdziwym padem, a dopasowanie idzie po VID/PID — pozostałe trzy interfejsy
prawdziwego pada okazały się do wiązania niepotrzebne. Gdyby kiedyś jednak były, poprawka jest
**wyłącznie w deskryptorze**: dopisać je (bajty są w komentarzu `usb_pad.c`), co nie rusza ani
jednej linii logiki.

### 4.38 Wersja USB na dwóch ESP32-S3 SuperMini: piny łącza, IDF 6.1 i instalator EIM

Rozpiska połączeń do lutowania: [`docs/WIRING-USB.pl.md`](docs/WIRING-USB.pl.md).
Tu tylko ustalenia, które wyszły przy przygotowaniu tego montażu. **Nic z tego nie było
jeszcze uruchomione na sprzęcie** — płytki są w trakcie lutowania listew.

#### Domyślne piny łącza były błędne dla dwóch osobnych płytek, i to w obie strony

Wartości w `Kconfig.projbuild` opisują wewnętrzne połączenie płytki ESP Thread BR
(`default 24 if IDF_TARGET_ESP32H2`, `default 17 if IDF_TARGET_ESP32S3`). Na targecie
`esp32s3` rozwiązywały się więc do **TX = −1 i RX = 17**, co dla wariantów USB znaczyło:

| Wariant | Rola | Co wychodziło z domyślnych | Skutek |
|---|---|---|---|
| `s3input` | nadajnik | TX = **−1** | `uart_set_pin()` dostaje `UART_PIN_NO_CHANGE`, czyli układ **w ogóle nie nadaje** |
| `s3pad` | odbiornik | RX = **17** | GPIO17 jest na **dolnych padach** SuperMini, nie na listwie 18-pinowej |

Pierwsze to zwykły błąd, nie kwestia płytki: wariant `s3input` nie mógł działać na żadnym
sprzęcie. Wyszło z odczytania wygenerowanego `sdkconfig`, a nie z lektury Kconfiga — warto
pamiętać, że `default X if TARGET` przy dwóch rolach na **jednym** targecie nie ma jak
rozróżnić ról.

Oba warianty podają teraz piny jawnie: **GPIO4 = TX, GPIO5 = RX**, kabel jest skrzyżowaniem.
Wybór: zwykłe GPIO z listwy, bez funkcji strapping (GPIO0, 3, 45, 46), poza USB (GPIO19/20)
i poza flashem czy PSRAM (GPIO26–37).

#### Konsola zostaje na domyślnych pinach UART0, bo są na listwie

Rozpiska listwy 18-pinowej ESP32-S3 SuperMini, **potwierdzona przez właściciela na fizycznej
płytce** (obie kolumny od strony USB w dół):

| Lewa krawędź | Prawa krawędź |
|---|---|
| `TX` = GPIO43, `RX` = GPIO44, GPIO1…GPIO7 | `5V`, `GND`, `3V3`, GPIO13…GPIO8 |

Czyli domyślne piny konsoli **są** dostępne i nie ma powodu jej przenosić. Zostawienie
domyślnych daje dwie rzeczy, których `ESP_CONSOLE_UART_CUSTOM` by nas pozbawił:

- **bootloader ROM-u drukuje po tych pinach niezależnie od konfiguracji**, bo jest w krzemie —
  to test okablowania konsoli niezależny od naszego firmware'u,
- **protokół wgrywania ROM-u chodzi po tych samych pinach**, więc UART zostaje awaryjną drogą
  programowania obok USB-C.

Zanotowane, bo przez jedną turę miałem tu błędnie, że GPIO43/44 są na tej płytce tylko padami
lutowniczymi, i na tej podstawie przeniosłem konsolę na GPIO13/GPIO12. Zmiana została wycofana.
Wniosek metodologiczny: rozpiskę listwy warto potwierdzić u kogoś, kto ma płytkę w ręku, zanim
się na niej cokolwiek oprze — obrazek producenta nie rozróżnia otworu od padu.

#### Sprzęt pomocniczy: co potwierdzone, co zostaje otwarte

- **ZMIERZONE: pin `5V` SuperMini podaje 5 V na VBUS gniazda USB-C.** Sprawdzone zasileniem
  jednego ESP32 z drugiego, przez adapter OTG. Nie ma tam więc diody blokującej: host poda
  urządzeniom zasilanie i wstrzykiwanie 5 V w kabel do huba nie jest potrzebne. Wynikają
  z tego dwie rzeczy — pin `5V` i VBUS gniazda to **jedna sieć**, więc płytka pada (ta na
  USB-C w PC) dostaje z przejściówki wyłącznie RXD/TXD/GND; oraz hub **pasywny** też się
  zgłosi, bo VBUS jest, i pytanie sprowadza się wyłącznie do prądu.
- **Przejściówka: CP2102 (SiLabs), listwa `DTR/RXD/TXD/+5V/GND/3V3`, poziomy logiczne 3,3 V**
  na RX, TX i DTR (specyfikacja producenta). Zamyka to obawę o przekroczenie maksimum wejść
  S3 bez mierzenia. `DTR` zostaje niepodłączony: automatyczne wejście w bootloader wymaga
  **dwóch** linii (EN i GPIO0), a żadnej nie ma na listwie 18-pinowej — RTS jest w tym module
  dodatkowo tylko padem lutowniczym.
- **Hub: aktywny USB 3.0.** Zadziała przez swoją część 2.0, bo USB-OTG w S3 to **Full Speed**
  (12 Mbit/s). Przejściówka USB-C (wtyk) na USB-A (gniazdo) — sprawdzony adapter OTG od
  Pixela 7; linie CC nie mają znaczenia, bo gniazdo SuperMini prowadzi tylko D+, D−, VBUS
  i GND.
- **Wejścia przez odbiorniki radiowe 2,4 GHz, nie kablem od urządzeń.** Dongle nie mają
  podświetlenia i nie ładują akumulatorów, więc cały hub schodzi poniżej 200 mA i przy
  wyłączonym podświetleniu wystarczyłby nawet hub pasywny. Konsekwencja dla kodu:
  `usb_hid_host.c` rozdziela raporty po **kodzie protokołu interfejsu**, a ten jest niezerowy
  tylko dla interfejsów **boot** — dongle zwykle takie wystawia (stąd działa w BIOS-ie), ale
  część odbiorników gamingowych prowadzi szybki strumień myszy interfejsem vendorowym, a na
  boot zostawia kopię o niższym tempie. Objawem byłaby więc niższa częstość, nie brak
  działania. `USB_HID_MAX_IFACES` podniesione **4 → 8**, bo jeden dongle wystawia zwykle
  trzy–cztery interfejsy HID, czyli dwa mieszczą się w czterech tylko przypadkiem;
  przepełnienie tablicy nie zatrzymuje raportów (callback dyspozycjonuje po `params.proto`),
  ale psuje księgowanie klas, więc firmware mówi o nim teraz wprost
  (`interface table full (8) - ... not tracked`).
- **Zysk na tempie jest węższy, niż wygląda — i tu łatwo o nadużycie.** Napisałem najpierw, że
  wersja USB znosi sufit 66 Hz z §4.33; to nieprawda dla podziału S3+H2, gdzie **mysz i pad
  pracowały na 7,5 ms (133 Hz)**, czyli powyżej deklarowanych przez AJ159 Pro 125 Hz (§4.36).
  66 Hz dotyczyło **wyłącznie klawiatury**, bo tylko ona wisiała na S3, czyli na starszej
  rodzinie kontrolerów. Realny zysk wersji USB to zatem klawiatura oraz ewentualnie mysz, jeśli
  dongle da `bInterval` krótszy niż 8 ms — do zmierzenia, nie do założenia.


#### IDF 6.1 buduje wszystkie warianty po jednej poprawce przenośności

Zbudowane na `v6.1`: `s3pad` 0x3a530 B, `s3input` 0x49800 B, oraz — jako regresja — wariant
BLE na `esp32s3` 0x83400 B. Jedyna potrzebna zmiana w kodzie:

```
FAILED: chip_link.c.obj
fatal error: driver/uart.h: No such file or directory
Compilation failed because chip_link.c includes driver/uart.h,
provided by esp_driver_uart component(s).
```

W IDF 5.x meta-komponent `driver` re-eksportował sterowniki UART i GPIO; w 6.x już nie.
`firmware/main/CMakeLists.txt` wymienia je teraz wprost (`esp_driver_gpio esp_driver_uart`) —
oba nazwy istnieją od 5.3, czyli od podłogi z `idf_component.yml`, więc build na 5.5.1 to nie
psuje. `CONFIG_USB_HOST_HUBS_SUPPORTED` w 6.1 nadal istnieje i jest w `s3input` włączone
(sprawdzone w wygenerowanym `sdkconfig`, nie w Kconfigu).

#### Instalator EIM ma inny układ katalogów i psuje wszystkie skrypty `.bat`

ESP-IDF ma teraz dwa instalatory o **niezgodnych** układach:

| Instalator | Gdzie venv Pythona |
|---|---|
| `install.bat` | `%IDF_TOOLS_PATH%\python_env\idf<x.y>_py<a.b>_env` ← jedyne miejsce, w które patrzy `export.bat` |
| EIM | `%IDF_TOOLS_PATH%\python\<x.y>\venv` + własny skrypt aktywacyjny dla PowerShella |

Na instalacji EIM `export.bat` kończy się

```
ERROR: ESP-IDF Python virtual environment
"C:\Espressif\tools\python_env\idf6.1_py3.13_env\Scripts\python.exe" not found.
```

a zaraz po nim każdy skrypt `.bat` z `scripts/` wywala `'idf.py' is not recognized`.
`IDF_PYTHON_ENV_PATH` **nie pomaga** — sprawdzone, `export.bat` go nie honoruje. Niczego nie
brakuje, venv jest tylko w innym miejscu.

Rozwiązanie bez duplikowania drugiego venva: `scripts/idf-env.ps1` znajduje aktywację w
kolejności `IDF_ACTIVATE` → rejestr EIM (`eim_idf.json`, jeden wpis na wersję) →
`export.ps1` ze starego układu. Na nim stoją `scripts/build-native-win.ps1` i
`scripts/flash-win.ps1`, odwzorowujące konwencje wariantów ze skryptów `.bat`.
`monitor-win.bat` zostaje wsadem, bo potrzebuje tylko interpretera z `pyserial`, nie całego
środowiska — nauczony szukać także w układzie EIM i drukujący, którego wybrał.

Dwie pułapki warte zapamiętania, obie kosztowały po jednym nieudanym przebiegu:

- `set "IDF_TOOLS_PATH=C:\Espressif\tools" && ...` — **cytowanie jest konieczne**, bo bez
  niego `cmd` wciąga spację przed `&&` do wartości zmiennej i ścieżka w komunikacie błędu
  wygląda na poprawną (`C:\Espressif\tools \python_env\...`), a nie jest.
- W PowerShellu potok zwracający **jeden** element daje skalar, nie tablicę, więc `[0]` na
  ścieżce zwraca jej **pierwszy znak**. `flash-win.ps1` opakowuje wynik w `@( )`; objaw był
  taki: `Cannot find path 'F:\ai\...\F'`.

#### Pierwsze uruchomienie na sprzęcie: cztery rzeczy, które kosztowały czas

**1. „Nie ma portu COM” znaczyło „nie ma sterownika”.** CP2102 był w drzewie urządzeń jako
`USB\VID_10C4&PID_EA60\0001` z `ConfigManagerErrorCode = 28`, czyli „sterowniki nie są
zainstalowane”. `pnputil /enum-drivers` nie miał **ani jednego** wpisu SiLabs, więc Windows nie
miał z czego go zainstalować — trzeba było pobrać CP210x Universal Windows Driver. Warto to
sprawdzać tym kodem, a nie brakiem portu: urządzenie było widoczne i „obecne”, tylko bezużyteczne.

**2. Firmware nie startował, bo układ siedział w trybie download.** Objaw: po `Hash of data
verified` i twardym resecie **nie pojawiał się** `VID_045E&PID_028E`, a `VID_303A&PID_1001`
(USB Serial/JTAG) trwał niewzruszony. Rozstrzygnął to jeden pomiar: **`esptool --before no-reset`
połączyło się**, a to udaje się wyłącznie wtedy, gdy układ już jest w bootloaderze. Dwa
programowe resety (`esptool run` oraz impuls RTS przy DTR = 0) nie wyprowadziły go z tego stanu;
pomogło dopiero naciśnięcie RESET na płytce.

Warto zapamiętać oba testy, bo rozdzielają stany, które z zewnątrz wyglądają identycznie:

| Test | Co rozstrzyga |
|---|---|
| `esptool --before no-reset flash-id` | przechodzi → układ **jest** w trybie download; nie przechodzi → wykonuje aplikację |
| kilka próbek obecności urządzenia USB po 1,5 s | miga → pętla restartów; stabilne → nie restartuje się |

W naszym przebiegu USB Serial/JTAG był obecny w ośmiu próbkach z rzędu, co od razu wykluczyło
pętlę paniki i skierowało uwagę na GPIO0.

**3. Po starcie aplikacji port COM znika i tak ma być.** `s3pad` przejmuje GPIO19/20 przez
TinyUSB, więc bootloader ROM-u przestaje być widoczny — kolejne wgranie wymaga BOOT+RESET.
Objawia się to jako `Could not open COM4, the port is busy or doesn't exist` przy próbie
ponownego flashowania, co brzmi jak awaria, a jest dowodem, że pad działa.

**3b. `Hard resetting via RTS pin` po wgraniu NIE wyprowadza tej płytki z trybu download.**
Zmierzone dwa razy: po `Hash of data verified` konsola milczy, `VID_045E&PID_028E` się nie
pojawia, a `VID_303A&PID_1001` trwa — i `--before no-reset` łączy się, czyli układ siedzi
w bootloaderze. Ani `esptool run`, ani impuls RTS przy DTR = 0 tego nie zmieniły; pomogło
naciśnięcie RESET. Pełny cykl to więc **BOOT+RESET → wgranie → RESET**, i warto to wiedzieć,
bo objaw jest nieodróżnialny od martwego firmware'u dopóki nie zrobi się testu z tabeli wyżej.

**4. Heartbeat drukował dwie linie na tik.** Znalezione w pierwszym logu z płytki:

```
alive 190 s | heap 371088 B (min 371088 B) | pad ready | inputs kbd=0 mouse=0 | rumble 0/0
alive 190 s | heap 371088 B | pad ready
```

Blok dla ról USB i gałąź `#else` dla „braku hosta BLE” nie wykluczały się wzajemnie, więc build
USB trafiał w oba. Gałąź awaryjna ma teraz warunek `#elif !APP_USB_PAD && !APP_USB_HID_HOST`,
a na układzie wejść, który nie ma pada, zmienna `pad` jest jawnie wyciszona — bez tego build
`s3input` sypał ostrzeżeniem o nieużywanej zmiennej.

#### Dongle 2,4 GHz: interfejsy odczytane z drzewa urządzeń Windows

Pytanie „czy dongle wystawiają interfejsy **boot**, których nasz host wymaga” dało się
rozstrzygnąć **bez** podłączania czegokolwiek do ESP — wystarczyło przeczytać `CompatibleID`
dzieci urządzenia kompozytowego:

| Dongle | Interfejs | SubClass / Prot | Znaczenie |
|---|---|---|---|
| AULA `3554:FA09` | MI00 | 01 / 01 | boot keyboard |
| | MI01 | 01 / 02 | boot mouse (warstwa Fn, §4.16) |
| AJAZZ `3151:402D` | MI00 | 01 / 02 | boot mouse |
| | MI01 | 01 / 01 | boot keyboard (§4.36 — AJ159 deklaruje klawiaturę) |
| | MI02 | 00 / 00 | vendorowy, nasz host go pomija |

Trzy wnioski. **Plan działa**: `proto 1` i `proto 2` są obecne. **Pięć interfejsów HID razem**,
czyli stary limit `USB_HID_MAX_IFACES = 4` przelałby się — podniesienie do 8 było koniecznością,
nie ostrożnością. I trzeci, nowy: **każdy dongle wystawia obie klasy**, więc host zobaczy po dwa
interfejsy klawiatury i myszy. Stan klawiatury jest absolutny i nadpisywany, więc raport
z interfejsu klawiatury *myszy* może na chwilę zwolnić trzymany klawisz. Bezczynny interfejs HID
nic nie nadaje, więc w praktyce może to nigdy nie wystąpić — ale to jest objaw do wypatrywania,
a nie do szukania potem w mapperze.

#### Najważniejszy błąd tego uruchomienia: mapper na padzie USB nigdy nie startował

Objaw był mylący, bo **wszystko po drodze działało**: pad się wyliczał, Windows wiązał `xusb22`,
`XInputGetState` widział go w slocie 0, wibracje z hosta dochodziły, płytka wejść czytała
klawiaturę i mysz i raportowała `sent N frames (dropped 0)`. A osie i przyciski pada stały
w zerze.

Przyczyna: warunek startu w `app_main.c` **nie odzwierciedlał** warunku kompilacji
w `main/CMakeLists.txt`.

```
CMakeLists.txt:  (APP_ENABLE_HID_HOST AND APP_ENABLE_GAMEPAD) OR APP_USB_PAD   -> kompiluj
app_main.c:       APP_ENABLE_HID_HOST && APP_ENABLE_GAMEPAD                    -> startuj
```

Czyli w wariancie `s3pad`, gdzie obie role BLE są wyłączone, `input_mapper.c` był kompilowany,
ale `input_mapper_start()` nigdy nie było wołane — nie istniało zadanie, które zamienia stan
wejść na raporty pada. Warunek jest teraz wspólny dla obu miejsc; przy okazji trzeba było
poprawić także warunek `#include "input_mapper.h"`, bo bez prototypu build w `gnu23` jest
błędem, a nie ostrzeżeniem.

Potwierdzenie, że poprawka trafia w sedno, jest widoczne w rozmiarze binarki: `s3pad` urósł
z `0x3a4e0` na `0x3ad00`, czyli o ~2 kB. Przy `-ffunction-sections` i `--gc-sections` linker
wyrzucał wcześniej cały mapper jako nieosiągalny — to jest ta różnica.

Wniosek do zapamiętania: **dwa warunki opisujące to samo są jednym błędem czekającym na
okazję.** Tu okazją było pierwsze uruchomienie na sprzęcie, bo build nie miał jak tego wykryć —
kod się kompilował, linkował i uruchamiał, tylko jedna funkcja nie była wołana. Gdyby
`input_mapper_start()` było wołane bezwarunkowo z pustą implementacją dla nieużywanych
konfiguracji, kompilator złapałby niezgodność sam.

#### Pułapka: Windows Terminal traktuje pada jak klawiaturę, i to szerzej niż tylko gałka

Warto to wiedzieć, zanim się na tym zbuduje wniosek. **Konsola Windows reaguje na pada**, i nie
chodzi wyłącznie o osie. Zmierzone w praktyce, na tym mostku i potwierdzone właścicielem prawdziwym
padem Xbox Series X, czyli niezależnie od naszego sprzętu:

| Wejście pada | Co robi konsola |
|---|---|
| lewa gałka | jak klawisze strzałek |
| `A` | jak Enter — **da się tym zamknąć PowerShella** |
| `B` | jak Esc / „wstecz" |

**To kosztowało utratę przebiegu testowego, i warto wiedzieć dlaczego.** Właściciel zgłosił, że po
`Ctrl+Alt+G` „Backspace zadziałało jak Esc", co wygląda na off-by-one w deskryptorze klawiatury
passthrough (Backspace to `0x2A`, Esc `0x29` — różnica jednego, więc hipoteza jest kusząca). Sam ją
postawiłem i **była błędna.** Prawdziwy mechanizm nie ma z Backspace nic wspólnego:

- `LCtrl` jest w domyślnej tablicy zmapowany na nominalny przycisk 6, czyli **`B`** w profilu Xbox,
- skrót wymaga trzymania `Ctrl`, więc przez cały czas jego trzymania pad **wysyła `B`**,
- konsola czyta `B` jako „wstecz", co wygląda jak Esc.

Backspace jest niezmapowany i nie robi nic. Deskryptor passthrough jest w porządku — używa
standardowego makra `TUD_HID_REPORT_DESC_KEYBOARD` z TinyUSB, a `tud_hid_keyboard_report()` sam
wstawia bajt `reserved`, więc nie ma gdzie przesunąć wartości.

Wnioski praktyczne, oba operacyjne:

1. **Nie oceniaj po konsoli, kto obsługuje klawiaturę** — ani co wysyła mostek. Rozstrzygają dwa
   logi czytane jednocześnie albo `XInputGetState`.
2. **Nie trzymaj terminala aktywnego, testując skróty w trybie pada.** Każdy skrót zaczyna się od
   `Ctrl`, `Ctrl` to `B`, a `B` zamyka rzeczy. Kliknij najpierw w cokolwiek innego.

Metodologicznie to trzeci raz, gdy objaw po stronie PC wyglądał na wadę firmware'u i nią nie był —
po §4.32 (generyczny sterownik zamiast XInput, wina wyboru PID) i po pierwszym przebiegu
end-to-end, gdzie ruch kursora w Terminalu wyglądał na dowód, że klawiatura należy do Windows,
a był dowodem odwrotnym.

Konsekwencja dla testów tego projektu jest zwodnicza. Mostek mapuje WASD na lewą gałkę, więc
naciśnięcie `W` na klawiaturze obsługiwanej przez ESP **poruszy kursorem w Terminalu** — a to
wygląda dokładnie tak, jakby klawiatura nadal należała do Windows i nic przez mostek nie
przechodziło. Jest odwrotnie: to dowód, że cały łańcuch działa.

Praktyczna reguła: **nie oceniaj po Terminalu, kto obsługuje klawiaturę.** Rozstrzygają dwa
logi czytane jednocześnie albo `XInputGetState`. U nas rozstrzygnęło pierwsze: w chwili, gdy
w konsoli płytki wejść pojawiło się `KBD report len=8 [00 00 1a 00]` (0x1a = `w`), pad w PC
pokazał `L=(0,32767)`.

Pierwszy przebieg testu end-to-end pokazał przy okazji ruch prawego analoga i zero na lewym,
czego przyczyny nie ustaliliśmy — w drugim przebiegu, z tą samą konfiguracją, lewy analog
wychylał się poprawnie. Odnotowane jako nierozstrzygnięte, żeby nie wpisywać domysłu w miejsce
pomiaru.

#### Pad aktualizował się 97 Hz, choć endpoint jest odpytywany co 4 ms

Zmierzone od strony PC, `scripts/xinput_rumble.py --rate`, przy nieprzerwanym ruchu myszą:

| Stan | Zmierzone tempo |
|---|---|
| `CONFIG_FREERTOS_HZ=100` (domyślny) | **97 Hz** |
| `CONFIG_FREERTOS_HZ=1000` | **243 Hz** |

Przyczyna: `pdMS_TO_TICKS()` zaokrągla **w dół** do całych tików. Przy tiku 100 Hz okres 4 ms,
o który prosi `APP_REPORT_RATE_HZ=250`, wychodzi **zero tików**; `input_mapper.c` ma tam
`period > 0 ? period : 1`, więc brał jeden tik, czyli 10 ms. Liczba w menuconfig była
nieosiągalna i nikt o tym nie wiedział, bo nic tego nie zgłaszało — a pad chodził **niżej niż
w wersji BLE** (133 Hz), mimo że USB miało być pod tym względem lepsze.

Dwie zmiany. `sdkconfig.defaults.s3pad` ustawia `CONFIG_FREERTOS_HZ=1000`, przy którym 4 ms jest
wyrażalne dokładnie i zostaje miejsce na 1 ms, gdyby passthrough tego kiedyś potrzebował. Oraz
mapper **loguje tempo, które faktycznie osiąga**, i ostrzega, gdy żądane jest nieosiągalne:

```
mapping task at 250 Hz (FreeRTOS tick 1000 Hz)
```

Metodologicznie warto zapamiętać, jak to wyszło: pomiar musiał być zrobiony **narzędziem
szybszym od mierzonego zjawiska**. Pierwszy podgląd (`--watch`) sypiał 10 ms na iterację, więc
sam ograniczał się do ~100 Hz i pokazywał 97 Hz jako sufit — czyli dokładnie tę wartość, którą
mierzył błędnie. Tryb `--rate` nie śpi wcale (415 000 odpytań na sekundę) i dopiero on rozdzielił
„pad daje 97 Hz” od „mój licznik daje 97 Hz”. To trzeci raz w tym projekcie, gdy narzędzie
pomiarowe kłamało spójnie i dlatego wiarygodnie — po `APP_DEBUG_SCAN_ONLY` i po drabince
interwałów z §4.33.

#### Otwarta decyzja: nasza kopia `esp_hid` przysłania naprawioną wersję z 6.1

Dotyczy tylko ról BLE, ale trzeba to wiedzieć przed jakimkolwiek buildem BLE na 6.1.
`firmware/components/esp_hid/` jest przypięte do 5.5.1 (`nimble_hidh.c`, 1178 linii) i
nadpisuje wersję z IDF 6.1 (**1295 linii**), która ma już naprawy §4.27
(`services_discovered = 0`), §4.29 (`ble_gap_security_initiate`) i §4.25
(`dev->connected = true`) — sprawdzone grepem w drzewie 6.1, zgodnie z tym, co §4.35
przewidywał dla 6.0.2. Kompiluje się bez zmian, ale znaczy to, że **na 6.1 tracimy naprawy
upstreamu**, zachowując w zamian nasze własne dodatki (limit próby 6 s zamiast 30 s,
budzenie otwierającego przy śmierci linku, log CCCD na INFO). Wersja USB tego nie dotyka —
`esp_hid` jest tam linkowane, ale nieużywane.

Przy okazji: `scripts/check_local_esp_hid.py` patrzy tylko w `build.esp32c3` i
`build.win.esp32c3`, więc dla innych targetów i wariantów nie odpowie na pytanie, czyja kopia
weszła do builda.


### 4.40 Pad USB na 1 kHz: co naprawdę ograniczało tempo (branch `usb-1khz`)

Pytanie brzmiało „czy da się 1 kHz". Odpowiedź: **da się, transport robi 997 Hz, a z realną myszą
wychodzi 830 Hz** — ale zysk przyszedł z arytmetyki w mapperze, nie z podniesienia interwału USB.
Po drodze wyszły trzy błędy i jedna lekcja metodyczna, którą złamałem trzy razy.

#### 8 kHz jest fizycznie nieosiągalne, 1 kHz to sufit

USB-OTG w ESP32-S3 to **Full Speed**, gdzie najkrótszy interwał endpointu interrupt wynosi 1 ms.
8 kHz, które deklaruje mysz, wymaga High Speed z mikroramkami 125 µs. Więc 1 kHz jest zarazem
celem i granicą, niezależnie od tego, co potrafi urządzenie wejściowe.

#### Trzy błędy, wszystkie w przeliczaniu, żaden w USB

**1. Stała czasowa filtra i czułość były wyrażone w TIKACH, nie w czasie.** `EMA_SHIFT 3` znaczyło
8 tików, a pełne wychylenie `div * 4` zliczeń **na tik**. Podniesienie tiku z 100 Hz na 1000 Hz
zmieniłoby więc filtr z 80 ms na 8 ms i czułość dziesięciokrotnie — a zmiana na 250 Hz, zrobiona
kilka godzin wcześniej, już po cichu przestawiła jedno i drugie 2,5×. Oba parametry są teraz
wyrażone w czasie (`MOUSE_TAU_MS`, zliczenia na **sekundę**) i wyliczane z tempa przy kompilacji,
więc przy 100 Hz dają dokładnie te same liczby co wersja dobrana ręcznie w §4.22.

**2. `EMA_FRAC` 256 było za małe przy dużej liczbie tików.** Krok filtra to
`(sample * EMA_FRAC - ema) / EMA_TICKS` w arytmetyce całkowitej, więc gdy różnica spadnie poniżej
`EMA_TICKS`, przyrost obcina się do zera i **filtr utyka**. Przy 1 kHz stała czasowa to 80 tików,
czyli zostawały trzy jednostki zapasu i powolny ruch przestawał się liczyć. Teraz 4096.

**3. Najważniejszy: `ema_step()` zwracał CAŁKOWITE zliczenia na tik.** Przy 1 kHz średni przyrost
to około jedno zliczenie, więc po obcięciu zostawały trzy poziomy — 0, 1, 2 — a potem mnożone
przez `127/9`. Cała rozdzielczość ginęła przed dojściem do osi. Ścieżka do osi liczy się teraz
w jednym wyrażeniu w `int64`, bez pośrednich obcięć.

Zmierzone skutki, przy tym samym rodzaju ruchu:

| Konfiguracja | Zmierzone |
|---|---|
| 1 ms / 1000 Hz, `EMA_FRAC` 256, obcinanie | 124 Hz |
| 1 ms / 1000 Hz, `EMA_FRAC` 4096, obcinanie | 144 Hz |
| 1 ms / 1000 Hz, bez obcinania | **466 Hz** |

#### Co jest ustalone bezspornie, a co tylko poglądowo

**Bezspornie**, bo pomiar nie zależy od ręki: `APP_DEBUG_PAD_RATE_PROBE` wymusza inny raport
w każdym tiku i daje **997 Hz**. Czyli endpoint 1 ms, zadanie 1000 Hz i `xusb22` przenoszą
praktycznie równy tysiąc pakietów na sekundę. Osobno, z arytmetyki: opóźnienie dodane przez
mostek spada z ≤8 ms (4 ms okres zadania + 4 ms odpytywanie) na **≤2 ms**.

**Poglądowo**, bo zależy od ruchu ręki: z realną myszą wyszło 830 Hz przy energicznym ruchu
i 70 Hz przy powolnym. Niska liczba przy powolnym ruchu **nie jest usterką** — filtr z założenia
utrzymuje stałe wychylenie przy stałej prędkości, a raport idzie tylko na zmianie stanu, więc
brak zmian znaczy „nie ma czego wysyłać".

Hipoteza, którą postawiłem i którą pomiar **odrzucił**: że 8-bitowe osie (127 kroków) ograniczają
tempo, bo powolniejsza gałka przechodzi mniej granic kroku. Przy energicznym ruchu i mniejszej
czułości wyszło 830 Hz, czyli 83 % sufitu — wartość zmienia się prawie w każdym tiku i tak.
Poszerzenie osi do `int16` nie jest więc potrzebne dla tempa; zostaje jako ewentualna poprawa
**precyzji** przy małych wychyleniach, gdzie kroków jest mało.

#### Lekcja metodyczna: trzy razy porównałem przebiegi o różnym ruchu ręki

Liczby 243, 153, 144, 466, 70 i 830 Hz zbierałem przy różnym machaniu myszą i trzy razy
wyciągnąłem z nich wniosek, którego nie wolno było wyciągnąć. Za każdym razem korekta przyszła
od właściciela: „machałem mniej", „ruszałem powoli". Miernik zależny od operatora nie służy do
porównywania konfiguracji — do tego jest przyrząd wymuszający zmianę stanu, i dopiero on dał
liczbę, na której można stać.

To czwarty raz w tym projekcie, gdy narzędzie pomiarowe kłamało spójnie i dlatego wiarygodnie:
po `APP_DEBUG_SCAN_ONLY`, drabince interwałów z §4.33 i trybie `--watch`, który sam siebie
ograniczał do 100 Hz.

#### Czułość trzeba było zmniejszyć, i to nie jest przypadek

Po usunięciu obcinania gałka dobijała do maksimum przy szarpnięciu myszą, bo pełne wychylenie
przy `div=24` to 9 600 zliczeń na sekundę — jeden ruch nadgarstkiem na myszy o dużym DPI.
`sdkconfig.defaults.s3pad` ustawia `APP_MOUSE_SCALE_DIV=64`, czyli 25 600 zliczeń/s; właściciel
potwierdził, że wtedy „nie dobija od razu do full wychylenia". Zakres opcji poszerzony do 512, bo
użyteczna wartość zależy od DPI myszy, czego firmware nie zna. Domyślna wartość dla builda BLE
zostaje 24 — tam tempo to 100 Hz i nic się nie zmieniło.

#### Stan weryfikacji

Zmierzone na sprzęcie: transport 997 Hz, realna mysz 830 Hz, czułość oceniona przez właściciela.
**Nie zweryfikowane:** wariant BLE po tych zmianach. `input_mapper.c` jest wspólny, a przy 100 Hz
nominalna czułość i stała czasowa wychodzą identyczne jak przed zmianą — ale rozdzielczość jest
teraz pełna, więc drobne ruchy będą się liczyć wyraźniej niż dotąd. Przed scaleniem do `main`
warto przejechać jeden przebieg na C3.

### 4.41 Panel konfiguracyjny po Wi-Fi (branch `usb-webui`)

Panel WWW na układzie pada: czułość, wygładzanie, kompensacja martwej strefy, mapowanie, cztery
profile w NVS, wizualizacja pada i aktualizacja firmware'u. **Zbudowane, nieuruchomione na
sprzęcie** — płytki nie były podłączone. Ta sekcja to decyzje projektowe i pięć pułapek, z których
trzy są przenośne poza ten projekt.

#### Wi-Fi jest wyłączone do skrótu, i to jest cała idea

Nie oszczędzanie prądu. Ten układ prowadzi zadanie 1 kHz karmiące endpoint odpytywany co
milisekundę, a zmierzone **997 Hz pochodzi z przebiegu z milczącym radiem**. Stale włączone Wi-Fi
wstawiłoby niezmierzony koszt pod każdy przyszły pomiar tempa. Podnoszone na czas konfiguracji
zostawia tę liczbę sensowną.

`Ctrl+Alt+W` włącza i wyłącza, `Ctrl+Alt+P` wymusza własny AP. Skrót widzi układ **wejść** (tam jest
klawiatura), a Wi-Fi stoi na układzie **pada** (tam jest mapper i nastawy), więc `APP_WEBUI` włącza
się na obu — dokładnie jak `APP_USB_PASSTHROUGH`.

#### PUŁAPKA PRZENOŚNA 1: warunkowe `REQUIRES` w CMake nie może działać

To najważniejsze ustalenie tej tury i dotyczy każdego projektu ESP-IDF. Plik `CMakeLists.txt`
komponentu jest przetwarzany **dwa razy**, a konfiguracja istnieje tylko w jednym przebiegu.
Zmierzone sondą `message(STATUS)`:

```
-- SONDA: APP_WEBUI=[]  USB_PAD=[]  faza=[__component_get_requirements]
-- SONDA: APP_WEBUI=[y] USB_PAD=[y] faza=[]
```

Pierwszy przebieg zbiera `REQUIRES`. Czyli **warunkowe `SRCS` działają, a warunkowe `REQUIRES`
cicho nie** — klasyczny kształt wady w tym projekcie: konfiguruje się, kompiluje, linkuje, zgłasza
sukces i robi coś innego.

Ten projekt miał to od początku i nie zauważył: `list(APPEND reqs bt)` w warunku BLE i
`list(APPEND reqs usb)` w warunku USB **nigdy nic nie robiły**. Działało, bo nagłówki NimBLE
docierają przechodnio przez `esp_hid`, a `usb` przez zarządzany `usb_host_hid`. Zależności panelu
takiej przypadkowej ścieżki nie mają, więc padły wprost na `esp_app_desc.h: No such file or
directory`.

**Co jest dostępne w tej fazie: `IDF_TARGET`** — na nim opiera się `idf_component.yml`, więc na nim
opiera się teraz i `main/CMakeLists.txt`. Bramkowanie po targecie jest tu zresztą konieczne
niezależnie: komponent `usb` **nie istnieje** na C3, więc lista bezwarunkowa wywala build z
`Failed to resolve component 'usb'`. Target i konfiguracja zgadzają się w tym przypadku — tylko
układy z USB-OTG mogą być padem USB, i tylko pad USB prowadzi panel.

Koszt wymienienia komponentu w wariancie, który go nie używa, to **czas kompilacji, nie rozmiar
obrazu**: `--gc-sections` wyrzuca to, co nieosiągalne. Zmierzone: C3 urósł o 224 B, i to od
walidacji nazw profilu, nie od Wi-Fi.

#### PUŁAPKA PRZENOŚNA 2: stan absolutny na drucie kontra decyzja lokalna

Ramka `MODE` niesie stan absolutny powtarzany z każdym keepalive — to robi ją odporną na zgubienie.
Ale układ wejść **nie ma pojęcia**, że pad wygasił Wi-Fi po bezczynności, więc powtarza „panel
włączony" cztery razy na sekundę i samo wygaszenie zostałoby odwrócone w 250 ms. W nieskończoność.
Łącze jest jednokierunkowe, więc nie da się odpowiedzieć „skasuj bit".

Rozstrzygnięcie: drut wygrywa znowu, gdy **zmienią się bity**, czyli gdy użytkownik coś zrobi.
Praktycznie: po wygaśnięciu `Ctrl+Alt+W` trzeba wcisnąć dwa razy, a `Ctrl+Alt+P` wraca jednym, bo
zmienia bit AP. Ta sama poprawka objęła drugą oscylację: nieudany start Wi-Fi zostawiał „chcę
sieci" prawdą, więc zadanie próbowałoby cztery razy na sekundę do końca sesji.

#### PUŁAPKA PRZENOŚNA 3: prawdziwa PWA po HTTP w LAN jest niemożliwa

Sprawdzone w regule platformy, nie założone. **Service worker wymaga bezpiecznego kontekstu**;
wyjątkiem jest tylko `localhost` i `127.x`, a `http://192.168.4.1` nim nie jest. Bez service workera
nie ma cache'u offline ani promptu instalacji w Chrome na Androidzie. Certyfikat self-signed **nie
pomaga** — service worker i tak się nie zarejestruje przy niezaufanym.

Właściciel po przedstawieniu tego zrezygnował z PWA, i słusznie: cache offline jest tu bezcelowy,
bo aplikacja bez osiągalnego ESP i tak jest bezużyteczna. Panel jest więc responsywny i mobile-first,
bez manifestu i bez obietnicy offline.

#### Trzy decyzje, które warto znać

- **Brak parsera JSON na urządzeniu.** cJSON wyszedł z ESP-IDF w 6.x (potwierdzone w migracji
  Espressifu: „The built-in json component has been removed… migrate to `espressif/cjson`"), ale
  lepszym argumentem jest to, że potrzebujemy JSON tylko **produkować**. Żądania przychodzą
  form-encoded, co `esp_http_server` już rozdziela, a jedyne miejsce wymagające prawdziwego parsera
  — import pliku profilu — robi przeglądarka, gdzie `JSON.parse` jest darmowy i nie może zepsuć nic
  po naszej stronie. Powierzchnia parsowania to dwie małe funkcje, nie parser ogólny osiągalny z
  sieci.
- **Strona wbudowana w obraz, nieskompresowana.** Jedno żądanie nie może rozjechać strony ze
  skryptem, nie ma partycji plików do uszkodzenia, a aktualizacja wymienia panel i API razem.
- **Nazwa profilu jest redukowana do bezpiecznego alfabetu** w walidacji, *oraz* escapowana przy
  wypisywaniu JSON. Dwie obrony dla jednej własności są tu słuszne: ten ciąg przychodzi z sieci,
  jest zapisywany, wraca w odpowiedzi i jest renderowany przez panel.

#### Znowu 4.39: wybór partycji nie wszedł z defaults

`CONFIG_PARTITION_TABLE_CUSTOM` dopisane do `sdkconfig.defaults.s3pad` **nic nie zmieniło**, bo
wybór partycji już był w wygenerowanym `sdkconfig.win.esp32s3.s3pad`, a wartość obecna wygrywa z
defaults. Build się udał, OTA było nieobecne, a jedynym objawem byłby komunikat panelu „no spare OTA
partition". Wykryte odczytaniem **wygenerowanego** pliku, nie założeniem — to jest cała reguła.

Dlatego `webui_start()` loguje teraz ostrzeżenie przy starcie, gdy `esp_ota_get_next_update_partition()`
zwraca NULL. Cicha wada nie przejdzie drugi raz.

Przy okazji: **`partitions_two_ota.csv` z IDF marnuje megabajt** — deklaruje trzy partycje
aplikacji (`factory` + dwa sloty OTA) po 1 MB na układzie 4 MB, a `factory` przy OTA nie jest
używane. Obrazowi zostawało 10 % zapasu. Własna `firmware/partitions.csv` ma dwa sloty po 1856 kB
bez `factory` (bootloader startuje `ota_0`, gdy `otadata` jest czyste), co daje **51 % zapasu** i
320 kB wolnego flasha.

#### Zjawisko zmierzone przy okazji: filtr EMA zaniża wychylenie

Całkowitoliczbowy EMA zatrzymuje się `ticks-1` przed celem, bo dzielenie obcina. Zaniżenie wynosi
`(ticks-1)*127*rate/(div*400*4096)`:

| Tempo | `div` | `ticks` | Zaniżenie |
|---|---|---|---|
| 100 Hz | 24 | 8 | ≤ 0,002 jednostki osi |
| 1000 Hz | 24 | 80 | ≤ 0,255 |
| 1000 Hz | **64** (wariant pada) | 80 | **≤ 0,096** |

Czyli niecała dziesiąta kroku osi w konfiguracji, którą wgrywamy. Zaokrąglanie zamiast obcinania by
to usunęło i **świadomie tego nie robię**: zmierzone 997 Hz i 830 Hz pochodzą z tej arytmetyki.
Granica jest policzona i zapisana w `scripts/check_mapper_math.py`.

#### Sprawdzacz arytmetyki dwa razy kłamał — czwarty i piąty raz w tym projekcie

`scripts/check_mapper_math.py` powstał, bo arytmetyka myszy była błędna trzy razy (§4.40), a płytek
nie było pod ręką. Dwa razy zgłosił błąd i **dwa razy błąd był w nim**:

1. Twierdził, że filtr utyka przy 100 Hz, gdzie jeden count na tik to zwyczajnie 1 % pełnego
   wychylenia. Asercja była bez sensu, nie firmware.
2. Twierdził, że czułość zależy od tempa zadania, bo dzielił prędkość całkowicie: przy 400 counts/s
   i 250 Hz podawał faktycznie 250 counts/s i porównywał z przebiegiem przy 400.

Po poprawkach sprawdza to, co ma znaczenie: że ta sama prędkość myszy daje to samo wychylenie przy
100, 250 i 1000 Hz (z tolerancją jednej jednostki, uzasadnioną wyżej), że filtr wraca do środka, że
stała czasowa jest czasem, i że kompensacja martwej strefy jest monotoniczna, symetryczna i nie
przestrzeliwuje na skosach. Dzielenie w modelu obcina **w stronę zera**, jak w C — pomyłka w tym
miejscu zmienia zachowanie tylko dla ujemnych przyrostów, czyli niewykrywalnie wyczuciem.

#### Stan weryfikacji: PRZEJECHANE NA SPRZĘCIE W CAŁOŚCI

Zbudowane: sześć konfiguracji, zero ostrzeżeń. Pad `0xe5f20` (941 kB) w slocie 1856 kB, 50 % wolne.

Zmierzone przez WiFi, z komputera podłączonego najpierw do AP mostka, potem po kablu do tej samej
sieci:

| Co | Dowód |
|---|---|
| strona i uwierzytelnianie | 32 462 B, HTTP 200; `/api/state` bez hasła → **401**, z hasłem → pełny stan |
| domyślne przypisania | 20, zgodne co do wiersza: `1:4→1` (A → lewa gałka w lewo), `1:26→3` (W → w górę) |
| częściowa aktualizacja nie wymazuje reszty | po wysłaniu samego `div_x` zostało `div_y=64` i `binds=20` |
| przycinanie wartości | `div_x` 9999→512, `div_y` 0→1, `tau` 1→5, `anti_dz` 200→80 |
| sanityzacja nazwy | `a"<b>c\d;e` → `a__b_c_d_e` |
| profile | zapis do slotu 2, reset do domyślnych, odczyt slotu 2 z powrotem wszystkich pól |
| **trwałość NVS przez restart** | po OTA wczytał się aktywny slot: `apex`, div 96/72, tau 60, anti_dz 18 |
| eksport | 707 B z `Content-Disposition` |
| **OTA** | trzy obiegi, każdy `HTTP 200`, każdy wstał |
| **AP wraca sam po restarcie pada** | bez naciskania skrótu — układ wejść powtarza bit, pad podnosi go z łącza w ciągu keepalive |
| **dołączenie do sieci zewnętrznej** | `net=station`, `http://192.168.1.32`, znalezione w LAN po MAC `90:da:72:49:a3:28` |

**POMIAR, KTÓRY USPRAWIEDLIWIA CAŁY PROJEKT „WI-FI NA ŻĄDANIE".** Tempo pętli mapującej, licznik
`ticks` w oknie liczonym z `uptime_ms`:

| Stan | Zmierzone |
|---|---|
| AP w górze, panel odpytywany | **1000,0 Hz** (6105 przebiegów w 6,105 s) |
| w trybie klienta, w sieci domowej | **1000 Hz** (okno 8,191 s) |

Czyli panel nie podkrada zadaniu 1 kHz **nic mierzalnego**, w żadnym trybie sieci. Heap w tym
stanie 242–248 kB wolne, więc Wi-Fi z serwerem kosztuje ~125 kB z ~371 kB, które ma pad bez radia.

#### Wycofanie OTA: udowodnione, nie zaprojektowane

Ostatnia nieudowodniona obietnica tej funkcji. Obraz z `abort()` na początku `app_main`:
**kryptograficznie poprawny**, więc przechodzi `esp_ota_end()` i faktycznie się uruchamia, ale
panikuje natychmiast. To istotne — uszkodzenie bajtów zostałoby odrzucone przy zapisie i
powiedziałoby coś o sumie kontrolnej, a nic o rollbacku.

Obraz wyszedł 172 kB zamiast 941 kB, bo `abort()` jest `noreturn`, kompilator uznał resztę
`app_main` za nieosiągalną, a linker ją wyrzucił. To **wzmacnia** dowód: w zepsutym obrazie nie ma
ani Wi-Fi, ani serwera, więc odpowiadający panel nie może być nim.

Łańcuch: `HTTP 200 {"ok":true,"rebooting":true}` znaczy, że `esp_ota_end()` przyjął obraz i
`esp_ota_set_boot_partition()` go ustawił, czyli **został uruchomiony**. Po 87 s panel odpowiada,
ma pole `sta_ssid`, `pad_ready=True`, `ticks=86425` w 87 s. Przyjęty, uruchomiony, wywalił się,
bootloader wrócił do poprzedniego — nikt niczego nie dotykał.

Wymuszony AP (`Ctrl+Alt+P`) przeżywa restarty pada, bo bit siedzi na układzie wejść, i **to właśnie
umożliwiło bezpieczne wykonanie tego testu**: gdyby mostek po restarcie dołączał do sieci domowej,
nie byłoby jak stwierdzić, czy wycofanie zadziałało, czy tylko zniknął z widoku.

#### Trzy wady znalezione dopiero przez uruchomienie, wszystkie ciche

Żadnej nie mógł złapać kompilator i każda milczała, co jest wspólnym mianownikiem: **odrzucone pole
formularza jest nie do odróżnienia od nieobecnego**, więc objaw brzmi „panel ignoruje to okienko"
i nie wskazuje nigdzie blisko przyczyny.

1. **Licznik raportów mierzył co innego, niż obiecywał.** `usb_pad_send()` zwraca `true` także gdy
   stan się nie zmienił i nic nie poszło na szynę, więc zliczanie jego wyniku dawało tempo
   **zadania**, nie raportów. Panel pokazywał 990 „raportów/s" na bezczynnym padzie, którego
   `dwPacketNumber` w XInput w ogóle się nie ruszał. Szósty raz w tym projekcie, gdy przyrząd kłamał
   spójnie. Teraz są dwa liczniki o rozdzielnych znaczeniach, a potwierdzenie naprawy jest najlepsze
   z możliwych: na bezczynnym padzie `ticks=50375, reports=0`.

2. **Bufor pola musiał pomieścić postać zakodowaną procentowo, nie zdekodowaną.**
   `httpd_query_key_value()` kopiuje wartość wciąż zakodowaną i zawodzi, gdy się nie mieści, a każdy
   bajt może stać się trzema znakami. Zmierzone: nazwa `abc def` (9 zakodowanych) zapisywała się,
   a `aa bb cc dd ee` (22 zakodowane) przepadała bez słowa — przy limicie 15 znaków w panelu znaczy
   to, że zwyczajna nazwa z dwiema spacjami nic nie robiła.

3. **Naprawiając (2), wprowadziłem trzecią:** wartownik `out_size * 3 >= sizeof(scratch)` wygląda na
   ostrożny i po cichu wyłączył **największe pole** — tablica przypisań ma cel 560 bajtów, więc
   `1680 >= 256` odrzucało ją w całości i edytor mapowania nie robił nic. Wykryte jednym testem:
   wysłałem cztery przypisania, wróciło dwadzieścia.

Naprawa: scratch mieści to, co klient może legalnie przysłać (720 znaków dla tablicy przypisań),
a cokolwiek dłuższego jest odrzucane **z logiem**. To ostatnie jest ważniejsze od samego rozmiaru.

#### Dane sieci były zapisywane poprawnie, ale nie stosowane

Właściciel zgłosił, że wpisał hasło i mostek nie dołączył. Zapis działał — odczyt z NVS pokazał
`PLAY_Swiatlowod_D4C0`. Dane były jednak czytane **wyłącznie przy podnoszeniu interfejsu**, więc
zapisanie ich przy stojącym AP nic nie zmieniało do następnego skrótu. „Wpisałem hasło i nie
dołączył" było trafnym opisem. `/api/wifi` restartuje teraz sieć, z restartem odłożonym do zadania
kontrolnego, żeby odpowiedź HTTP zdążyła dojść przed zniknięciem sieci, którą przyszła.

Przy okazji wyjaśniła się obserwacja wyglądająca na usterkę: po resecie układu **wejść** panel nie
wraca. Poprawnie — bit „panel włączony" żyje tam i jest powtarzany z keepalive, więc jego reset
zeruje maskę i pad słusznie trzyma radio wyłączone.

I jeszcze jedno, czego lista DHCP routera nie pokazała, a skan pokazał: mostek **był** w sieci pod
`192.168.1.32`. Adres klienta używa **bazowego** MAC (`…a3:28`), a AP inkrementowanego (`…a3:29`) —
warto wiedzieć, szukając go w tablicy ARP.

### 4.39 Passthrough na skrót: dwie tożsamości USB, nie jedno urządzenie złożone

Skrót `Ctrl+Alt+G` przełącza układ pada między padem XInput a zwykłą klawiaturą i myszą HID,
żeby te same urządzenia dały się używać do pisania bez odłączania czegokolwiek.

#### Dlaczego dwie tożsamości, a nie jedno urządzenie z trzema interfejsami

To jest rozstrzygające ustalenie tej funkcji i wynika wprost z tego, jak Windows wiąże `xusb22`:
**na poziomie URZĄDZENIA, nie interfejsu.** Prawdziwy pad Xbox 360 ma cztery interfejsy i
sterownik bierze je wszystkie; widać to w naszym własnym drzewie, gdzie `xusb22` siedzi na
`USB\VID_045E&PID_028E`, a dziecko `IG_00` dostaje `HidUsb` od niego, a nie od hosta USB.

Gdybyśmy więc dopisali interfejsy klawiatury i myszy pod tym samym VID/PID, `xusb22` zagarnąłby
je razem z padem i Windows nigdy nie zobaczyłby urządzeń wejściowych. Dlatego układ **odłącza
się i wylicza od nowa** z innym zestawem deskryptorów:

| Tryb | Tożsamość | Co widzi Windows |
|---|---|---|
| gamepad | `045E:028E` | interfejs vendorowy XInput, `Service=xusb22` |
| passthrough | `303A:4004` | jeden interfejs HID, dwa report ID → `kbdhid` + `mouhid` |

Kosztem jest znikanie pada na czas passthrough — właściciel uznał to za akceptowalne, bo
w większości gier nie przeszkadza. `0x303A` to VID Espressifu, a `0x4004` to wartość, którą sam
`esp_tinyusb` wylicza dla urządzenia wyłącznie HID (`0x4000` z bitem klasy HID), więc nie
squattujemy na cudzym numerze produktu.

#### Trzy decyzje projektowe warte zapisania

- **Tryb jest stanem absolutnym na drucie, nie komendą „przełącz".** Ramka `0x04 MODE` idzie
  natychmiast po skrócie i jest **powtarzana z każdym keepalive**. Zgubiona ramka albo reset
  jednej płytki naprawia się w 250 ms, zamiast zostawić układy niezgodne co do tego, które
  urządzenie jest na szynie — a taka niezgodność byłaby trudna do rozpoznania, bo obie strony
  raportowałyby, że działają.
- **Przełączanie dzieje się w zadaniu, które wysyła raporty.** `usb_pad_service_mode()` jest
  wołane raz na tik z pętli mappera, więc re-enumeracja nie ma jak wejść w kolizję z transferem
  na endpoincie. Alternatywą był muteks wokół obu ścieżek; jedno zadanie jest tańsze i łatwiejsze
  do uzasadnienia.
- **Skrót jest wyzwalany zboczem i zjadany.** Klawiatura powtarza ten sam raport, dopóki klawisz
  jest trzymany, więc przełączanie po stanie dałoby kilkadziesiąt re-enumeracji USB na sekundę.
  Kombinacja nie jest przekazywana dalej, bo w passthrough `G` trafiłoby do PC jako znak.

Instalacja sterownika TinyUSB jest w **jednym** miejscu (`install_identity()`), używanym i przy
starcie, i przy przełączaniu. Dwie kopie tej konfiguracji byłyby dwoma opisami jednej rzeczy,
czyli dokładnie kształtem błędu z §4.38.

Ścieżka awaryjna: gdy instalacja drugiej tożsamości padnie, kod wraca do pada. Obie nie mogą
leżeć, bo skrót przychodzi łączem **do tego właśnie układu** — bez urządzenia na szynie nie
byłoby czym poprosić o powrót.

Kontrola regresji po obiegu: pad zmierzony **236 Hz** po powrocie z passthrough, wobec 243 Hz na
świeżo uruchomionym układzie. Różnica mieści się w tym, ile tików miało realną zmianę stanu przy
ręcznym ruchu myszą, więc re-enumeracja nie degraduje tempa. Warto było to sprawdzić, bo zadanie
mappera przeżywa przełączenie, ale endpoint jest po nim nowy.

#### Pułapka budowania: nowa opcja Kconfig nie wchodzi do istniejącego `sdkconfig` wariantu

Kosztowała jeden cichy zły build i jest groźniejsza niż sama funkcja. Po dodaniu
`APP_USB_PASSTHROUGH` wariant `s3input` zbudował się **bez niej**, zgłaszając sukces, z binarką
bajt w bajt identyczną jak wcześniej (`0x497a0`). Opcji po prostu nie było w wygenerowanym
`sdkconfig.win.esp32s3.s3input`, bo ten plik już istniał.

Przyczyna: `scripts/build-native-win.ps1` podawał `-D SDKCONFIG=` i `-D SDKCONFIG_DEFAULTS=`
**tylko przy pierwszym** wywołaniu (`set-target`). Teraz podaje je przy każdym buildzie.
Sprawdzone tak, że usunąłem obie opcje z pliku ręcznie i uruchomiłem build — wróciły same,
a rozmiar wzrósł do `0x49950`. Wartości ustawione przez menuconfig nadal wygrywają, więc to
niczego nie nadpisuje.

To ta sama klasa błędu co niestartujący mapper z §4.38: kompiluje się, linkuje, zgłasza sukces
i robi coś innego niż się wydaje. Jedyną obroną jest czytanie **wygenerowanego** `sdkconfig`,
a nie zakładanie, że Kconfig się zastosował.

### 4.36 Mostek rozdzielony na dwa układy płytki BR (branch `esp32-br-split`)

Płytka ESP Thread Border Router ma **dwa układy połączone na PCB** — ESP32-S3 i ESP32-H2 —
a my używaliśmy naraz tylko jednego. Ten branch dzieli mostek na oba:

```
klawiatura BLE ──→ S3  (central) ─┐
                                  ├─→ pad BLE ──→ PC        (S3 jest peryferialem)
mysz BLE ──────→ H2  (central) ──→ UART ──→ S3
```

Sens tego podziału **nie jest w szybkości drutu**, i to trzeba powiedzieć wprost, bo intuicja
podpowiada inaczej. Ramka myszy ma 10 bajtów, czyli przy 921600 bodach **108 µs** na drucie,
wobec interwału połączenia BLE 15 ms — cztery rzędy wielkości różnicy. Transport nie jest
składnikiem opóźnienia. Zyskiem jest **czas radia**: zamiast jednej anteny przeplatającej
klawiaturę, mysz i pada, każdy układ obsługuje mniej linków.

Drugi zysk wychodzi z §4.35 i jest wręcz elegancki: **mysz działa na C6/H2 bez zarzutu, a
klawiatura wymaga starej rodziny kontrolerów.** Ten podział daje każdemu urządzeniu układ,
który je obsługuje — mysz na H2, klawiatura na S3. Ograniczenie z §4.35 przestaje w tej
konfiguracji przeszkadzać, zamiast być obchodzone.

#### Pin połączenia ZMIERZONY, bo dokumentacja go nie podaje

Szukanie po dokumentacji dało tylko fałszywy trop. `esp_ot_config.h` z przykładu `ot_br`
w ESP-IDF ma na sztywno `rx_pin = 4, tx_pin = 5` — ale README tego przykładu pokazuje te piny
jako wiring **DevKit do DevKitu**, nie jako wewnętrzne połączenie płytki BR. Przykład
z `esp-thread-br` używa `CONFIG_PIN_TO_RCP_TX/RX`, których wartości domyślnych nie udało się
znaleźć; strona `hardware_platforms` podaje warianty zamówieniowe, a `build_and_run` tylko
tyle, że „domyślnym interfejsem na płytce BR jest UART0, 460800".

Na pinach 4/5 satelita nadawał (`sent 111 frames`), a gospodarz nie odbierał **nic**. Zamiast
zgadywać dalej, firmware zmierzył to sam: `CONFIG_APP_LINK_PROBE_RX` przestawia UART na kolejne
piny wejściowe i liczy, na którym pojawiają się ramki z poprawnym CRC. Satelita nadaje keepalive
co 250 ms, więc sekunda na pin wystarcza.

```
W link: RX PIN SWEEP: 30 candidates, ~1 s each - the peer keepalives every 250 ms
W link:   GPIO17: 8 bytes, 4 syncs, 4 VALID FRAMES   <<< THIS IS THE PIN
W link: sweep done, most traffic on GPIO17 (8 bytes)
```

**S3 GPIO17 ← H2 GPIO24 (U0TXD).** Cztery poprawne ramki na sekundę przy keepalive co 250 ms —
zgodność, która nie zostawia miejsca na przypadek, tym bardziej że sonda sprawdza CRC, więc
przypadkowe `A5 5A` w szumie nie liczy się jako trafienie.

Kierunku S3 → H2 **nie zmierzyliśmy i nie potrzebujemy**: łącze jest jednokierunkowe, bo
gospodarz nie ma o co pytać satelity. TX po stronie S3 zostaje `-1`, czyli nie sterujemy siecią,
której drugiego końca nie potwierdziliśmy. Przez adjacencję to prawdopodobnie GPIO18, ale to
domysł i nic od niego nie zależy.

#### Konsola H2 musi zejść z UART — to nie preferencja, to wymóg

Łącze biegnie po GPIO24/23, czyli **domyślnych pinach UART0 układu H2**. Gdyby konsola została
na UART0, tekst logu jechałby tym samym drutem, dwa peryferia sterowałyby jednym pinem, a każda
linia logu wpadałaby do odbiornika S3. Dlatego `sdkconfig.defaults.esp32h2` na tym branchu ma
`ESP_CONSOLE_USB_SERIAL_JTAG=y` + `SECONDARY_NONE=y`.

Nic nie tracimy, bo płytka daje **każdemu układowi własne gniazdo USB** — potwierdzone
sprzętowo: oba porty to `VID_303A&PID_1001`, czyli natywne USB obu układów (S3 na COM10, H2 na
COM11). Uwaga: to odwrotnie niż w konfiguracji dla H2-DevKitM-1 na `main`, gdzie konsola jest
na UART, bo tam jedno z dwóch gniazd idzie przez mostek CH343.

Samo łącze używa **UART1**, nie UART0, przez matrycę GPIO. ROM pisze po UART0 przy każdym
starcie niezależnie od konfiguracji, więc trzymamy się od niego z daleka — ramkowanie i tak by
tę paplaninę odrzuciło, ale nie ma powodu jej zapraszać.

#### Protokół

```
0xA5 0x5A | typ | dlugosc | payload | crc8(typ, dlugosc, payload)
```

- `0x01 MOUSE` — przyciski `u8`, dx `i16`, dy `i16`, kółko `i8`. Przesyłane **już
  zdekodowane**, więc układ raportu konkretnej myszy (§4.10) zostaje w jednym miejscu,
  a odbiornik dostaje czyste przyrosty.
- `0x02 KEEPALIVE` — nadawany co 250 ms nawet gdy mysz stoi. Dzięki temu **cisza jest
  informacją**: odbiornik po 1,5 s bez ramki czyści stan myszy, bo przycisk trzymany w chwili
  zerwania łącza zostałby wciśnięty na zawsze.

Ramka jest samoograniczająca (dwa bajty synchronizacji + CRC), bo odbiornik musi przetrwać
śmieci na drucie. Przyrosty są per raport, nie kumulowane — kumuluje odbiornik, dokładnie tak
jak robiłby to lokalny `ble_hid_host`.

Wysyłka **nigdy nie blokuje wątku hosta NimBLE**: `chip_link_send_mouse()` tylko wkłada
zdarzenie do kolejki, a pisze osobne zadanie. Zablokowanie tamtego wątku zatrzymałoby ten sam
link BLE, z którego raport przyszedł.

#### Szew w istniejącym kodzie okazał się jednopunktowy

Mapper **pobiera** stan (`ble_hid_host_take_state()`), a nie dostaje go pchnięciem, więc
wystarczyło wstrzykiwać mysz do tego samego akumulatora: `ble_hid_host_inject_mouse()`.
Mapper i `ble_gamepad` nie wiedzą, że mysz jest na innym układzie, i nie mają w sobie ani
jednej linii o tym podziale.

Jedna pułapka: `refresh_connected_flags_locked()` przelicza flagi z **lokalnej** tablicy
urządzeń, więc kasowałoby zdalną mysz przy każdym podłączeniu i rozłączeniu czegokolwiek.
Dlatego zdalna mysz ma osobną flagę `s_remote_mouse`, ORowaną do stanu.

#### Rozdział urządzeń między układy

`APP_HID_WANT_KEYBOARD` / `APP_HID_WANT_MOUSE` decydują, jakiej klasy szuka dany układ.
Rozróżnienie idzie po **appearance z rozgłoszenia** (0x03C1 klawiatura, 0x03C2 mysz), a nie po
mapie raportów — bo nasza klawiatura deklaruje własny raport myszy (§4.16), więc jej maska
usage mówi MOUSE i po niej tych urządzeń rozróżnić nie sposób.

Pakiet **bez** pola appearance nie jest oceniany, bo tak wraca sparowany peer po uśpieniu
(§4.20) i odrzucanie go zepsułoby rekonekcję. Zabezpieczeniem na ten przypadek jest kontrola
po otwarciu: urządzenie, które nie obsługuje żadnej chcianej klasy, jest zamykane
(`mask 0x%02x is not a class this chip serves`) zamiast zajmować slot.

#### Stan weryfikacji

Najpierw sam drut, bez udziału urządzeń BLE — keepalive na to pozwala:

```
S3: link: UART1 up: tx=GPIO-1 rx=GPIO17 921600 baud
    link: mode: receiver (mouse arrives over UART)
    link: peer link up                                 <- 306 ms po starcie
    link: received 111 frames (CRC errors 0)           <- 30 s, zero bledow
H2: link: sent 111 frames (dropped 0)                  <- liczniki zgadzaja sie po obu stronach
```

Potem **cały łańcuch na sprzęcie**, z myszą sparowaną z H2, klawiaturą z S3 i padem z PC:

```
H2: OPEN f4:ee:25:36:cf:75 'AJ159 PRO' vid=0x3151 pid=0x402c
    connected (usage mask 0x63), 1/2 devices total
    MOU map=0 id=5 len=7 [00 63 00 e1 ff 00 00]        <- mysz raportuje ruch
    link: sent 117 frames (dropped 0)

S3: inputs 1 (kbd=1 mouse=1) | pad ready               <- mouse=1 przychodzi z lacza
    link: received 1388 frames (CRC errors 0)
    mapper: pad: L(   0,0) R(  22, 64)                 <- RUCH MYSZY -> PRAWA GALKA
    mapper: pad: L(   0,0) R(-123,-22)
    hid_host: KBD map=0 id=1 len=8 [00 00 06 …]        <- klawiatura lokalnie na S3
    mapper: pad: L(-127,0) R(   0,  0)                 <- klawisz A -> lewa galka
    mapper: pad: … btn=0x002  ->  xbox: LT=1023        <- prawy przycisk myszy przez UART
    mapper: pad: … btn=0x001  ->  xbox: RT=0 RT=1023   <- lewy przycisk myszy
```

Czyli **mysz na jednym układzie, klawiatura i pad na drugim, a PC widzi jednego pada**.
Ruch, przyciski i klawisze przechodzą całą drogę, przy zerowych błędach CRC na 1388 ramkach.

Do tego **potwierdzenie właściciela** (nie log): **test pada w Steam działa również na
podzielonym mostku**. To istotne, bo znaczy, że podział nie ruszył niczego, co widzi Windows —
deskryptor, tożsamość i profil XInput zostały po stronie S3 nietknięte, a mysz z drugiego
układu wchodzi do tego samego raportu przez akumulator w `ble_hid_host`. Z punktu widzenia PC
nie ma śladu, że urządzenia są na dwóch radiach.

#### Dwa błędy z pierwszego uruchomienia, oba pouczające

**1. AJAZZ AJ159 Pro deklaruje raporty KLAWIATURY.** Filtr klasy miał regułę: układ od myszy
odrzuca urządzenie, które deklaruje raporty klawiatury — bo tak wygląda nasza klawiatura
(§4.16). Pomiar pokazał, że mysz wygląda tak samo:

```
OPEN f4:ee:25:36:cf:75 'AJ159 PRO'
    map=0 id=5 typ=INPUT usage=MOUSE    len=3
    map=0 id=4 typ=INPUT usage=KEYBOARD len=8     <- mysz z raportem klawiatury
    map=0 id=4 typ=INPUT usage=KEYBOARD len=15
    map=0 id=1 typ=INPUT usage=KEYBOARD len=8
```

Maska wychodzi **0x63, dokładnie jak u klawiatury**. Reguła odrzucała więc tę jedną mysz, dla
której ten układ istnieje. Wniosek nie jest lokalny: **mapa raportów nie rozróżnia klasy
urządzenia** i nie ma sensu do tego wracać. Rozróżnia **appearance z rozgłoszenia**, i tam ta
decyzja należy. W komentarzu przy tamtej regule sam napisałem, że mysz deklarująca klawiaturę
zostanie odrzucona — i wysłałem ją mimo to; pomiar zajął minutę, rozumowanie zawiodło.

**2. Zwolnienie urządzenia bez zerwania linku zalewa log.** Objaw był spektakularny: kilkaset
linii na sekundę

```
E NIMBLE_HIDH: NOTIFY received for unknown device
```

Odrzucając urządzenie po otwarciu, wołaliśmy tylko `device_mark_dead()`, czyli zdejmowaliśmy
**nasz** wpis. Link BLE zostawał zestawiony razem z subskrypcjami CCCD, więc mysz dalej
notyfikowała, a `esp_hidh` nie miał już do czego dopasować uchwytu. Naprawa: na tej ścieżce
wołamy `esp_hidh_dev_close()`, które na NimBLE robi
`ble_gap_terminate(conn_id, BLE_ERR_REM_USER_CONN_TERM)`.

Dlaczego nie wyszło to wcześniej, choć `device_mark_dead()` był w kodzie od dawna: dotychczas
odrzucaliśmy tak wyłącznie urządzenia z maską `0x00`, czyli takie, które **nie mają czego**
notyfikować. Ta ścieżka była pierwszą, która odrzuca urządzenie już zasubskrybowane.

**3. Zerwanie łącza nie zwalniało trzymanego przycisku — mimo że był na to watchdog.**
Znalezione **dopiero pomiarem**, bo logika wyglądała poprawnie. `set_remote_mouse(false)`
wołało `refresh_connected_flags_locked()`, licząc na jego regułę „nie ma myszy → zeruj
przyciski". Tylko że ta funkcja liczy flagę z **lokalnej** maski usage, a nasza klawiatura
deklaruje własny raport myszy (§4.16), więc jej maska `0x63` trzyma bit MOUSE ustawiony.
Warunek nigdy nie odpalał.

Widać to było wprost w logu i przez chwilę wyglądało niewinnie: `peer silent ... clearing
mouse state`, a następna linia `alive` nadal `mouse=1`. Naprawa zeruje przyciski i akumulatory
wprost, niezależnie od przeliczonej flagi.

Dowód po naprawie, z trzymanym lewym przyciskiem myszy w chwili ubicia satelity:

```
gamepad: xbox: LT=0 RT=1023                                      <- przycisk trzymany
link:    peer silent for 1500 ms - clearing mouse state
hid_host: remote mouse gone - released buttons still held: 0x01   <- watchdog zadzialal
gamepad: xbox: LT=0 RT=0                                         <- pad puscil spust, ta sama ms
link:    peer link up  ->  peer reports mouse connected
gamepad: xbox: LT=0 RT=1023                                      <- dziala po powrocie
```

Metodologicznie: log **sam nosi teraz dowód** tej ścieżki (drukuje, co zwolnił, i tylko gdy
faktycznie coś było trzymane), bo test zależny od trzymania przycisku w konkretnej milisekundzie
jest niepowtarzalny. Pierwsze podejście do tego testu nic nie wykazało właśnie dlatego, że
w feralnej sekundzie nikt nie trzymał przycisku.

#### Oba brakujące cykle przejechane

- **Uśpienie i powrót myszy** — potwierdzenie właściciela: mysz wraca poprawnie przy
  podzielonym mostku.
- **Restart jednego układu** — zmierzone (log powyżej). Gospodarz zgłasza `peer silent`,
  czyści stan myszy, a po powrocie satelity dostaje `peer link up` i `peer reports mouse
  connected`. Przez cały czas `pad ready`, klawiatura działa, heap bez zmian: **utrata
  satelity nie rusza pada ani klawiatury**, mostek degraduje się łagodnie.

Liczby z dłuższego przebiegu: **10 951 ramek, zero błędów CRC**.

#### Tempo każdego odcinka, zmierzone w stanie ustalonym

Interwały logowały się dotąd tylko przy **zmianie**, więc stanu ustalonego nie było jak
odczytać. Heartbeat drukuje teraz interwał każdego aktywnego linku (`links:` w `app_main.c`),
bo to jedyna liczba, która ogranicza tempo raportów na danym odcinku:

```
S3: links: [2] peripheral 7.50 ms = 133 Hz | [3] central 15.00 ms = 66 Hz
H2: links: [0] central 7.50 ms = 133 Hz
```

| Odcinek | Tempo | Kto o tym decyduje |
|---|---|---|
| mysz → H2 | **7,50 ms = 133 Hz** | kontroler H2 (nowa rodzina) potrafi to zainicjować |
| H2 → S3 po UART | 108 µs na ramkę | nie jest ogranicznikiem, cztery rzędy wielkości zapasu |
| klawiatura → S3 | **15,00 ms = 66 Hz** | sufit starej rodziny kontrolerów (§4.33) |
| S3 → PC (pad) | **7,50 ms = 133 Hz** | Windows, jako central tego linku |

Czyli **133 Hz jest na trzech odcinkach z czterech**, a wyjątkiem jest klawiatura. Powód jest
znany i udowodniony: S3 należy do starej rodziny kontrolerów, która w roli centrala odmawia
zainicjowania interwału krótszego niż 15 ms (§4.33). Klawiatura dostaje 7,5 ms tylko wtedy,
gdy **sama** o to poprosi — widzieliśmy oba przypadki na tym samym firmware, i to rozstrzyga
się przy każdym połączeniu na nowo, bo zależy od tego, kto poprosi pierwszy. Nasza drabinka
nigdy tego nie psuje: gdy link już jest na 6, przerywa (`interval on link 3 is already 6`).

Podziału **nie warto odwracać**, choć na pierwszy rzut oka to by pomogło. Klawiatura na H2
miałaby 133 Hz, ale ona się z H2 **nie łączy w ogóle** (§4.35, HCI 0x3E) — a niezależnie od
tego obecne przypisanie jest lepsze merytorycznie: ruch myszy zasila **analogową** gałkę, gdzie
częstość widać wprost w gładkości, a klawisze są binarne i 15 ms na wciśnięcie nie zmienia
odczucia.

### 4.34 `esp_hidh` zapisuje przez wskaźnik NULL, gdy urządzenie nie jest sparowane

Znalezione przy porcie na ESP32-C6, ale **to nie jest błąd specyficzny dla C6** — ten sam
kod jest na C3 i tam też wystrzeli, tylko trudniej go trafić.

Objaw: pętla restartów. Za każdym razem, gdy mostek znajdzie **nieparowaną** klawiaturę
i próbuje ją otworzyć, leci panika z identycznym zestawem rejestrów:

```
Guru Meditation Error: Core  0 panic'ed (Store access fault)
MEPC : 0x4200f14e   RA : 0x4200f14a   MCAUSE : 0x00000007   MTVAL : 0x00000044
rst:0xc (SW_CPU)
```

`MCAUSE=0x07` to zapis pod niedozwolony adres, a `MTVAL=0x44` mówi wprost, o co chodzi:
to nie śmieciowy wskaźnik, tylko **NULL plus offset pola**. `addr2line` wskazał
`nimble_hidh.c:730`, a kod tam wygląda tak:

```c
dev = esp_hidh_dev_get_by_bda(desc.peer_ota_addr.val);
if (!dev) {
    ESP_LOGE(TAG, "Connect received for unknown device");   /* tylko log */
}
dev->status = -1;                                 /* <- zapis przez NULL */
dev->ble.conn_id = event->connect.conn_handle;
```

Gałąź `if (!dev)` **nie ma `return`**. Kod stwierdza „nieznane urządzenie" i natychmiast
przez to nieznane urządzenie zapisuje. `0x44` to offset pola `status` w `esp_hidh_dev_t` —
zgadza się co do bajtu.

**Kiedy wyszukiwanie zawodzi.** `esp_hidh_dev_get_by_bda()` szuka po adresie widzianym
**w eterze**. Urządzenie, które nie ma jeszcze bondu, rozgłasza się z adresem losowym, a ten
może się zmienić między naszym skanem a nawiązaniem połączenia — wtedy peer łączy się pod
adresem, którego `esp_hidh` nigdy nie zarejestrował. Widać to wprost w logu: klawiatura
w trybie parowania pojawiała się kolejno jako `ee:5a:12:30:0c:aa`, `cf:0c:18:5e:52:94`
i `db:6a:de:fc:b3:7a`. Urządzenie sparowane wraca ze stabilnym adresem tożsamości, dlatego
przy testach z gotowymi bondami ten błąd nie wystąpił ani razu.

**To samo w gałęzi obok.** Gdy połączenie się nie uda, `esp_hidh` robi
`dev->status = event->connect.status`, ale w tej gałęzi `dev` **nigdy nie jest
przypisywane** — jedyne przypisanie jest w gałęzi sukcesu. Zmienna startuje z `NULL`,
czyli to ten sam zapis przez NULL, tylko na ścieżce nieudanego połączenia.

**Naprawa w naszej kopii:** w obu gałęziach wychodzimy przez `SEND_CB(); return 0;`.
`SEND_CB()` jest tu konieczne, bo bez niego wołający zostaje w `WAIT_CB()` na zawsze
(§4.23); po zwolnieniu semafora `esp_hidh_dev_open()` widzi własne `dev->ble.conn_id < 0`
i kończy się czystym niepowodzeniem, które nasz kod już obsługuje (cooldown i kolejna
próba). Gałąź `BLE_GAP_EVENT_DISCONNECT` ma prawidłowy `break` po logu i jest bezpieczna.

Stan weryfikacji: po łatce płytka pracuje bez restartu, ale **sama naprawiona ścieżka
nie została jeszcze przejechana na sprzęcie** — do tego trzeba nieparowanego urządzenia
w trybie parowania. Do potwierdzenia: w logu ma się pojawić
`Connect received for unknown device`, a zaraz po nim `open failed, cooldown 15 s`,
bez paniki.

To **dziewiąta** udokumentowana wada `esp_hid` na ścieżce NimBLE (po §4.2, §4.8, §4.11,
§4.15, §4.23, §4.25, §4.27, §4.29) i pierwsza, która jest zwykłym brakiem `return`.

### 4.35 Klawiatura paruje się z C3, a nie paruje z C6 ani z H2 — różnicą jest kontroler

Objaw: na ESP32-C6 i ESP32-H2 każda próba połączenia z AULA F99 Pro w trybie parowania
kończy się `Connection failed; status=13` po upływie limitu. Mysz AJAZZ AJ159 Pro na tych
samych płytkach łączy się w **250–310 ms**. Na ESP32-C3 klawiatura łączy się normalnie.

**Rozstrzygający test: ten sam firmware na czterech płytkach.** Nie inny build, nie inna
konfiguracja — ten sam commit zbudowany na cztery targety, przy tej samej klawiaturze, w tym
samym pokoju:

| Płytka | Kontroler | RSSI klawiatury | Wynik |
|---|---|---|---|
| ESP32-C3 SuperMini | **stary** (`lib_esp32c3_family`) | −46 … −53 dBm | **łączy się, przekazuje klawisze** |
| ESP32-S3 (ESP Thread BR 1.2) | **stary** (`lib_esp32c3_family`) | **−79 dBm** | **łączy się, przekazuje klawisze** |
| MuseLab nanoESP32-C6 | nowy (`BT_LE_*`) | −68…−73 dBm (płytka **dotyka** klawiatury) | timeout, zawsze |
| ESP32-H2-DevKitM-1 | nowy (`BT_LE_*`) | **−37 … −58 dBm** | timeout, zawsze |

Dowód z C3:

```
inputs 1 (kbd=1 mouse=1)
[0] fe:ee:96:4f:6c:4a reports: keyboard mouse inne (mask 0x63)
KBD map=0 id=1 len=8 [00 00 07 00 00 00 00 00]      <- realne klawisze
```

Dowód z S3, przy sygnale **o 42 dB słabszym** niż ten, przy którym H2 zawodzi:

```
HID f4:51:67:91:13:c3 rssi=-79 pkts=1 ADV_IND 'AULA-F99Pro'
encryption: conn_handle=3 status=0 | enc=1 auth=0 bond=1
OPEN f4:51:67:91:13:c3 'AULA-F99Pro 5.0 ' vid=0x3554 pid=0xfa07
inputs 1 (kbd=1 mouse=1)
KBD map=0 id=1 len=8 [00 00 2c 00 00 00 00 00]      <- Spacja
```

**Dlaczego S3 jest drugim punktem pomiarowym starej rodziny, a nie powtórką C3:** to inny
układ, dwurdzeniowy, z inną anteną i na innej płytce, ale
`components/bt/controller/esp32s3/Kconfig.in` to **jedna linia**, która `source`-uje plik C3,
a `components/bt/CMakeLists.txt` linkuje `esp32s3` z `lib_esp32c3_family`. Wspólna jest więc
i biblioteka kontrolera, i przestrzeń opcji. Kontroler przedstawia się tam starym formatem
(`BT controller compile version [2edb0b0]`, `Feature Config, ADV:1, BLE_50:1, …`), tak jak
na C3, a nie `ble controller commit:[…]` jak na C6/H2.

**Siła sygnału jest tym wykluczona ponad wszelką wątpliwość.** Stara rodzina łączy się przy
**−79 dBm**, nowa zawodzi przy **−37 dBm**. To 42 dB w kierunku przeciwnym do tego, którego
wymagałaby jakakolwiek hipoteza o marginesie łącza. Wcześniejsza wersja tej sekcji obwiniała
odbiór na C6 i **była błędna**; zapisuję to, bo pomyłka jest pouczająca — wystarczyło jedno
urządzenie z dobrym RSSI i tym samym objawem, żeby ją zburzyć, a potem jedno ze złym RSSI
i przeciwnym wynikiem, żeby zamknąć temat.

**Kontencja radia nie jest przyczyną.** Przebieg na H2 z wyłączoną rolą pada
(`roles: hid_host=on gamepad=off`), czyli z radiem zajętym wyłącznie skanowaniem
i inicjowaniem, też kończy się timeoutem — dwa razy w jednym przebiegu.

Zostaje jedna cecha wspólna: **C6 i H2 mają ten sam, nowszy kontroler BLE** (przestrzeń
opcji `BT_LE_*`, biblioteka `controller/esp32c6` i `controller/esp32h2`), a C3 ma starszy
(`BT_CTRL_*`). Dwie fizycznie różne płytki, jedna chińska i jedna oficjalna od Espressifu,
o wyraźnie różnej jakości odbioru, zawodzą **identycznie**.

Najważniejsza obserwacja diagnostyczna, która wskazuje warstwę: **`status=13` to timeout
hosta**, czyli kontroler nigdy nie zgłosił zakończenia połączenia. Gdyby wysłał
`CONNECT_IND` i nie dostał odpowiedzi, dostalibyśmy HCI `0x3E`
(„Connection Failed to be Established"). Zgodnie ze specyfikacją inicjator raportuje
`LE Connection Complete` zaraz po **wysłaniu** `CONNECT_IND`, więc brak jakiegokolwiek
zdarzenia znaczy, że kontroler w stanie inicjowania **nie dopasował ani jednego pakietu ADV**
tego urządzenia — choć w stanie skanowania odbiera je bez problemu i widzimy je w logu
razem z pełną treścią.

Czego o tym nie wiem: dlaczego inicjator nie dopasowuje pakietu, który skaner odbiera.
Rozstrzygnięcie wymagałoby sniffera BLE, którego nie mamy.

#### Rozstrzygający pomiar: jeden usłyszany pakiet wystarcza — ale tylko na C3

Dołożony licznik pakietów **per adres** (`pkts=` w linii skanu) odpowiada na pytanie, którego
licznik zbiorczy nie rozstrzygał: czy urządzenie rozgłasza się rzadko, czy rozgłasza się
normalnie, a kontroler gubi jego pakiety. Wynik jest ten sam na obu płytkach i **nie o to
chodzi**:

```
C3:  HID e9:1d:c9:51:8f:fd rssi=-46 pkts=1 ADV_IND 'AULA-F99Pro'
     encryption: conn_handle=3 status=0 | enc=1 bond=1
     OPEN e9:1d:c9:51:8f:fd 'AULA-F99Pro 5.0 ' vid=0x3554 pid=0xfa07
     inputs 1 (kbd=1 mouse=1)

H2:  HID ed:27:ea:0d:d2:e5 rssi=-58 pkts=1 ADV_IND 'AULA-F99Pro'
     Connection failed; status=13
```

**C3 łączy się z pakietu usłyszanego dokładnie raz.** Upada więc hipoteza, którą wcześniej
tu zapisałem — że adres nie powtarza się nigdy, więc inicjator nie ma czego dopasować.
Do połączenia wystarczy jeden pakiet, jeśli tylko kontroler potrafi go użyć. Dla porównania
beacon Windows daje w tej samej rundzie `pkts=49` na C3 i `pkts=51` na H2, czyli oba skanery
odbierają porównywalnie dobrze.

Zostaje więc czysty wniosek: **przy tym samym firmware, tym samym urządzeniu i porównywalnym
sygnale kontroler C3 potrafi zainicjować to połączenie, a kontroler C6/H2 nie.**

#### To nie regresja w jednej wersji IDF — starszy kontroler zachowuje się tak samo

Kontroler jest w ESP-IDF prekompilowaną biblioteką, więc jedyną nietkniętą zmienną po
wyczerpaniu wszystkiego po stronie hosta była **jego wersja**. Bloby faktycznie się różnią:
`libble_app.a` dla H2 ma 4 602 484 B w IDF 5.4.3 i 3 786 294 B w 5.5.1, inne sumy MD5.

Zbudowane więc to samo drzewo na IDF 5.4.3 (osobny katalog `build.h2.idf543`, żeby nie
mieszać wersji) i wgrane na H2. Nasza łatana kopia `esp_hid` skompilowała się pod 5.4.3 bez
zmian. Wynik:

```
BLE_INIT: ble controller commit:[390a8ef]        <- 5.5.1 ma [898f73c]
HID d5:9d:64:ab:ea:2f rssi=-45 pkts=1 ADV_IND 'AULA-F99Pro'
Connection failed; status=13
```

**Ten sam objaw, i to przy −45 dBm** — najlepszym sygnale zanotowanym w całym śledztwie,
lepszym niż na C3 w chwili udanego parowania. Hipoteza „regresja w 5.5.1" upada; zachowanie
jest cechą rodziny kontrolerów C6/H2, nie jednej wersji IDF.

Uwaga praktyczna dla kogoś, kto powtórzy ten test: `scripts/build.sh` przyjmuje `IDF_DIR`,
ale katalog build wylicza z targetu, więc przy dwóch wersjach IDF trzeba wołać `idf.py`
wprost z osobnym `-B` i `-D SDKCONFIG=`. Do wgrania obrazu z takiego katalogu skrypty się
nie nadają (szukają `build.<t>` i `build.win.<t>`) — offsety bierze się z
`flasher_args.json` i podaje `esptool` ręcznie.

#### Gdzie dokładnie zawodzi: `CONNECT_IND` dociera, pierwsze zdarzenie połączenia nie

Obserwacja właściciela, która przesunęła diagnozę o warstwę: **w chwili gdy w logu pojawia
się wykrycie AULI, klawiatura wychodzi z trybu parowania** — po maksymalnie dwóch sekundach —
i zaczyna szukać poprzednio sparowanego hosta. Urządzenie robi tak tylko wtedy, gdy
**przyjęło żądanie połączenia**. Czyli nasz `CONNECT_IND` do niej dociera i z jej punktu
widzenia link powstaje; nasz kontroler po prostu nigdy tego nie kończy.

Zgadza się z tym rytm w logu: ten sam adres `d1:be:9b:7c:63:44` widzieliśmy o 9,6 s i znowu
o 22,5 s, a dokładnie pomiędzy — w naszym oknie inicjowania — klawiatura milczała.
Wygląda to na cykl „przyjęła połączenie, przestała rozgłaszać, po swoim timeoucie wróciła".

Adres nie jest więc problemem, bo żyje kilkanaście sekund. Problemem jest **pierwsze
zdarzenie połączenia**, czyli moment, w którym obie strony mają się spotkać po raz pierwszy.
To już warstwa kontrolera i z hosta sterują tym tylko parametry w `CONNECT_IND` — sprawdzone,
patrz tabela.

Co sprawdzone i **nieskuteczne** na tej warstwie, każde osobnym przebiegiem na H2:

| Zmiana | Mechanizm, dla którego warto było spróbować | Wynik |
|---|---|---|
| `BT_LE_LL_SCA` 60 → 500 ppm | SCA mastera jedzie w `CONNECT_IND` i peryferial wymiaruje po nim okno odbioru; zbyt optymistyczna wartość = zbyt wąskie okno | `status=13` przy −44 dBm |
| `BT_LE_SCAN_DUPL` wyłączone | kontroler C6/H2 ma własny filtr duplikatów po adresie, którego C3 nie ma w tej postaci — mógł ukrywać pakiety (`pkts=1` przy stałym adresie, a `pkts=51` przy beaconie rotującym adres) | `pkts=1` bez zmian, `status=13` |
| jawne parametry w `CONNECT_IND` | 15 ms na sztywno (`min == max`) i timeout nadzoru 4 s zamiast domyślnych 30–50 ms i 2560 ms — inne okno pierwszego spotkania | `status=13` |
| IDF 5.4.3 zamiast 5.5.1 | inny blob kontrolera (4 602 484 B wobec 3 786 294 B) | `status=13` przy −45 dBm |



`esp_hidh` inicjuje na sztywno z adresu **publicznego** (`own_addr_type = 0; // set to public
for now`), a Windows łączy się z tą klawiaturą z adresu prywatnego — to była ostatnia
nietknięta różnica po stronie inicjowania. Sprawdzone: rejestracja losowego adresu
statycznego obok publicznego i inicjowanie z niego **nic nie zmieniło** dla tej klawiatury
(nadal `status=13` przy −53 dBm) i **zepsuło powrót urządzeń już sparowanych**.

Dlaczego psuje: sparowany peer wraca rozgłoszeniem **kierunkowym**, a `ADV_DIRECT_IND` nosi
w sobie adres inicjatora. Urządzenie, które sparowało się z naszym adresem publicznym,
celuje w ten adres — więc inicjator używający innego adresu własnego nie ma prawa
odpowiedzieć i nie odpowiada. Na sprzęcie wyglądało to tak, że klawiatura nadawała `DIR_IND`
co 500 ms ze stabilnego adresu tożsamości przy −42 dBm, a nasza próba i tak kończyła się
timeoutem. Zmiana została wycofana, a w kodzie został komentarz, żeby nikt tego nie powtórzył.

Dwie pułapki z tej próby, obie warte pamięci: `ble_hs_id_gen_rnd()` wołane przed
synchronizacją hosta zwraca `rc=22` (`BLE_HS_ENOTSYNCED`) — adresy można ustawiać dopiero
w `on_sync()`. Oraz `ble_hs_id_addr()` jest wewnętrzne (`ble_hs_priv.h`), a publiczne API to
`ble_hs_id_copy_addr()` z `host/ble_hs_id.h`.

Co zostało **wykluczone osobnym pomiarem** (warto zapisać, żeby nikt nie wracał do tych
tropów):

| Podejrzany | Jak wykluczony |
|---|---|
| Windows przechwytuje klawiaturę | wpisy AULI usunięte z systemu i potwierdzone jako nieobecne; objaw został |
| druga płytka konkuruje | w każdym pomiarze tylko jedna płytka podłączona, sprawdzone listą portów COM |
| stary bond na kanale Fn+3 | przytrzymanie Fn+3 to pełne parowanie i usuwa poprzedniego hosta |
| moc nadawania | podniesiona z domyślnych +3 dBm do +20 dBm, potwierdzona odczytem `TX power level: 15` |
| siła sygnału / margines łącza | H2 przy −52 dBm zawodzi, C3 przy −53 dBm działa |
| głodzenie inicjatora przez link pada | przebieg z `pad no PC`, a na H2 z całkowicie wyłączoną rolą pada |
| zwłoka uzbrojenia inicjatora | sonda łącząca się **z callbacku GAP**, mikrosekundy po pakiecie, też `status=13` |
| własny adres inicjatora (publiczny kontra losowy) | sprawdzone oba; przy losowym nadal `status=13`, a dodatkowo psuje się powrót sparowanych urządzeń (`DIR_IND`) |
| „adres nie powtarza się, więc nie ma czego dopasować" | C3 łączy się z pakietu o `pkts=1`, czyli jedno usłyszenie wystarcza |
| filtrowanie po stronie klawiatury | sonda przy braku parowania z Windows również odrzucona |
| tryb powrotu zamiast parowania | pakiety mają `flags=0x05` (limited discoverable), UUID 0x1812, appearance 0x03C1 i beacon Swift Pair — to jest zaproszenie do parowania |
| `BT_LE_50_FEATURE_SUPPORT` na C6/H2 | opcja ma `depends on !BT_NIMBLE_ENABLED`, więc przy NimBLE jest nieaktywna; `BT_NIMBLE_EXT_ADV` też jest wyłączone, skan idzie ścieżką legacy |

Co z tego wynika praktycznie:

- **C3 zostaje platformą odniesienia** dla pełnego zestawu klawiatura + mysz. Tam wszystko
  jest potwierdzone logiem.
- **Port na C6 i H2 jest poprawny wszędzie poza tą jedną ścieżką**: pad działa z XInput
  (na C6 potwierdzone wpisem `_VID&02045E_PID&0B13_REV&0509_404CCA5FC62A` w systemie), mysz
  łączy się i mapuje, a wejścia dostają 7,5 ms zamiast 15 ms z C3.
- H2 buduje się i startuje bez żadnej zmiany w kodzie aplikacji; jedyny nowy plik to
  `sdkconfig.defaults.esp32h2`. Pamięć: `heap before BLE 212 kB`, po starcie stacku ~162 kB
  (C3: ~192 kB, C6: ~357 kB), więc jest ciaśniej niż na C6, ale z zapasem.

#### Poprawki, które wyszły przy okazji i zostają

Śledztwo wymusiło kilka zmian wartych zachowania niezależnie od przyczyny:

- **skan pasywny** (`passive = 1`) i okno równe interwałowi. Aktywny skan kazał nam nadawać
  `SCAN_REQ` po każdym pakiecie; po przejściu na pasywny liczba usłyszanych pakietów
  klawiatury wzrosła z jednego na 90 s do dwóch na 15 s. Nic nie tracimy, bo ta klawiatura
  ma w samym ADV flagi, UUID 0x1812, appearance i pełną nazwę w 31 bajtach,
- **`ble_gap_disc_cancel()` nie dostarcza `DISC_COMPLETE`** — to zdarzenie przychodzi tylko
  wtedy, gdy skan skończy się sam. Kod, który przerywał skan i czekał na nie, blokował się
  do końca okna ze **zatrzymanym** skanerem: pakiet usłyszany o 28,0 s dawał próbę
  połączenia o 34,3 s. Po ustawianiu `EV_DISC_DONE` samemu jest 10 ms,
- **świeżość kandydata**: nie dzwonimy pod adres, którego nie słyszeliśmy od 8 s. Runda
  raportująca `0 ADV reports` potrafiła wcześniej wybrać adres sprzed 13 s, a przy dwóch
  adresach tej samej klawiatury w tablicy trafialiśmy w starszy, martwy,
- **limit próby połączenia 30 s → 6 s** w naszej kopii `esp_hid` i przerwa 15 s → 5 s.
  Peer, który zamierza odpowiedzieć, odpowiada w 250–310 ms; jedna długa próba tylko blokuje
  zadanie skanujące, które w tym czasie nie słyszy nic,
- **moc nadawania na maksimum** zamiast domyślnych +3 dBm, z odczytem poziomu z powrotem.

#### Niezależny przegląd świeżym okiem: co zamknął, a co dołożył

Zlecony przegląd problemu bez dostępu do naszego kontekstu (dwa równoległe zadania: kopanie
w źródłach zewnętrznych oraz audyt naszego rozumowania). Warto zapisać jedno i drugie, bo
**część twierdzeń nie przetrwała weryfikacji w drzewie IDF** — a to samo w sobie jest
lekcją: podpowiedź trzeba sprawdzić w źródle, nie przyjąć.

Zamknięte, każde weryfikacją w plikach, nie rozumowaniem:

| Hipoteza z przeglądu | Weryfikacja |
|---|---|
| NimBLE idzie na C6/H2 ścieżką `LE_Extended_Create_Connection`, bo kontroler wspiera BLE 5.0 | **zamknięte** — `CONFIG_BT_NIMBLE_EXT_ADV` nie jest ustawione ani w `sdkconfig`, ani w rozwiązanym `sdkconfig.h`; inicjowanie idzie legacy |
| `BT_LE_TX_CCA_ENABLED` blokuje nam nadawanie (ocena zajętości kanału) | **zamknięte** — brak w rozwiązanym `sdkconfig.h`, czyli wyłączone; to samo `BT_LE_CTRL_CHAN_ASS_EN` |
| errata układu opisuje coś o BLE | **zamknięte** — errata C6 i H2 nie mają **ani jednego** wpisu o BLE |
| ktoś to już zgłosił | **zamknięte** — brak zgłoszenia w `espressif/esp-idf`, `espressif/esp-nimble` i na esp32.com; problem wygląda na niezgłoszony |
| `BT_LE_CTRL_CHECK_CONNECT_IND_ACCESS_ADDRESS` | dotyczy **przyjmowania** `CONNECT_IND`, czyli roli rozgłaszającego — nie nasz przypadek |

**Sprostowanie do mojego wcześniejszego zdania:** napisałem, że opcji scan backoff nie ma dla
C6/H2, bo grep po `controller/esp32h2/Kconfig.in` nic nie dał. To było błędne.
`CONFIG_BT_CTRL_SCAN_BACKOFF_UPPERLIMITMAX=32` **jest** w naszym `sdkconfig` dla H2 i trafia
do `sdkconfig.h` — definicja siedzi w innym pliku Kconfig. Natomiast
`NIMBLE_DISABLE_SCAN_BACKOFF` jest definiowane tylko dla H4, a dla H2 występuje wyłącznie
w treści makra `BT_CONTROLLER_INIT_CONFIG_DEFAULT`, którego ścieżka NimBLE u nas nie rozwija.

Co przegląd **dołożył** i czeka na przebieg ze sprzętem:

- **`BT_LE_LL_PEER_SCA_SET_ENABLE=y` z `BT_LE_LL_PEER_SCA=10000`.** Mechanicznie najlepszy
  pomysł z całego przeglądu i lustrzane odbicie testu, który już zrobiłem: wtedy podniosłem
  **własne** SCA, czyli poszerzyłem okno odbioru u klawiatury, a ta opcja każe kontrolerowi
  liczyć poszerzenie okna po stałej, maksymalnej niedokładności zegara peera, czyli poszerza
  okno **u nas**. Nasz objaw to dokładnie nieodebrany pierwszy pakiet od peera.
- **`BT_CTRL_SCAN_BACKOFF_UPPERLIMITMAX` z 32 na 1.**
- **Połączenie przez listę akceptacji** (`ble_gap_wl_set()` plus `ble_gap_connect()`
  z `peer_addr = NULL`) — najwyższy typ obu recenzentów. Sprawdza, czy problemem jest
  dopasowanie adresu, i może być gotowym obejściem: w tablicy kandydatów mamy kilkanaście
  adresów tej klawiatury, więc można wpisać je wszystkie i trafić w ten, którego użyje
  następnym razem.

Audyt zwrócił też uwagę, że wniosek „kontroler nigdy nie zgłosił zakończenia połączenia"
opiera się na tym, że `ble_gap_connect()` **wróciło zerem**. To jest w logu potwierdzone
pośrednio: gdy zwraca błąd, nasza kopia komponentu drukuje `esp_ble_gattc_open failed: %d`
(widzieliśmy to raz, `rc=2`, gdy sonda kolidowała z normalną ścieżką), a w przebiegach
z timeoutem tej linii nie ma.

#### Trzy pomysły z przeglądu, przejechane na sprzęcie — żaden nie pomaga

| Zmiana | Wynik |
|---|---|
| `BT_LE_LL_PEER_SCA_SET_ENABLE=y` z `BT_LE_LL_PEER_SCA=10000` | `status=13` przy −44 i −43 dBm |
| `BT_CTRL_SCAN_BACKOFF_UPPERLIMITMAX` 32 → 1 | bez zmian |
| połączenie przez listę akceptacji (`peer_addr = NULL`) | mechanicznie działa (`accept list armed with 1 address`), `status=13` |

Wszystkie trzy wycofane, bo nie pomogły; peer SCA i backoff to nastawy w generowanym
`sdkconfig`, a ścieżka listy akceptacji została usunięta z kodu.

**Dlaczego lista akceptacji niczego nie rozstrzygnęła, choć była najwyższym typem obu
recenzentów.** Przy jednym adresie na liście jest to równoważne dzwonieniu pod ten adres
wprost — a więcej adresów nie mieliśmy, bo tablica kandydatów czyści wpisy, których nie
słyszymy. Żeby ta droga cokolwiek dała, klawiatura musiałaby **wracać** do adresów już
użytych, a przy ponad dwudziestu zanotowanych nie powtórzył się żaden. Lista akceptacji nie
umie zgadnąć adresu, którego jeszcze nie było, więc strukturalnie nie może tu pomóc.

#### Realna naprawa, którą przy okazji ujawnił ten przebieg

W jednej rundzie skanu w eterze były **dwa** urządzenia z appearance klawiatury:

```
fb:1c:8e:df:af:e3  flags=0x04 (NOT-DISCOVERABLE)  raw len=11, bez nazwy
ed:a6:0a:52:f8:20  flags=0x05 (limited-disc)      raw len=31, 'AULA-F99Pro' + Swift Pair
```

Pierwsze to klawiatura szukająca **innego** hosta — nie oferuje się do parowania. Nasz kod
wybierał właśnie ją, przepalał na niej pełny limit próby i w tym czasie był głuchy na tę
drugą, która czekała na sparowanie. Ten sam adres `fb:1c:…` wybieraliśmy też dzień wcześniej,
czyli część nieudanych prób na H2 poszła w ogóle nie w tę klawiaturę, o którą nam chodziło.

Poprawka: kandydat, który **nie ma z nami bondu**, nie rozgłasza się kierunkowo i ma flagi bez
bitu Limited ani General Discoverable, jest pomijany. Dwa świadome wyjątki: urządzenie
z bondem przechodzi niezależnie od flag (peer wracający ze snu nadaje minimalny payload,
§4.20), a pakiet **bez pola flag** nie jest oceniany, bo brak pola nie jest wypowiedzią
o dostępności. Potwierdzone logiem: po zmianie dzwonimy już tylko pod `flags=0x05` z nazwą.

To nie naprawia parowania na C6/H2 — te próby nadal kończą się `status=13` — ale usuwa realne
marnowanie prób i tłumaczy część wcześniejszego szumu w pomiarach.



Rozpoznanie mówiło, że to najlepszy pozostały trop, bo zmienia dokładnie ten jeden komponent,
który wskazaliśmy jako różnicę. **Sprawdzone na sprzęcie i trop jest zamknięty.**

Instalacja i build przeszły bez oporu: klon płaski IDF 6.0.2 zajmuje 695 MB, `install.sh`
dla `esp32h2,esp32c6,esp32c3` bez uwag, a projekt zbudował się **bez ani jednej zmiany
w kodzie** — nawet nasza łatana kopia `esp_hid`, przypięta do 5.5.1, skompilowała się pod
6.0.2 bez błędu. Dla tego testu została właśnie taka, bo to wariant najbardziej porównywalny:
różni się tylko kontroler i host NimBLE.

Firmware wstaje i kontroler jest wyraźnie nowszy:

```
ESP-IDF: v6.0.2
ble controller commit:[4adb29e,7f63735]     <- 5.5.1 ma [898f73c], 5.4.3 [390a8ef]
heap before BLE 226 728 B, po starcie stacku 170 984 B   <- na 5.5.1 bylo 212 kB i 162 kB
```

Wynik testu klawiatury, przy niej w prawdziwym trybie parowania (`ADV_IND`,
`appearance=0x03c1`) i przy **−37…−38 dBm**, czyli najlepszym sygnale zanotowanym w całym
śledztwie:

```
candidate cc:85:1b:b1:aa:05 ... rssi=-38
Connection failed; status=13        <- piec razy w 60 s
```

Trzy wersje kontrolera, trzy takie same wyniki: **5.4.3, 5.5.1 i 6.0.2 zachowują się
identycznie**. Hipoteza „to kwestia wersji kontrolera" upada w całości — to nie regresja
i nie brak poprawki, tylko trwała cecha rodziny C6/H2 wobec tej klawiatury.

Rzeczy warte zapisania z tego przebiegu:

- **cztery nasze wady `esp_hid` są w 6.0.2 naprawione** (§4.27, §4.34, §4.25, §4.29), a pięć
  zostaje (§4.8, §4.11, §4.15, §4.23, §4.33) — więc gdyby projekt kiedyś przechodził na 6.x,
  `PATCH.diff` trzeba **napisać od nowa**, nie przenieść; szczegóły w tabeli wyżej,
- **nasze API przeżyło przejście na 6.x**: `esp_ble_tx_power_set/get`, `ESP_PWR_LVL_P20`,
  `BT_NIMBLE_GATTC_AUTO_PAIR`, `BT_NIMBLE_HANDLE_REPEAT_PAIRING_DELETION`,
  `BT_NIMBLE_HID_SERVICE` — wszystko na miejscu,
- pod 6.0.2 zdarzyło się, że `esp_hidh_dev_open()` **nie wróciło** w limicie i zadziałał nasz
  watchdog z §4.23 (`restarting the chip - bonds live in NVS`). Objaw znany, obsługa
  zadziałała, ale to znaczy, że §4.23 jest tam nadal realnym problemem — zgodne z tym, że
  `WAIT_CB()` w 6.0.2 wciąż czeka `portMAX_DELAY`,
- **pułapka z konsolą, która kosztowała rundę diagnozy**: ten devkit ma dwa gniazda USB-C,
  jedno przez mostek CH343 (`VID_1A86`), drugie wprost do układu (`VID_303A&PID_1001`).
  Firmware logujący tylko przez USB Serial/JTAG milczy na tym pierwszym, a objaw jest
  zwodniczy — widać paplaninę ROM-u, bo ROM pisze po UART0 niezależnie od konfiguracji,
  i potem nic, dokładnie jak przy martwym firmware. Dlatego `sdkconfig.defaults.esp32h2`
  ustawia teraz UART jako konsolę główną i USB Serial/JTAG jako zapasową.

Jak to powtórzyć: `build.sh` przyjmuje `IDF_DIR` i `BUILD_SUFFIX`, więc
`IDF_DIR=~/esp/v6.0.2/esp-idf BUILD_SUFFIX=.idf602 ./scripts/build.sh esp32h2` daje osobny
katalog build i osobny `sdkconfig`, nie ruszając zwykłego builda. Wgranie obrazu z takiego
katalogu trzeba zrobić `esptool` ręcznie, bo `flash-win.bat` szuka `build.<t>`
i `build.win.<t>`; offsety są w `flasher_args.json`.
`scripts/check_controller_versions.sh` porównuje commity submodułu kontrolera między
wydaniami bez pobierania czegokolwiek.



| Układ | IDF 5.5.1 | IDF 6.0.2 |
|---|---|---|
| ESP32-H2 | `a2ba40fae246` | `b2b8fd009c31` |
| ESP32-C6 | `3655fcdaadb6` | `e7be018522c8` |
| ESP32-C3 | `0c68809d62e4` | `0a08c4b32f36` |

To istotne, bo dotychczasowy test „innej wersji kontrolera" zrobiliśmy na IDF **5.4.3**,
czyli wersji **starszej** niż nasza — obie z linii 5.x i obie zachowują się identycznie.
6.0.2 to inna generacja i jej kontrolera nie sprawdzaliśmy.

**Cztery z dziewięciu naszych wad `esp_hid` są tam naprawione.** Plik `nimble_hidh.c` urósł
z 966 do 1281 linii i został przepisany:

| Wada | Stan w 6.0.2 |
|---|---|
| §4.27 `services_discovered` nigdy nie zerowane | **naprawione** — jest `services_discovered = 0;` |
| §4.34 zapis przez NULL przy nieznanym urządzeniu | **naprawione** — komunikat i cała ta gałąź zniknęły |
| §4.25 `connected` nigdy nie ustawiane na `true` | **naprawione** — jest `dev->connected = true;`, więc `CLOSE_EVENT` zacznie przychodzić |
| §4.29 brak inicjowania szyfrowania | **naprawione** — jest `ble_gap_security_initiate()` |
| §4.11 tablice odkrywania na stosie wołającego | zostaje (ale z nazwanymi granicami) |
| §4.23 `WAIT_CB()` bez timeoutu | zostaje — nadal `portMAX_DELAY` |
| §4.15 `dev->usage` nigdy nie ustawiane | zostaje — zero trafień |
| §4.8 cały plik gatowany `BT_NIMBLE_HID_SERVICE` | zostaje |
| §4.33 `ble_gap_connect(..., 30000, NULL, ...)` | zostaje, w tym `own_addr_type = 0; // set to public for now` |

**Nasze API przeżyło przejście na 6.x** — sprawdzone w nagłówkach wydania:
`esp_ble_tx_power_set()`/`esp_ble_tx_power_get()` i `ESP_PWR_LVL_P20` są na miejscu (doszły
warianty `_enhanced` z uchwytem), a w Kconfigu NimBLE nadal jest
`BT_NIMBLE_GATTC_AUTO_PAIR` (§4.1), `BT_NIMBLE_HANDLE_REPEAT_PAIRING_DELETION` (§4.29) oraz
`BT_NIMBLE_HID_SERVICE` (§4.8).

Czego to **nie** obiecuje: nikt nie zapowiedział naprawy naszego objawu i nie znaleźliśmy
zgłoszenia z takim opisem. To trop oparty na tym, że zmienia się dokładnie ten jeden
komponent, który wskazaliśmy jako różnicę — nie na dowodzie.

Koszt, gdyby ktoś to podjął:

- osobna instalacja IDF 6.0.2 (po stronie Windows jest tylko 5.5.1),
- **odtworzenie łatki `esp_hid` z nowego pliku** — połowa wpisów staje się zbędna, więc
  `PATCH.diff` trzeba napisać od nowa, nie przenieść,
- 6.0 jest wydaniem **z łamiącymi zmianami**; te, które nas dotyczą, sprawdzone powyżej i są
  bezpieczne, ale reszta drzewa nie była przeglądana,
- test ma sens wyłącznie na sprzęcie, z klawiaturą w trybie parowania, i rozstrzyga w jednym
  przebiegu: albo `OPEN … 'AULA-F99Pro'`, albo znowu `Connection failed; status=13`.



#### ROZSTRZYGNIĘTE trace'em HCI: połączenie POWSTAJE i umiera z HCI 0x3E

Przez dwa dni wszystko opierało się na `Connection failed; status=13`, czyli na timeoucie
**hosta**. Wewnętrzny log kontrolera (`APP_DEBUG_CTRL_LOG_DUMP`, dekoder
`scripts/decode_ctrl_log.py`) pokazał, że obraz był odwrotny do naszych wniosków. Pełna
sekwencja jednej próby, zdekodowana z surowych rekordów:

```
0d 20 19 10 00 10 00 00 01 b4 ab 54 24 9f ec 00 18 00 28 00 00 00 00 01 00 00 00 00
  host -> LE_Create_Connection: peer ec:9f:24:54:ab:b4 (random), own=public,
          scan 10/10 ms, itvl 30-50 ms, latency 0, supervision 2560 ms
0f 04 00 01 0d 20
  ctrl -> Command Status: status=0x00, komenda przyjeta
3e 1f 0a 00 01 00 00 01 b4 ab 54 24 9f ec ... 28 00 00 00 00 01 00
  ctrl -> LE ENHANCED CONNECTION COMPLETE: status=0x00, handle=1, rola=central,
          peer ec:9f:24:54:ab:b4, itvl 0x0028 = 50 ms
3e 04 14 01 00 00
  ctrl -> LE Channel Selection Algorithm, handle 1
16 20 02 01 00
  host -> LE_Read_Remote_Features dla handle 1     <- host normalnie idzie dalej
3e 0c 04 3e 01 00 22 00 ...
  ctrl -> Read Remote Features Complete: status=0x3E
05 04 00 01 00 3e
  ctrl -> DISCONNECTION COMPLETE: handle 1, reason 0x3E
0d 20 19 10 00 10 00 00 01 b4 ab 54 24 9f ec ...
  host -> LE_Create_Connection, ten sam peer, jeszcze raz
```

**Kontroler wysyła `CONNECT_IND` i tworzy połączenie.** Zgłasza je jako `status=0x00`
z uchwytem 1 i interwałem 50 ms. Host to przyjmuje i rusza dalej — pyta o cechy peera.
I dopiero wtedy link **umiera z HCI `0x3E`**, czyli „Connection Failed to be Established":
błąd, który znaczy, że strony nie spotkały się na zdarzeniach połączenia.

Liczby z jednego nagrania, przy dwóch wciśnięciach Fn:

| Zdarzenie | Ile |
|---|---|
| `LE_Create_Connection` | 4 |
| Enhanced Connection Complete, `status=0x00` | **2** |
| Disconnection Complete, `reason=0x3E` | **2** |
| Read Remote Features Complete, `status=0x3E` | 2 |
| `LE_Create_Connection_Cancel` | 4 |

Czyli **każda próba kończy się nawiązaniem i natychmiastową śmiercią linku**, a nie brakiem
reakcji kontrolera.

**Dlaczego nie widzieliśmy tego dwa dni.** `CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT=y`
z `CONFIG_BT_NIMBLE_MAX_CONN_REATTEMPT=3` — NimBLE po cichu ponawia połączenie po takim
błędzie i **nie mówi o tym aplikacji**. Stąd cztery komendy na dwie próby. Aplikacja widzi
wyłącznie to, że jej własny limit 6 s minął, i dostaje `BLE_HS_ETIMEOUT`. Nasze zdanie
„kontroler nigdy nie zgłosił zakończenia połączenia" było więc **błędne**: zgłosił, i to
sukcesem, tylko host tę informację zużył na ciche ponawianie.

Co to wyjaśnia bez dodatkowych założeń:

- **dlaczego klawiatura wychodzi z trybu parowania w dwie sekundy** — bo link naprawdę
  powstaje po jej stronie; obserwacja właściciela była trafna i teraz ma potwierdzenie
  na poziomie HCI,
- **dlaczego sztuczna klawiatura, mysz i pad działają** — u nich pierwsze zdarzenia
  połączenia dochodzą, więc nie ma `0x3E`,
- **dlaczego żadna zmiana w rozgłaszaniu ani w sile sygnału nie pomogła** — problem jest
  po nawiązaniu linku, a nie przed.

Zawężenie jest teraz bardzo wąskie: **pierwsze zdarzenia połączenia między kontrolerem
C6/H2 a tą klawiaturą**. Znane parametry, z którymi to zawodzi: interwał 50 ms (górna
granica domyślnego zakresu NimBLE 30–50 ms), latency 0, timeout nadzoru 2560 ms, adres własny
publiczny, adres peera losowy statyczny. Okno transmisji wybiera kontroler i w HCI go nie
widać.

#### Dwie naprawy, które z tego wyszły i zostają

**1. Wyłączone ciche ponawianie NimBLE.** `CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT=n`
sprawia, że aplikacja widzi prawdę od razu: w logu jest `disconnect; reason=574`
(czyli `BLE_HS_HCI_ERR(0x3E)`) 350 ms po próbie, zamiast timeoutu po 6 s. Przy trzech cichych
ponowieniach na próbę diagnostyka była wcześniej praktycznie niemożliwa — i to jest powód,
dla którego dwa dni patrzyliśmy na zły etap.

**2. Otwierający jest budzony, gdy link umiera w trakcie otwierania.** Nasza kopia
`esp_hid` oddaje semafor w obu gałęziach obsługi rozłączenia, jeśli ktoś czeka na otwarcie
(flaga `s_open_waiting` ustawiana wokół `WAIT_CB()`).

Bez tego było tak: kontroler zgłasza połączenie, link umiera z `0x3E`, a `esp_hidh` **nie zna
uchwytu** — log mówi wprost `disconnect handling: conn_handle=1 dev=NOT FOUND
open_waiting=1`, bo zdarzenie połączenia nigdy nie dotarło do jego handlera. Upstream w tym
miejscu tylko robi `break`, więc wołający zostaje w `WAIT_CB()` bez końca (§4.23), a nasz
własny nadzór **restartuje układ** dwie sekundy później. Przy urządzeniu, które zawodzi
w pętli, to restart co kilkanaście sekund.

Po naprawie, zmierzone: `candidate` o 11556, `dev open failed! status: 0x0` o 11946 i kolejna
próba o 18696. **390 ms od kandydata do czystej porażki**, bez restartu, z normalnym
cooldownem — czyli mostek działa dalej i obsługuje pozostałe urządzenia, mimo że ta jedna
klawiatura się nie łączy.

To jest realna poprawa odporności niezależna od przyczyny: każde urządzenie, które nawiąże
link i natychmiast go zerwie, było dla nas wcześniej powodem restartu.



`CONFIG_APP_ROLE_FAKE_KEYBOARD` zamienia płytkę w nadawcę, który udaje AULĘ **bajt w bajt** —
te same 31 bajtów payloadu (flagi `0x05`, UUID 0x1812, appearance `0x03C1`, beacon Swift Pair
Microsoftu, skrócona nazwa), `ADV_IND`, adres statyczny losowy, interwał 200 ms — i przyjmuje
połączenia. Uruchomiona na S3 z płytki ESP Thread BR, obok H2 na tej samej PCB.

Wynik pierwszego przebiegu, z H2 w normalnej roli mostka:

```
adv from 'AULA-F99Pro' e5:e7:f7:47:50:49 type=1 rssi=-7
connection established, conn_handle=0
encryption: conn_handle=0 status=0 | enc=1 auth=0 bond=1
```

**560 ms i połączone, razem z szyfrowaniem.** Wyzwalacz nie siedzi więc w treści rozgłoszenia:
ani flagi, ani appearance, ani UUID, ani typ adresu, ani interwał.

Zostawało zastrzeżenie: `rssi=-7`, bo obie płytki leżą na jednej PCB, a prawdziwa AULA zawodzi
przy −40. Dlatego atrapa dostała regulację mocy (`APP_FAKE_KBD_TX_POWER`) — jedyny sposób,
by udawać odległość bez przenoszenia sprzętu. Przy poziomie **0, czyli −24 dBm**:

```
adv from 'AULA-F99Pro' e5:bc:b0:26:67:fb type=1 rssi=-41
connection established, conn_handle=0
encryption: conn_handle=0 status=0 | enc=1 auth=0 bond=1
```

**−41 dBm to dokładnie zakres, w którym prawdziwa klawiatura zawodzi** (−37 … −48), a H2
łączy się w 560 ms. To domyka sprawę sygnału kontrolą z dopasowanym RSSI, nie rozumowaniem.

Co z tego wynika dla obrazu całości:

- **inicjator C6/H2 jest sprawny**: łączy się z atrapą przy −41 dBm, łączy się z myszą,
  łączy się z padem w roli peryferiala,
- **nie łączy się wyłącznie z prawdziwą AULĄ**, a ta sama AULA łączy się z C3 i S3,
- czyli różnica nie jest ani w rozgłoszeniu, ani w sile sygnału, ani w ogólnej zdolności
  inicjowania — zostaje **zachowanie tej klawiatury na pierwszym zdarzeniu połączenia**,
  czyli jedyny fragment, którego jeszcze nie zmierzyliśmy.

Dalszy krok, który z tego wynika: porównać **ruch HCI** przy nieudanej próbie z prawdziwą
klawiaturą i przy udanej z atrapą, na tej samej płytce. Atrapa daje nam wreszcie
**referencję znanego dobrego przebiegu** dla dokładnie tej samej ścieżki kodu, więc różnica
w HCI wskaże krok, na którym rzecz się rozjeżdża — a to jest już materiał, który zamyka
zgłoszenie do Espressifu.

Drobiazg zanotowany przy okazji: atrapa nie wystawia usługi HID, więc nasz kod słusznie
odrzuca ją przez `no input reports (mask 0x00)`, a ponowna próba w następnej rundzie kończy
się `esp_ble_gattc_open failed: 14` (`BLE_HS_EDONE`), bo `esp_hidh` trzyma już wpis dla tego
adresu. Dla testu inicjowania to bez znaczenia — pytanie brzmi „czy link powstaje", a nie
„czy urządzenie jest użyteczne".



Wszystkie wcześniejsze pomiary tempa rozgłoszeń były **skażone naszym własnym zachowaniem**:
skaner przerywał rundę po pierwszym usłyszanym pakiecie, żeby zdążyć zadzwonić, więc obraz
„jeden pakiet na rundę, za każdym razem inny adres" mówił o nas, a nie o klawiaturze.

`CONFIG_APP_DEBUG_SCAN_ONLY` wyłącza łączenie i zostawia sam nasłuch. Wynik jest zupełnie
inny:

```
46220 ff:3a:0d:7a:43:7c -45      <- ten sam adres
46540 ff:3a:0d:7a:43:7c -45
48060 ff:3a:0d:7a:43:7c -45
...
59410 ff:3a:0d:7a:43:7c -40      <- 13,2 s później, nadal ten sam
```

- **adres jest stabilny** przez całą sesję parowania, zmierzone ≥ 13,2 s bez zmiany,
- **pakiety lecą co 50–900 ms**, często kilka na sekundę,
- flagi `0x05`, `ADV_IND`, pełne 31 bajtów z nazwą i beaconem Swift Pair,
- RSSI −39 … −47 dBm.

Czyli **to zwyczajny, zdrowy nadawca**. Hipoteza „adres rotuje przy każdym zdarzeniu, więc
inicjator nie ma czego dopasować" upada w całości — różne adresy w poprzednich logach to
osobne sesje parowania, bo każde wciśnięcie Fn generuje nowy adres.

To **zaostrza** wniosek o kontrolerze, zamiast go osłabiać: inicjator C6/H2 nasłuchuje ciągle
przez 6 s urządzenia, które w tym czasie nadaje kilkanaście razy pod stałym adresem przy
−40 dBm, i nie kończy połączenia ani razu. C3 i S3 łączą się z tym samym urządzeniem
natychmiast.

Wniosek metodologiczny wart zapamiętania: **narzędzie pomiarowe nie może zmieniać tego, co
mierzy.** Ta pomyłka żyła w notatkach kilka tur i zbudowaliśmy na niej dwie hipotezy (rotacja
adresu, lista akceptacji), obie martwe od początku.

### 4.3 Co przenosimy z OpenLary, a co piszemy inaczej

Źródło: lokalny port OpenLary na ESP32-S3, katalog
`components/OpenLara/src/platform/retrogo/` (projekt zewnętrzny, **tylko do czytania**;
ścieżka u siebie — patrz `AGENTS.local.md`).

| Plik | Co z nim robimy |
|---|---|
| `esp_hid_gap.c` / `.h` | przenosimy; to adaptacja przykładu IDF `esp_hid_host` (wariant NimBLE) |
| `rg_bluetooth.cpp` | **nie przenosimy 1:1** — pętla skanująca obsługuje tylko jedno urządzenie |

Ograniczenie `rg_bluetooth.cpp`: jedna flaga `ble_device_connected` i wyjście z pętli po
pierwszym połączonym urządzeniu. Do mostka potrzebujemy dwóch jednoczesnych połączeń
centralnych, więc piszemy własny moduł. Samo `esp_hidh` obsługuje wiele urządzeń
(lista `esp_hidh_dev_t`), ograniczenie jest w logice aplikacji.

Dekodowanie raportów z `rg_bluetooth.cpp` jest do wzięcia wprost:

- `ESP_HID_USAGE_KEYBOARD`, `length >= 8` → boot protocol: `data[0]` = modyfikatory,
  `data[2..7]` = 6 keycodów USB HID,
- `ESP_HID_USAGE_MOUSE`, `length >= 3` → `data[0]` = przyciski, `data[1]` = dx, `data[2]` = dy
  (oba `int8_t`).

### 4.4 Co było potrzebne, żeby AULA F99 Pro się połączyła

Z `AGENTS.md` OpenLary (2026-07-15), potwierdzone na sprzęcie na ESP32-S3:

- **`filter_duplicates = 0`** w `start_nimble_scan` — klawiatura rozgłasza pakiety ADV bez
  pełnej nazwy w nagłówku głównym, a filtr duplikatów powodował, że nie trafiała na listę
  kandydatów,
- **akceptowanie adresów BLE Random** (`addr_type = 1`) przy wyborze kandydata,
- **`CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR=y`** (patrz §4.1).

### 4.5 Pamięć: brak PSRAM na C3

OpenLara na S3 wrzuca NimBLE do PSRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`).
C3 SuperMini **nie ma PSRAM**, więc ta opcja jest niedostępna — wszystko w internal SRAM.
Nie powinno to boleć: aplikacja to samo BLE, bez Wi-Fi, bez Mattera, bez grafiki.
Dla porównania w projekcie przekaźnika (Matter + Wi-Fi + BLE) `free_heap` spadał do ~24 kB;
tutaj punkt wyjścia jest znacznie luźniejszy. **Do zmierzenia po Etapie 1.**

### 4.6 Trzy jednoczesne połączenia

Potrzebujemy 3 linków: 2× central (klawiatura, mysz) + 1× peripheral (PC).

| Opcja | Wartość | Uwagi |
|---|---|---|
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 3 | domyślnie 3; na C3 limit `range 1 9` |
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | y | domyślnie y |
| `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` | y | domyślnie y |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | 6 | domyślnie 6; pokrywa 3 połączenia + skan + adv |
| `CONFIG_BT_NIMBLE_MAX_BONDS` | 3 | klawiatura + mysz + PC |
| `CONFIG_BT_NIMBLE_NVS_PERSIST` | y | domyślnie **n** — trzeba włączyć, inaczej parowanie ginie po restarcie |

Konfiguracyjnie to przechodzi. **Czy C3 utrzyma dwa połączenia centralne naraz — nie jest
sprawdzone w żadnym z projektów źródłowych.** To główne ryzyko PoC, dlatego Etap 1 sprawdza
właśnie to, przed pisaniem czegokolwiek po stronie pada.

### 4.7 Windows cache'uje deskryptor HID

Windows zapamiętuje Report Map per sparowane urządzenie. Po każdej zmianie deskryptora trzeba
usunąć pada z listy urządzeń Bluetooth i sparować ponownie — inaczej `joy.cpl` pokaże stary
układ osi i przycisków. Warto to robić rzadko: ustalić deskryptor raz i się go trzymać.

## 5. Plan pracy

Etapy 1 i 2 są niezależne — można je budować osobno i scalić na końcu. Ułatwia to diagnozę,
bo w razie problemu wiadomo, która rola zawiodła.

- [x] **Etap 0** — szkielet ESP-IDF (target esp32c3, konsola USB Serial/JTAG), skrypty
      build/flash/monitor, boot potwierdzony na COM6.
- [x] **Etap 1** — tylko host. Klawiatura **i mysz** podłączone jednocześnie, raporty obu
      w logu, rekonekcja po wybudzeniu. **Trzy jednoczesne połączenia BLE potwierdzone**
      (`razem 2/2 urzadzen` + `pad gotowy`), `heap 186960 B`.
- [x] **Etap 2** — pad. Własna usługa HID, syntetyczny wzorzec testowy. Windows paruje,
      `joy.cpl` pokazuje ruch. **Zweryfikowane na sprzęcie 2026-08-16.**
- [x] **Etap 3** — scalenie. Klawiatura **i mysz** → pad, potwierdzone w `joy.cpl`: WASD na
      lewym analogu, ruch myszy na osi Z / obrocie Z, przyciski i kółko działają.
- [x] **Etap 4** — profil XInput. Mostek podaje się za pada Xbox Series X (PID `0x0B13`):
      deskryptor 283 B bajt w bajt z prawdziwego pada, PnP ID z VID Microsoftu, własna
      usługa HID i DIS napisane wprost na GATT (§4.30, §4.31, §4.32).
      **Zweryfikowane na sprzęcie:** `joy.cpl` pokazuje „Urządzenie wejściowe Bluetooth LE
      zgodne z interfejsem XINPUT", Rocket League i Apex Legends obsługują pada, Steam go
      widzi, a Windows przysyła polecenia wibracji (`raport wyjsciowy id=3`).

Do zamknięcia PoC zostaje:

- [x] **weryfikacja łatki z §4.27: cykl uśpienie → powrót myszy bez crashu.** Potwierdzone
      logiem: otwarcie 3 przechodzi, dwa cykle rozłączenia i powrotu w jednym przebiegu.
- [x] **weryfikacja naprawy z §4.28: klawiatura jako pierwsza, mysz jako druga.** Przeszło:
      `conn_handle=4`, `mtu update event … mtu=247`, bez paniki, 180 s i `razem 2/2`.
- [x] **naprawa §4.29: mysz milczała po ponownym połączeniu** (brak szyfrowania linku).
      Potwierdzone logiem: `enc=1 bond=1`, wszystkie `zapis CCCD: status=0`, raporty `MOU`.
- [x] **potwierdzenie, że ruch myszy znowu rusza prawym analogiem.** W logu widać pełny
      łańcuch: `MOU … dx=-18 dy=-2` → `pad: R(-15,0)`, plus przyciski myszy i skos
      klawiatury `L(90,90)` w tym samym przebiegu.
- [x] **dobranie `CONFIG_APP_MOUSE_SCALE_DIV` do gustu** — 8 → 24 po zgłoszeniu, że gałka
      zbyt szybko dobija do maksimum.
- [x] **pomiar i optymalizacja interwałów połączeń** — pad → PC 7,5 ms (133 Hz), wejścia
      15 ms (66 Hz) wobec 45 ms na starcie; 15 ms to udowodniony sufit kontrolera (§4.33).

## 6. Zasady dla agenta

- Projekty referencyjne wymienione w §4.3 (port OpenLary i projekt przekaźnika) są
  **wyłącznie do czytania**. Nie modyfikować ich pod żadnym pozorem. Ścieżki do nich —
  jeśli są lokalnie dostępne — trzymać w `AGENTS.local.md`, nie tutaj.
- Nie wpisywać do §2 niczego, co nie ma dowodu z logu urządzenia. „Kompiluje się" ≠
  „działa" — trzymać te dwie rzeczy w osobnych tabelach.
- Po każdej zmianie deskryptora HID dopisać w logu commita, że wymaga re-parowania w Windows.
- Zmiany w `sdkconfig.defaults` opisywać komentarzem w pliku (dlaczego, nie tylko co).
