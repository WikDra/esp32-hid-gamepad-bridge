/*
 * USB gamepad: presents an Xbox 360 wired controller (XInput) over USB.
 *
 * WHY THE XBOX 360 AND NOT THE SERIES X. Windows binds its XInput driver by matching the
 * USB device ID, and the match list is in C:\Windows\INF\xusb22.inf - read off this machine:
 *
 *     %XUSB22.DeviceName.Wired%=CC_Install, USB\Vid_045E&Pid_028E   <- Xbox 360 wired
 *     %XUSB22.DeviceName%=CC_Install,       USB\Vid_045E&Pid_0719   <- 360 wireless receiver
 *     %XUSB22.DeviceName.Jump%=CC_Install,  USB\Vid_045E&Pid_028F   <- play and charge
 *
 * VID/PID only - no interface class in the match. The Series X pad speaks GIP over USB and
 * is served by xboxgip.sys, a different and far larger protocol. So over USB the 360 wired
 * pad is the right thing to emulate; games still see a plain XInput controller.
 *
 * AND WHY THE BLE DESCRIPTOR DOES NOT TRANSFER. Our BLE profile works because
 * xinputhid.inf binds BTHLEDevice VID&02045E PID&0B13 over HID. That file contains ZERO
 * "USB\" entries (checked), so a USB HID device with the same identity gets the generic
 * driver and no XInput. USB XInput is not HID at all: it is a vendor-specific interface
 * (0xFF / 0x5D / 0x01) with two interrupt endpoints and no report descriptor.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gamepad_state.h"
#include "input_state.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the USB device stack and registers the XInput class driver. Call once. */
esp_err_t usb_pad_start(void);

/* Whether the host has configured us and the IN endpoint is usable. */
bool usb_pad_is_ready(void);

/*
 * Converts the mapper's state into a 20-byte XInput report and queues it on EP 0x81.
 * Returns true when the report was accepted (or was identical to the last one, which is
 * deliberately not resent). Mirrors ble_gamepad_send()'s contract.
 */
/*
 * Reports ACTUALLY put on the wire since boot. Monotonic.
 *
 * Distinct from how often the mapper ran: usb_pad_send() returns true when the state has not
 * changed and nothing was transmitted, so counting its return value measures the task rate. Both
 * numbers are useful and they are not the same - see the comment at the increment.
 */
uint32_t usb_pad_reports_sent(void);

bool usb_pad_send(const gamepad_state_t *state);

/*
 * Last rumble values received from the host, 0..255. The host writing rumble is the
 * decisive proof that the XInput driver bound rather than a generic one - the same test
 * that settled the BLE profile (AGENTS.md 4.32).
 */
void usb_pad_get_rumble(uint8_t *left, uint8_t *right);

#if CONFIG_APP_USB_PASSTHROUGH

/*
 * PASSTHROUGH MODE. The chip carries two USB identities and only one can be on the bus at a
 * time, so switching means disconnecting and re-enumerating as the other one:
 *
 *   gamepad      VID 0x045E PID 0x028E   vendor XInput interface, bound by xusb22
 *   passthrough  VID 0x303A PID 0x4004   one HID interface, keyboard + mouse by report ID
 *
 * WHY NOT ONE COMPOSITE DEVICE: xusb22 binds at DEVICE level, not per interface - the real
 * Xbox 360 pad has four interfaces and the driver owns all of them. Keyboard and mouse
 * interfaces under the same VID/PID would be swallowed by it and never reach Windows as input
 * devices. The pad therefore disappears while passthrough is active.
 */

/* Asks for a mode. Safe from any task: it only records the request. */
void usb_pad_request_mode(bool passthrough);

/*
 * Applies a pending request and returns the mode now in force. MUST be called from the same
 * task that calls the send functions below - that is what keeps re-enumeration and report
 * submission in one task, so neither needs a lock. Call once per tick.
 */
bool usb_pad_service_mode(void);

/*
 * Passthrough mode: turns raw input state into HID keyboard and mouse reports. Only one report
 * fits in flight per tick, so keyboard and mouse take turns and unsent mouse motion is carried
 * over rather than dropped.
 */
bool usb_pad_send_passthrough(const hid_input_state_t *state);

#endif /* CONFIG_APP_USB_PASSTHROUGH */

#ifdef __cplusplus
}
#endif
