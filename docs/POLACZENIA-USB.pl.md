# Rozpiska połączeń: dwa ESP32-S3 SuperMini, wersja USB

Arkusz warsztatowy do wersji USB mostka (`AGENTS.md` §4.37) zbudowanej na **dwóch osobnych
płytkach ESP32-S3 SuperMini**, a nie na płytce ESP Thread BR z dwoma układami na jednej PCB.
Różnica jest istotna: tam łącze biegło wewnętrznym połączeniem na PCB (S3 GPIO17 ← H2 GPIO24),
tutaj biegnie drutem po **listwie 18-pinowej**, więc piny trzeba wybrać i podać jawnie.

```
klawiatura ~2,4 GHz~ dongle ─┐                              ┌─ pad Xbox 360 (XInput) ─→ PC
                             ├─→ hub ─→ [A] ──UART──→ [B] ──┘
mysz ~2,4 GHz~ dongle ───────┘            s3input     s3pad
```

Wejścia wchodzą przez **odbiorniki radiowe 2,4 GHz** wetknięte w hub, nie przez kabel od samych
urządzeń — klawiatura i mysz zostają bezprzewodowe, a pobór prądu z huba spada do dziesiątek
mA (§4). Kablem też by działało, ale wtedy urządzenia jednocześnie ładują akumulatory.

| | Płytka A | Płytka B |
|---|---|---|
| wariant | `s3input` | `s3pad` |
| rola USB | **host** (dwa dongle przez hub) | **urządzenie** (pad XInput dla PC) |
| gniazdo USB-C | zajęte przez hub | zajęte przez kabel do PC |
| łącze UART | nadaje | odbiera |
| BLE | wyłączone | wyłączone |

Obie płytki mają zajęte USB, więc na obu konsola siedzi na **UART0** — i to nie jest
preferencja. Na ESP32-S3 peryferium USB Serial/JTAG i USB-OTG są podłączone do **tych samych
pinów GPIO19/20** i tylko jedno może nimi sterować; gdy TinyUSB albo host USB je zabiera,
konsola po USB zamilknie, a objaw wygląda jak martwy firmware.

---

## 1. Listwa 18-pinowa — co jest czym

Widok od góry, gniazdo USB-C u góry. To dokładnie te 18 pinów, które lutujesz:

```
                    ┌───── USB-C ─────┐
         TX  (GPIO43)                   5V
         RX  (GPIO44)                   GND
             GPIO1     [BOOT]  [RESET]  3V3 (OUT)
             GPIO2                      GPIO13
             GPIO3                      GPIO12
             GPIO4      ESP32-S3        GPIO11
             GPIO5      SuperMini       GPIO10
             GPIO6                      GPIO9
             GPIO7                      GPIO8
                    └─────────────────┘
```

| Pin listwy | Do czego w tym projekcie |
|---|---|
| **TX** (GPIO43) | konsola, UART0 TX → **RXD** przejściówki |
| **RX** (GPIO44) | konsola, UART0 RX ← **TXD** przejściówki |
| **GPIO4** | łącze międzyukładowe, UART1 **TX** (nadaje tylko płytka A) |
| **GPIO5** | łącze międzyukładowe, UART1 **RX** (odbiera tylko płytka B) |
| **GND** | masa — wspólna dla łącza i dla przejściówki, obowiązkowo |
| **5V** | zasilanie **wejściowe** |
| **3V3** | **wyjście** stabilizatora płytki — nie podawaj tu napięcia |
| GPIO3 | lepiej zostawić wolny: pin strapping (wybór źródła JTAG) |
| GPIO1, GPIO2, GPIO6–GPIO13 | wolne, nic ich nie używa |

Dlaczego łącze na GPIO4/GPIO5: to zwykłe GPIO, bez funkcji strapping (te są na GPIO0, 3, 45,
46), nie są pinami USB (GPIO19/20), nie są flashem ani PSRAM (GPIO26–37) i nie kolidują
z UART0, którego potrzebuje konsola. Leżą obok siebie na listwie, więc kabelek jest krótki.

**Czego na listwie nie ma**, a bywa potrzebne:

- **GPIO19/GPIO20** — linie danych USB, idą wprost do gniazda USB-C. Dobrze, bo znaczy to, że
  nic ich przypadkiem nie zwarłeś.
- **EN (reset) i GPIO0 (boot)** — tylko jako przyciski **RESET** i **BOOT** na płytce. Żadna
  przejściówka ani drugi układ nie wprowadzi więc płytki w tryb wgrywania automatycznie
  (patrz §5).
- **GPIO26–GPIO32** — pamięć flash.
- **GPIO14–GPIO18, GPIO21, GPIO33–GPIO42, GPIO45–GPIO48** — na dolnych padach płytki, nie na
  listwie. Trzymaj się listwy: GPIO33–37 są zajęte przy wersjach z ośmiobitowym PSRAM,
  GPIO45/46 to piny strapping, a GPIO48 obsługuje wbudowaną diodę RGB.

---

## 2. Płytka A ↔ Płytka B (łącze)

| Płytka A (`s3input`) | → | Płytka B (`s3pad`) | Sygnał | Konieczne |
|---|---|---|---|---|
| GPIO4 | → | GPIO5 | ramki wejść, 921600 8N1 | **tak** |
| GND | — | GND | masa wspólna | **tak** |
| GPIO5 | ← | GPIO4 | kanał powrotny, dziś nieużywany | nie, ale warto |

Wystarczą **dwa druty**: GPIO4 płytki A do GPIO5 płytki B oraz masa do masy. Łącze jest
jednokierunkowe, bo odbiornik nie ma o co pytać nadajnika — sam podejmuje wszystkie decyzje
o padzie.

Trzeci drut (GPIO4 płytki B do GPIO5 płytki A) nie jest przez firmware sterowany:
`s3pad` ma `APP_LINK_TX_GPIO=-1`, co `uart_set_pin()` rozumie jako „nie ruszaj tego pinu”.
Warto go jednak przylutować od razu — jest gotowym miejscem na kanał powrotny, gdyby log
z płytki B miał kiedyś iść przez płytkę A.

Konwencja jest symetryczna: **GPIO4 to zawsze TX, GPIO5 to zawsze RX**, więc kabel jest
zwykłym skrzyżowaniem i nie ma jak go pomylić.

---

## 3. Przejściówka CP2102 → płytka (konsola)

Moduł: **CP2102 (SiLabs)**, złącze USB-A, listwa goldpin `DTR / RXD / TXD / +5V / GND / 3V3`.
Konsola chodzi **115200 8N1** — dokładnie tyle ustawia `scripts\monitor-win.bat`.

Mapowanie 1:1 po kolejności pinów na listwie, żeby nie było pomyłki:

| # | CP2102 | → | Płytka | Uwaga |
|---|---|---|---|---|
| 1 | **DTR** | | *nie podłączać* | nie ma gdzie: EN i GPIO0 nie są na listwie (§5) |
| 2 | **RXD** | → | **TX** (GPIO43) | moduł odbiera to, co płytka nadaje |
| 3 | **TXD** | → | **RX** (GPIO44) | moduł nadaje do płytki |
| 4 | **+5V** | → | **5V** | **tylko płytka A**, patrz niżej |
| 5 | **GND** | → | **GND** | obowiązkowo |
| 6 | **3V3** | | *nie podłączać* | 3V3 płytki jest wyjściem jej stabilizatora |

Etykiety na listwie są z punktu widzenia **modułu**, więc TXD i RXD krzyżują się z pinami
płytki. To najczęstsza pomyłka przy pierwszym podłączeniu; jeśli w konsoli nie ma nic, zamień
te dwa druty przed szukaniem czegokolwiek innego.

**Poziomy logiczne tego modułu to 3,3 V na RX, TX i DTR** (specyfikacja producenta), więc
odpada obawa o przekroczenie maksimum ESP32-S3 (~3,6 V) — mierzenia napięcia nie trzeba.
Pin `+5V` podaje 5 V z USB i tym właśnie zasilimy płytkę A.

Jedno, o czym trzeba pamiętać: **nie dawaj dwóch źródeł 5 V**. Płytka B wisi na USB-C w PC
i ma stamtąd zasilanie, więc do niej podłącz z przejściówki **wyłącznie RXD, TXD i GND**
(piny 2, 3 i 5).

Trzy diody na module to zasilanie oraz ruch na TXD i RXD — przy pracującej konsoli miga ta
od RXD modułu, czyli od danych płynących z płytki.

---

## 4. Zasilanie i hub

| Płytka | Skąd 5 V | Dlaczego |
|---|---|---|
| A `s3input` | pin **5V**, z pinu `+5V` przejściówki CP2102 | jej USB-C jest portem hosta, więc nie dostaje przez niego zasilania |
| B `s3pad` | **USB-C z PC** | tam jest zarazem pad i zasilanie |

Płytka A ma więc **cztery druty do przejściówki** (RXD, TXD, +5V, GND), która ją zarazem
zasila i daje konsolę, oraz **dwa druty do płytki B** (GPIO4 → GPIO5 i masa). Płytka B ma
trzy druty do przejściówki (RXD, TXD, GND — bez 5 V!) i USB-C do PC.

### VBUS gniazda: ZMIERZONE, jest 5 V

**Pin `5V` podaje 5 V na VBUS gniazda USB-C** — sprawdzone na sprzęcie, zasileniem jednego
ESP32 z drugiego przez adapter OTG. Nie ma tam diody blokującej, więc host poda urządzeniom
zasilanie sam. To była jedyna niepewność sprzętowa tego montażu i jest zamknięta:
wstrzykiwanie 5 V w kabel do huba nie będzie potrzebne.

Ma to jednak konsekwencję: **pin `5V` i VBUS gniazda to jedna sieć**, więc nie zasilaj płytki
jednocześnie z dwóch stron. Dla płytki B jest to reguła twarda, nie ostrożność — skoro wisi na
USB-C w PC, podłączenie `+5V` przejściówki do jej pinu `5V` zwarłoby zasilanie PC z zasilaniem
przejściówki. Do płytki B idą **tylko RXD, TXD i GND**.

### Hub i wejścia

**Wejścia wchodzą przez odbiorniki radiowe 2,4 GHz wetknięte w hub**, nie kablem od samych
urządzeń. To zdejmuje problem prądu niemal całkowicie, bo dongle nie mają podświetlenia i nie
ładują żadnego akumulatora:

| Odbiornik prądu | Rząd wielkości |
|---|---|
| ESP32-S3 z aktywnym hostem USB (bez radia w `s3input`) | dziesiątki mA |
| hub | kilkadziesiąt mA |
| dongle 2,4 GHz × 2 | ~20–30 mA każdy |
| **razem** | grubo poniżej 200 mA |

Dla porównania, gdyby kablem podłączyć same urządzenia: klawiatura z podświetleniem RGB to
setki mA, a przy pracy z kabla dochodzi jeszcze **ładowanie akumulatora** — i to ono, nie RGB,
najszybciej przekracza 500 mA typowego portu. Objawem podnapięcia nie jest czysty komunikat,
tylko losowe rozłączenia i resety, które w logu wyglądają jak błąd firmware'u.

Przy dongle'ach **hub pasywny też wystarczy**: VBUS jest, a pobór jest znikomy. Skoro aktywny
już masz, nie ma powodu z niego rezygnować — zdejmuje temat prądu z dyskusji na dobre. Hub
jest potrzebny w każdym razie, bo dwa dongle to dwa porty.

Dwie rzeczy o tym konkretnym sprzęcie:

- **USB 3.0 zadziała, ale w trybie 2.0.** Peryferium USB-OTG w ESP32-S3 to **Full Speed**
  (12 Mbit/s), bez High Speed i bez SuperSpeed. Hub 3.0 zawiera w sobie osobny hub 2.0 i przy
  hoście Full Speed używana jest właśnie ta część. Nic nie tracimy — dongle HID to Full Speed,
  a raport to kilka bajtów.
- **Przejściówka USB-C (wtyk, do płytki) na USB-A (gniazdo, dla kabla huba).** Sprawdzona:
  **adapter OTG od Google Pixela 7** — właśnie przez niego zmierzono 5 V na VBUS, więc wiadomo,
  że przepuszcza zasilanie i nie wymaga od płytki żadnej negocjacji. Linie CC nie mają tu
  znaczenia, bo gniazdo USB-C SuperMini prowadzi tylko USB 2.0: D+, D−, VBUS i GND. Adapter
  jest wprawdzie 3.x, ale jego pary SuperSpeed nie mają z czym się połączyć.

### Czego wymagają dongle od naszego hosta

Jedno zastrzeżenie, którego nie da się rozstrzygnąć bez podłączenia. `usb_hid_host.c`
rozdziela raporty **po kodzie protokołu interfejsu**:

```c
if (params.proto == HID_PROTOCOL_KEYBOARD)      handle_keyboard(...);
else if (params.proto == HID_PROTOCOL_MOUSE)    handle_mouse(...);
```

Ten kod jest niezerowy tylko dla interfejsów **boot** (`bInterfaceSubClass = 1`). Dongle
zwykle takie wystawiają — właśnie dzięki nim klawiatura działa w BIOS-ie — ale część
odbiorników gamingowych prowadzi szybki strumień myszy przez **interfejs vendorowy**, a na
boot zostawia kopię o niższym tempie. Nasz host taki interfejs zignoruje (`proto == 0`),
więc w najgorszym razie dostaniemy niższą częstość raportów, nie brak działania.

Co z tego wynika praktycznie: **w logu po podłączeniu policz linie `HID connected`**. Każdy
dongle zgłosi ich tyle, ile ma interfejsów HID — typowo trzy do czterech. Interesują nas te
z `proto 1` (klawiatura) i `proto 2` (mysz); pozostałe są nieszkodliwe. Jeśli tablica
interfejsów się przepełni, firmware powie to wprost:
`interface table full (8) - addr … iface … not tracked` (limit podniesiony z 4 na 8 właśnie
dlatego, że dwa dongle mieszczą się w czterech tylko przypadkiem).

Co do tempa — i tu trzeba uważać, żeby nie przypisać sobie zysku, którego nie ma. Wersja BLE
**nie** była zablokowana na 66 Hz w całości; zmierzone tempo zależało od odcinka
(`AGENTS.md` §4.36):

| Odcinek w podziale S3+H2 | Zmierzone |
|---|---|
| mysz → H2 | **7,5 ms = 133 Hz** (powyżej deklarowanych przez AJ159 Pro 125 Hz) |
| pad → PC | **7,5 ms = 133 Hz** |
| klawiatura → S3 | 15 ms = 66 Hz, czyli sufit starej rodziny kontrolerów (§4.33) |

Czyli mysz i pad już tam działały poprawnie na pełnym tempie, a 66 Hz dotyczyło **wyłącznie
klawiatury** — i to dlatego, że S3 należy do starszej rodziny kontrolerów, która w roli
centrala nie inicjuje interwału krótszego niż 15 ms.

Realny zysk wersji USB jest więc węższy, niż mogłoby się zdawać: dotyczy **klawiatury** (której
tempo przestaje zależeć od kontrolera BLE) oraz ewentualnie myszy, jeśli dongle zaoferuje
`bInterval` krótszy niż 8 ms. Przy klawiszach 66 Hz i tak nie było odczuwalne, bo są binarne;
warto to jednak zmierzyć, bo to jedyne miejsce, gdzie ta wersja może przebić poprzednią.

---

## 5. Wgrywanie firmware'u

**Najprościej: przez własne USB-C każdej płytki.** Trzymaj **BOOT**, stuknij **RESET**, puść
BOOT — układ wchodzi w bootloader ROM-u, zgłasza się jako port COM (USB Serial/JTAG) i można
wgrywać. Działa niezależnie od tego, co aplikacja robi z USB, bo aplikacja jeszcze nie
działa. Po wgraniu stuknij RESET.

Alternatywnie przez CP2102 na UART0, ta sama sekwencja przycisków:

```powershell
scripts\flash-win.ps1 COM<n> esp32s3 s3pad -Uart
```

Przełącznik `-Uart` mówi esptoolowi, żeby nie próbował resetować układu
(`--before no-reset --after no-reset`).

**Twój moduł ma DTR wyprowadzony na listwę, ale nie ma go gdzie podłączyć.** Automatyczne
wejście w bootloader wymaga sterowania **dwiema** liniami układu — EN (reset) i GPIO0 (boot) —
a żadnej z nich nie ma na listwie 18-pinowej SuperMini; RTS jest w tym module dodatkowo tylko
padem lutowniczym. Dałoby się przylutować druty wprost do padów przycisków BOOT i RESET, ale
przy dwóch płytkach, które te przyciski mają pod palcem, to nakład bez zysku. Zostaw DTR
niepodłączony — `monitor.py` i tak otwiera port z opuszczonym DTR/RTS, więc nic się przez
przypadek nie zresetuje.

**Dlaczego flashowanie jednej płytki przez drugą po UART nic tu nie daje**, mimo że drut i tak
jest: do wprowadzenia celu w tryb wgrywania trzeba sterować **EN i GPIO0**, a tych na
listwie 18-pinowej nie ma — więc przyciski wciskasz ręcznie w każdym wariancie, i przez USB-C,
i przez UART. Poza tym w docelowej konfiguracji USB drugiej płytki jest już zajęte jej własną
rolą, więc nie może udawać przejściówki CDC dla PC bez uprzedniego przeprogramowania. Byłby to
objazd, nie skrót. Kanał powrotny z §2 zostaje jednak sensowny — ale jako **proxy konsoli**,
nie jako droga wgrywania.

**Jedna przejściówka, dwie płytki.** Konsola na obu siedzi na UART0 na stałe, więc CP2102 się
po prostu przekłada: trzy druty, bez rekompilacji. Debugujesz jedną płytkę naraz.

---

## 6. Środowisko i komendy

ESP-IDF w wersji **6.1** buduje wszystkie warianty tego projektu (sprawdzone). Jeśli
instalowałeś go **instalatorem EIM**, a nie `install.bat`, to **skrypty `.bat` z repozytorium
nie wystartują**: wołają `export.bat`, który szuka środowiska Pythona w
`%IDF_TOOLS_PATH%\python_env\idf<x.y>_py<a.b>_env`, a EIM tworzy je w
`%IDF_TOOLS_PATH%\python\<x.y>\venv` i dokłada własny skrypt aktywacyjny dla PowerShella.
Dlatego są wersje `.ps1`, które rozumieją oba układy — szczegóły w komentarzu
`scripts\idf-env.ps1`.

Ścieżki do swojej instalacji trzymaj w `AGENTS.local.md` (wzór w `AGENTS.local.example.md`),
nie tutaj.

```powershell
# Tylko gdy masz kilka instalacji IDF i chcesz wskazać konkretną:
$env:IDF_WIN = 'C:\esp\v<wersja>\esp-idf'

scripts\build-native-win.ps1 esp32s3 s3input
scripts\build-native-win.ps1 esp32s3 s3pad

scripts\flash-win.ps1 COM<n> esp32s3 s3input      # przez USB-C, po BOOT+RESET
scripts\flash-win.ps1 COM<m> esp32s3 s3pad

scripts\monitor-win.bat COM<cp2102> 30            # konsola, 115200
```

Każdy wariant ma osobny katalog builda i osobny `sdkconfig`
(`build.win.esp32s3.s3pad`, `sdkconfig.win.esp32s3.s3pad` i analogicznie dla `s3input`), więc
nie nadpisują się wzajemnie.

Konfiguracja łącza jest w `firmware\sdkconfig.defaults.s3pad` i `.s3input` — piny podane tam
jawnie, bo domyślne z `Kconfig` opisują płytkę BR i na osobnych płytkach są błędne w obie
strony: na esp32s3 wychodziło z nich TX = **−1** (nadajnik w ogóle by nie nadawał) i RX = 17
(pin z dolnych padów, nie z listwy).

---

## 7. Kolejność uruchamiania

Po kolei, bo każdy krok odcina inną klasę problemów.

**1. Sam pad, bez płytki A.** Wgraj `s3pad`, podłącz USB-C do PC, konsolę na CP2102.

```
USB XInput pad up: VID 0x045E PID 0x028E (Xbox 360 wired)
XInput interface open: IN 0x81, OUT 0x01
```

W Windows: `joy.cpl` ma pokazać kontroler, a `Get-PnpDevice` identyfikator
`USB\VID_045E&PID_028E`. Pad będzie nieruchomy — wejść jeszcze nie ma, to normalne.

**2. Dowód, że to naprawdę XInput.** Uruchom cokolwiek, co wibruje padem. W logu ma być:

```
rumble from host: left=… right=…
```

Polecenia wibracji wysyła **wyłącznie** sterownik pada, nie generyczna obsługa HID. Ten sam
test rozstrzygnął sprawę dla wersji BLE (`AGENTS.md` §4.32).

**3. Sam host, bez pada.** Wgraj `s3input`, podłącz hub, a do huba **dwa dongle 2,4 GHz**
(klawiatury i myszy), z podświetleniem urządzeń wyłączonym.

```
USB host up, waiting for a keyboard and a mouse
  external hubs: supported (multi-level)
HID connected: addr … iface … sub_class 1 proto 1     <- klawiatura (boot)
HID connected: addr … iface … sub_class 1 proto 2     <- mysz (boot)
KBD report len=8 [00 00 04 00]                        <- przy pisaniu
MOU report len=… [..]                                 <- przy ruchu
```

Linii `HID connected` będzie więcej niż dwie — każdy dongle zgłasza wszystkie swoje interfejsy
HID. Liczą się te z `proto 1` i `proto 2`; reszta jest ignorowana i to jest w porządku
(szczegóły w §4). Jeśli któregoś z tych dwóch nie ma, dongle nie wystawia interfejsu boot dla
tej klasy i trzeba będzie sięgnąć po jego deskryptor raportu — ale najpierw sprawdź, czy
urządzenie jest w trybie 2,4 GHz, a nie Bluetooth.

Bez linii o hubach host obsłuży dokładnie jedno urządzenie podłączone bezpośrednio, czyli dwa
dongle naraz nie byłyby możliwe. Opcja `CONFIG_USB_HOST_HUBS_SUPPORTED` istnieje w IDF 6.1
i jest w tym wariancie włączona — sprawdzone w wygenerowanym `sdkconfig`.

**4. Drut.** Połącz płytki jak w §2 i sprawdź samo łącze, jeszcze bez patrzenia na pada.
Nadajnik wysyła keepalive co 250 ms, więc cisza jest informacją.

```
A:  link: UART1 up: tx=GPIO4 rx=GPIO-1 921600 baud
    link: mode: sender (mouse -> host chip)
    link: sent N frames (dropped 0)

B:  link: UART1 up: tx=GPIO-1 rx=GPIO5 921600 baud
    link: peer link up
    link: peer serves: mouse keyboard
    link: received N frames (CRC errors 0)
```

Liczniki po obu stronach mają się zgadzać, a `CRC errors` zostać na zerze. Jeśli B nie widzi
nic, sprawdź kolejno: masa wspólna, czy drut idzie z GPIO4 do GPIO5 (a nie 4→4), czy A
faktycznie melduje `tx=GPIO4`.

**5. Całość.** Ruch myszy → prawa gałka, WASD → lewa, klawisze i przyciski zgodnie z tabelą
mapowania wspólną z wersją BLE (`README.pl.md`, sekcja o mapowaniu).

---

## 8. Czego nie wiem

Po pomiarach została **jedna** rzecz, i jest po stronie Windows, nie sprzętu:

**Czy Windows zwiąże XUSB przy jednym zadeklarowanym interfejsie.** Prawdziwy pad ma cztery,
my deklarujemy tylko interfejs 0 — bajt w bajt taki jak w prawdziwym padzie, a dopasowanie
w `xusb22.inf` idzie po VID/PID, więc powinien. To jedyne miejsce, gdzie świadomie odbiegamy
strukturalnie od pierwowzoru; jeśli nie zadziała, poprawka jest **wyłącznie w deskryptorze**
(dopisać pozostałe trzy interfejsy, bajty są w komentarzu `usb_pad.c`) i nie rusza ani jednej
linii logiki. Rozstrzyga to krok 1 i 2 z §7.

Rozstrzygnięte pomiarem i **nie** wymagające już sprawdzania:

- **VBUS gniazda USB-C** — jest 5 V, gdy płytka jest zasilona z pinu `5V` (§4). Host poda
  urządzeniom zasilanie, przejściówka OTG od Pixela 7 je przepuszcza.
- **Poziom logiczny przejściówki** — ten moduł CP2102 ma RX, TX i DTR na 3,3 V zgodnie ze
  specyfikacją producenta, więc nie ma ryzyka przekroczenia maksimum na wejściach ESP32-S3.
