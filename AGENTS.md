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
