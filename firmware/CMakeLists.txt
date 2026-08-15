cmake_minimum_required(VERSION 3.16)

# Czysty projekt ESP-IDF - bez esp-matter, bez Wi-Fi. Tylko BLE (NimBLE).
set(PROJECT_VER "0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(hid_gamepad_bridge)
