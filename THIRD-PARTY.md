# Third-party material

Two parts of this repository come from other projects. Both keep their original
licences; the project's own MIT licence in [`LICENSE`](LICENSE) does not override them.

## 1. Xbox HID report descriptor

**Where:** `firmware/main/xbox_report_map.h` (generated file)
**Origin:** [Mystfit/ESP32-BLE-CompositeHID](https://github.com/Mystfit/ESP32-BLE-CompositeHID),
file `XboxDescriptors.h`, array `XboxOneS_1914_HIDDescriptor`. That project is a fork of
[lemmingDev/ESP32-BLE-Gamepad](https://github.com/lemmingDev/ESP32-BLE-Gamepad) and read the
descriptor from a physical Xbox controller.
**Licence:** MIT.

We do **not** vendor their source code. Only the descriptor byte sequence is used, and it is
regenerated on demand by `scripts/gen_xbox_report_map.py`:

```
git clone --depth 1 https://github.com/Mystfit/ESP32-BLE-CompositeHID .ref
python3 scripts/gen_xbox_report_map.py .ref/XboxDescriptors.h
```

The generated header is committed so that a normal build does not need the reference
repository. The generator also prints a `sha256` of the descriptor, so any change is visible.

Original MIT notice from the upstream project:

```
Software License Agreement (MIT License)

Copyright (c) 2021 lemmingDev - https://github.com/lemmingDev

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## 2. Patched copy of the ESP-IDF `esp_hid` component

**Where:** `firmware/components/esp_hid/`
**Origin:** [espressif/esp-idf](https://github.com/espressif/esp-idf), component
`components/esp_hid`, version **v5.5.1**.
**Licence:** Apache-2.0. Source files keep their original
`SPDX-FileCopyrightText: 2017-2024 Espressif Systems (Shanghai) CO LTD` headers.

Exactly one file differs from upstream: `src/nimble_hidh.c`. The changes are marked with
`LOKALNA LATKA` comments and are also isolated in
[`firmware/components/esp_hid/PATCH.diff`](firmware/components/esp_hid/PATCH.diff), which
applies with `patch -p0`. Reproducibility was verified: a clean copy from IDF plus the patch
yields a byte-identical file.

What the patch fixes (details in `AGENTS.md` §4.27):

- `services_discovered` was never reset, so the counter grew for the lifetime of the
  firmware while the discovery callback wrote into a 10-element array on the **caller's
  stack**. The third device open corrupted that stack frame, which is why a device could not
  reconnect after sleeping.
- Bounds checks were added to all three discovery callbacks; upstream has only a comment
  ("fatal if services are more than 10") and no test.
- Two log lines were raised from DEBUG to INFO, because the message that actually reports a
  successful subscription was invisible while a misleading one was printed at INFO
  (`AGENTS.md` §4.29).

## Trademarks

"Xbox", "Xbox Wireless Controller" and "Windows" are trademarks of Microsoft Corporation.
"ESP32" and "ESP-IDF" are trademarks of Espressif Systems.

This project is not affiliated with, endorsed by, or sponsored by Microsoft or Espressif.
The firmware presents Microsoft's USB vendor ID and an Xbox product ID so that Windows loads
its own driver — this is interoperability with hardware the author owns, done for personal
use, and the repository is not a product distribution.
