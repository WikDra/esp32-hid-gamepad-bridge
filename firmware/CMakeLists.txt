cmake_minimum_required(VERSION 3.16)

# Czysty projekt ESP-IDF - bez esp-matter, bez Wi-Fi. Tylko BLE (NimBLE).
set(PROJECT_VER "0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(hid_gamepad_bridge)

#
# NimBLE: pula wpisow procedury aktualizacji parametrow polaczenia.
#
# W ESP-IDF domyslna wartosc to 1 (esp_nimble_cfg.h:698, przez #ifndef), co jest
# rozsadne dla urzadzenia z jednym linkiem. My mamy trzy jednoczesne polaczenia
# (klawiatura, mysz, PC), a kazdy peer moze zazadac zmiany parametrow. Na sprzecie
# skonczylo sie to twardym crashem WEWNATRZ NimBLE:
#
#   Load access fault, MTVAL=0x00000858
#   ble_gap_update_next_exp  (ble_gap.c:1460)  <- SLIST_FOREACH po wpisach
#    <- ble_gap_update_timer <- ble_gap_master_timer <- ble_gap_timer
#
# czyli przejscie po liscie wpisow trafilo na wskaznik-smiec. W logu towarzyszyly
# temu powtarzajace sie odrzucenia HCI_LE_Connection_Update
# (ogf=0x08 ocf=0x0013 hci_err=0x212 INV_HCI_CMD_PARMS), czyli ta sama sciezka kodu.
#
# Opcja nie jest wystawiona w Kconfig, ale naglowek uzywa #ifndef, wiec wystarczy
# zdefiniowac ja globalnie - dotyczy to takze kompilacji komponentu bt.
# UWAGA: to jest zlagodzenie objawu, nie udowodniona naprawa przyczyny.
idf_build_set_property(COMPILE_DEFINITIONS
    "MYNEWT_VAL_BLE_GAP_MAX_PENDING_CONN_PARAM_UPDATE=4" APPEND)
