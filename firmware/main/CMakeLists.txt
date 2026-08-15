set(srcs "app_main.c")

if(CONFIG_APP_ENABLE_HID_HOST)
    list(APPEND srcs "ble_hid_host.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt nvs_flash esp_timer
    PRIV_REQUIRES spi_flash
)
