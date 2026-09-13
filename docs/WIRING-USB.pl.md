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

*English version of this document: [`WIRING-USB.md`](WIRING-USB.md). Przegląd projektu:
[`../README.pl.md`](../README.pl.md).*

## Spis treści

1. [Listwa 18-pinowa](#1-listwa-18-pinowa--co-jest-czym)
2. [Płytka A ↔ Płytka B: łącze](#2-płytka-a--płytka-b-łącze)
3. [Przejściówka CP2102 → płytka: konsola](#3-przejściówka-cp2102--płytka-konsola)
4. [Zasilanie i hub](#4-zasilanie-i-hub)
5. [Wgrywanie firmware'u](#5-wgrywanie-firmwareu)
6. [Środowisko i komendy](#6-środowisko-i-komendy)
7. [Kolejność uruchamiania](#7-kolejność-uruchamiania)
8. [Stan weryfikacji](#8-stan-weryfikacji)

---

## 1. Listwa 18-pinowa — co jest czym

Rozpiska potwierdzona przez właściciela na fizycznej płytce. Widok od góry, gniazdo USB-C u góry,
obie kolumny liczone od strony USB w dół:

```
                    ┌───── USB-C ─────┐
   konsola TX ->  TX (GPIO43)           5V           <- zasilanie plytki A
   konsola RX ->  RX (GPIO44)           GND          <- masa, obowiazkowo
                  GP1   [BOOT] [RESET]  3V3 (OUT)
                  GP2                   GP13
                  GP3                   GP12
    lacze TX ->   GP4                   GP11
    lacze RX ->   GP5                   GP10
                  GP6                   GP9
                  GP7                   GP8
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

**Konsola zostaje na domyślnych pinach UART0**, czyli na tych oznaczonych `TX` i `RX`. Nie ma
powodu jej przenosić, a zostawienie domyślnych daje dwie rzeczy, których własne piny by nas
pozbawiły: bootloader ROM-u drukuje po tych pinach **niezależnie od konfiguracji**, co jest
testem okablowania niezależnym od naszego firmware'u (§5), a protokół wgrywania ROM-u chodzi po
nich, więc UART zostaje awaryjną drogą programowania. Gdyby te dwa piny kiedyś były potrzebne na
co innego, konsolę przenosi się opcją `CONFIG_ESP_CONSOLE_UART_CUSTOM` — peryferium zostaje
UART0, zmieniają się tylko numery pinów.

Konsola i łącze **nie kolidują**, bo to dwa różne peryferia: konsola na UART0, łącze na UART1
(`APP_LINK_UART_PORT=1`). Piny przypisuje matryca GPIO, więc numer pinu i numer peryferium to
dwie niezależne rzeczy.

Dlaczego łącze na GPIO4/GPIO5: to zwykłe GPIO, bez funkcji strapping (te są na GPIO0, 3, 45,
46), nie są pinami USB (GPIO19/20) ani flashem czy PSRAM (GPIO26–37), i nie zabierają pinów
konsoli. Leżą obok siebie na listwie, więc kabelek jest krótki.

**Czego na listwie nie ma**, a bywa potrzebne:

- **GPIO19/GPIO20** — linie danych USB, idą wprost do gniazda USB-C. Dobrze, bo znaczy to, że
  nic ich przypadkiem nie zwarłeś.
- **EN (reset) i GPIO0 (boot)** — tylko jako przyciski **RESET** i **BOOT** na płytce. Żadna
  przejściówka ani drugi układ nie wprowadzi więc płytki w tryb wgrywania automatycznie (§5).
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

Fizycznie wszystko siedzi przy górnej krawędzi, po obu stronach gniazda USB-C: `TX` i `RX` to
dwie pierwsze pozycje **lewej** kolumny, a `5V` i `GND` dwie pierwsze **prawej**.

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

**Sterownik trzeba zainstalować ręcznie.** Windows nie ma CP210x w magazynie sterowników
(`pnputil /enum-drivers` — zero wpisów SiLabs), więc po wetknięciu modułu **nie dostaniesz portu
COM**. Urządzenie jest przy tym widoczne i „obecne”, tylko bezużyteczne:

```
USB\VID_10C4&PID_EA60\0001 | ConfigManagerErrorCode = 28
```

Kod 28 znaczy „sterowniki nie są zainstalowane”. Potrzebny jest **CP210x Universal Windows
Driver** ze strony Silicon Labs. Diagnozuj tym kodem, nie brakiem portu — brak portu ma kilka
możliwych przyczyn, a kod 28 tylko jedną.

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

**Najprościej przez własne USB-C każdej płytki.** Trzymaj **BOOT**, stuknij **RESET**, puść
BOOT — układ wchodzi w bootloader ROM-u, zgłasza się jako port COM (USB Serial/JTAG) i można
wgrywać. Działa niezależnie od tego, co aplikacja robi z USB, bo aplikacja jeszcze nie działa.
Po wgraniu stuknij RESET. Pusty układ wchodzi w ten tryb sam, więc przyciski są potrzebne
dopiero przy **kolejnych** wgraniach.

```powershell
scripts\flash-win.ps1 COM<port_plytki> esp32s3 s3pad
```

**Po wgraniu stuknij RESET palcem — i to nie jest opcjonalne.** Zmierzone dwa razy na tej
płytce: `Hard resetting via RTS pin`, którym esptool kończy wgrywanie, **nie wyprowadza układu
z trybu download**. Objaw jest zwodniczy, bo wygląda jak martwy firmware: konsola milczy,
`VID_045E&PID_028E` nie pojawia się, a `VID_303A&PID_1001` trwa niewzruszony. Rozstrzyga jedno
polecenie — jeśli `esptool --before no-reset flash-id` **przechodzi**, układ siedzi
w bootloaderze i po prostu nie wykonuje aplikacji:

```powershell
python -m esptool --chip esp32s3 --port COM4 --before no-reset flash-id
```

Programowe próby wyjścia z tego stanu (`esptool run`, impuls RTS przy DTR = 0) też nie
zadziałały, więc nie ma sensu ich powtarzać. Cały cykl wygląda tak: **BOOT+RESET → wgranie →
RESET**.

**Przez CP2102 też można**, bo protokół pobierania ROM-u chodzi po sprzętowym UART0, czyli po
tych samych pinach `TX`/`RX`, do których podłączona jest konsola. Ta sama sekwencja przycisków,
plus przełącznik, który mówi esptoolowi, żeby nie próbował resetować układu:

```powershell
scripts\flash-win.ps1 COM<cp2102> esp32s3 s3pad -Uart
```

Traktuj to jako drogę awaryjną — USB-C jest szybsze i nie wymaga niczego poza kablem.

**DTR przejściówki zostaje niepodłączony.** Automatyczne wejście w bootloader wymaga sterowania
**dwiema** liniami układu — EN (reset) i GPIO0 (boot) — a żadnej z nich nie ma na listwie
18-pinowej; RTS jest w tym module dodatkowo tylko padem lutowniczym. Dałoby się przylutować
druty do padów przycisków BOOT i RESET, ale przy dwóch płytkach, które te przyciski mają pod
palcem, to nakład bez zysku. `monitor.py` i tak otwiera port z opuszczonym DTR/RTS, więc nic się
przez przypadek nie zresetuje.

**Dlaczego flashowanie jednej płytki przez drugą po UART nic tu nie daje**, mimo że drut i tak
jest: przyciski wciskasz ręcznie w każdym wariancie, bo EN i GPIO0 nie są wyprowadzone — więc
druga płytka nie oszczędza ani jednego ruchu. Poza tym w docelowej konfiguracji jej USB jest już
zajęte własną rolą, więc nie mogłaby udawać przejściówki CDC dla PC bez uprzedniego
przeprogramowania. Kanał powrotny z §2 zostaje sensowny, ale jako **proxy konsoli**, nie droga
wgrywania.

**Jedna przejściówka, dwie płytki.** Konsola na obu siedzi na tych samych pinach, więc CP2102 się
po prostu przekłada: trzy druty, bez rekompilacji. Debugujesz jedną płytkę naraz.

### Dwa porty COM naraz i dwie pułapki

Płytka jest podłączona **jednocześnie** kablem USB-C (zasilanie plus rola USB) i przejściówką
do pinów konsoli. Nie kolidują, bo to dwie różne rzeczy w układzie: gniazdo USB-C to
GPIO19/20, a konsola to GPIO43/44. W systemie widać wtedy dwa urządzenia:

| Port | Co to |
|---|---|
| `USB JTAG/serial debug unit` (VID:PID `303a:1001`) | bootloader ROM-u samego układu — tym wgrywasz |
| `Silicon Labs CP210x` | przejściówka — tym czytasz konsolę |

**Port ESP-a znika po wgraniu i tak ma być.** Aplikacja zabiera GPIO19/20 na swoją rolę USB
(pad XInput albo host), więc bootloader ROM-u przestaje być widoczny. Wygląda to jak zniknięcie
płytki, a jest normalnym skutkiem tego, że USB Serial/JTAG i USB-OTG dzielą te piny. Żeby
wgrać ponownie: przytrzymaj **BOOT**, stuknij **RESET** — port wraca.

**Tryb `reset` monitora nie zadziała przez CP2102.** `reset_monitor.py` resetuje układ impulsem
na RTS, co działa wyłącznie przez natywne USB, gdzie RTS steruje linią CHIP_EN. Przejściówka ma
RTS tylko jako pad lutowniczy i nigdzie go nie prowadzimy, więc:

```powershell
scripts\monitor-win.bat COM<cp2102> 30          # tak
scripts\monitor-win.bat COM<cp2102> 30 reset    # połączy się, ale NIE zresetuje
```

Żeby złapać log od pierwszej linii: uruchom monitor, a potem **stuknij RESET palcem**.

**Sprawdzenie samego okablowania konsoli, niezależne od naszego firmware'u.** Bootloader ROM-u
drukuje po UART0 przy każdym starcie, bez względu na to, co jest wgrane i jak skonfigurowane —
i właśnie dlatego konsola została na pinach domyślnych. Uruchom monitor i stuknij RESET:

```
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
...
ESP-IDF v6.1 2nd stage bootloader
```

Jeśli widzisz choćby pierwszą linię, to `TX` płytki trafia do `RXD` przejściówki, masa jest
wspólna, a prędkość się zgadza. Cisza w tym miejscu to błąd w tych trzech drutach, a nie
w firmwarze — nie ma sensu szukać dalej, dopóki paplanina ROM-u nie dochodzi.

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

**2. Dowód, że to naprawdę XInput — bez żadnej gry.** `scripts\xinput_rumble.py` woła XInput
wprost przez `XInput1_4.dll`:

```powershell
python scripts\xinput_rumble.py
```

Skrypt czyta cztery sloty przez `XInputGetState`, a potem wysyła trzy serie wibracji o **różnych**
wartościach i zeruje. Wynik na tej płytce:

```
slot 0: CONNECTED, buttons=0x0000 LT=0 RT=0 L=(0,0) R=(0,0)
  left full   -> XInputSetState(0xffff, 0x0000) rc=0
  right full  -> XInputSetState(0x0000, 0xffff) rc=0
  both ~25%   -> XInputSetState(0x4000, 0x4000) rc=0
```

a w konsoli płytki:

```
I (21280) usb_pad: rumble from host: left=255 right=0
I (22770) usb_pad: rumble from host: left=0 right=255
I (24280) usb_pad: rumble from host: left=64 right=64
I (25770) usb_pad: rumble from host: left=0 right=0
```

Trzy różne wartości i trzy zgodne linie w tej samej kolejności, ze skalowaniem 16→8 bitów
(`0xFFFF` → 255, `0x4000` → 64). Polecenia wibracji wysyła **wyłącznie** sterownik pada, nie
generyczna obsługa HID, więc to jednocześnie dowód wiązania sterownika, obecności w slocie
XInput i działania endpointu OUT. Osie i przyciski są zerowe, bo wejść jeszcze nie ma.

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

## 8. Stan weryfikacji

**Wszystkie pięć kroków zaliczone.** Mostek USB działa end-to-end.

| Co | Dowód |
|---|---|
| pad wiązany przez XInput | `USB\VID_045E&PID_028E\08FEC93` → `Service=xusb22`, slot 0 `CONNECTED` |
| ścieżka host → urządzenie | trzy różne pary wartości `XInputSetState` → trzy zgodne `rumble from host` |
| host USB + hub + dongle | `usb ifaces 4 (kbd=1 mouse=1)`, `KBD report len=8`, `MOU report len=7` |
| łącze UART | `sent 19884 frames (dropped 0)` |
| mysz → prawy analog | 977 zmian stanu w 18 s, z gładkim opadaniem do `R=(0,0)` |
| WASD → lewy analog | `w` → `L=(0,32767)`, `a` → `L=(-32767,0)` |
| **przyciski, spusty, krzyżak** | potwierdzenie właściciela: **test kontrolera w Steam pokazuje wszystko poprawnie** |
| **w grze** | potwierdzenie właściciela: **Apex Legends działa bez zarzutu** |
| stabilność | heap płytki wejść 347 404 B (min 346 116 B) przez ~24 min |
| passthrough | `Ctrl+Alt+G` przełącza tożsamość w obie strony; szczegóły w [`../README.pl.md`](../README.pl.md#passthrough-na-skrót-klawiaturowy) |

Rozstrzygnięte pomiarem i **nie** wymagające już sprawdzania:

- **VBUS gniazda USB-C** — jest 5 V, gdy płytka jest zasilona z pinu `5V` (§4).
- **Poziom logiczny przejściówki** — RX, TX i DTR na 3,3 V ze specyfikacji modułu.
- **Interfejsy dongle'i** — oba wystawiają boot keyboard i boot mouse, pięć interfejsów razem.
- **Cykl wgrywania** — `BOOT+RESET → wgranie → RESET` (§5). Wgrywanie po UART przez CP2102 też
  działa, sprawdzone na płytce wejść, której USB-C zajmuje hub.

**Uwaga do testowania, żeby nie wyciągnąć złego wniosku.** Windows Terminal **reaguje na pada**:
wychylenie lewej gałki działa w nim jak strzałki (sprawdzone prawdziwym padem Xbox Series X).
Skoro mostek mapuje WASD na lewą gałkę, naciśnięcie `W` poruszy kursorem w Terminalu — i wygląda
to dokładnie tak, jakby klawiatura nadal należała do Windows, a mostek nic nie przekazywał. Jest
odwrotnie: to dowód, że łańcuch działa. Nie oceniaj po Terminalu; rozstrzygają dwa logi czytane
jednocześnie albo `XInputGetState`.
