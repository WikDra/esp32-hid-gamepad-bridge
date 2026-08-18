cmake_minimum_required(VERSION 3.16)

# A plain ESP-IDF project - no esp-matter, no Wi-Fi. BLE only (NimBLE).
set(PROJECT_VER "0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(hid_gamepad_bridge)

#
# NimBLE: pool of connection-parameter update procedure entries.
#
# In ESP-IDF the default is 1 (esp_nimble_cfg.h:698, via #ifndef), which is
# reasonable for a single-link device. We have three simultaneous connections
# (keyboard, mouse, PC) and any peer may request a parameter change. On hardware
# that ended in a hard crash INSIDE NimBLE:
#
#   Load access fault, MTVAL=0x00000858
#   ble_gap_update_next_exp  (ble_gap.c:1460)  <- SLIST_FOREACH over the entries
#    <- ble_gap_update_timer <- ble_gap_master_timer <- ble_gap_timer
#
# i.e. walking the entry list hit a garbage pointer. The log showed repeated
# rejections of HCI_LE_Connection_Update alongside it
# (ogf=0x08 ocf=0x0013 hci_err=0x212 INV_HCI_CMD_PARMS), i.e. the same code path.
#
# The option is not exposed in Kconfig, but the header uses #ifndef, so defining it
# globally is enough - that covers the compilation of the bt component too.
# NOTE: this mitigates the symptom; the proven root cause is in AGENTS.md 4.28.
idf_build_set_property(COMPILE_DEFINITIONS
    "MYNEWT_VAL_BLE_GAP_MAX_PENDING_CONN_PARAM_UPDATE=4" APPEND)
