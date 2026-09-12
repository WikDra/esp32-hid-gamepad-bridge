/*
 * The keyboard/mouse state that input_mapper consumes, and the accumulator behind it.
 *
 * Two producers exist and only one is built at a time:
 *   - ble_hid_host.c  - BLE central; keeps its own copy of this struct (historical, and it
 *                       is entangled with the device table), so it does NOT use the
 *                       accumulator below. It only needs the type.
 *   - chip_link.c / usb_hid_host.c - the USB build; both feed the accumulator here.
 *
 * The type lives here rather than in ble_hid_host.h so that a USB-only build does not have
 * to include a BLE header for a plain struct.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB HID keyboard boot protocol: up to 6 keys held simultaneously. */
#define HID_KEYS_MAX 6

typedef struct {
    /* Keyboard: modifier bitmap (bit0 LCtrl ... bit7 RGui) and USB HID keycodes. */
    uint8_t modifiers;
    uint8_t keys[HID_KEYS_MAX];

    /* Mouse: button bitmap (bit0 left, bit1 right, bit2 middle). */
    uint8_t mouse_buttons;

    /* Mouse: motion accumulated since the previous state read. The mouse reports
     * deltas and the pad task runs at a different rate, so we sum them up. */
    int32_t mouse_dx;
    int32_t mouse_dy;
    int32_t mouse_wheel;

    bool keyboard_connected;
    bool mouse_connected;
} hid_input_state_t;

/*
 * Snapshot the state. The mouse motion accumulators are cleared in the process, so every
 * delta ends up in exactly one gamepad report - same contract as
 * ble_hid_host_take_state().
 */
void input_state_take(hid_input_state_t *out);

/* Absolute keyboard state, as it arrives (boot-protocol layout). */
void input_state_set_keyboard(uint8_t modifiers, const uint8_t keys[HID_KEYS_MAX]);

/* One mouse report: buttons are absolute, motion accumulates. */
void input_state_accum_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel);

/*
 * Presence. Clearing a device also clears what it was holding down - a key or mouse button
 * held at the instant a device vanished would otherwise stay held in the pad report
 * forever. That failure mode is not hypothetical: it bit us on the BLE split bridge
 * (AGENTS.md 4.36) and the fix there had to be explicit for the same reason.
 */
void input_state_set_keyboard_present(bool present);
void input_state_set_mouse_present(bool present);

#ifdef __cplusplus
}
#endif
