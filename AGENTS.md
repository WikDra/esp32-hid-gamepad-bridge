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
| Mysz AJAZZ AJ159 Pro | **jeszcze nie testowana** — dekodowanie osi nadal zgadywane (§4.10) |

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
inaczej 8-bit. **To jest hipoteza, nie wiedza.** Żeby ją rozstrzygnąć danymi, a nie
zgadywaniem, `handle_mouse_report()` przy pierwszych 40 raportach loguje surowe bajty
i obok nich **wszystkie trzy** możliwe interpretacje:

```
MOU map=0 id=5 len=6 [00 12 00 f4 ff 00]
  btn=0x00 | 8-bit(  18,   0) 12-bit(   18,   -1) 16-bit(    18,   -12) | uzywam(18,-12)
```

- **8-bit**: `d[1]`, `d[2]` — klasyczny boot protocol,
- **12-bit**: `X = d1 | (d2 & 0x0F) << 8`, `Y = (d2 >> 4) | d3 << 4` — typowe pakowanie
  w myszach o wyższym DPI,
- **16-bit**: `d[1..2]`, `d[3..4]` little-endian.

Przy powolnym ruchu **w jedną oś** poprawny wariant jest widoczny natychmiast: pozostałe
dwa dają albo śmieci, albo niezerową drugą oś. Po ustaleniu układu heurystykę trzeba
zastąpić twardym dekodowaniem i opisać tu wynik.

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
| `Subscribe complete; status=259` / `269` | `0x103` i `0x10D` to błędy ATT 0x03 (Write Not Permitted) i 0x0D (Invalid Attribute Value Length) — `esp_hidh` próbuje włączyć notyfikacje na charakterystykach, które ich nie mają (raporty OUTPUT/VENDOR). Subskrypcja raportu klawiatury kończy się `status=0` |
| `ogf=0x08, ocf=0x0013, hci_err=0x212` | `HCI_LE_Connection_Update` z parametrami, których kontroler nie przyjął. Połączenie działa dalej z dotychczasowymi parametrami. **Do sprawdzenia przy pomiarach opóźnienia** — jeśli interwał połączenia zostanie duży, to jest pierwszy podejrzany |

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
- [~] **Etap 1** — tylko host. **Klawiatura zweryfikowana na sprzęcie** (połączenie,
      raporty, rekonekcja po wybudzeniu, `heap 192100 B`). Mysz AJ159 Pro nadal nietestowana,
      więc dwa jednoczesne połączenia centralne **nie są jeszcze potwierdzone**.
- [x] **Etap 2** — pad. Własna usługa HID, syntetyczny wzorzec testowy. Windows paruje,
      `joy.cpl` pokazuje ruch. **Zweryfikowane na sprzęcie 2026-08-16.**
- [~] **Etap 3** — scalenie. `input_mapper.c` napisany i wystartowany na płytce
      (`mapowanie wejsc na pada wlaczone (100 Hz, dzielnik myszy 8)`), selftest wyłączony.
      **Weryfikacja end-to-end w `joy.cpl` jeszcze nie zrobiona.**

## 6. Zasady dla agenta

- Projekty `D:\wysypisko\openlara_esp32` i `D:\wysypisko\esp32_przekaznik_czujnik_obecnosci`
  są **wyłącznie do czytania**. Nie modyfikować ich pod żadnym pozorem.
- Nie wpisywać do §2 niczego, co nie ma dowodu z logu urządzenia. „Kompiluje się" ≠
  „działa" — trzymać te dwie rzeczy w osobnych tabelach.
- Po każdej zmianie deskryptora HID dopisać w logu commita, że wymaga re-parowania w Windows.
- Zmiany w `sdkconfig.defaults` opisywać komentarzem w pliku (dlaczego, nie tylko co).
