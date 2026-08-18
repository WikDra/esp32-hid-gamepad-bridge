/*
 * Shared NimBLE stack setup for both roles (central + peripheral).
 *
 * Why a separate module: ble_hs_cfg is global and singular. That is exactly where
 * ESP-IDF's esp_hidd and esp_hidh fall over when used together - each overwrites the
 * other's sync_cb and reset_cb (AGENTS.md 4.2). To avoid repeating that mistake here,
 * the whole of ble_hs_cfg is configured in ONE place, and the ordering relative to
 * esp_hidh_init() is enforced by the API: init -> (roles register) -> start.
 *
 * Call order:
 *   ble_stack_init();          // nimble_port_init + security + key store
 *   ble_hid_host_start();      // calls esp_hidh_init(), which clobbers sync_cb
 *   ble_gamepad_start();       // registers the HID service in GATT
 *   ble_stack_start();         // restores our callbacks and starts the host task
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nimble_port_init() + ble_hs_cfg setup. Does not bring up the radio yet. */
esp_err_t ble_stack_init(void);

/* Restores our sync_cb/reset_cb (esp_hidh_init overwrites them) and starts the
 * NimBLE host task. Call last, once every role has registered. */
esp_err_t ble_stack_start(void);

/* Blocks until the stack is ready for use (BLE_GAP synced). */
bool ble_stack_wait_synced(uint32_t timeout_ms);

/* Own address type as inferred by ble_hs_id_infer_auto(). */
uint8_t ble_stack_own_addr_type(void);

#ifdef __cplusplus
}
#endif
