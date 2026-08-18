# esp32-hid-gamepad-bridge

A BLE bridge on an ESP32-C3: **your Bluetooth keyboard and mouse become an Xbox controller
that Windows exposes through XInput.**

```
BLE HID keyboard ─┐
                  ├─→ ESP32-C3 ─→ BLE HID gamepad ─→ PC (Windows)
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
- **Three concurrent BLE links** on a single ESP32-C3, with ~180 kB of heap to spare.
- **Sleep/wake cycles** of the input devices work: disconnects are detected, resources freed,
  and the device reconnects on its own.
- **Measured report rates:** pad → PC at 7.5 ms (133 Hz), inputs at 15 ms (66 Hz).

Getting there required fixing eight separate defects in ESP-IDF's `esp_hid` component and
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
scripts\build-native-win.bat              REM build
scripts\build-native-win.bat menuconfig   REM configure
scripts\flash-win.bat COM6                REM flash
scripts\monitor-win.bat COM6 30           REM console for 30 s, without resetting the board
scripts\monitor-win.bat COM6 30 reset     REM ...and with a deliberate reset
```

Or building in WSL, flashing from Windows:

```bat
scripts\build-win.bat esp32c3
scripts\flash-win.bat COM6
```

Both build paths use **separate build directories** (`build.esp32c3` for WSL,
`build.win.esp32c3` for Windows), because absolute paths differ between the two and CMake
will not tolerate them in one directory. `flash-win.bat` finds whichever is present.

To wipe the chip completely, including the pairing keys in NVS:

```bat
scripts\erase-win.bat COM6
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

If the log shows `esp_hidh_dev_open() nie wrocilo w 45 s`, the bridge **restarts itself**.
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

- **Input report rate is capped at 66 Hz (15 ms).** The ESP32-C3 controller refuses to
  *initiate* any connection interval below 15 ms as central, returning HCI `0x12`. Six
  hypotheses were tested and eliminated by separate measurements — radio capacity, link
  count, grid alignment, `ce_len`, supervision timeout and the scanner — and there is no
  Kconfig option for it. Details and the full evidence in `AGENTS.md` §4.33. The same
  controller happily *maintains* a 7.5 ms link when Windows dictates it, which is how the pad
  link reaches 133 Hz.
- **Windows only.** The XInput profile targets Windows specifically. The generic profile
  should work anywhere, but is untested elsewhere.
- **The `esp_hid` patch is pinned to IDF 5.5.1.**
- ESP32-C3 has no USB-OTG, so a USB (rather than Bluetooth) XInput device is not possible on
  this chip.

## Licence and attribution

This project is released under the MIT licence — see [`LICENSE`](LICENSE).

- The Xbox HID report descriptor is derived from
  [**Mystfit/ESP32-BLE-CompositeHID**](https://github.com/Mystfit/ESP32-BLE-CompositeHID)
  (MIT), which read it from a physical controller. We do not vendor their code: the
  descriptor is regenerated by `scripts/gen_xbox_report_map.py` into
  `firmware/main/xbox_report_map.h`. See [`THIRD-PARTY.md`](THIRD-PARTY.md).
- `firmware/components/esp_hid/` is a modified copy of a component from
  [**ESP-IDF**](https://github.com/espressif/esp-idf) (Apache-2.0). The modifications are
  marked with `LOKALNA LATKA` comments and isolated in `PATCH.diff`.

This project is not affiliated with or endorsed by Microsoft or Espressif. It presents
Microsoft's vendor and product IDs so that Windows will load its own driver; that is
interoperability with hardware the author owns, and it is not intended for redistribution as
a product.
