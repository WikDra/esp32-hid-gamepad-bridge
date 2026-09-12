/* Shared input accumulator - see input_state.h. */

#include "input_state.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "input";

static hid_input_state_t s_state;

/*
 * A spinlock rather than a mutex, matching ble_hid_host.c: the producers run in driver
 * callbacks and the consumer is a 100 Hz task, so the critical sections are a handful of
 * assignments and must not block.
 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void input_state_take(hid_input_state_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_state;
    s_state.mouse_dx = 0;
    s_state.mouse_dy = 0;
    s_state.mouse_wheel = 0;
    taskEXIT_CRITICAL(&s_mux);
}

void input_state_set_keyboard(uint8_t modifiers, const uint8_t keys[HID_KEYS_MAX])
{
    taskENTER_CRITICAL(&s_mux);
    s_state.keyboard_connected = true;
    s_state.modifiers = modifiers;
    memcpy(s_state.keys, keys, HID_KEYS_MAX);
    taskEXIT_CRITICAL(&s_mux);
}

void input_state_accum_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel)
{
    taskENTER_CRITICAL(&s_mux);
    s_state.mouse_connected = true;
    s_state.mouse_buttons = buttons;
    s_state.mouse_dx += dx;
    s_state.mouse_dy += dy;
    s_state.mouse_wheel += wheel;
    taskEXIT_CRITICAL(&s_mux);
}

void input_state_set_keyboard_present(bool present)
{
    uint8_t was_mods;
    uint8_t was_key;

    taskENTER_CRITICAL(&s_mux);
    s_state.keyboard_connected = present;
    was_mods = s_state.modifiers;
    was_key = s_state.keys[0];
    if (!present) {
        s_state.modifiers = 0;
        memset(s_state.keys, 0, sizeof(s_state.keys));
    }
    taskEXIT_CRITICAL(&s_mux);

    /* Logged outside the critical section, and only when something was actually held, so the
     * log carries the proof that this path releases stuck keys. */
    if (!present && (was_mods || was_key)) {
        ESP_LOGW(TAG, "keyboard gone - released keys still held: mod=0x%02x key=0x%02x",
                 was_mods, was_key);
    }
}

void input_state_set_mouse_present(bool present)
{
    uint8_t was_held;

    taskENTER_CRITICAL(&s_mux);
    s_state.mouse_connected = present;
    was_held = s_state.mouse_buttons;
    if (!present) {
        s_state.mouse_buttons = 0;
        s_state.mouse_dx = 0;
        s_state.mouse_dy = 0;
        s_state.mouse_wheel = 0;
    }
    taskEXIT_CRITICAL(&s_mux);

    if (!present && was_held) {
        ESP_LOGW(TAG, "mouse gone - released buttons still held: 0x%02x", was_held);
    }
}
