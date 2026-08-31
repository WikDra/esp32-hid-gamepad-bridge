/*
 * Peripheral role: the HID gamepad the PC sees.
 *
 * The HID service (0x1812) and the Device Information service (0x180A) are written
 * directly on GATT here rather than taken from NimBLE - both bundled services turned
 * out to be unusable for impersonating an Xbox pad (AGENTS.md 4.30). This module owns
 * the report descriptor, advertising, a single connection (with an explicit role check,
 * which esp_hidd lacks) and notification delivery.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* D-pad bitmap and gamepad_state_t are shared with the USB pad. */
#include "gamepad_state.h"

/* Registers the HID service in GATT and starts the advertising task.
 * Call after ble_stack_init() and before ble_stack_start(). */
esp_err_t ble_gamepad_start(void);

/* Whether the PC is connected and subscribed to report notifications. */
bool ble_gamepad_is_ready(void);

/* Sends a report, but only when the state differs from the previously sent one.
 * Returns true if a notification went out. */
bool ble_gamepad_send(const gamepad_state_t *state);

#ifdef __cplusplus
}
#endif
