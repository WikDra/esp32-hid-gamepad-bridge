/*
 * Ties the central and peripheral roles together: reads state from ble_hid_host and
 * sends reports through ble_gamepad. Requires both roles enabled in Kconfig.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the mapping task. Call after ble_hid_host_start() and ble_gamepad_start(). */
esp_err_t input_mapper_start(void);

#ifdef __cplusplus
}
#endif
