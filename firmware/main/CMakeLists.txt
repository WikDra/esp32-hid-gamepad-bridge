set(srcs "app_main.c" "ble_stack.c")

if(CONFIG_APP_ENABLE_HID_HOST)
    list(APPEND srcs "ble_hid_host.c")
endif()

if(CONFIG_APP_ENABLE_GAMEPAD)
    list(APPEND srcs "ble_gamepad.c")
endif()

# The glue between the roles only makes sense when both are enabled and the pad is
# not running the test pattern.
if(CONFIG_APP_ENABLE_HID_HOST AND CONFIG_APP_ENABLE_GAMEPAD AND NOT CONFIG_APP_GAMEPAD_SELFTEST)
    list(APPEND srcs "input_mapper.c")
endif()

if(CONFIG_APP_ROLE_FAKE_KEYBOARD)
    list(APPEND srcs "fake_keyboard.c")
endif()

# Split bridge across the two SoCs of an ESP Thread BR board.
if(NOT CONFIG_APP_LINK_DISABLED)
    list(APPEND srcs "chip_link.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt nvs_flash esp_timer driver
    PRIV_REQUIRES spi_flash
)
