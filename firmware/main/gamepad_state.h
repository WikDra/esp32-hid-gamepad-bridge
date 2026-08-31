/*
 * The pad state produced by input_mapper, independent of how it reaches the PC.
 *
 * Split out of ble_gamepad.h when the USB pad was added: the mapper builds this struct and
 * either ble_gamepad.c (BLE HID) or usb_pad.c (USB XInput) turns it into wire format. The
 * mapper does not know which, and neither transport knows about the other.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* D-pad (hat switch). A bitmap, because the arrow keys are independent; opposite
 * directions pressed together cancel out in the transport layer.
 *
 * NOTE: on BLE the D-pad only works in the Xbox profile - the generic pad descriptor has no
 * hat switch. On USB XInput it always works, because the Xbox 360 report has D-pad bits. */
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

#ifdef __cplusplus
}
#endif
