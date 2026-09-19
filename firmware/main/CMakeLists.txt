set(srcs "app_main.c")
# The GPIO and UART drivers are named EXPLICITLY rather than pulled in through the "driver"
# meta-component. On IDF 5.x "driver" still re-exported them, but IDF 6.x dropped that: a
# build there fails with "chip_link.c includes driver/uart.h, provided by esp_driver_uart".
# Both component names exist since 5.3, which is this project's floor (idf_component.yml).
set(reqs esp_hid nvs_flash esp_timer esp_driver_gpio esp_driver_uart)

# The BLE stack is only linked when a BLE role is on, so a USB-only build does not pay for
# NimBLE (~60 kB of heap and a lot of flash).
if(CONFIG_APP_ENABLE_HID_HOST OR CONFIG_APP_ENABLE_GAMEPAD OR CONFIG_APP_ROLE_FAKE_KEYBOARD)
    list(APPEND srcs "ble_stack.c")
    list(APPEND reqs bt)
endif()

if(CONFIG_APP_ENABLE_HID_HOST)
    list(APPEND srcs "ble_hid_host.c")
endif()

if(CONFIG_APP_ENABLE_GAMEPAD)
    list(APPEND srcs "ble_gamepad.c")
endif()

# The shared input accumulator is the state source whenever ble_hid_host is not - i.e. in
# every USB build. ble_hid_host keeps its own copy for historical reasons.
if(NOT CONFIG_APP_ENABLE_HID_HOST)
    list(APPEND srcs "input_state.c")
endif()

if(CONFIG_APP_USB_PAD)
    list(APPEND srcs "usb_pad.c")
    list(APPEND reqs espressif__esp_tinyusb)
endif()

if(CONFIG_APP_USB_HID_HOST)
    list(APPEND srcs "usb_hid_host.c")
    list(APPEND reqs espressif__usb_host_hid usb)
endif()

# The configuration panel. Built only on the chip that owns the pad, because that is where the
# mapper and the settings it reads live; the input chip contributes hotkey detection, which is in
# usb_hid_host.c and needs nothing extra. webui.h supplies empty inlines when this is skipped, so
# no call site has to know - see AGENTS.md 4.38 for why that matters.
if(CONFIG_APP_WEBUI AND CONFIG_APP_USB_PAD)
    list(APPEND srcs "webui.c")
    list(APPEND reqs esp_wifi esp_netif esp_event)
endif()

# The glue between input and pad. Needed for either pad transport, but not when the pad is
# running its own test pattern - the two would fight over the same report.
#
# bridge_config goes with it rather than being unconditional: it exists to feed the mapper, and
# the two are gated on the same condition ON PURPOSE. A build where one is present and the other
# is not would be the shape of AGENTS.md 4.38 all over again.
if(NOT CONFIG_APP_GAMEPAD_SELFTEST)
    if((CONFIG_APP_ENABLE_HID_HOST AND CONFIG_APP_ENABLE_GAMEPAD) OR CONFIG_APP_USB_PAD)
        list(APPEND srcs "input_mapper.c" "bridge_config.c")
    endif()
endif()

if(CONFIG_APP_ROLE_FAKE_KEYBOARD)
    list(APPEND srcs "fake_keyboard.c")
endif()

# Inter-chip UART link: used by both the BLE split (ESP Thread BR board) and the USB split.
if(NOT CONFIG_APP_LINK_DISABLED)
    list(APPEND srcs "chip_link.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES ${reqs}
    PRIV_REQUIRES spi_flash
)
