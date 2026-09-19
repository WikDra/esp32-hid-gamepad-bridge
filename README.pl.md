# esp32-hid-gamepad-bridge

**Klawiatura i mysz stają się padem Xbox, którego Windows udostępnia przez XInput.**
Dwa transporty, oba zweryfikowane na sprzęcie: po **Bluetooth LE** na jednym układzie ESP32, bez
żadnego okablowania, albo po **USB** jako mostek USB Host → USB Device na dwóch płytkach ESP32-S3,
w którym każdy odcinek chodzi co 1 ms i nie ma żadnego parowania.

| | [Bluetooth LE](#bluetooth-le) | [USB](#usb) |
|---|---|---|
| architektura | 2× central BLE + 1× peripheral BLE, jeden układ | host USB → UART → urządzenie USB, dwa układy |
| układy | **jeden** ESP32 | **dwa** ESP32-S3 |
| urządzenia wejściowe | klawiatura i mysz BLE | dowolna klawiatura i mysz USB albo ich dongle 2,4 GHz |
| co widzi PC | bezprzewodowy pad **Xbox Series X** | przewodowy pad **Xbox 360** |
| parowanie | klawiatura, mysz i pad raz każde | **żadne** |
| działa przed startem Windows | nie | **tak**, także w ekranach konfiguracyjnych firmware'u |
| tempo raportów pada | 133 Hz | **1 kHz** (zmierzone 997 Hz) |
| opóźnienie dodane przez mostek | ≤ 15 ms | **≤ 2 ms** |
| dodatkowy sprzęt | żaden, jeden kabel USB-C | zasilany hub, przejściówka USB-UART, trzy druty |
| strojenie i remapowanie | przebudowa i wgranie | [panel WWW](#panel-konfiguracyjny-po-wi-fi), na żywo, plus aktualizacja firmware |

**Zmierzone na sprzęcie, nie deklarowane:**

- **Ogrywane w Apex Legends**, a Rocket League i test kontrolera w Steam widzą pada jako zwyczajny
  kontroler XInput. Windows ładuje w obu transportach **własny** sterownik pada Xbox i przysyła nam
  wibracje — co robi wyłącznie ten sterownik.
- **1 kHz na całej drodze po USB.** Dongle jest odpytywany co 1 ms, ramka na łączu UART zajmuje
  108 µs, a endpoint IN pada jest odpytywany co 1 ms; **997 Hz** zmierzone przez `XInputGetState`
  przyrządem, który usuwa z pomiaru rękę. Dodane opóźnienie to jeden okres zadania plus jedno
  odpytanie, czyli **≤ 2 ms**.
- **Zero parowania po USB**, a pad działa przed startem Windows — dla komputera jest zwyczajnym
  przewodowym kontrolerem.

Liczby **66 Hz / 133 Hz**, które pojawiają się dalej, dotyczą transportu **Bluetooth**, gdzie
interwał jest negocjowany w eterze, a kontroler w roli centrala odmawia zejścia poniżej 15 ms
([dlaczego](#znane-ograniczenia)). USB nie jest tym w żaden sposób ograniczone.

Naciśnięcia klawiszy i ruch myszy są mapowane na gałki, spusty, krzyżak i przyciski, więc
z punktu widzenia PC jest to jeden kontroler. Tabela mapowania, krzywa myszy i wszystkie
osobliwości wejść są **wspólne dla obu transportów**; wymieniane w czasie kompilacji są tylko dwa
końce.

*English version of this document: [`README.md`](README.md). Notatki techniczne, ustalenia
i pułapki, z dowodem przy każdej decyzji: [`AGENTS.md`](AGENTS.md).*

## Spis treści

- [Stan projektu](#stan-projektu)
- [Bluetooth LE](#bluetooth-le) — jeden układ, trzy połączenia
  - [Sprzęt i obsługiwane układy](#sprzęt-i-obsługiwane-układy)
  - [Parowanie](#parowanie)
  - [Dwa profile pada](#dwa-profile-pada)
  - [Opcjonalnie: podział na dwa układy](#opcjonalnie-podział-na-dwa-układy)
- [USB](#usb) — dwa układy, zero parowania
  - [Czego to wymaga](#czego-to-wymaga)
  - [Połączenia i wgrywanie](#połączenia-i-wgrywanie)
  - [Passthrough na skrót klawiaturowy](#passthrough-na-skrót-klawiaturowy)
  - [Panel konfiguracyjny po Wi-Fi](#panel-konfiguracyjny-po-wi-fi)
- [Mapowanie wejść](#mapowanie-wejść) — wspólne dla obu transportów
- [Tempo raportów](#tempo-raportów) — wszystkie zmierzone liczby w jednym miejscu
- [Wymagania i budowanie](#wymagania-i-budowanie)
- [Znane ograniczenia](#znane-ograniczenia)
- [Diagnostyka](#diagnostyka)
- [Licencja i pochodzenie](#licencja-i-pochodzenie)

## Stan projektu

Oba transporty są zweryfikowane end-to-end. Dowód przy każdym twierdzeniu, transport po
transporcie:

**Bluetooth LE:**

- **XInput działa.** Windows ładuje własny sterownik pada Xbox i pokazuje urządzenie jako
  *„Urządzenie wejściowe Bluetooth LE zgodne z interfejsem XINPUT"*. Przysyła nam nawet polecenia
  wibracji, co robi wyłącznie sterownik pada.
- **Trzy jednoczesne połączenia BLE** na jednym układzie, z zapasem ~180 kB heapu na C3
  i ~250 kB na S3.
- **Cykle uśpienia i powrotu** urządzeń wejściowych działają: rozłączenia są wykrywane, zasoby
  zwalniane, a urządzenie wraca samo.
- **Zweryfikowane end-to-end na ESP32-C3 i ESP32-S3.** C6 i H2 budują się i działają, obsługują
  pada i mysz, ale nie łączą się z naszą klawiaturą testową — patrz
  [Znane ograniczenia](#znane-ograniczenia).

**USB:**

- **Wiąże się ten sam sterownik XInput**, tylko dopasowany po `USB\Vid_045E&Pid_028E`:
  `Service=xusb22`, a `XInputGetState` widzi pada w slocie 0.
- **Ruch idzie w obie strony** — trzy celowo różne pary wartości `XInputSetState` dały trzy zgodne
  linie `rumble from host` w logu płytki.
- **Cały łańcuch zmierzony z dwóch stron jednocześnie:** `usb ifaces 4 (kbd=1 mouse=1)` oraz
  `link: sent 19884 frames (dropped 0)` na układzie wejść, wobec `w` → `L=(0,32767)` i
  `a` → `L=(-32767,0)` odczytanych przez XInput w PC.
- **Przyciski, spusty i krzyżak** potwierdzone testem kontrolera w Steam, a całość **ogrywana
  w Apex Legends**.

Dojście do tego wymagało naprawienia dziewięciu osobnych wad w komponencie `esp_hid` z ESP-IDF
i obejścia dwóch ograniczeń gotowych usług NimBLE. Wszystko jest w [`AGENTS.md`](AGENTS.md)
razem z dowodem, który doprowadził do każdego wniosku.

---

## Bluetooth LE

Jeden układ utrzymuje **trzy jednoczesne połączenia BLE**: dwa jako central, odbierając raporty
z klawiatury i myszy, oraz jedno jako peripheral, wystawiając pada do PC.

```
BLE HID keyboard ─┐
                  ├─→ ESP32 ─→ BLE HID Gamepad ─→ PC (Windows)
BLE HID mouse   ──┘   (2× central + 1× peripheral)
```

### Sprzęt i obsługiwane układy

| Element | Uwagi |
|---|---|
| ESP32-C3 SuperMini | 4 MB flash, **brak PSRAM**, natywne USB (USB Serial/JTAG) — nie ma mostka USB-UART |
| klawiatura BLE | rozwijane przy AULA F99 Pro w trybie BLE 5.0 |
| mysz BLE | rozwijane przy AJAZZ AJ159 Pro w trybie BLE 5.0 |

Płytka podłączona do PC jednym kablem USB-C, który ją zasila, służy do wgrywania i przenosi
konsolę. Żadnego dodatkowego okablowania.

Inne klawiatury i myszy powinny działać: nic w kodzie nie jest specyficzne dla tych dwóch modeli
poza rozpoznawaniem układu raportu, a ono opiera się na mapach raportów samych urządzeń. Dwie
osobliwości, które trzeba było obsłużyć, są opisane (`AGENTS.md` §4.10 i §4.17) i obie są
obsłużone ogólnie.

Każdy target buduje się z tych samych źródeł; jedyny plik zależny od układu to
`firmware/sdkconfig.defaults.<target>`.

| Target | Kontroler | Stan |
|---|---|---|
| `esp32c3` | stara rodzina (`BT_CTRL_*`) | **platforma odniesienia**, wszystko zweryfikowane |
| `esp32s3` | stara rodzina, dzieli bibliotekę kontrolera z C3 | **w pełni zweryfikowane**: klawiatura, mysz, pad, XInput |
| `esp32c6` | nowa rodzina (`BT_LE_*`) | pad i mysz działają; nasza klawiatura testowa nie łączy się |
| `esp32h2` | nowa rodzina (`BT_LE_*`) | jak wyżej; ten układ nie ma Wi-Fi, co dla mostka BLE jest zaletą |

Problem z klawiaturą na nowszych kontrolerach nie wynika z konfiguracji ani z siły sygnału —
został doprowadzony do warstwy łącza trace'em HCI. Patrz [Znane ograniczenia](#znane-ograniczenia).

### Parowanie

Kolejność ma znaczenie — najpierw wejścia, potem PC:

1. Wgraj firmware i zostaw płytkę zasiloną.
2. Wprowadź klawiaturę w tryb parowania BLE. Mostek skanuje w pętli i połączy się sam.

   Windows też zobaczy klawiaturę i zaproponuje sparowanie — **odrzuć to okno.** Jeśli klawiatura
   sparuje się z Windows, połączy się tam, a nie z mostkiem.
3. To samo z myszą.
4. W PC: *Ustawienia → Bluetooth → Dodaj urządzenie*. W profilu Xbox mostek rozgłasza się jako
   **Xbox Wireless Controller**.
5. Sprawdź, czy Windows podpiął właściwy sterownik. W `joy.cpl` (Win+R → `joy.cpl`) urządzenie ma
   się nazywać *„Urządzenie wejściowe Bluetooth LE zgodne z interfejsem XINPUT"*. Jeśli widzisz
   *„Kontroler gier zgodny z HID"* albo *„6-osiowy 17-przyciskowy pad"*, podpiął się sterownik
   generyczny — diagnoza w `AGENTS.md` §4.32.

Klucze parowania są w NVS, więc po restarcie wszystko wraca samo.

Jeśli w logu pojawi się `esp_hidh_dev_open() nie wrocilo w 45 s`, mostek **restartuje się sam**.
Obchodzi to błąd w ESP-IDF, w którym zerwanie linku w trakcie odkrywania usług zawiesza wołające
zadanie na zawsze (`AGENTS.md` §4.23). PC wraca po ~2 s, a urządzenia wejściowe łączą się same.

**Po każdej zmianie deskryptora HID trzeba usunąć pada z listy urządzeń Bluetooth w Windows
i sparować od nowa** — Windows cache'uje deskryptor per sparowane urządzenie.

### Dwa profile pada

Tylko dla Bluetootha; transport USB zawsze wystawia pada Xbox 360. Wybierane w `menuconfig`
(`APP_GAMEPAD_PROFILE`):

| Profil | Co widzi PC |
|---|---|
| **Xbox (XInput)** — domyślny | Mostek podaje się za bezprzewodowy pad Xbox Series X: deskryptor raportu bajt w bajt z prawdziwego pada, a PnP ID nosi VID Microsoftu z PID `0x0B13`. Windows ładuje wtedy swój sterownik pada Xbox i udostępnia urządzenie przez **XInput**, więc widzą go też gry obsługujące wyłącznie XInput. |
| generyczny (DirectInput) | 4 osie i 12 przycisków, widoczne w `joy.cpl`. Gry korzystające tylko z XInput takiego pada nie zobaczą. Zostaje jako wyjście awaryjne. |

Sterownik XInput dostają wyłącznie PID `0x0B13` i `0x0B20`–`0x0B27` — Windows dopasowuje po samym
VID/PID, a starszy PID Xbox One S `0x02FD` **nie jest** na tej liście (`AGENTS.md` §4.32; kosztowało
to pełną rundę diagnozy). Przełączenie profilu zmienia i deskryptor, i tożsamość urządzenia, więc
pada trzeba usunąć z Windows i sparować od nowa.

### Opcjonalnie: podział na dwa układy

Na płytce z dwoma układami połączonymi na PCB — **ESP Thread Border Router**, gdzie siedzą
ESP32-S3 i ESP32-H2 — mostek może chodzić rozdzielony na oba:

```
klawiatura BLE ──→ S3 (central) ─┐
                                 ├─→ pad BLE ──→ PC     (S3 jest peryferialem)
mysz BLE ────────→ H2 (central) ──→ UART ──→ S3
```

**Sens nie jest w szybkości drutu.** Ramka myszy ma 10 bajtów, czyli przy 921600 bodach ~108 µs,
wobec interwału połączenia BLE 7,5–15 ms: cztery rzędy wielkości różnicy, więc transport nie jest
składnikiem opóźnienia. Zyskiem jest **czas radia** — każdy układ obsługuje mniej połączeń,
zamiast jednej anteny przeplatającej klawiaturę, mysz i pada.

Obchodzi to przy okazji ograniczenie z klawiaturą: mysz działa bez zarzutu na nowszej rodzinie
kontrolerów, a klawiatura wymaga starszej, więc każde urządzenie trafia na układ, który je
obsługuje. Tempo każdego odcinka jest w [Tempie raportów](#tempo-raportów).

Buduj i wgrywaj każdy układ z jego własnym targetem; role wynikają z ustawień per target:

```bat
scripts\build-native-win.bat esp32s3
scripts\flash-win.bat COM10 esp32s3     REM gospodarz: klawiatura + pad
scripts\build-native-win.bat esp32h2
scripts\flash-win.bat COM11 esp32h2     REM satelita: mysz
```

Sparuj mysz z H2, klawiaturę z S3, a pada z PC. PC widzi jeden kontroler i żadnego śladu tego, że
mysz jest na innym radiu.

Dwie rzeczy warte wiedzy przy przenoszeniu tego na inną płytkę:

- **Pin połączenia został zmierzony, nie odczytany z dokumentacji.** Przykład `ot_br` z ESP-IDF ma
  na sztywno GPIO4/5, ale jego README pokazuje to jako wiring DevKit do DevKitu i dla tej płytki
  jest błędne. `APP_LINK_PROBE_RX` przemiata piny wejściowe i pokazuje, na którym pojawiają się
  ramki z poprawnym CRC; na płytce BR odpowiedź to **S3 GPIO17 ← H2 GPIO24**. Łącze jest
  jednokierunkowe, więc TX po stronie gospodarza zostaje nieprzypisany.
- **Konsola H2 musi zejść z UART.** Łącze ląduje na domyślnych pinach UART0 tego układu, więc
  konsola na UART pisałaby tekst logu po tym samym drucie. Płytka daje każdemu układowi własne
  gniazdo USB, więc konsola idzie po USB Serial/JTAG.

Łącze niesie ramkowany protokół z CRC i keepalive, dzięki czemu **cisza jest informacją**: gdy
satelita zniknie, gospodarz zwalnia trzymany przycisk myszy, a pad i klawiatura pracują dalej.
Zmierzone na 10 951 ramkach, zero błędów CRC.

---

## USB

Dwie płytki ESP32-S3, połączone trzema drutami, przyjmują urządzenia wejściowe USB po jednej
stronie i wystawiają **przewodowego pada Xbox 360** po drugiej. Żadnego Bluetootha:

```
dongle klawiatury 2,4 GHz ─┐                                       ┌─ pad Xbox 360 (XInput) ─→ PC
                           ├─→ hub ─→ [S3 #1] ────UART────→ [S3 #2] ┘
dongle myszy 2,4 GHz ──────┘         host USB          urzadzenie USB
```

Windows wiąże `xusb22`, ten sam sterownik XInput co w profilu Bluetooth, tylko dopasowany po
`USB\Vid_045E&Pid_028E`. **Nie ma żadnego parowania**, a pad działa od chwili wetknięcia kabla.

**XInput po USB to w ogóle nie HID**, więc nic z pracy nad Bluetoothem się tu nie przenosi: to
interfejs vendorowy (klasa `0xFF`, podklasa `0x5D`, protokół `0x01`) z dwoma endpointami interrupt
i bez żadnego deskryptora raportu. `xinputhid.inf`, który wiąże pada BLE, ma **zero** wpisów
`USB\`, więc urządzenie USB HID z PID `0x0B13` dostałoby sterownik generyczny. Deskryptor
interfejsu jest tu bajt w bajt taki jak w prawdziwym padzie, sprawdzany wobec przechwytu Wireshark
przez `scripts/check_xinput_descriptor.py` — i to na **zbudowanej binarce**, nie na źródle.

### Czego to wymaga

- **ESP32-S3, -S2 albo -P4 na każdy koniec.** C3 nie ma peryferium USB-OTG, więc tego nie zrobi.
- **Przejściówka USB-UART na konsolę.** Obie płytki mają zajęte gniazdo USB, a na S3 peryferium
  USB Serial/JTAG dzieli GPIO19/20 z USB-OTG, więc konsola musi zejść na UART. Jedna przejściówka
  wystarcza — przekłada się między płytkami.
- **Zasilany hub** na urządzenia wejściowe i przejściówka USB-C na USB-A (OTG) do płytki hosta.
- **Trzy druty między płytkami:** `GPIO4` → `GPIO5` (skrzyżowane) i wspólna masa.

Urządzenia wejściowe można wetknąć wprost albo, jak w zestawie, na którym to powstawało, przez ich
**dongle 2,4 GHz** — co pozostawia klawiaturę i mysz bezprzewodowe i pobiera znacznie mniej prądu,
bo dongle ani nie świeci, ani nie ładuje akumulatora.

### Połączenia i wgrywanie

Pełna rozpiska, z układem listwy, arytmetyką zasilania i wszystkimi pułapkami z uruchamiania:
**[`docs/WIRING-USB.pl.md`](docs/WIRING-USB.pl.md)**
(wersja angielska: [`docs/WIRING-USB.md`](docs/WIRING-USB.md)).

```bat
scripts\build-native-win.ps1 esp32s3 s3input
scripts\flash-win.ps1  COM<n> esp32s3 s3input
scripts\build-native-win.ps1 esp32s3 s3pad
scripts\flash-win.ps1  COM<m> esp32s3 s3pad
```

Dwie rzeczy o wgrywaniu tych płytek, obie wyglądają na awarię i nią nie są:

- **Cykl to BOOT+RESET → wgranie → RESET.** Twardy reset, który esptool wykonuje po zapisie, nie
  wyprowadza układu z trybu download, a objaw jest nieodróżnialny od martwego firmware'u, dopóki
  nie sprawdzi się tego przez `esptool --before no-reset flash-id` — bo to polecenie przechodzi
  wyłącznie wtedy, gdy układ siedzi w bootloaderze.
- **Gdy aplikacja wystartuje, przejmuje piny USB, więc port COM znika.** To dowód, że pad działa,
  nie awaria.

### Passthrough na skrót klawiaturowy

`Ctrl+Alt+G` na klawiaturze przełącza układ pada między padem a zwykłą klawiaturą i myszą HID, żeby
tych samych urządzeń dało się używać do pisania bez odłączania czegokolwiek. Zweryfikowane w obie
strony:

| Tryb | Tożsamość | Co wiąże Windows |
|---|---|---|
| gamepad | `045E:028E` | `xusb22`, slot 0 XInput |
| passthrough | `303A:4004` | `kbdhid` i `mouhid` na dwóch kolekcjach raportów |

**Muszą to być dwie tożsamości, a nie jedno urządzenie złożone.** `xusb22` wiąże się na poziomie
*urządzenia*, nie interfejsu — prawdziwy pad Xbox 360 ma cztery interfejsy i sterownik bierze je
wszystkie, więc interfejsy klawiatury i myszy pod tym samym VID/PID zostałyby przez niego
zagarnięte i nigdy nie dotarłyby do Windows jako urządzenia wejściowe. Układ odłącza się zatem,
podmienia deskryptory i wylicza od nowa, co znaczy, że **pad znika na czas passthrough**. To
przyjęty koszt; szczegóły projektowe w `AGENTS.md` §4.39.

Klawisz ustawia `APP_PASSTHROUGH_KEYCODE`, a całą funkcję wyłącza `APP_USB_PASSTHROUGH`.

### Panel konfiguracyjny po Wi-Fi

`Ctrl+Alt+W` podnosi panel WWW do zmiany czułości, wygładzania, kompensacji martwej strefy,
mapowania klawiszy i profili — oraz do aktualizacji firmware'u. **Wi-Fi jest wyłączone do tego
skrótu i gasi się samo po dziesięciu minutach bezczynności**, i to właśnie czyni tę funkcję
darmową: nic nie konkuruje ze ścieżką raportów 1 kHz w trakcie gry, więc zmierzone 997 Hz pozostaje
liczbą o tym firmware, a nie o radiu.

| Skrót | Co robi |
|---|---|
| `Ctrl+Alt+W` | włącza i wyłącza Wi-Fi razem z panelem |
| `Ctrl+Alt+P` | wymusza własny punkt dostępowy i podnosi Wi-Fi, jeśli było wyłączone |

Bez zapisanych danych sieci mostek serwuje punkt dostępowy `hid-bridge-XXXX`, gdzie `XXXX` pochodzi
z MAC. **Hasło jest wypisywane w konsoli przy starcie** i — o ile nie ustawisz
`APP_WEBUI_PASSWORD` — wyprowadzane z MAC, czyli unikalne dla płytki, a nie wspólne dla każdej
kopii tego repozytorium. To jednocześnie klucz WPA2 i hasło panelu; użytkownik panelu to `admin`.

Podaj w zakładce System swoją sieć i mostek dołączy do niej, zdejmując własny AP — panel siedzi
wtedy pod stabilnym adresem, osiągalnym z telefonu, który już jest w tej sieci. `Ctrl+Alt+P` to
droga powrotna, gdyby tamta sieć była nieosiągalna; bez niej do panelu, do którego nie ma dostępu,
trzeba by sięgnąć po przejściówkę szeregową.

Co można zmienić:

- **Mysz na prawą gałkę:** czułość osobno na każdą oś, stała czasowa wygładzania, inwersja Y oraz
  kompensacja martwej strefy — podnosi każde niezerowe wychylenie powyżej wewnętrznej strefy, którą
  gry odrzucają, i działa na **długości** wektora, więc skosy nie przestrzeliwują. Zmiany działają
  natychmiast, czyli nastawa oceniana wyczuciem nie wymaga już przebudowy i wgrywania.
- **Mapowanie**, jako tabela: wciskasz *Listen*, potem klawisz na klawiaturze obsługiwanej przez
  mostek, i przypisanie jest gotowe. Każdy nominalny przycisk jest opisany kontrolką Xbox, którą
  steruje.
- **Cztery profile w NVS**, z nazwami, z eksportem i importem jako plik JSON.
- **Firmware**, przez wgranie `hid_gamepad_bridge.bin`. Obraz jest weryfikowany przed jakimkolwiek
  restartem i potwierdzany dopiero po tym, jak nowy firmware utrzyma się 30 s — więc taki, który
  się wywala albo wpada w pętlę restartów, wycofuje się sam.

Panel pokazuje też stan pada na żywo, surowe wejścia z klawiatury i myszy przed mapowaniem oraz
tempo raportów mierzone po stronie urządzenia — tę samą liczbę, którą `scripts/xinput_rumble.py
--rate` odczytuje od strony PC.

> **Dwie rzeczy do wiedzenia, zanim się na tym oprzesz.**
>
> **Nie ma szyfrowania.** Uwierzytelnianie to HTTP Basic, czyli base64, nie szyfrogram. Na punkcie
> dostępowym eter osłania WPA2; po dołączeniu do Twojej sieci ruch panelu idzie tą siecią jawnie.
> Hasło i tak ma znaczenie — to API zmienia to, co raportuje pad, i wgrywa firmware, więc bez hasła
> oddawałoby jedno i drugie każdemu, kto ma dostęp.
>
> **Wariant pada używa innej tablicy partycji**
> ([`firmware/partitions.csv`](firmware/partitions.csv)), bo aktualizacja firmware'u potrzebuje
> dwóch slotów aplikacji. To przesuwa NVS, więc pierwsze wgranie kasuje, co tam było — co tutaj nic
> nie kosztuje, bo ten wariant nie ma bondów Bluetooth. Jeśli aktualizujesz istniejący klon,
> **usuń raz `firmware/sdkconfig.win.esp32s3.s3pad`**: wartość już obecna w wygenerowanym
> sdkconfigu wygrywa z plikiem defaults, więc inaczej build po cichu zostawi starą
> jednoslotową tablicę i aktualizacje będą padać. Firmware ostrzega o tym w logu przy starcie.

Włącza się opcją `APP_WEBUI`, **na obu układach** — panel prowadzi układ pada, a skróty widzi tylko
układ wejść, bo tam jest klawiatura.

---

## Mapowanie wejść

Wspólne dla obu transportów. W buildzie Bluetooth znaczenie zależy od wybranego profilu; po USB
zawsze obowiązuje kolumna Xbox.

| Wejście | Profil Xbox (XInput) | Profil generyczny |
|---|---|---|
| `W` / `S` / `A` / `D` | lewy analog | lewy analog |
| strzałki | **krzyżak** | nieużywane |
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
| `Tab` | View | przycisk 11 |
| `Esc` | Menu | przycisk 12 |

Skosy na lewym analogu są skalowane tak, żeby ruch po skosie nie był szybszy niż na wprost. Prawy
analog dostaje wygładzony i przeskalowany przyrost myszy i wraca do środka, gdy mysz się zatrzyma.

Dwie rzeczy warte wiedzy przy testowaniu profilu Xbox:

- **Spusty są analogowe, nie przyciskowe.** Kliknięcie myszą wychyla jeden do maksimum, ale
  w siatce przycisków `joy.cpl` nic się nie zaświeci — spust pokazuje się jako oś. Log płytki mówi
  to wprost: `xbox: LT=1023 RT=0 …`.
- **Krzyżak istnieje tylko w profilu Xbox.** Deskryptor generyczny nie ma hat switcha.

Przypisanie siedzi w jednej tablicy, `s_xbox_ctrl` w
[`firmware/main/ble_gamepad.c`](firmware/main/ble_gamepad.c). `input_mapper` nie wie, który profil
ani transport jest aktywny, więc zmiana mapowania nie rusza strony wejściowej.

**Czułość myszy ustawia `APP_MOUSE_SCALE_DIV`** (większa wartość = mniejsza czułość). Pełne
wychylenie gałki odpowiada prędkości myszy `div * 400` zliczeń **na sekundę** — na sekundę, nie na
tik zadania, więc odczucie nie zmienia się przy zmianie tempa raportów. Domyślne 24 to 9 600
zliczeń/s; wariant pada USB podnosi to do 64, bo przy 1 kHz z pełną rozdzielczością filtra
9 600 zliczeń/s to jeden ruch nadgarstkiem na myszy o dużym DPI. Nastaw się na kilka prób:
użyteczna wartość zależy od DPI myszy, czego firmware nie zna. Zmiana wymaga tylko przebudowania,
nigdy ponownego parowania.

Interwały połączeń też są konfigurowalne, dla transportu Bluetooth: `APP_PAD_CONN_ITVL` dla linku
do PC i `APP_INPUT_CONN_ITVL` dla linków wejściowych, oba w jednostkach 1,25 ms. Firmware
przechodzi drabinką od zadanej wartości w górę, bierze najkrótszy interwał, który kontroler
przyjmie, i loguje każdą próbę.

## Tempo raportów

Wszystkie liczby dotyczące tempa w tym projekcie, zmierzone, a nie założone. **Bluetooth, jeden
układ:**

| Odcinek | Tempo | Kto o tym decyduje |
|---|---|---|
| klawiatura / mysz → ESP32 | 15 ms = **66 Hz** | podłoga kontrolera w roli centrala — patrz [Znane ograniczenia](#znane-ograniczenia) |
| ESP32 → PC (pad) | 7,5 ms = **133 Hz** | Windows, który jest centralem tego linku |

**Bluetooth, podzielony na dwa układy** — mysz zyskuje, bo nowszy kontroler H2 potrafi zainicjować
krótszy interwał:

| Odcinek | Tempo |
|---|---|
| mysz → H2 | 7,5 ms = **133 Hz** |
| H2 → S3 po UART | 108 µs na ramkę — nie jest ogranicznikiem |
| klawiatura → S3 | 15 ms = 66 Hz (133 Hz, gdy klawiatura sama o to poprosi) |
| S3 → PC (pad) | 7,5 ms = **133 Hz** |

**USB:**

| Odcinek | Tempo | Kto o tym decyduje |
|---|---|---|
| dongle → układ wejść | **~750 Hz** | `bInterval` dongle'a, czyli odpytywanie co 1 ms |
| układ wejść → układ pada | 108 µs na ramkę przy 921600 bodach | nie jest ogranicznikiem — ~11 % drutu przy 1000 ramkach/s |
| układ pada → PC | **997 Hz** sufitu transportu; 830 Hz *odczytane* z realną myszą | interwał 1 ms endpointu IN |

Te dwie ostatnie liczby znaczą różne rzeczy i ta różnica jest ważna. Oba pomiary od strony PC przez
`scripts/xinput_rumble.py --rate`, w pętli dość szybkiej, by sama nie była ograniczeniem
(415 000 odpytań na sekundę):

- **997 Hz** to sufit transportu, zmierzony przy `APP_DEBUG_PAD_RATE_PROBE`, który wymusza inny
  raport w każdym tiku. Nie zależy od niczyjej ręki, więc to liczba, na której można stać.
- **830 Hz** to realna mysz przy energicznym ruchu. Powolny ruch daje znacznie mniej — zmierzone
  70 Hz — i to jest poprawne, nie zepsute: raport idzie tylko na zmianie stanu, a filtr ma
  z założenia utrzymywać **stałe** wychylenie przy stałej prędkości myszy. Liczba aktualizacji
  mierzy więc zmienność wartości gałki, nie jakość łańcucha.

Doprowadziły do tego trzy poprawki arytmetyki w mapperze, żadna w USB: stała czasowa filtra
i czułość były wyrażone w tikach, nie w czasie, więc podniesienie tempa po cichu zmieniało
odczucie; akumulator stałoprzecinkowy był za gruby i przy 1 kHz utykał; a wynik był obcinany do
całych zliczeń myszy na tik, co przy 1 kHz zostawiało trzy użyteczne poziomy. Liczby przed i po są
w `AGENTS.md` §4.40.

**Dwie pułapki, jeśli będziesz te ustawienia zmieniać.** `pdMS_TO_TICKS()` zaokrągla w dół do
całych tików, więc okres raportu krótszy niż jeden tik FreeRTOS po cichu staje się jednym tikiem —
przy domyślnym tiku 100 Hz okres 1 ms staje się 10 ms. Dlatego wariant pada USB ustawia
`CONFIG_FREERTOS_HZ=1000`, a mapper loguje tempo, które **faktycznie osiąga**, a nie to, o które go
poproszono. Oraz `APP_XINPUT_EP_INTERVAL_MS` przy 1 ms jest **jedynym polem, w którym deskryptor
USB przestaje być zgodny bajt w bajt** z prawdziwym padem; jest to bezpieczne, bo `xusb22.inf`
dopasowuje po samym VID/PID i deskryptora nie czyta, a `check_xinput_descriptor.py` zgłasza to
odstępstwo, zamiast milcząco przepuszczać. Powrót na 4 daje dokładną zgodność przy 250 Hz.

## Wymagania i budowanie

- **ESP-IDF v5.5.1** dla transportu Bluetooth. Nie starszy: `CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR` nie
  istnieje przed v5.4.3, a bez niego klawiatura nie odda raportów (`AGENTS.md` §4.1). IDF 6.1
  buduje wszystkie warianty po jednej poprawce przenośności, ale patrz uwaga o załatanym
  komponencie niżej.
- Windows do wgrywania i konsoli. Budować można natywnie w Windows albo w WSL.
- Python z `pyserial` do skryptów konsoli. Systemowy Python zwykle go nie ma; ten z ESP-IDF ma,
  a `scripts\monitor-win.bat` znajduje go sam.

Ścieżki do ESP-IDF domyślnie `%USERPROFILE%\esp\v5.5.1\esp-idf`, nadpisywane przez
`set IDF_WIN=D:\esp\v5.5.1\esp-idf`.

> **Projekt zawiera załataną kopię komponentu ESP-IDF.**
> W `firmware/components/esp_hid/` leży kopia komponentu `esp_hid` z jednolinijkową poprawką błędu,
> który uniemożliwiał ponowne połączenie urządzenia po jego uśpieniu: `services_discovered` nie
> było zerowane, więc trzecie otwarcie urządzenia psuło stos wołającego (`AGENTS.md` §4.27). Kopia
> jest **przypięta do IDF 5.5.1** i po zmianie wersji IDF trzeba ją odtworzyć;
> `firmware/components/esp_hid/PATCH.diff` zawiera samą różnicę. Że build użył naszej kopii, a nie
> wersji z IDF, sprawdza `python scripts/check_local_esp_hid.py`.

Natywnie w Windows, bez WSL:

```bat
scripts\build-native-win.bat              REM build dla esp32c3 (domyslny target)
scripts\build-native-win.bat esp32s3      REM ...albo dowolnego innego
scripts\build-native-win.bat menuconfig   REM konfiguracja
scripts\flash-win.bat COM6                REM wgranie
scripts\monitor-win.bat COM6 30           REM konsola na 30 s, bez resetu plytki
scripts\monitor-win.bat COM6 30 reset     REM ...i ze swiadomym resetem
scripts\reboot-win.bat COM6               REM restart bez otwierania konsoli
scripts\erase-win.bat COM6 esp32c3        REM wyczyszczenie ukladu, razem z kluczami parowania
```

Są też odpowiedniki `.ps1` skryptów budowania i wgrywania, a warianty USB ich wymagają: nowszy
**instalator EIM** ESP-IDF umieszcza środowisko Pythona tam, gdzie `export.bat` nie zagląda, więc na
takiej instalacji każdy skrypt `.bat` kończy się na `'idf.py' is not recognized`.
`scripts/idf-env.ps1` rozumie oba układy, a na nim stoją `build-native-win.ps1` i `flash-win.ps1`.
Przyjmują też nazwę **wariantu**, czym jeden target obsługuje dwie role:

```powershell
scripts\build-native-win.ps1 esp32s3 s3pad
scripts\flash-win.ps1 COM5 esp32s3 s3input -Uart   # -Uart: wgranie przez przejsciowke USB-UART
```

Albo build w WSL, wgrywanie z Windows:

```bat
scripts\build-win.bat esp32c3
scripts\flash-win.bat COM6 esp32c3
```

Obie drogi używają **osobnych katalogów build** (`build.esp32c3` dla WSL, `build.win.esp32c3` dla
Windows), bo ścieżki absolutne są w nich różne, a CMake nie zniesie obu w jednym.
`flash-win.bat` bierze ten obraz, który zbudowano później.

Dwie wersje ESP-IDF mogą współistnieć — tak ten projekt porównywał zachowanie kontrolera między
wydaniami. `scripts/build.sh` przyjmuje ścieżkę do IDF i sufiks katalogu build:

```bash
IDF_DIR=~/esp/v6.0.2/esp-idf BUILD_SUFFIX=.idf602 ./scripts/build.sh esp32h2
```

Daje to `build.esp32h2.idf602` i własny `sdkconfig`, nie ruszając zwykłego builda. Wgranie takiego
obrazu wymaga `esptool` wprost — skrypty szukają tylko `build.<target>` i `build.win.<target>`;
offsety są w `flasher_args.json`.

Na płytce z natywnym USB linie DTR/RTS sterują resetem i bootloaderem, dlatego `monitor.py` otwiera
port z wyłączonymi oboma i nie zrestartuje układu, a `reset_monitor.py` resetuje świadomie.

## Znane ograniczenia

> **Arytmetyka przeliczania myszy na gałkę zmieniła się po tym, jak wariant Bluetooth był
> ostatnio weryfikowany na sprzęcie, i ta zmiana jest po BLE NIEPRZETESTOWANA.**
> `input_mapper.c` wyraża teraz stałą czasową filtra i czułość w czasie, a nie w tikach zadania,
> i nie obcina już wyniku do całych zliczeń myszy na tik (`AGENTS.md` §4.40). Zmierzone zostało to
> wyłącznie na padzie USB.
>
> Przy tempie 100 Hz, którego używa wariant BLE, nominalna czułość i stała czasowa wychodzą
> identyczne jak w wartościach dobranych ręcznie w `AGENTS.md` §4.22. Różni się rozdzielczość:
> drobne ruchy liczą się teraz proporcjonalnie, zamiast być zaokrąglane do zera, więc prawa gałka
> będzie bardziej precyzyjna i możliwie żywsza niż dotąd.
>
> **Jeśli wariant BLE zacznie się dziwnie zachowywać, wróć do commita `c25c017`** — to ostatni
> commit z arytmetyką dokładnie w takim stanie, w jakim mostek Bluetooth był weryfikowany
> end-to-end, i zawiera już całą pracę nad USB:
>
> ```
> git checkout c25c017          # zobaczyc
> git revert 9a5f535            # albo cofnac sama te zmiane na galezi
> ```

- **Tempo raportów z wejść po Bluetooth jest ograniczone do 66 Hz (15 ms).** Kontroler w roli
  centrala odmawia *zainicjowania* interwału krótszego niż 15 ms, zwracając HCI `0x12` — zmierzone
  identycznie na ESP32-C3 i ESP32-S3, które dzielą bibliotekę kontrolera, więc to cecha tej
  rodziny, a nie jednej płytki. Sześć hipotez wykluczono osobnymi pomiarami (przepustowość radia,
  liczba linków, wspólna siatka, `ce_len`, timeout nadzoru i skaner), a w Kconfigu nie ma na to
  żadnej opcji (`AGENTS.md` §4.33). Ten sam kontroler bez problemu *utrzymuje* krótszy interwał,
  gdy poprosi o niego peer — tak link pada dochodzi do 7,5 ms z Windows, i tak nasza klawiatura
  testowa kończy na 7,5 ms, podczas gdy mysz, która nie prosi, zostaje na 15 ms. Zarówno
  [podział](#opcjonalnie-podział-na-dwa-układy), jak i transport USB to obchodzą.
- **Na ESP32-C6 i ESP32-H2 nasza klawiatura testowa nie łączy się.** Pad i mysz tam działają. Trace
  HCI z wewnętrznego logu kontrolera pokazuje, co się dzieje: kontroler zgłasza połączenie jako
  nawiązane, a link natychmiast umiera z HCI `0x3E` („Connection Failed to be Established"), czyli
  strony nie spotykają się na pierwszych zdarzeniach połączenia. Ten sam firmware, ta sama
  klawiatura i ten sam pokój działają na C3 i S3 — przy sygnale **42 dB słabszym**. Piętnaście
  hipotez wykluczono pomiarem, w tym trzy wersje ESP-IDF i sztuczną klawiaturę, która kopiuje
  prawdziwą bajt w bajt i łączy się bez problemu. Pełny materiał w `AGENTS.md` §4.35.
- **Widoczne tempo aktualizacji pada USB zależy od tego, jak ruszasz** — między 70 Hz a 830 Hz przy
  suficie transportu 997 Hz. To cecha filtra wygładzającego, nie usterka; patrz
  [Tempo raportów](#tempo-raportów).
- **Tylko Windows.** Profile XInput celują w Windows. Profil generyczny DirectInput powinien działać
  wszędzie, ale nie był testowany poza Windows.
- **Łatka `esp_hid` jest przypięta do IDF 5.5.1.** IDF 6.0.2 i 6.1 naprawiają cztery z dziewięciu
  wad, które łatamy, więc tam trzeba napisać różnicę od nowa, a nie przenieść — a nasza kopia
  obecnie **przysłania** naprawioną wersję z upstreamu. Dotyczy to wyłącznie rol BLE; warianty USB
  linkują ten komponent, ale go nie używają.

## Diagnostyka

Opcje wyłączone domyślnie, wszystkie w `menuconfig`. Każda powstała, bo odpowiadała na pytanie, na
które zgadywanie nie wystarczyło:

| Opcja | Co robi |
|---|---|
| `APP_DEBUG_SCAN_ONLY` | tylko skanuje i loguje, nigdy nie łączy — jedyny sposób, by zmierzyć, co urządzenie faktycznie robi w eterze, bo łączenie przerywa skan i zniekształca pomiar |
| `APP_ROLE_FAKE_KEYBOARD` | zamienia płytkę w nadawcę udającego naszą klawiaturę testową bajt w bajt, z regulacją interwału, typu adresu, flag i mocy nadawania; daje peera, którego zachowanie kontrolujemy |
| `APP_DEBUG_CTRL_LOG_DUMP` | zrzuca wewnętrzny log kontrolera C6/H2 razem z ruchem HCI, po nieudanym i po udanym otwarciu urządzenia; `scripts/decode_ctrl_log.py` zamienia hex na czytelne HCI |
| `APP_LINK_PROBE_RX` | przemiata piny wejściowe i pokazuje, na którym pojawiają się ramki z poprawnym CRC z drugiego układu — tak ustaliliśmy połączenie S3↔H2 na płytce BR, bo dokumentacja go nie podaje |
| `APP_GAMEPAD_SELFTEST` | pad sam przemiata gałkami i cyklicznie wciska przyciski, więc deskryptor można sprawdzić bez klawiatury i myszy |
| `APP_DEBUG_PAD_RATE_PROBE` | wymusza inny raport pada w każdym tiku, więc licznik pakietów mierzy transport, a nie filtr wygładzający i rękę na myszy |
| `APP_DEBUG_WATCH_ADDR` | uzbraja sprzętowy watchpoint na zapis pod adres, więc uszkodzenie pamięci daje panikę z backtrace'em **sprawcy**, a nie ofiary |

Dwa narzędzia po stronie PC istnieją z tego samego powodu — odpowiadają na pytania, na które log
urządzenia nie odpowie:

| Skrypt | Na co odpowiada |
|---|---|
| `scripts/xinput_rumble.py` | czy XInput naprawdę *rozmawia* z padem, a nie tylko czy sterownik się związał: czyta wszystkie cztery sloty przez `XInputGetState`, a potem wysyła trzy celowo **różne** pary wartości silników, żeby log urządzenia dał się z nimi zestawić linia w linię. `--watch <s>` to podgląd stanu na żywo, kluczowany po `dwPacketNumber`, czyli test end-to-end od strony, którą czyta gra; `--rate <s>` mierzy, jak często pad faktycznie się aktualizuje |
| `scripts/check_xinput_descriptor.py` | czy deskryptor USB, który faktycznie pójdzie na drut, zgadza się z przechwytem Wireshark prawdziwego kontrolera — parsuje **zbudowaną binarkę**, nie źródło |
| `scripts/check_doc_links.py` | czy każdy odsyłacz w tej dokumentacji nadal prowadzi gdzieś — zarówno linki do plików, jak i `#kotwice`. Dokumenty celowo wskazują na siebie, żeby jeden fakt mieszkał w jednym miejscu, a to zostaje prawdą tylko wtedy, gdy zmiana nazwy nie zepsuje tego po cichu |

## Licencja i pochodzenie

Projekt jest wydany na licencji MIT — patrz [`LICENSE`](LICENSE).

- Deskryptor raportu HID pada Xbox pochodzi z
  [**Mystfit/ESP32-BLE-CompositeHID**](https://github.com/Mystfit/ESP32-BLE-CompositeHID) (MIT),
  gdzie odczytano go z fizycznego kontrolera. Nie wciągamy ich kodu: deskryptor generuje
  `scripts/gen_xbox_report_map.py` do `firmware/main/xbox_report_map.h`. Patrz
  [`THIRD-PARTY.md`](THIRD-PARTY.md).
- Deskryptor USB XInput i format raportu pochodzą z przechwytu Wireshark prawdziwego przewodowego
  pada Xbox 360 opublikowanego na partsnotincluded.com, skonfrontowanego ze sterownikiem `xpad`
  z Linuksa. Patrz [`THIRD-PARTY.md`](THIRD-PARTY.md).
- `firmware/components/esp_hid/` to zmodyfikowana kopia komponentu z
  [**ESP-IDF**](https://github.com/espressif/esp-idf) (Apache-2.0). Zmiany są opatrzone
  komentarzami `LOCAL PATCH` i wyodrębnione w `PATCH.diff`.

Projekt nie jest powiązany z Microsoftem ani Espressifem i nie jest przez nich firmowany.
Przedstawia identyfikatory producenta i produktu Microsoftu, żeby Windows załadował swój własny
sterownik; jest to interoperacyjność ze sprzętem, który autor posiada, i nie jest przeznaczone do
redystrybucji jako produkt.
