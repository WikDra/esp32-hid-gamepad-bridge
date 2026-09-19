set(srcs "app_main.c")

# ---------------------------------------------------------------------------------------------
# REQUIRES MUST NOT BE GATED ON CONFIG_*. It cannot work, and it fails silently.
#
# ESP-IDF processes this file TWICE, and the configuration exists for only one of the passes.
# Measured on this project with a message(STATUS) probe:
#
#   -- SONDA: APP_WEBUI=[]  USB_PAD=[]  faza=[__component_get_requirements]
#   -- SONDA: APP_WEBUI=[y] USB_PAD=[y] faza=[]
#
# The first pass is the one that collects REQUIRES. So conditional SOURCES work and conditional
# REQUIREMENTS silently do not - the classic shape of a fault in this project: it configures,
# compiles, links, reports success and does something other than it says.
#
# This was found the hard way. The file used to append `bt` inside a BLE condition and `usb` inside
# a USB one, and both appeared to work - because NimBLE's headers arrive transitively through
# esp_hid, and `usb` through the managed usb_host_hid component. Neither line ever did anything.
# The panel's dependencies have no such accidental path, so they failed outright with
# "esp_app_desc.h: No such file or directory".
#
# WHAT IS AVAILABLE IN THAT PASS IS IDF_TARGET, which is what idf_component.yml already gates on,
# so that is what is used here. It also has to be: `usb` does not exist on the ESP32-C3 at all, so
# an unconditional list breaks that build with "Failed to resolve component 'usb'". Target and
# configuration happen to agree here - only the USB-OTG parts can be a USB pad, and only a USB pad
# runs the panel (see APP_WEBUI in Kconfig.projbuild).
#
# The cost of naming a component a variant does not use is compile time, NOT image size:
# -ffunction-sections with --gc-sections drops whatever is never referenced.
#
# The GPIO and UART drivers are named explicitly rather than pulled in through the "driver"
# meta-component: IDF 5.x re-exported them, 6.x does not, and a build there fails with
# "chip_link.c includes driver/uart.h, provided by esp_driver_uart". Both names exist since 5.3,
# which is this project's floor.
# ---------------------------------------------------------------------------------------------
set(reqs esp_hid nvs_flash esp_timer esp_driver_gpio esp_driver_uart bt)

if(IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    # USB host role, and everything the configuration panel needs: Wi-Fi, the HTTP server, base64
    # for the password check, and the OTA plus application-description APIs.
    list(APPEND reqs usb esp_wifi esp_netif esp_event esp_http_server mbedtls app_update
                     esp_app_format)
endif()

# Files embedded into the image. Empty unless the configuration panel is built; EMBED_FILES with
# an empty list is legal, which is what keeps the page out of every other variant.
set(embeds "")

# The BLE stack is only linked when a BLE role is on, so a USB-only build does not pay for
# NimBLE (~60 kB of heap and a lot of flash).
if(CONFIG_APP_ENABLE_HID_HOST OR CONFIG_APP_ENABLE_GAMEPAD OR CONFIG_APP_ROLE_FAKE_KEYBOARD)
    list(APPEND srcs "ble_stack.c")
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
endif()

if(CONFIG_APP_USB_HID_HOST)
    list(APPEND srcs "usb_hid_host.c")
endif()

# The configuration panel. Built only on the chip that owns the pad, because that is where the
# mapper and the settings it reads live; the input chip contributes hotkey detection, which is in
# usb_hid_host.c and needs nothing extra. webui.h supplies empty inlines when this is skipped, so
# no call site has to know - see AGENTS.md 4.38 for why that matters.
if(CONFIG_APP_WEBUI AND CONFIG_APP_USB_PAD)
    list(APPEND srcs "webui.c" "webui_http.c")
    # The page is embedded in the application image rather than kept in a filesystem partition, so
    # an update replaces the panel and the API it talks to together and they cannot disagree about
    # each other's version.
    set(embeds "web/index.html")
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
    EMBED_FILES ${embeds}
)
