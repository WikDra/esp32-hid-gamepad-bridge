# esp32-hid-gamepad-bridge

A BLE bridge on an ESP32: **your Bluetooth keyboard and mouse become an Xbox controller
that Windows exposes through XInput.**

```
BLE HID keyboard ─┐
                  ├─→ ESP32 ─→ BLE HID gamepad ─→ PC (Windows)
BLE HID mouse   ──┘   (2× central + 1× peripheral)
```

One chip holds **three simultaneous BLE links**: two as central (receiving reports from the
keyboard and the mouse) and one as peripheral (presenting the gamepad to the PC). Keystrokes
and mouse motion are mapped onto sticks, triggers, a D-pad and buttons, so from the PC's
point of view there is a single game controller.

*Polish version of this document: [`README.pl.md`](README.pl.md). Engineering notes, findings
and traps: [`AGENTS.md`](AGENTS.md).*

## Status

This is a working proof of concept, verified on hardware. Confirmed by device logs and by
games:

- **XInput works.** Windows binds its Xbox controller driver and lists the device as
  *"Bluetooth LE XINPUT compatible input device"*. Rocket League, Apex Legends and Steam all
  see the pad. Windows even sends rumble commands back to us, which only the Xbox driver does.
- **Three concurrent BLE links** on a single chip, with ~180 kB of heap to spare on the C3
  and ~250 kB on the S3.
- **Sleep/wake cycles** of the input devices work: disconnects are detected, resources freed,
  and the device reconnects on its own.
- **Measured report rates:** pad → PC at 7.5 ms (133 Hz), inputs at 15 ms (66 Hz). Split
  across two chips the mouse reaches 7.5 ms too — see *Optional: split across two chips*.
- **Verified end to end on the ESP32-C3 and the ESP32-S3.** The ESP32-C6 and ESP32-H2 build
  and run, and handle the pad and the mouse, but will not connect to our test keyboard — see
  *Known limitations*.

Getting there required fixing nine separate defects in ESP-IDF's `esp_hid` component and
working around two limitations in NimBLE's bundled services. All of it is documented in
[`AGENTS.md`](AGENTS.md) with the evidence that led to each conclusion.

## Two gamepad profiles

Selected in `menuconfig` (`APP_GAMEPAD_PROFILE`):

| Profile | What the PC sees |
|---|---|
| **Xbox (XInput)** — default | The bridge impersonates a wireless Xbox Series X controller: the HID report descriptor is byte-for-byte that of a real pad, and the PnP ID carries Microsoft's vendor ID with product ID `0x0B13`. Windows then loads its own Xbox driver and exposes the device through **XInput**, so even games that only support XInput can use it. |
| Generic (DirectInput) | 4 axes and 12 buttons, visible in `joy.cpl`. Games that only speak XInput will not see this. Kept as a fallback. |

Only PIDs `0x0B13` and `0x0B20`–`0x0B27` get the XInput driver — Windows matches on VID/PID
alone, and the older Xbox One S PID `0x02FD` is **not** on that list. See `AGENTS.md` §4.32;
this cost us a full debugging round.

Switching profiles changes both the descriptor and the device identity, so you must remove
the pad from the Windows Bluetooth device list and pair it again.

## Hardware

| Part | Notes |
|---|---|
| ESP32-C3 SuperMini | 4 MB flash, **no PSRAM**, native USB (USB Serial/JTAG) — no USB-UART bridge |
| BLE keyboard | developed against an AULA F99 Pro in BLE 5.0 mode |
| BLE mouse | developed against an AJAZZ AJ159 Pro in BLE 5.0 mode |

The board connects to the PC with a single USB-C cable, which powers it, flashes it and
carries the console. No extra wiring.

Other keyboards and mice should work: nothing in the code is specific to these two models
beyond the report-layout detection, which is driven by the devices' own HID report maps.
Two device-specific quirks we had to handle are documented (`AGENTS.md` §4.10 and §4.17) and
both are handled generically.

### Which chips this runs on

Every target below builds from the same sources; the only per-target file is
`firmware/sdkconfig.defaults.<target>`.

| Target | Controller | State |
|---|---|---|
| `esp32c3` | older family (`BT_CTRL_*`) | **reference platform**, everything verified |
| `esp32s3` | older family, shares the C3's controller library | **fully verified**: keyboard, mouse, pad, XInput |
| `esp32c6` | newer family (`BT_LE_*`) | pad and mouse work; our test keyboard does not connect |
| `esp32h2` | newer family (`BT_LE_*`) | as above; no Wi-Fi on this chip, which suits a BLE-only bridge |

The keyboard problem on the newer controllers is not a configuration mistake and not signal
strength — it was chased down to the link layer with an HCI trace. See *Known limitations*.

## Optional: split across two chips

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

It also happens to route around the limitation above: the mouse works fine on the newer
controller, and the keyboard needs the older one. Each device ends up on the chip that can
serve it.

Measured rates per hop, with the split running:

| Hop | Rate |
|---|---|
| mouse → H2 | 7.5 ms = **133 Hz** |
| H2 → S3 over UART | 108 µs per frame — not a limiter |
| keyboard → S3 | 15 ms = 66 Hz (133 Hz whenever the keyboard itself asks) |
| S3 → PC (pad) | 7.5 ms = **133 Hz** |

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

## Requirements

- **ESP-IDF v5.5.1.** Not older: `CONFIG_BT_NIMBLE_GATTC_AUTO_PAIR` does not exist before
  v5.4.3, and without it the keyboard will not hand over its reports (`AGENTS.md` §4.1).
- Windows for flashing and the console. Building works either natively on Windows or in WSL.
- Python with `pyserial` for the console scripts. The system Python usually lacks it; the
  interpreter shipped with ESP-IDF has it, and `scripts\monitor-win.bat` finds it for you.

Paths to ESP-IDF default to `%USERPROFILE%\esp\v5.5.1\esp-idf` and can be overridden:

```bat
set IDF_WIN=D:\esp\v5.5.1\esp-idf
```

> **This project ships a patched copy of an ESP-IDF component.**
> `firmware/components/esp_hid/` holds a copy of `esp_hid` with a one-line fix for a bug that
> made a device unable to reconnect after sleeping: `services_discovered` was never reset, so
> the third device open corrupted the caller's stack (`AGENTS.md` §4.27). The copy is
> **pinned to IDF 5.5.1** and must be regenerated if you change IDF version;
> `firmware/components/esp_hid/PATCH.diff` contains just the delta. To confirm the build used
> our copy rather than the one from IDF:
> ```
> python scripts/check_local_esp_hid.py
> ```

## Build and flash

Natively on Windows (no WSL needed):

```bat
scripts\build-native-win.bat              REM build for esp32c3 (the default target)
scripts\build-native-win.bat esp32s3      REM ...or any other supported target
scripts\build-native-win.bat menuconfig   REM configure
scripts\flash-win.bat COM6                REM flash
scripts\monitor-win.bat COM6 30           REM console for 30 s, without resetting the board
scripts\monitor-win.bat COM6 30 reset     REM ...and with a deliberate reset
```

Or building in WSL, flashing from Windows:

```bat
scripts\build-win.bat esp32c3
scripts\flash-win.bat COM6 esp32c3
```

Both build paths use **separate build directories** (`build.esp32c3` for WSL,
`build.win.esp32c3` for Windows), because absolute paths differ between the two and CMake
will not tolerate them in one directory. `flash-win.bat` picks whichever was built later.

Two ESP-IDF versions can coexist, which is how this project compared controller behaviour
across releases. `scripts/build.sh` takes both the IDF path and a build-directory suffix:

```bash
IDF_DIR=~/esp/v6.0.2/esp-idf BUILD_SUFFIX=.idf602 ./scripts/build.sh esp32h2
```

That yields `build.esp32h2.idf602` and its own `sdkconfig`, leaving the normal build alone.
Flashing such an image needs `esptool` directly — the scripts only look for `build.<target>`
and `build.win.<target>`; the offsets are in `flasher_args.json`.

To reboot the board without opening a console session:

```bat
scripts\reboot-win.bat COM6
```

To wipe the chip completely, including the pairing keys in NVS:

```bat
scripts\erase-win.bat COM6 esp32c3
```

Note on native USB: the DTR/RTS lines drive reset and the bootloader, so `monitor.py` opens
the port with both disabled (it will not restart the chip), while `reset_monitor.py` resets
deliberately.

## Pairing

Order matters — inputs first, then the PC:

1. Flash the firmware and leave the board powered.
2. Put the keyboard into BLE pairing mode. The bridge scans in a loop and connects on its own.

   Windows will also see the keyboard and offer to pair it — **dismiss that dialog.** If the
   keyboard pairs with Windows it will connect there instead of to the bridge.
3. Same for the mouse.
4. On the PC: *Settings → Bluetooth → Add device*. In the Xbox profile the bridge advertises
   as **Xbox Wireless Controller**.
5. Check that Windows bound the right driver. In `joy.cpl` (Win+R → `joy.cpl`) the device
   should be called *"Bluetooth LE XINPUT compatible input device"*. If you instead see
   *"HID-compliant game controller"* or *"6-axis 17-button gamepad"*, the generic driver
   attached — diagnosis in `AGENTS.md` §4.32.

Pairing keys live in NVS, so after a reboot everything reconnects by itself.

If the log shows `esp_hidh_dev_open() did not return within 45 s`, the bridge **restarts itself**.
That works around an ESP-IDF bug where a link dropping mid-discovery hangs the calling thread
forever (`AGENTS.md` §4.23). The PC comes back in ~2 s and the input devices reconnect on
their own.

**After any change to the HID report descriptor you must remove the pad from the Windows
Bluetooth list and pair again** — Windows caches the descriptor per bonded device.

## Input mapping

The inputs are identical in both profiles; only their meaning on the PC side differs.

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

- **Triggers are analog, not buttons.** A mouse click drives them to full deflection (1023),
  but nothing lights up in the `joy.cpl` button grid — triggers show up as an axis. The
  device log states it plainly: `xbox: LT=1023 RT=0 …`.
- **The D-pad only exists in the Xbox profile.** The generic descriptor has no hat switch.

The assignment lives in one table, `s_xbox_ctrl` in
[`firmware/main/ble_gamepad.c`](firmware/main/ble_gamepad.c). `input_mapper` does not know
which profile is active, so changing the mapping leaves the input side untouched.

Mouse sensitivity is `CONFIG_APP_MOUSE_SCALE_DIV` (higher = less sensitive). The default of
**24** means full deflection at an average of 96 mouse counts per task tick. Raise it to 32
or 48 if the stick still saturates too quickly, lower it to 16 if it feels sluggish. This
does **not** require re-pairing, since the descriptor is unchanged.

Connection intervals are configurable too: `APP_PAD_CONN_ITVL` for the PC link and
`APP_INPUT_CONN_ITVL` for the input links, both in units of 1.25 ms. The firmware walks a
ladder from the configured value upwards and uses the shortest interval the controller
accepts, logging each attempt.

To exercise the HID descriptor without a keyboard and mouse, enable
`CONFIG_APP_GAMEPAD_SELFTEST`: the pad then sweeps its sticks and cycles through buttons.

## Known limitations

- **Input report rate is capped at 66 Hz (15 ms).** As central, the controller refuses to
  *initiate* any connection interval below 15 ms, returning HCI `0x12` — measured identically
  on the ESP32-C3 and the ESP32-S3, which share the same controller library, so this is a
  property of that controller family rather than of one board. Six hypotheses were tested and
  eliminated by separate measurements — radio capacity, link count, grid alignment, `ce_len`,
  supervision timeout and the scanner — and there is no Kconfig option for it (`AGENTS.md`
  §4.33). The same controller happily *maintains* a shorter interval when the peer asks for
  one: that is how the pad link reaches 7.5 ms with Windows, and how our test keyboard ends up
  at 7.5 ms while the mouse, which never asks, stays at 15 ms.
- **On the ESP32-C6 and ESP32-H2 our test keyboard does not connect.** The pad and the mouse
  work there. An HCI trace from the controller's own log shows what happens: the controller
  reports the connection as established, and the link then dies immediately with HCI `0x3E`
  ("Connection Failed to be Established"), i.e. the two sides never meet on the first
  connection events. The same firmware, keyboard and room work on the C3 and S3 — at a signal
  42 dB *weaker*. Fifteen hypotheses were eliminated by measurement, including three ESP-IDF
  versions and a synthetic keyboard that copies the real one byte for byte and does connect.
  Full evidence in `AGENTS.md` §4.35.
- **Windows only.** The XInput profile targets Windows specifically. The generic profile
  should work anywhere, but is untested elsewhere.
- **The `esp_hid` patch is pinned to IDF 5.5.1.** IDF 6.0.2 fixes four of the nine defects we
  patch, so the diff has to be rewritten rather than moved.
- ESP32-C3 has no USB-OTG, so a USB (rather than Bluetooth) XInput device is not possible on
  this chip.

## Diagnostics

Optional, all off by default, all in `menuconfig`. They exist because each one answered a
question that guesswork could not:

| Option | What it does |
|---|---|
| `APP_DEBUG_SCAN_ONLY` | scans and logs, never connects — the only way to measure what a device really does in the air, since connecting stops the scan and distorts the measurement |
| `APP_ROLE_FAKE_KEYBOARD` | turns the board into an advertiser impersonating our test keyboard byte for byte, with adjustable interval, address type, flags and transmit power; gives a peer whose behaviour you control |
| `APP_DEBUG_CTRL_LOG_DUMP` | dumps the C6/H2 controller's internal log, HCI included, after a device open fails or succeeds; `scripts/decode_ctrl_log.py` turns the hex into readable HCI |
| `APP_LINK_PROBE_RX` | sweeps candidate input pins and reports where CRC-valid frames from the other chip arrive — how the S3↔H2 wiring on the BR board was established, since the documentation does not give it |
| `APP_GAMEPAD_SELFTEST` | the pad sweeps its sticks and cycles buttons, so the descriptor can be exercised with no keyboard or mouse present |
| `APP_DEBUG_WATCH_ADDR` | arms a hardware write watchpoint on an address, so a memory corruption panics with the backtrace of the culprit rather than the victim |

## Licence and attribution

This project is released under the MIT licence — see [`LICENSE`](LICENSE).

- The Xbox HID report descriptor is derived from
  [**Mystfit/ESP32-BLE-CompositeHID**](https://github.com/Mystfit/ESP32-BLE-CompositeHID)
  (MIT), which read it from a physical controller. We do not vendor their code: the
  descriptor is regenerated by `scripts/gen_xbox_report_map.py` into
  `firmware/main/xbox_report_map.h`. See [`THIRD-PARTY.md`](THIRD-PARTY.md).
- `firmware/components/esp_hid/` is a modified copy of a component from
  [**ESP-IDF**](https://github.com/espressif/esp-idf) (Apache-2.0). The modifications are
  marked with `LOCAL PATCH` comments and isolated in `PATCH.diff`.

This project is not affiliated with or endorsed by Microsoft or Espressif. It presents
Microsoft's vendor and product IDs so that Windows will load its own driver; that is
interoperability with hardware the author owns, and it is not intended for redistribution as
a product.
