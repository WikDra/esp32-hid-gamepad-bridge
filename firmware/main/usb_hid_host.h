/*
 * USB host role: reads a keyboard and a mouse over USB, through a hub.
 *
 * Hub support is a real Kconfig option in IDF 5.5.1 (CONFIG_USB_HOST_HUBS_SUPPORTED, plus
 * USB_HOST_HUB_MULTI_LEVEL for chained hubs) and not an experimental flag - ESP-IDF's own
 * usb/host/hid example turns it on in its defaults. Without it the host serves exactly one
 * directly attached device, which is not enough for keyboard AND mouse.
 *
 * Report decoding stays in the boot protocol. Both classes of device are asked for it
 * explicitly (SET_PROTOCOL 0), because that gives a fixed 8-byte keyboard and 3+ byte mouse
 * layout with no report-map parsing - and because the BLE side already taught us how much
 * grief per-device report maps cause (AGENTS.md 4.10, 4.16).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the USB host stack and the HID class driver, and starts the event task. */
esp_err_t usb_hid_host_start(void);

/* Number of HID interfaces currently open (keyboard and mouse count separately). */
int usb_hid_host_device_count(void);

#ifdef __cplusplus
}
#endif
