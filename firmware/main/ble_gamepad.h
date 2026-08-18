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

/* D-pad (hat switch). A bitmap, because the arrow keys are independent; opposite
 * directions pressed together cancel out in ble_gamepad.c.
 *
 * NOTE: the D-pad only works in the Xbox profile. The generic pad descriptor has no
 * hat switch, and adding one would require re-pairing that profile in Windows. */
#define GAMEPAD_DPAD_UP    0x01
#define GAMEPAD_DPAD_RIGHT 0x02
#define GAMEPAD_DPAD_DOWN  0x04
#define GAMEPAD_DPAD_LEFT  0x08

/* Axes are signed 8-bit, range -127..127, centred on 0.
 * Buttons: bit 0 = button 1, ..., bit 11 = button 12. */
typedef struct {
    int8_t lx;
    int8_t ly;
    int8_t rx;
    int8_t ry;
    uint16_t buttons;
    uint8_t dpad;
} gamepad_state_t;

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
