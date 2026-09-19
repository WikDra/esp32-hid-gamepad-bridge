# esp32-hid-gamepad-bridge

**Your keyboard and mouse become an Xbox controller that Windows exposes through XInput.**
Two transports, both verified on hardware: over **Bluetooth LE** on a single ESP32, with no wiring
at all, or over **USB** as a USB Host → USB Device bridge on two ESP32-S3 boards, running every hop
at 1 ms with no pairing anywhere.

| | [Bluetooth LE](#bluetooth-le) | [USB](#usb) |
|---|---|---|
| architecture | 2× BLE central + 1× BLE peripheral, one chip | USB host → UART → USB device, two chips |
| chips | **one** ESP32 | **two** ESP32-S3 |
| input devices | BLE keyboard and mouse | any USB keyboard and mouse, or their 2.4 GHz dongles |
| what the PC sees | wireless **Xbox Series X** pad | wired **Xbox 360** pad |
| pairing | keyboard, mouse and pad each pair once | **none at all** |
| works before Windows boots | no | **yes**, including firmware setup screens |
| pad update rate | 133 Hz | **1 kHz** (997 Hz measured) |
| latency added by the bridge | ≤ 15 ms | **≤ 2 ms** |
| extra hardware | none, one USB-C cable | powered hub, USB-UART adapter, three wires |
| tuning and remapping | rebuild and reflash | [web panel](#configuration-panel-over-wi-fi), live, plus firmware updates |

**Measured on hardware, not claimed:**

- **Played in Apex Legends**, and Rocket League and Steam's controller test see the pad as an
  ordinary XInput controller. Windows loads *its own* Xbox driver in both transports and sends
  rumble back to us — which only that driver does.
- **1 kHz end to end over USB.** The dongle is polled every 1 ms, the UART link needs 108 µs per
  frame, and the pad's IN endpoint is polled every 1 ms; **997 Hz** measured through
  `XInputGetState` with an instrument that removes the hand from the measurement. Added latency
  is one task period plus one poll, so **≤ 2 ms**.
- **Zero pairing on USB**, and the pad works before Windows boots — it is a plain wired
  controller as far as the machine is concerned.

The **66 Hz / 133 Hz** figures further down are the **Bluetooth** transport's, where the interval
is negotiated over the air and the controller refuses to go below 15 ms as central
([why](#known-limitations)). USB is not bound by any of that.

Keystrokes and mouse motion are mapped onto sticks, triggers, a D-pad and buttons, so from the
PC's point of view there is a single game controller. The mapping, the mouse curve and every input
quirk are shared by both transports; only the two ends are swapped at compile time.

*Polish version of this document: [`README.pl.md`](README.pl.md). Engineering notes, findings
and traps, with the evidence behind every decision: [`AGENTS.md`](AGENTS.md).*

## Contents

- [Status](#status)
- [Bluetooth LE](#bluetooth-le) — one chip, three links
  - [Hardware and supported chips](#hardware-and-supported-chips)
  - [Pairing](#pairing)
  - [Two gamepad profiles](#two-gamepad-profiles)
  - [Optional: split across two chips](#optional-split-across-two-chips)
- [USB](#usb) — two chips, no pairing
  - [What it needs](#what-it-needs)
  - [Wiring and flashing](#wiring-and-flashing)
  - [Passthrough on a hotkey](#passthrough-on-a-hotkey)
  - [Configuration panel over Wi-Fi](#configuration-panel-over-wi-fi)
- [Input mapping](#input-mapping) — shared by both transports
- [Report rates](#report-rates) — every measured number, in one place
- [Requirements and build](#requirements-and-build)
- [Known limitations](#known-limitations)
- [Diagnostics](#diagnostics)
- [Licence and attribution](#licence-and-attribution)

## Status

Both transports are verified end to end. The evidence behind each claim, transport by transport:

**Bluetooth LE:**

- **XInput works.** Windows binds its own Xbox controller driver and lists the device as
  *"Bluetooth LE XINPUT compatible input device"*. It even sends rumble commands back to us,
  which only the Xbox driver does.
- **Three concurrent BLE links** on a single chip, with ~180 kB of heap to spare on the C3 and
  ~250 kB on the S3.
- **Sleep/wake cycles** of the input devices work: disconnects are detected, resources freed,
  and the device reconnects on its own.
- **Verified end to end on the ESP32-C3 and the ESP32-S3.** The C6 and H2 build and run, and
  handle the pad and the mouse, but will not connect to our test keyboard — see
  [Known limitations](#known-limitations).

**USB:**

- **The same XInput driver binds**, matched on `USB\Vid_045E&Pid_028E` instead:
  `Service=xusb22`, and `XInputGetState` reports the pad in slot 0.
- **Traffic flows both ways** — three deliberately different `XInputSetState` motor pairs
  produced three matching `rumble from host` lines in the device log.
- **The whole chain measured from both ends at once:** `usb ifaces 4 (kbd=1 mouse=1)` and
  `link: sent 19884 frames (dropped 0)` on the input chip, against `w` → `L=(0,32767)` and
  `a` → `L=(-32767,0)` read through XInput on the PC.
- **Buttons, triggers and D-pad** confirmed by Steam's controller test, and the whole thing
  played in **Apex Legends**.

Getting there required fixing nine separate defects in ESP-IDF's `esp_hid` component and
working around two limitations in NimBLE's bundled services. All of it is in
[`AGENTS.md`](AGENTS.md) with the evidence that led to each conclusion.

---

## Bluetooth LE

One chip holds **three simultaneous BLE links**: two as central, receiving reports from the
keyboard and the mouse, and one as peripheral, presenting the gamepad to the PC.

```
BLE HID keyboard ─┐
                  ├─→ ESP32 ─→ BLE HID gamepad ─→ PC (Windows)
BLE HID mouse   ──┘   (2× central + 1× peripheral)
```

### Hardware and supported chips

| Part | Notes |
|---|---|
| ESP32-C3 SuperMini | 4 MB flash, **no PSRAM**, native USB (USB Serial/JTAG) — no USB-UART bridge |
| BLE keyboard | developed against an AULA F99 Pro in BLE 5.0 mode |
| BLE mouse | developed against an AJAZZ AJ159 Pro in BLE 5.0 mode |

The board connects to the PC with a single USB-C cable, which powers it, flashes it and carries
the console. No extra wiring.

Other keyboards and mice should work: nothing in the code is specific to these two models
beyond the report-layout detection, which is driven by the devices' own HID report maps. Two
device-specific quirks we had to handle are documented (`AGENTS.md` §4.10 and §4.17) and both
are handled generically.

Every target builds from the same sources; the only per-target file is
`firmware/sdkconfig.defaults.<target>`.

| Target | Controller | State |
|---|---|---|
| `esp32c3` | older family (`BT_CTRL_*`) | **reference platform**, everything verified |
| `esp32s3` | older family, shares the C3's controller library | **fully verified**: keyboard, mouse, pad, XInput |
| `esp32c6` | newer family (`BT_LE_*`) | pad and mouse work; our test keyboard does not connect |
| `esp32h2` | newer family (`BT_LE_*`) | as above; no Wi-Fi on this chip, which suits a BLE-only bridge |

The keyboard problem on the newer controllers is not a configuration mistake and not signal
strength — it was chased down to the link layer with an HCI trace. See
[Known limitations](#known-limitations).

### Pairing

Order matters — inputs first, then the PC:

1. Flash the firmware and leave the board powered.
2. Put the keyboard into BLE pairing mode. The bridge scans in a loop and connects on its own.

   Windows will also see the keyboard and offer to pair it — **dismiss that dialog.** If the
   keyboard pairs with Windows it will connect there instead of to the bridge.
3. Same for the mouse.
4. On the PC: *Settings → Bluetooth → Add device*. In the Xbox profile the bridge advertises as
   **Xbox Wireless Controller**.
5. Check that Windows bound the right driver. In `joy.cpl` (Win+R → `joy.cpl`) the device should
   be called *"Bluetooth LE XINPUT compatible input device"*. If you instead see *"HID-compliant
   game controller"* or *"6-axis 17-button gamepad"*, the generic driver attached — diagnosis in
   `AGENTS.md` §4.32.

Pairing keys live in NVS, so after a reboot everything reconnects by itself.

If the log shows `esp_hidh_dev_open() did not return within 45 s`, the bridge **restarts
itself**. That works around an ESP-IDF bug where a link dropping mid-discovery hangs the calling
thread forever (`AGENTS.md` §4.23). The PC comes back in ~2 s and the input devices reconnect on
their own.

**After any change to the HID report descriptor you must remove the pad from the Windows
Bluetooth list and pair again** — Windows caches the descriptor per bonded device.

### Two gamepad profiles

Bluetooth only; the USB transport always presents an Xbox 360 pad. Selected in `menuconfig`
(`APP_GAMEPAD_PROFILE`):

| Profile | What the PC sees |
|---|---|
| **Xbox (XInput)** — default | The bridge impersonates a wireless Xbox Series X controller: the HID report descriptor is byte-for-byte that of a real pad, and the PnP ID carries Microsoft's vendor ID with product ID `0x0B13`. Windows then loads its own Xbox driver and exposes the device through **XInput**, so even games that only support XInput can use it. |
| Generic (DirectInput) | 4 axes and 12 buttons, visible in `joy.cpl`. Games that only speak XInput will not see this. Kept as a fallback. |

Only PIDs `0x0B13` and `0x0B20`–`0x0B27` get the XInput driver — Windows matches on VID/PID
alone, and the older Xbox One S PID `0x02FD` is **not** on that list (`AGENTS.md` §4.32; this
cost a full debugging round). Switching profiles changes both the descriptor and the device
identity, so the pad has to be removed from Windows and paired again.

### Optional: split across two chips

On a board that carries two SoCs wired together — the **ESP Thread Border Router board**, with
an ESP32-S3 and an ESP32-H2 on one PCB — the bridge can run split across both:

```
BLE keyboard ──→ S3 (central) ─┐
                               ├─→ BLE gamepad ──→ PC     (the S3 is the peripheral)
BLE mouse ─────→ H2 (central) ──→ UART ──→ S3
```

**The point is not the speed of the wire.** A mouse frame is 10 bytes, which at 921600 baud is
~108 µs, against a BLE connection interval of 7.5–15 ms: four orders of magnitude apart, so the
transport is not part of the latency. The gain is **radio time** — each chip serves fewer links
instead of one antenna interleaving keyboard, mouse and pad.

It also routes around the keyboard limitation above: the mouse works fine on the newer
controller family and the keyboard needs the older one, so each device ends up on the chip that
can serve it. Rates per hop are in [Report rates](#report-rates).

Build and flash each chip with its own target; the roles come from the per-target defaults:

```bat
scripts\build-native-win.bat esp32s3
scripts\flash-win.bat COM10 esp32s3     REM host: keyboard + pad
scripts\build-native-win.bat esp32h2
scripts\flash-win.bat COM11 esp32h2     REM satellite: mouse
```

Pair the mouse with the H2, the keyboard with the S3, and the pad with the PC. The PC sees one
controller and no trace of the mouse being on a different radio.

Two things worth knowing if you adapt this to another board:

- **The interconnect pin was measured, not read from a datasheet.** ESP-IDF's `ot_br` example
  hardcodes GPIO4/5, but its README shows that as DevKit-to-DevKit wiring and it is wrong for
  this board. `APP_LINK_PROBE_RX` sweeps the input pins and reports where CRC-valid frames
  arrive; on the BR board the answer is **S3 GPIO17 ← H2 GPIO24**. The link is one-directional,
  so the host chip's TX stays unassigned.
- **The H2's console must leave UART.** The link lands on that chip's default UART0 pins, so a
  UART console would put log text on the same wire. The board gives each chip its own USB
  socket, so the console goes over USB Serial/JTAG instead.

The link carries a framed, CRC-checked protocol with a keepalive, so silence is meaningful: if
the satellite disappears, the host releases any held mouse button and the pad and keyboard keep
working. Measured over 10 951 frames with zero CRC errors.

---

## USB

Two ESP32-S3 boards, wired together with three wires, take USB input devices on one end and
present a **wired Xbox 360 controller** on the other. No Bluetooth anywhere:

```
keyboard 2.4 GHz dongle ─┐                                    ┌─ Xbox 360 pad (XInput) ─→ PC
                         ├─→ hub ─→ [S3 #1] ────UART────→ [S3 #2] ┘
mouse 2.4 GHz dongle ────┘         USB host              USB device
```

Windows binds `xusb22`, the same XInput driver as in the Bluetooth profile, but matched on
`USB\Vid_045E&Pid_028E`. **There is no pairing at all**, and the pad works from the moment it is
plugged in.

**USB XInput is not HID**, so none of the Bluetooth work carries over: it is a vendor-specific
interface (class `0xFF`, subclass `0x5D`, protocol `0x01`) with two interrupt endpoints and no
report descriptor at all. `xinputhid.inf`, which binds the BLE pad, contains zero `USB\` entries,
so a USB HID device with PID `0x0B13` would get the generic driver instead. The interface
descriptor here is byte-for-byte a real controller's, checked against a Wireshark capture by
`scripts/check_xinput_descriptor.py` — and on the **built binary**, not on the source.

### What it needs

- **An ESP32-S3, -S2 or -P4 for each end.** The C3 has no USB-OTG peripheral, so it cannot do
  this at all.
- **A USB-UART adapter for the console.** Both boards have their USB port taken, and on the S3
  the USB Serial/JTAG peripheral shares GPIO19/20 with USB-OTG, so the console must move to
  UART. One adapter is enough — it swaps between the boards.
- **A powered hub** for the input devices, and a USB-C to USB-A OTG adapter for the host board.
- **Three wires between the boards:** `GPIO4` → `GPIO5` (crossed) and a common ground.

Input devices can be plugged in directly or, as in the setup this was developed on, through
their **2.4 GHz dongles** — which keeps the keyboard and mouse wireless and draws far less
current, since a dongle neither lights up nor charges a battery.

### Wiring and flashing

Full bench sheet, with the pin header layout, the power arithmetic and every trap met while
bringing it up: **[`docs/WIRING-USB.md`](docs/WIRING-USB.md)**
(Polish version: [`docs/WIRING-USB.pl.md`](docs/WIRING-USB.pl.md)).

```bat
scripts\build-native-win.ps1 esp32s3 s3input
scripts\flash-win.ps1  COM<n> esp32s3 s3input
scripts\build-native-win.ps1 esp32s3 s3pad
scripts\flash-win.ps1  COM<m> esp32s3 s3pad
```

Two things about flashing these boards, both of which look like failures and are not:

- **The cycle is BOOT+RESET → flash → RESET.** The hard reset esptool performs after writing
  does not leave download mode, and the symptom is indistinguishable from dead firmware until
  you check with `esptool --before no-reset flash-id`, which only succeeds while the chip sits
  in the bootloader.
- **Once the application runs it owns the USB pins, so the COM port disappears.** That is the
  pad working, not a fault.

### Passthrough on a hotkey

`Ctrl+Alt+G` on the keyboard switches the pad chip between the gamepad and a plain HID keyboard
plus mouse, so the same devices can be used for typing without unplugging anything. Verified in
both directions:

| Mode | Identity | What Windows binds |
|---|---|---|
| gamepad | `045E:028E` | `xusb22`, XInput slot 0 |
| passthrough | `303A:4004` | `kbdhid` and `mouhid` on two report collections |

**It has to be two identities rather than one composite device.** `xusb22` binds at *device*
level, not per interface — the real Xbox 360 pad has four interfaces and the driver owns all of
them, so keyboard and mouse interfaces under the same VID/PID would be swallowed by it and never
reach Windows as input devices. The chip therefore disconnects, swaps descriptors and
re-enumerates, which means **the pad disappears while passthrough is active**. That is the
accepted cost; design details in `AGENTS.md` §4.39.

The key is `APP_PASSTHROUGH_KEYCODE` and the feature can be turned off with
`APP_USB_PASSTHROUGH`.

### Configuration panel over Wi-Fi

`Ctrl+Alt+W` brings up a web panel for changing sensitivity, smoothing, deadzone compensation, the
key mapping and profiles — and for updating the firmware. **Wi-Fi is down until that hotkey, and
shuts itself off after ten idle minutes**, which is what keeps it free: nothing competes with the
1 kHz report path while a game is running, so the measured 997 Hz ceiling stays a statement about
this firmware rather than about the radio.

| Hotkey | What it does |
|---|---|
| `Ctrl+Alt+W` | Wi-Fi and the panel on / off |
| `Ctrl+Alt+P` | force the bridge's own access point, and bring Wi-Fi up if it was off |

Without stored credentials it serves an access point named `hid-bridge-XXXX`, where `XXXX` comes
from the MAC. **The password is printed in the console at startup** and, unless you set
`APP_WEBUI_PASSWORD`, is derived from the MAC — unique per board rather than a value every clone of
this repository would share. It is the WPA2 key and the panel's password both; the panel's username
is `admin`.

Give it your own network from the System tab and it joins that instead and drops the access point,
so the panel sits at a stable address reachable from a phone already on your Wi-Fi. `Ctrl+Alt+P` is
the way back if that network is ever out of reach — which matters, because a panel you cannot get
to would otherwise need a serial adapter to diagnose.

What it can change:

- **Mouse to right stick:** sensitivity per axis, smoothing time constant, Y inversion, and
  deadzone compensation — which lifts any non-zero deflection above the inner deadzone that games
  discard, applied to the *length* of the vector so diagonals do not overshoot. Changes apply
  immediately, so a setting you can only judge by feel no longer needs a rebuild and a reflash.
- **The mapping**, as a table: press *Listen* and then a key on the keyboard the bridge owns, and
  it binds. Every nominal button is labelled with the Xbox control it drives.
- **Four profiles in NVS**, named, with export and import as a JSON file.
- **Firmware**, by uploading `hid_gamepad_bridge.bin`. The image is verified before anything
  reboots and is only confirmed after the new firmware has stayed up for 30 s, so one that crashes
  or boot-loops rolls back on its own.

It also shows the live pad state, the raw keyboard and mouse input before mapping, and the report
rate measured from the device side — the same figure `scripts/xinput_rumble.py --rate` reads from
the PC.

> **Two things to know before relying on it.**
>
> **It is not encrypted.** Authentication is HTTP Basic, which is base64 rather than ciphertext. On
> the access point WPA2 covers the air; joined to your network, panel traffic is in the clear on
> that network. The password still matters — this API can change what the pad reports and can flash
> firmware, so an unauthenticated one would hand both to anyone who can reach it.
>
> **The pad variant uses a different partition table** ([`firmware/partitions.csv`](firmware/partitions.csv)),
> because firmware updates need two application slots. That moves NVS, so the first flash with it
> wipes whatever was stored — which costs nothing here, since this variant has no Bluetooth bonds.
> If you are updating an existing checkout, **delete `firmware/sdkconfig.win.esp32s3.s3pad` once**:
> a value already in a generated sdkconfig beats a defaults file, so otherwise the build quietly
> keeps the old single-slot table and updates fail. The firmware logs a warning at startup when
> that has happened.

Enabled with `APP_WEBUI`, **on both chips** — the pad chip runs the panel, the input chip has the
keyboard and so is the only one that can see the hotkeys.

---

## Input mapping

Shared by both transports. In the Bluetooth build the meaning depends on the selected profile;
over USB it is always the Xbox column.

| Input | Xbox profile (XInput) | Generic profile |
|---|---|---|
| `W` / `S` / `A` / `D` | left stick | left stick |
| arrow keys | **D-pad** | unused |
| mouse movement | right stick | right stick |
| left mouse button | **right trigger (RT)** | button 1 |
| right mouse button | **left trigger (LT)** | button 2 |
| middle mouse button | right stick click (RS) | button 3 |
| `Space` | **A** | button 4 |
| left `Shift` | left stick click (LS) | button 5 |
| left `Ctrl` | **B** | button 6 |
| `E` | **X** | button 7 |
| `Q` | **Y** | button 8 |
| `R` | LB | button 9 |
| `F` | RB | button 10 |
| `Tab` | View | button 11 |
| `Esc` | Menu | button 12 |

Diagonals on the left stick are scaled so that moving diagonally is not faster than moving
straight. The right stick gets a smoothed, scaled mouse delta and returns to centre when the
mouse stops.

Two things worth knowing when testing the Xbox profile:

- **Triggers are analog, not buttons.** A mouse click drives one to full deflection, but nothing
  lights up in the `joy.cpl` button grid — a trigger shows up as an axis. The device log states
  it plainly: `xbox: LT=1023 RT=0 …`.
- **The D-pad only exists in the Xbox profile.** The generic descriptor has no hat switch.

The assignment lives in one table, `s_xbox_ctrl` in
[`firmware/main/ble_gamepad.c`](firmware/main/ble_gamepad.c). `input_mapper` does not know which
profile or transport is active, so changing the mapping leaves the input side untouched.

**Mouse sensitivity is `APP_MOUSE_SCALE_DIV`** (higher = less sensitive). Full stick deflection
corresponds to a mouse speed of `div * 400` counts **per second** — per second, not per task
tick, so the feel does not change when the report rate does. The default of 24 means 9600
counts/s; the USB pad variant raises it to 64, because at 1 kHz with the full resolution of the
filter 9600 counts/s is one flick of the wrist on a high-DPI mouse. Expect to try a few values:
the useful setting depends on mouse DPI, which the firmware cannot know. Changing it needs only a
rebuild, never re-pairing.

Connection intervals are configurable too, for the Bluetooth transport: `APP_PAD_CONN_ITVL` for
the PC link and `APP_INPUT_CONN_ITVL` for the input links, both in units of 1.25 ms. The firmware
walks a ladder from the configured value upwards, uses the shortest interval the controller
accepts, and logs each attempt.

## Report rates

Every rate figure in this project, measured rather than assumed. **Bluetooth, one chip:**

| Hop | Rate | Set by |
|---|---|---|
| keyboard / mouse → ESP32 | 15 ms = **66 Hz** | the controller's floor as central — see [Known limitations](#known-limitations) |
| ESP32 → PC (pad) | 7.5 ms = **133 Hz** | Windows, which is the central on that link |

**Bluetooth, split across two chips** — the mouse gains because the H2's newer controller will
initiate a shorter interval:

| Hop | Rate |
|---|---|
| mouse → H2 | 7.5 ms = **133 Hz** |
| H2 → S3 over UART | 108 µs per frame — not a limiter |
| keyboard → S3 | 15 ms = 66 Hz (133 Hz whenever the keyboard itself asks) |
| S3 → PC (pad) | 7.5 ms = **133 Hz** |

**USB:**

| Hop | Rate | Set by |
|---|---|---|
| dongle → input chip | **~750 Hz** | the dongle's `bInterval`, i.e. 1 ms polling |
| input chip → pad chip | 108 µs per frame at 921600 baud | not a limiter — ~11 % of the wire at 1000 frames/s |
| pad chip → PC | **997 Hz** transport ceiling; 830 Hz *observed* with a real mouse | the IN endpoint's 1 ms interval |

Those last two numbers mean different things and the difference matters. Both were measured from
the PC with `scripts/xinput_rumble.py --rate`, in a poll loop fast enough not to be the limit
itself (415 000 polls/s):

- **997 Hz** is the transport ceiling, taken with `APP_DEBUG_PAD_RATE_PROBE`, which forces a
  different report every tick. It does not depend on anyone's hand, so it is the number to trust.
- **830 Hz** is with real mouse input during brisk movement. Slow movement gives far less — 70 Hz
  was measured — and that is correct rather than broken: a report only goes out when the state
  changes, and the filter is designed to hold a *steady* deflection at a steady mouse speed. The
  update count therefore measures how much the stick value varies, not how good the pipeline is.

Three arithmetic fixes in the mapper were needed to get there, none of them in USB: the filter
time constant and the sensitivity were expressed in ticks rather than in time, so raising the rate
silently changed the feel; the fixed-point accumulator was too coarse and stalled at 1 kHz; and
the result was truncated to whole mouse counts per tick, which at 1 kHz left three usable levels.
Before-and-after numbers are in `AGENTS.md` §4.40.

**Two traps worth knowing if you change these settings.** `pdMS_TO_TICKS()` rounds down to whole
ticks, so a report period shorter than one FreeRTOS tick silently becomes one tick — at the
default 100 Hz tick a 1 ms period becomes 10 ms. The USB pad variant therefore sets
`CONFIG_FREERTOS_HZ=1000`, and the mapper logs the rate it actually achieves rather than the one
requested. And `APP_XINPUT_EP_INTERVAL_MS` at 1 ms is **the one field where the USB descriptor is
no longer byte-for-byte** the real controller's; that is safe, because `xusb22.inf` matches on
VID/PID alone and never reads the descriptor, and `check_xinput_descriptor.py` reports the
deviation rather than passing silently. Set it back to 4 for exact fidelity at 250 Hz.

## Requirements and build

- **ESP-IDF v5.5.1** for the Bluetooth transport. Not older: `CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR`
  does not exist before v5.4.3, and without it the keyboard will not hand over its reports
  (`AGENTS.md` §4.1). IDF 6.1 builds every variant after one portability fix, but see the note on
  the patched component below.
- Windows for flashing and the console. Building works either natively on Windows or in WSL.
- Python with `pyserial` for the console scripts. The system Python usually lacks it; the
  interpreter shipped with ESP-IDF has it, and `scripts\monitor-win.bat` finds it for you.

Paths to ESP-IDF default to `%USERPROFILE%\esp\v5.5.1\esp-idf` and can be overridden with
`set IDF_WIN=D:\esp\v5.5.1\esp-idf`.

> **This project ships a patched copy of an ESP-IDF component.**
> `firmware/components/esp_hid/` holds a copy of `esp_hid` with a one-line fix for a bug that
> made a device unable to reconnect after sleeping: `services_discovered` was never reset, so the
> third device open corrupted the caller's stack (`AGENTS.md` §4.27). The copy is **pinned to IDF
> 5.5.1** and must be regenerated if you change IDF version;
> `firmware/components/esp_hid/PATCH.diff` contains just the delta. To confirm the build used our
> copy rather than the one from IDF: `python scripts/check_local_esp_hid.py`.

Natively on Windows, no WSL needed:

```bat
scripts\build-native-win.bat              REM build for esp32c3 (the default target)
scripts\build-native-win.bat esp32s3      REM ...or any other supported target
scripts\build-native-win.bat menuconfig   REM configure
scripts\flash-win.bat COM6                REM flash
scripts\monitor-win.bat COM6 30           REM console for 30 s, without resetting the board
scripts\monitor-win.bat COM6 30 reset     REM ...and with a deliberate reset
scripts\reboot-win.bat COM6               REM reboot without opening a console
scripts\erase-win.bat COM6 esp32c3        REM wipe the chip, pairing keys included
```

There are `.ps1` equivalents of the build and flash scripts, and the USB variants need them:
ESP-IDF's newer **EIM installer** puts the Python environment somewhere `export.bat` does not
look, so every `.bat` script fails on such an installation with `'idf.py' is not recognized`.
`scripts/idf-env.ps1` understands both layouts, and `build-native-win.ps1` and `flash-win.ps1`
stand on it. They also take a **variant** name, which is how one target carries two roles:

```powershell
scripts\build-native-win.ps1 esp32s3 s3pad
scripts\flash-win.ps1 COM5 esp32s3 s3input -Uart   # -Uart: flash through a USB-UART adapter
```

Or building in WSL, flashing from Windows:

```bat
scripts\build-win.bat esp32c3
scripts\flash-win.bat COM6 esp32c3
```

Both build paths use **separate build directories** (`build.esp32c3` for WSL,
`build.win.esp32c3` for Windows), because absolute paths differ between the two and CMake will
not tolerate them in one directory. `flash-win.bat` picks whichever was built later.

Two ESP-IDF versions can coexist, which is how this project compared controller behaviour across
releases. `scripts/build.sh` takes both the IDF path and a build-directory suffix:

```bash
IDF_DIR=~/esp/v6.0.2/esp-idf BUILD_SUFFIX=.idf602 ./scripts/build.sh esp32h2
```

That yields `build.esp32h2.idf602` and its own `sdkconfig`, leaving the normal build alone.
Flashing such an image needs `esptool` directly — the scripts only look for `build.<target>` and
`build.win.<target>`; the offsets are in `flasher_args.json`.

On a board with native USB the DTR/RTS lines drive reset and the bootloader, so `monitor.py`
opens the port with both disabled and will not restart the chip, while `reset_monitor.py` resets
deliberately.

## Known limitations

> **The mouse-to-stick arithmetic changed after the Bluetooth build was last verified on
> hardware, and that change is UNTESTED over BLE.** `input_mapper.c` now expresses the filter
> time constant and the sensitivity in time rather than in task ticks, and no longer truncates the
> result to whole mouse counts per tick (`AGENTS.md` §4.40). It was measured on the USB pad only.
>
> At a 100 Hz report rate, which is what the BLE build uses, the nominal sensitivity and time
> constant come out identical to the hand-tuned values of `AGENTS.md` §4.22. What differs is
> resolution: small movements now register proportionally instead of being rounded away, so the
> right stick will feel finer and possibly livelier than before.
>
> **If the BLE build misbehaves, go back to commit `c25c017`** — the last commit with the
> arithmetic exactly as it was when the Bluetooth bridge was verified end to end, and one that
> already contains all of the USB work:
>
> ```
> git checkout c25c017          # inspect it
> git revert 9a5f535            # or drop just this change on a branch
> ```

- **Bluetooth input rate is capped at 66 Hz (15 ms).** As central, the controller refuses to
  *initiate* any connection interval below 15 ms, returning HCI `0x12` — measured identically on
  the ESP32-C3 and the ESP32-S3, which share the same controller library, so this is a property
  of that controller family rather than of one board. Six hypotheses were eliminated by separate
  measurements — radio capacity, link count, grid alignment, `ce_len`, supervision timeout and the
  scanner — and there is no Kconfig option for it (`AGENTS.md` §4.33). The same controller happily
  *maintains* a shorter interval when the peer asks for one: that is how the pad link reaches
  7.5 ms with Windows, and how our test keyboard ends up at 7.5 ms while the mouse, which never
  asks, stays at 15 ms. The [split](#optional-split-across-two-chips) and USB transports both
  route around it.
- **On the ESP32-C6 and ESP32-H2 our test keyboard does not connect.** The pad and the mouse work
  there. An HCI trace from the controller's own log shows what happens: the controller reports the
  connection as established, and the link then dies immediately with HCI `0x3E` ("Connection
  Failed to be Established"), i.e. the two sides never meet on the first connection events. The
  same firmware, keyboard and room work on the C3 and S3 — at a signal 42 dB *weaker*. Fifteen
  hypotheses were eliminated by measurement, including three ESP-IDF versions and a synthetic
  keyboard that copies the real one byte for byte and does connect. Full evidence in `AGENTS.md`
  §4.35.
- **The USB pad's observable update rate depends on how you move**, between 70 Hz and 830 Hz
  against a 997 Hz transport ceiling. That is a property of the smoothing filter rather than a
  fault — see [Report rates](#report-rates).
- **Windows only.** The XInput profiles target Windows specifically. The generic DirectInput
  profile should work anywhere, but is untested elsewhere.
- **The `esp_hid` patch is pinned to IDF 5.5.1.** IDF 6.0.2 and 6.1 fix four of the nine defects
  we patch, so on those the diff has to be rewritten rather than moved — and our copy currently
  *shadows* the fixed upstream one. This affects the Bluetooth roles only; the USB variants link
  the component but never use it.

## Diagnostics

Optional, all off by default, all in `menuconfig`. Each exists because it answered a question
guesswork could not:

| Option | What it does |
|---|---|
| `APP_DEBUG_SCAN_ONLY` | scans and logs, never connects — the only way to measure what a device really does in the air, since connecting stops the scan and distorts the measurement |
| `APP_ROLE_FAKE_KEYBOARD` | turns the board into an advertiser impersonating our test keyboard byte for byte, with adjustable interval, address type, flags and transmit power; gives a peer whose behaviour you control |
| `APP_DEBUG_CTRL_LOG_DUMP` | dumps the C6/H2 controller's internal log, HCI included, after a device open fails or succeeds; `scripts/decode_ctrl_log.py` turns the hex into readable HCI |
| `APP_LINK_PROBE_RX` | sweeps candidate input pins and reports where CRC-valid frames from the other chip arrive — how the S3↔H2 wiring on the BR board was established, since the documentation does not give it |
| `APP_GAMEPAD_SELFTEST` | the pad sweeps its sticks and cycles buttons, so the descriptor can be exercised with no keyboard or mouse present |
| `APP_DEBUG_PAD_RATE_PROBE` | forces a different pad report every tick, so a packet count measures the transport instead of the smoothing filter and the hand on the mouse |
| `APP_DEBUG_WATCH_ADDR` | arms a hardware write watchpoint on an address, so a memory corruption panics with the backtrace of the culprit rather than the victim |

Two host-side tools exist for the same reason — they answer questions the device log cannot:

| Script | What it answers |
|---|---|
| `scripts/xinput_rumble.py` | whether XInput actually *talks* to the pad, rather than merely whether a driver bound: it reads all four slots through `XInputGetState`, then sends three deliberately different motor pairs so the device log can be matched against them line by line. `--watch <s>` is a live state view keyed on `dwPacketNumber`, the end-to-end test taken from the side a game reads; `--rate <s>` measures how often the pad really updates |
| `scripts/check_xinput_descriptor.py` | whether the USB descriptor that will really be on the wire matches a Wireshark capture of a genuine controller — it parses the **built binary**, not the source |
| `scripts/check_mapper_math.py` | whether the mouse-to-stick arithmetic still means what it says. It models the integer maths and asserts the properties that broke before: that the same mouse speed gives the same deflection at 100 Hz and at 1 kHz, that the filter does not stall, and that deadzone compensation stays monotonic and symmetric. A model can drift from the firmware, so a failure means "one of these two is wrong" |
| `scripts/check_doc_links.py` | whether every cross-reference in this documentation still resolves — file links and `#anchors` alike. The docs deliberately point at each other so that one fact lives in one place, and that only stays true if a rename cannot break it silently |

## Licence and attribution

This project is released under the MIT licence — see [`LICENSE`](LICENSE).

- The Xbox HID report descriptor is derived from
  [**Mystfit/ESP32-BLE-CompositeHID**](https://github.com/Mystfit/ESP32-BLE-CompositeHID) (MIT),
  which read it from a physical controller. We do not vendor their code: the descriptor is
  regenerated by `scripts/gen_xbox_report_map.py` into `firmware/main/xbox_report_map.h`. See
  [`THIRD-PARTY.md`](THIRD-PARTY.md).
- The USB XInput descriptor and report format come from a Wireshark capture of a genuine Xbox 360
  wired controller published on partsnotincluded.com, cross-checked against the Linux `xpad`
  driver. See [`THIRD-PARTY.md`](THIRD-PARTY.md).
- `firmware/components/esp_hid/` is a modified copy of a component from
  [**ESP-IDF**](https://github.com/espressif/esp-idf) (Apache-2.0). The modifications are marked
  with `LOCAL PATCH` comments and isolated in `PATCH.diff`.

This project is not affiliated with or endorsed by Microsoft or Espressif. It presents
Microsoft's vendor and product IDs so that Windows will load its own driver; that is
interoperability with hardware the author owns, and it is not intended for redistribution as a
product.
