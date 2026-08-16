/*
 * Etap 3: spina role central i peripheral. Czyta stan z ble_hid_host i wysyla
 * raporty przez ble_gamepad. Wymaga, zeby obie role byly wlaczone w Kconfig.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Startuje zadanie mapujace. Wolac po ble_hid_host_start() i ble_gamepad_start(). */
esp_err_t input_mapper_start(void);

#ifdef __cplusplus
}
#endif
