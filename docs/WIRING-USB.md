# Wiring sheet: two ESP32-S3 SuperMini boards, the USB transport

Bench sheet for the USB transport (`AGENTS.md` §4.37) built on **two separate ESP32-S3 SuperMini
boards** rather than on an ESP Thread BR board with two SoCs on one PCB. That difference matters:
there the link ran over an internal PCB trace (S3 GPIO17 ← H2 GPIO24), here it runs over a wire on
the **18-pin side header**, so the pins have to be chosen and stated explicitly.

*Polish version of this document: [`WIRING-USB.pl.md`](WIRING-USB.pl.md). Project overview:
[`../README.md`](../README.md).*

```
keyboard 2.4 GHz dongle ─┐                                    ┌─ Xbox 360 pad (XInput) ─→ PC
                         ├─→ powered hub ─→ [A] ──UART──→ [B] ┘
mouse 2.4 GHz dongle ────┘                 s3input      s3pad
```

| | Board A | Board B |
|---|---|---|
| variant | `s3input` | `s3pad` |
| USB role | **host** (two dongles through a hub) | **device** (XInput pad for the PC) |
| USB-C socket | taken by the hub | taken by the cable to the PC |
| UART link | transmits | receives |
| BLE | off | off |

Input devices go in through their **2.4 GHz receivers** plugged into the hub, not by cable from
the devices themselves — that keeps the keyboard and mouse wireless and drops the hub's current
draw to tens of milliamps (§4). A cable works too, but then the devices also charge their
batteries.

Both boards have their USB socket taken, so on both the console lives on **UART0** — and that is
a requirement, not a preference. On the ESP32-S3 the USB Serial/JTAG peripheral and USB-OTG are
wired to the **same pins, GPIO19/20**, and only one of them can drive those pins; when TinyUSB or
the USB host takes them, a USB console goes silent, and the symptom looks exactly like dead
firmware.

## Contents

1. [The 18-pin header](#1-the-18-pin-header)
2. [Board A ↔ Board B: the link](#2-board-a--board-b-the-link)
3. [CP2102 adapter → board: the console](#3-cp2102-adapter--board-the-console)
4. [Power and the hub](#4-power-and-the-hub)
5. [Flashing](#5-flashing)
6. [Environment and commands](#6-environment-and-commands)
7. [Bring-up order](#7-bring-up-order)
8. [What is verified](#8-what-is-verified)

## 1. The 18-pin header

Layout confirmed by the owner on a physical board. Viewed from the top, USB-C at the top, both
columns counted from the USB end downwards:

```
                    ┌───── USB-C ─────┐
   console TX ->  TX (GPIO43)           5V           <- power in, board A
   console RX ->  RX (GPIO44)           GND          <- ground, mandatory
                  GP1   [BOOT] [RESET]  3V3 (OUT)
                  GP2                   GP13
                  GP3                   GP12
      link TX ->  GP4                   GP11
      link RX ->  GP5                   GP10
                  GP6                   GP9
                  GP7                   GP8
                    └─────────────────┘
```

| Header pin | What it is used for here |
|---|---|
| **TX** (GPIO43) | console, UART0 TX → adapter's **RXD** |
| **RX** (GPIO44) | console, UART0 RX ← adapter's **TXD** |
| **GPIO4** | inter-chip link, UART1 **TX** (only board A transmits) |
| **GPIO5** | inter-chip link, UART1 **RX** (only board B receives) |
| **GND** | ground — shared by the link and the adapter, mandatory |
| **5V** | power **in** |
| **3V3** | the board's regulator **output** — do not feed anything into it |
| GPIO3 | better left alone: it is a strapping pin (JTAG source select) |
| GPIO1, GPIO2, GPIO6–GPIO13 | free, nothing uses them |

**The console stays on the default UART0 pins**, the ones labelled `TX` and `RX`. There is no
reason to move it, and keeping the defaults buys two things that custom pins would cost:

- the **ROM bootloader prints on these pins regardless of configuration**, because it is in
  silicon — that is a wiring test independent of our firmware (§5),
- the **ROM download protocol runs on them too**, so UART remains a fallback way to flash.

If those two pins were ever needed for something else, `CONFIG_ESP_CONSOLE_UART_CUSTOM` moves the
console; the peripheral stays UART0 and only the pin numbers change.

The console and the link **do not collide**, because they are two different peripherals: the
console is UART0, the link is UART1 (`APP_LINK_UART_PORT=1`). Pins are assigned through the GPIO
matrix, so a pin number and a peripheral number are independent things.

Why GPIO4/GPIO5 for the link: they are plain GPIOs with no strapping function (those are GPIO0, 3,
45, 46), they are not the USB pins (GPIO19/20), not flash or PSRAM (GPIO26–37), and they do not
take the console's pins. They sit next to each other on the header, so the jumper is short.

**What is not on the header**, and is sometimes wanted:

- **GPIO19/GPIO20** — the USB data lines, wired straight to the USB-C socket. That is good news:
  it means you cannot short them by accident.
- **EN (reset) and GPIO0 (boot)** — only as the **RESET** and **BOOT** buttons. No adapter and no
  second chip can therefore put the board into download mode automatically (§5).
- **GPIO26–GPIO32** — the flash.
- **GPIO14–GPIO18, GPIO21, GPIO33–GPIO42, GPIO45–GPIO48** — on the pads underneath the board, not
  on the header. Stay on the header: GPIO33–37 are taken on the variants with octal PSRAM,
  GPIO45/46 are strapping pins, and GPIO48 drives the on-board RGB LED.

## 2. Board A ↔ Board B: the link

| Board A (`s3input`) | → | Board B (`s3pad`) | Signal | Required |
|---|---|---|---|---|
| GPIO4 | → | GPIO5 | input frames, 921600 8N1 | **yes** |
| GND | — | GND | shared ground | **yes** |
| GPIO5 | ← | GPIO4 | reverse channel, unused today | no, but worth soldering |

**Two wires are enough**: board A's GPIO4 to board B's GPIO5, and ground to ground. The link is
one-directional because the receiver has nothing to ask the transmitter — it owns the pad and
makes every decision.

The third wire (board B's GPIO4 to board A's GPIO5) is not driven by the firmware: `s3pad` has
`APP_LINK_TX_GPIO=-1`, which `uart_set_pin()` reads as "leave that pin alone". It is still worth
running it now, as a ready place for a reverse channel should board B's log ever need to travel
through board A.

The convention is symmetric — **GPIO4 is always TX, GPIO5 is always RX** — so the cable is a plain
crossover and there is no way to get it backwards.

## 3. CP2102 adapter → board: the console

Module: **CP2102 (SiLabs)**, USB-A plug, pin header `DTR / RXD / TXD / +5V / GND / 3V3`. The
console runs at **115200 8N1**, which is exactly what `scripts\monitor-win.bat` uses.

One-to-one mapping in header order, so there is nothing to get wrong:

| # | CP2102 | → | Board | Note |
|---|---|---|---|---|
| 1 | **DTR** | | *leave unconnected* | nowhere to put it: EN and GPIO0 are not on the header (§5) |
| 2 | **RXD** | → | **TX** (GPIO43) | the module receives what the board sends |
| 3 | **TXD** | → | **RX** (GPIO44) | the module sends to the board |
| 4 | **+5V** | → | **5V** | **board A only**, see below |
| 5 | **GND** | → | **GND** | mandatory |
| 6 | **3V3** | | *leave unconnected* | the board's 3V3 is its regulator's output |

Physically everything sits along the top edge, on both sides of the USB-C socket: `TX` and `RX`
are the first two positions of the **left** column, `5V` and `GND` the first two of the **right**.

The labels are from the **module's** point of view, so TXD and RXD cross over to the board's pins.
That is the most common first-time mistake; if the console shows nothing, swap those two wires
before looking anywhere else.

**This module's logic levels are 3.3 V on RX, TX and DTR** (manufacturer's specification), so
there is no need to measure anything to rule out exceeding the ESP32-S3's maximum (~3.6 V). The
`+5V` pin supplies 5 V from USB, and that is what powers board A.

One thing to keep in mind: **do not create two 5 V sources.** Board B hangs off the PC's USB-C and
gets power from there, so connect only **RXD, TXD and GND** (pins 2, 3 and 5) to it.

The three LEDs on the module are power and traffic on TXD and RXD — with the console running, the
module's RXD one blinks, i.e. the one fed by data coming out of the board.

**The driver has to be installed by hand.** Windows does not carry CP210x in its driver store
(`pnputil /enum-drivers` — zero SiLabs entries), so plugging the module in gives you **no COM
port**. The device is present and "working" all the while, just useless:

```
USB\VID_10C4&PID_EA60\0001 | ConfigManagerErrorCode = 28
```

Code 28 means "the drivers for this device are not installed". You need the **CP210x Universal
Windows Driver** from Silicon Labs. Diagnose with that code rather than with the missing port: a
missing port has several possible causes, code 28 has exactly one.

## 4. Power and the hub

| Board | Where 5 V comes from | Why |
|---|---|---|
| A `s3input` | the **5V** pin, from the adapter's `+5V` | its USB-C is the host port, so it gets no power through it |
| B `s3pad` | **USB-C from the PC** | that is both the pad and the power |

Board A therefore has **four wires to the adapter** (RXD, TXD, +5V, GND), which both powers it and
gives it a console, plus **two wires to board B** (GPIO4 → GPIO5 and ground). Board B has three
wires to the adapter (RXD, TXD, GND — **no 5 V**) and USB-C to the PC.

### The socket's VBUS: MEASURED, 5 V is present

**The `5V` pin does put 5 V on the USB-C socket's VBUS** — measured on hardware by powering one
ESP32 from another through an OTG adapter. There is no blocking diode, so the host supplies power
to the devices by itself. This was the only hardware unknown of the build and it is closed:
injecting 5 V into the cable to the hub will not be necessary.

It does have a consequence: **the `5V` pin and the socket's VBUS are one net**, so do not power a
board from two sides. For board B that is a hard rule rather than a precaution — it hangs off the
PC's USB-C, so connecting the adapter's `+5V` to its `5V` pin would tie the PC's supply to the
adapter's. Board B gets **RXD, TXD and GND only**.

### The hub and the inputs

**The inputs come in through 2.4 GHz receivers plugged into the hub**, not by cable from the
devices themselves. That removes the current problem almost entirely, because a dongle neither
lights up nor charges a battery:

| Consumer | Order of magnitude |
|---|---|
| ESP32-S3 with the USB host active (no radio in `s3input`) | tens of mA |
| the hub | tens of mA |
| two 2.4 GHz dongles | ~20–30 mA each |
| **total** | well under 200 mA |

For comparison, cabling the devices themselves: a keyboard with RGB backlighting is hundreds of
mA, and running from a cable adds **battery charging** on top — and it is the charging, not the
RGB, that most quickly exceeds the 500 mA of a typical port. The symptom of a brown-out is not a
clean message but random disconnects and resets, which in a log look like a firmware bug.

With dongles, **a passive hub is enough too**: VBUS is there and the draw is negligible. Since a
powered one is already at hand there is no reason to drop it — it takes current off the table for
good. A hub is needed either way, because two dongles need two ports.

Two notes on this particular hardware:

- **A USB 3.0 hub works, but in 2.0 mode.** The ESP32-S3's USB-OTG peripheral is **Full Speed**
  (12 Mbit/s), with no High Speed and no SuperSpeed. A 3.0 hub contains a separate 2.0 hub inside
  it, and with a Full Speed host that is the part in use. Nothing is lost — HID dongles are Full
  Speed and a report is a few bytes.
- **A USB-C (plug) to USB-A (socket) OTG adapter is needed.** Verified: **the OTG adapter from a
  Google Pixel 7** — it is the one the 5 V on VBUS was measured through, so it is known to pass
  power and to demand no role negotiation from the board. The CC lines are irrelevant here,
  because the SuperMini's USB-C socket carries only USB 2.0: D+, D−, VBUS and GND. The adapter is
  a 3.x one, but its SuperSpeed pairs have nothing to connect to.

### What the dongles need from our host

One caveat that could not be settled without plugging things in. `usb_hid_host.c` dispatches
reports by the **interface protocol code**:

```c
if (params.proto == HID_PROTOCOL_KEYBOARD)      handle_keyboard(...);
else if (params.proto == HID_PROTOCOL_MOUSE)    handle_mouse(...);
```

That code is non-zero only for **boot** interfaces (`bInterfaceSubClass = 1`). Dongles usually
expose them — that is why a keyboard works in a BIOS — but some gaming receivers carry the fast
mouse stream on a **vendor-specific interface** and leave a lower-rate copy on the boot one. Our
host ignores such an interface (`proto == 0`), so the worst case is a lower report rate, not a
dead device.

It turned out to be answerable from the Windows device tree, without touching the ESP at all — the
`CompatibleID` of a composite device's children states the subclass and protocol:

| Dongle | Interface | SubClass / Prot | Meaning |
|---|---|---|---|
| AULA `3554:FA09` | MI00 | 01 / 01 | boot keyboard |
| | MI01 | 01 / 02 | boot mouse (the Fn layer, `AGENTS.md` §4.16) |
| AJAZZ `3151:402D` | MI00 | 01 / 02 | boot mouse |
| | MI01 | 01 / 01 | boot keyboard (§4.36 — the AJ159 does declare a keyboard) |
| | MI02 | 00 / 00 | vendor-specific, our host skips it |

Three conclusions. **The plan works**: `proto 1` and `proto 2` are both present. **Five HID
interfaces in total**, so the old limit of `USB_HID_MAX_IFACES = 4` would have overflowed —
raising it to 8 was a necessity, not caution. And a third one: **each dongle exposes both
classes**, so the host sees two interfaces of each. Keyboard state is absolute and overwritten, so
a report from the *mouse's* keyboard interface could briefly release a held key. An idle HID
interface transmits nothing, so in practice this may never happen — but it is a symptom to watch
for rather than to go hunting for in the mapper afterwards.

## 5. Flashing

**Easiest over each board's own USB-C.** Hold **BOOT**, tap **RESET**, release BOOT — the chip
enters the ROM bootloader, appears as a COM port (USB Serial/JTAG) and can be written. This works
regardless of what the application does with USB, because the application is not running yet. A
blank chip enters that mode on its own, so the buttons are only needed for **subsequent** writes.

```powershell
scripts\flash-win.ps1 COM<board_port> esp32s3 s3pad
```

**Tap RESET after writing — this is not optional.** Measured twice on this board: the
`Hard resetting via RTS pin` that esptool performs at the end **does not leave download mode**.
The symptom is deceptive, because it looks like dead firmware: the console is silent,
`VID_045E&PID_028E` never appears, and `VID_303A&PID_1001` sits there unchanged. One command
settles it — if `esptool --before no-reset flash-id` **succeeds**, the chip is in the bootloader
and simply is not running the application:

```powershell
python -m esptool --chip esp32s3 --port COM4 --before no-reset flash-id
```

Software attempts to leave that state (`esptool run`, an RTS pulse with DTR low) did not work
either, so there is no point repeating them. The full cycle is **BOOT+RESET → write → RESET**.

**Over the CP2102 also works**, because the ROM download protocol runs on the hardware UART0, the
same `TX`/`RX` pins the console is wired to. Same button sequence, plus a switch that tells
esptool not to try resetting the chip:

```powershell
scripts\flash-win.ps1 COM<cp2102> esp32s3 s3pad -Uart
```

Treat that as the fallback — USB-C is faster and needs nothing but a cable. It does become the
*only* route for board A once its USB-C is occupied by the hub.

**The adapter's DTR stays unconnected.** Entering the bootloader automatically requires driving
**two** lines of the chip — EN (reset) and GPIO0 (boot) — and neither is on the 18-pin header;
RTS on this module is only a solder pad anyway. Wires could be soldered to the BOOT and RESET
button pads, but with two boards whose buttons are under your finger that is effort for nothing.
`monitor.py` opens the port with DTR/RTS deasserted, so nothing resets by accident.

For the same reason **flashing one board through the other over UART buys nothing**: you press the
buttons by hand in either case, so the second board saves not a single motion. Beyond that, in the
final configuration its USB is already taken by its own role, so it could not act as a CDC adapter
for the PC without being reflashed first. The reverse channel from §2 remains sensible, but as a
**console proxy**, not as a route for flashing.

**One adapter, two boards.** The console sits on the same pins on both, so the CP2102 simply moves
across: three wires, no rebuild. You debug one board at a time.

### Two COM ports at once, and two traps

A board is connected **simultaneously** by USB-C (power plus its USB role) and by the adapter to
the console pins. They do not collide, because they are two different things in the chip: the
USB-C socket is GPIO19/20, the console is GPIO43/44. The system then shows two devices:

| Port | What it is |
|---|---|
| `USB JTAG/serial debug unit` (VID:PID `303a:1001`) | the chip's own ROM bootloader — this is what you flash through |
| `Silicon Labs CP210x` | the adapter — this is what you read the log through |

**The board's port disappears after writing, and that is correct.** The application takes
GPIO19/20 for its own USB role (the XInput pad, or the host), so the ROM bootloader stops being
visible. It looks like the board vanished; it is the normal consequence of USB Serial/JTAG and
USB-OTG sharing those pins. To write again: hold **BOOT**, tap **RESET** — the port comes back.

**The monitor's `reset` mode will not work through the CP2102.** `reset_monitor.py` resets the chip
with a pulse on RTS, which only works over native USB, where RTS drives the CHIP_EN line. The
adapter exposes RTS as a solder pad only and we do not route it, so:

```powershell
scripts\monitor-win.bat COM<cp2102> 30          # yes
scripts\monitor-win.bat COM<cp2102> 30 reset    # connects, but does NOT reset
```

To catch the log from its first line: start the monitor, then **tap RESET by hand**.

**A wiring check for the console that does not depend on our firmware.** The ROM bootloader prints
on UART0 at every start, whatever is flashed and however it is configured — which is precisely why
the console stayed on the default pins. Start the monitor and tap RESET:

```
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
...
ESP-IDF v6.1 2nd stage bootloader
```

If even the first line appears, the board's `TX` reaches the adapter's `RXD`, the ground is shared
and the baud rate matches. Silence here is a fault in those three wires, not in the firmware —
there is no point looking further until the ROM's chatter arrives.

## 6. Environment and commands

ESP-IDF **6.1** builds every variant of this project (verified). If it was installed with the
**EIM installer** rather than `install.bat`, the **`.bat` scripts in this repository will not
start**: they call `export.bat`, which looks for the Python environment in
`%IDF_TOOLS_PATH%\python_env\idf<x.y>_py<a.b>_env`, whereas EIM creates it in
`%IDF_TOOLS_PATH%\python\<x.y>\venv` and ships its own PowerShell activation script instead. Hence
the `.ps1` versions, which understand both layouts — details in the comment at the top of
`scripts\idf-env.ps1`.

Keep the paths to your own installation in `AGENTS.local.md` (template in
`AGENTS.local.example.md`), not here.

```powershell
# only when several IDF installations exist and you want to pick one:
$env:IDF_WIN = 'C:\esp\v<version>\esp-idf'

scripts\build-native-win.ps1 esp32s3 s3input
scripts\build-native-win.ps1 esp32s3 s3pad

scripts\flash-win.ps1 COM<n> esp32s3 s3input      # over USB-C, after BOOT+RESET
scripts\flash-win.ps1 COM<m> esp32s3 s3pad

scripts\monitor-win.bat COM<cp2102> 30            # console, 115200
```

Each variant gets its own build directory and its own `sdkconfig`
(`build.win.esp32s3.s3pad`, `sdkconfig.win.esp32s3.s3pad` and likewise for `s3input`), so the two
never overwrite each other.

The link configuration lives in `firmware\sdkconfig.defaults.s3pad` and `.s3input`, with the pins
stated explicitly there — the Kconfig defaults describe the BR board and on separate boards they
are wrong in both directions: on `esp32s3` they resolved to TX = **−1** (the transmitter would
never transmit at all) and RX = 17 (a pin on the underside pads, not on the header).

## 7. Bring-up order

In this order, because each step rules out a different class of problem.

**1. The pad alone, without board A.** Flash `s3pad`, plug USB-C into the PC, console on the
CP2102.

```
USB XInput pad up: VID 0x045E PID 0x028E (Xbox 360 wired)
XInput interface open: IN 0x81, OUT 0x01
```

In Windows: `joy.cpl` should show a controller, and `Get-PnpDevice` the identifier
`USB\VID_045E&PID_028E`. The pad will not move — there are no inputs yet, which is correct.

**2. Proof that it really is XInput, with no game involved.** `scripts\xinput_rumble.py` calls
XInput directly through `XInput1_4.dll`:

```powershell
python scripts\xinput_rumble.py
```

It reads all four slots through `XInputGetState`, then sends three bursts of vibration with
**deliberately different** values and stops. Result on this board:

```
slot 0: CONNECTED, buttons=0x0000 LT=0 RT=0 L=(0,0) R=(0,0)
  left full   -> XInputSetState(0xffff, 0x0000) rc=0
  right full  -> XInputSetState(0x0000, 0xffff) rc=0
  both ~25%   -> XInputSetState(0x4000, 0x4000) rc=0
```

and in the board's console:

```
I (21280) usb_pad: rumble from host: left=255 right=0
I (22770) usb_pad: rumble from host: left=0 right=255
I (24280) usb_pad: rumble from host: left=64 right=64
I (25770) usb_pad: rumble from host: left=0 right=0
```

Three different values and three matching lines in the same order, scaled 16→8 bits (`0xFFFF` →
255, `0x4000` → 64). Rumble commands are sent **only** by the pad driver, never by generic HID
handling, so this proves the driver bound, the pad occupies an XInput slot, and the OUT endpoint
works — all at once. The axes and buttons are zero because there are no inputs yet.

**3. The host alone, without the pad.** Flash `s3input`, plug in the hub and **two 2.4 GHz
dongles**, with the devices' backlighting off.

```
USB host up, waiting for a keyboard and a mouse
  external hubs: supported (multi-level)
HID connected: addr … iface … sub_class 1 proto 1     <- keyboard (boot)
HID connected: addr … iface … sub_class 1 proto 2     <- mouse (boot)
KBD report len=8 [00 00 04 00]                        <- while typing
MOU report len=… [..]                                 <- while moving
```

There will be more than two `HID connected` lines — each dongle reports all of its HID interfaces.
The ones that matter are `proto 1` and `proto 2`; the rest are ignored and that is fine (§4). If
either of those two is missing, the dongle exposes no boot interface for that class and its report
descriptor will have to be looked at — but check first that the device is in 2.4 GHz mode and not
Bluetooth.

Without the line about hubs the host serves exactly one directly attached device, so two dongles
at once would be impossible. `CONFIG_USB_HOST_HUBS_SUPPORTED` exists in IDF 6.1 and is enabled in
this variant — verified in the generated `sdkconfig`.

**4. The wire.** Connect the boards as in §2 and check the link itself, without looking at the pad
yet. The transmitter sends a keepalive every 250 ms, so silence is meaningful.

```
A:  link: UART1 up: tx=GPIO4 rx=GPIO-1 921600 baud
    link: mode: sender (mouse -> host chip)
    link: sent N frames (dropped 0)

B:  link: UART1 up: tx=GPIO-1 rx=GPIO5 921600 baud
    link: peer link up
    link: peer serves: mouse keyboard
    link: received N frames (CRC errors 0)
```

The counters on both sides should agree and `CRC errors` should stay at zero. If B sees nothing,
check in order: shared ground, whether the wire goes from GPIO4 to GPIO5 (not 4→4), and whether A
really reports `tx=GPIO4`.

**5. The whole thing.** Mouse motion → right stick, WASD → left stick, keys and buttons per the
mapping table shared with the Bluetooth build ([`../README.md`](../README.md#input-mapping)).

**A trap that cost two wrong readings, so it is worth knowing in advance.** **Windows Terminal
responds to a gamepad: deflecting the left stick acts like the arrow keys** — verified by the owner
with a genuine Xbox Series X pad, independently of this bridge. Since the bridge maps WASD onto the
left stick, pressing `W` on the keyboard served by the ESP **will move the cursor in Terminal**,
which looks exactly as though the keyboard still belonged to Windows and nothing was crossing the
bridge. It is the opposite: that is evidence the whole chain works. Do not judge by Terminal;
two logs read side by side, or `XInputGetState`, are what settle it.

## 8. What is verified

**All five steps passed.** The USB bridge works end to end.

| What | Evidence |
|---|---|
| the pad is bound by XInput | `USB\VID_045E&PID_028E\08FEC93` → `Service=xusb22`, slot 0 `CONNECTED` |
| host → device path | three different `XInputSetState` pairs → three matching `rumble from host` |
| USB host + hub + dongles | `usb ifaces 4 (kbd=1 mouse=1)`, `KBD report len=8`, `MOU report len=7` |
| the UART link | `sent 19884 frames (dropped 0)` |
| mouse → right stick | 977 state changes in 18 s, decaying smoothly back to centre |
| WASD → left stick | `w` → `L=(0,32767)`, `a` → `L=(-32767,0)` |
| buttons, triggers, D-pad | owner's confirmation: **Steam's controller test shows everything correctly** |
| in a game | owner's confirmation: **Apex Legends plays fine** |
| stability | input chip's heap 347 404 B (min 346 116 B) over ~24 min |
| passthrough | `Ctrl+Alt+G` switches identity both ways; details in [`../README.md`](../README.md#passthrough-on-a-hotkey) |

Settled by measurement and needing no further checking:

- **the socket's VBUS** — 5 V is present when the board is powered from the `5V` pin (§4),
- **the adapter's logic levels** — 3.3 V on RX, TX and DTR per the module's specification,
- **the dongles' interfaces** — both expose a boot keyboard and a boot mouse, five HID interfaces
  in total (§4),
- **the flashing cycle** — `BOOT+RESET → write → RESET` (§5); flashing over UART through the
  CP2102 works too, verified on board A whose USB-C is occupied by the hub.
