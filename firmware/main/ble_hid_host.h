/*
 * Central role: receives HID reports from a BLE keyboard and mouse (HOGP profile).
 *
 * The module starts the NimBLE stack, scans in a loop and connects to EVERY HID
 * device it finds until it reaches the configured limit. Unlike the pattern in the
 * reference project there is no single "connected" flag here - we need the keyboard
 * and the mouse at the same time (AGENTS.md 4.3).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* hid_input_state_t and HID_KEYS_MAX live here, shared with the USB build. */
#include "input_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts NimBLE, esp_hidh and the scanning task. Call once, from app_main. */
esp_err_t ble_hid_host_start(void);

/* Snapshot of the input state. The mouse motion accumulators are cleared in the
 * process, so every delta ends up in exactly one gamepad report. */
void ble_hid_host_take_state(hid_input_state_t *out);

/*
 * Split bridge: feed a mouse report that arrived from the other chip over UART, as if it
 * had come from a mouse connected here. Accumulates exactly like the local path, so the
 * mapper and the pad need to know nothing about where the mouse is.
 */
void ble_hid_host_inject_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel);

/*
 * Split bridge: mark the remote mouse present or gone. Called with false when the link
 * goes silent or the peer reports that its mouse dropped - without it a button held at
 * that moment would stay held in the pad report forever.
 */
void ble_hid_host_set_remote_mouse(bool connected);

/* Number of currently open HID devices (0..HID_HOST_MAX_DEVICES). */
int ble_hid_host_device_count(void);

/*
 * Whether a device open is in progress (connection + full GATT service discovery).
 * That is the heaviest moment for the stack: one link runs dozens of GATT procedures
 * while the other two are active. The mapping task suspends pad notifications during
 * that window so as not to add load - see AGENTS.md 4.26.
 */
bool ble_hid_host_is_opening(void);

/* Prints the list of connected devices to the console - for diagnostics. */
void ble_hid_host_log_devices(void);

#ifdef __cplusplus
}
#endif
