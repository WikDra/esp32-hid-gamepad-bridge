/*
 * The binding vocabulary: what an input source is, what a pad action is, and the table that
 * connects the two.
 *
 * This lives in its own header for the same reason gamepad_state.h does: more than one module
 * needs the vocabulary and only one of them is the mapper. input_mapper applies a table, and a
 * configuration layer will later load one from NVS and let it be edited.
 *
 * THE TABLE IS THE EDITABLE FORM, NOT THE FORM THAT IS USED. input_mapper derives constant-time
 * lookup arrays from it whenever it changes, because the mapping runs up to 1000 times per
 * second and scanning a table there would be paying per tick for the convenience of an editor.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Upper bound on bindings. The default table uses 20. The limit exists because the table is
 * copied into a fixed buffer (and, later, into an NVS blob of a known size) rather than because
 * anything in the algorithm cares.
 */
#define INPUT_BIND_MAX 40

/* Where an input comes from. */
typedef enum {
    BIND_SRC_NONE = 0,
    BIND_SRC_KEY,       /* code = USB HID keycode, e.g. 0x1A for W */
    BIND_SRC_MOD,       /* code = modifier MASK: bit0 LCtrl, bit1 LShift ... bit7 RGui */
    BIND_SRC_MOUSE_BTN, /* code = mouse button MASK: bit0 left, bit1 right, bit2 middle */
} input_bind_src_t;

/*
 * What an input does to the pad.
 *
 * Buttons are NOMINAL, numbered 1..12, exactly as they were when the mapping was a chain of
 * hardcoded conditions. ble_gamepad.c translates them into Xbox controls through its own table
 * and usb_pad.c does the same for XInput, so this enum knows nothing about profiles, triggers
 * or wire formats - and the mapping stays valid across both transports.
 *
 * Only the LEFT stick is drivable from a digital input. The right stick belongs to the mouse,
 * and mixing a key-driven deflection with a filtered mouse delta on the same axis needs a
 * combining rule that nothing so far has asked for.
 */
typedef enum {
    ACT_NONE = 0,

    ACT_LSTICK_LEFT,
    ACT_LSTICK_RIGHT,
    ACT_LSTICK_UP,
    ACT_LSTICK_DOWN,

    ACT_DPAD_UP,
    ACT_DPAD_RIGHT,
    ACT_DPAD_DOWN,
    ACT_DPAD_LEFT,

    ACT_BTN_1,
    ACT_BTN_2,
    ACT_BTN_3,
    ACT_BTN_4,
    ACT_BTN_5,
    ACT_BTN_6,
    ACT_BTN_7,
    ACT_BTN_8,
    ACT_BTN_9,
    ACT_BTN_10,
    ACT_BTN_11,
    ACT_BTN_12,

    ACT_COUNT
} input_action_t;

/* One row of the table. Three bytes, so a full table is 120 B. */
typedef struct {
    uint8_t src;    /* input_bind_src_t */
    uint8_t code;   /* keycode or mask, per src */
    uint8_t action; /* input_action_t */
} input_bind_t;

#ifdef __cplusplus
}
#endif
