# Mostek BLE HID: klawiatura + mysz → pad na ESP32-C3

Notatki projektu i plan pracy dla kolejnego agenta. Stan na 2026-08-15.

Repo: `https://github.com/WikDra/esp32-hid-gamepad-bridge` (prywatne).
Instrukcja dla człowieka: [`README.md`](README.md).

## 1. Cel

Jeden ESP32-C3 SuperMini pełni jednocześnie dwie role BLE:

- **2× central / GATT client** — odbiera raporty HID z klawiatury AULA F99 Pro i myszy
  AJAZZ AJ159 Pro (profil HOGP, usługa 0x1812),
- **1× peripheral / GATT server** — wystawia PC własną usługę HID 0x1812 z deskryptorem
  raportu pada.

Wejścia są mapowane na osie i przyciski pada, więc z punktu widzenia PC klawiatura i mysz
wyglądają jak jeden kontroler.

Zakres świadomie ograniczony: **brak XInput**, brak emulacji konkretnego pada Xbox. PC widzi
generyczny pad HID (DirectInput / `joy.cpl`).

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
| **Klawiatura podłączona i wysyła raporty** | `KBD len=8 [00 00 07 …]` przy pisaniu — keycody `07`/`04`/`16`/`0b`/`0d` to `d`/`a`/`s`/`h`/`j` |
| Bond klawiatury zapisany w NVS | `bondow w NVS: 2` (PC + klawiatura) po restarcie C3 |
| Pamięć z padem + klawiaturą | `heap 192140 B (min 191192 B)` |
| Klasyfikacja urządzenia (kbd/mouse) | naprawiona: liczona z listy raportów, nie z `esp_hidh_dev_usage_get()` (§4.15). **Do potwierdzenia w logu.** |
| Mysz AJAZZ AJ159 Pro | **jeszcze nie testowana** |

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
| Build | WSL, `~/esp/v5.5.1/esp-idf` (dostępne też v5.3.1, v5.4.2, v5.4.3) |
| Flash + konsola | Windows, `C:\Users\1thew\esp\v5.5.1\esp-idf` |
| Port | COM6, VID:PID `303a:1001` |
| `usbipd-win` | `C:\Program Files\usbipd-win\usbipd.exe`, COM6 już na liście *persisted* |

**IDF musi być ≥ 5.4.3** — patrz §4.1. Wybrane v5.5.1, bo jest po obu stronach (WSL i Windows).

Płytka ma natywne USB, więc konsola idzie przez USB Serial/JTAG:
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` + `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`.

Dwuetapowy workflow (build w WSL, flash z Windows) jest przeniesiony z projektu
`esp32_przekaznik_czujnik_obecnosci`, bo jest sprawdzony. `usbipd attach` do WSL też by
zadziałało i pozwoliłoby na `idf.py flash monitor` w jednym miejscu — zostawione jako opcja,
jeśli dwuetapowość zacznie przeszkadzać.

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

`rg_bluetooth.cpp` z OpenLary zakłada boot protocol: `data[1]`/`data[2]` jako 8-bitowe
`dx`/`dy`. Dla AJAZZ AJ159 Pro **to jest niesprawdzone** — mysz gamingowa o wysokim DPI
zwykle raportuje 12- lub 16-bitowo, a OpenLara nigdy nie potwierdziła myszy na sprzęcie
(w jej `AGENTS.md` jest tylko „powinna działać").

`ble_hid_host.c` rozpoznaje po długości raportu: `len >= 5` → 16-bit little-endian,
inaczej 8-bit. Równolegle loguje surowe bajty (`MOU len=… [hex]`) przy pierwszych ośmiu
raportach i potem raz na sekundę, żeby dało się odczytać prawdziwy układ z logu
i poprawić przed Etapem 3.

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

### 4.16 AULA F99 Pro wysyła serie `ErrorRollOver` po każdym naciśnięciu

Log z płytki przy pisaniu na klawiaturze (`07` = `d`, `04` = `a`, `16` = `s`):

```
KBD len=8 [00 00 07 00 00 00 00 00]
KBD len=8 [00 00 00 00 00 00 00 00]
KBD len=8 [00 00 01 00 00 00 00 00]     <- trzy razy, ~130 ms odstępu
KBD len=8 [00 00 00 00 00 00 00 00]
```

`0x01` w pozycji keycodu to **`ErrorRollOver`** z tabeli USB HID Keyboard/Keypad
(0x01 ErrorRollOver, 0x02 POSTFail, 0x03 ErrorUndefined) — nie klawisz. Bez filtra pad
dostawałby fantomowy przycisk po każdym naciśnięciu.

`handle_keyboard_report()` zeruje keycody 0x01–0x03, a raport złożony wyłącznie z nich
(bez modyfikatorów) ignoruje w całości — nadpisanie stanu zerami zgubiłoby klawisz
naprawdę trzymany. Skąd te raporty się biorą, powie `map=`/`id=` w logu; filtr jest
poprawny niezależnie od źródła, bo wynika ze specyfikacji.

### 4.17 Beacon Swift Pair z Windows w wynikach skanu

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

### 4.3 Co przenosimy z OpenLary, a co piszemy inaczej

Źródło: `D:\wysypisko\openlara_esp32\retro-go\openlara\components\OpenLara\src\platform\retrogo\`
(tylko do czytania — **nie modyfikować tamtego projektu**).

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
- [ ] **Etap 1** — tylko host. Klawiatura **i** mysz połączone jednocześnie, raporty w logu.
      Zmierzyć `free_heap` z dwoma połączeniami.
- [x] **Etap 2** — pad. Własna usługa HID, syntetyczny wzorzec testowy. Windows paruje,
      `joy.cpl` pokazuje ruch. **Zweryfikowane na sprzęcie 2026-08-16.**
- [ ] **Etap 3** — scalenie. Zadanie raportujące ~100 Hz, mapowanie wejść, wysyłka tylko przy
      zmianie stanu. Weryfikacja end-to-end.

## 6. Zasady dla agenta

- Projekty `D:\wysypisko\openlara_esp32` i `D:\wysypisko\esp32_przekaznik_czujnik_obecnosci`
  są **wyłącznie do czytania**. Nie modyfikować ich pod żadnym pozorem.
- Nie wpisywać do §2 niczego, co nie ma dowodu z logu urządzenia. „Kompiluje się" ≠
  „działa" — trzymać te dwie rzeczy w osobnych tabelach.
- Po każdej zmianie deskryptora HID dopisać w logu commita, że wymaga re-parowania w Windows.
- Zmiany w `sdkconfig.defaults` opisywać komentarzem w pliku (dlaczego, nie tylko co).
