/*
 * Mostek BLE HID: klawiatura + mysz -> pad, na ESP32-C3 SuperMini.
 *
 * Stan: Etap 1 - rola central (odbior raportow z klawiatury i myszy).
 * Kolejne etapy (patrz AGENTS.md, sekcja 5) dodaja tutaj:
 *   Etap 2 - ble_gamepad_start()   (rola peripheral, wlasny serwer GATT)
 *   Etap 3 - zadanie mapujace wejscia na raport pada
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "ble_stack.h"

#if CONFIG_APP_ENABLE_HID_HOST
#include "ble_hid_host.h"
#endif
#if CONFIG_APP_ENABLE_GAMEPAD
#include "ble_gamepad.h"
#endif

static const char *TAG = "bridge";

static void log_boot_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    ESP_LOGI(TAG, "=== esp32-hid-gamepad-bridge ===");
    ESP_LOGI(TAG, "IDF %s, target %s rev v%d.%d, %d rdzen(i)",
             esp_get_idf_version(), CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "flash %" PRIu32 " kB, PSRAM %s",
             flash_size / 1024,
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "jest" : "brak");
    ESP_LOGI(TAG, "heap przed BLE: free %" PRIu32 " B, najwiekszy blok %" PRIu32 " B",
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    /* Role sa przelacznikami z menuconfig, zeby dalo sie testowac etapy osobno. */
    ESP_LOGI(TAG, "role: hid_host=%s gamepad=%s selftest=%s",
#if CONFIG_APP_ENABLE_HID_HOST
             "on",
#else
             "off",
#endif
#if CONFIG_APP_ENABLE_GAMEPAD
             "on",
#else
             "off",
#endif
#if CONFIG_APP_GAMEPAD_SELFTEST
             "on"
#else
             "off"
#endif
    );
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS wymaga wyczyszczenia (%s), kasuje partycje", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    log_boot_banner();

    /*
     * Kolejnosc jest wymuszona przez to, ze ble_hs_cfg jest globalne i esp_hidh_init()
     * je nadpisuje. ble_stack_start() ustawia nasze callbacki na koniec (AGENTS.md 4.2).
     */
    ESP_ERROR_CHECK(ble_stack_init());

#if CONFIG_APP_ENABLE_HID_HOST
    ESP_ERROR_CHECK(ble_hid_host_start());
#endif
#if CONFIG_APP_ENABLE_GAMEPAD
    ESP_ERROR_CHECK(ble_gamepad_start());
#endif

    ESP_ERROR_CHECK(ble_stack_start());

    /* Heartbeat: odroznia "firmware stoi i czeka" od "firmware sie wysypal".
     * Przy okazji pokazuje pamiec, co jest kluczowe na C3 bez PSRAM. */
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        tick += 5;

#if CONFIG_APP_ENABLE_GAMEPAD
        const char *pad = ble_gamepad_is_ready() ? "gotowy" : "brak PC";
#else
        const char *pad = "wyl.";
#endif

#if CONFIG_APP_ENABLE_HID_HOST
        hid_input_state_t st;
        ble_hid_host_take_state(&st);
        ESP_LOGI(TAG, "alive %" PRIu32 " s | heap %" PRIu32 " B (min %" PRIu32 " B) | wejscia %d (kbd=%d mouse=%d) | pad %s",
                 tick, (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size(),
                 ble_hid_host_device_count(), st.keyboard_connected, st.mouse_connected, pad);
        if (st.mouse_dx || st.mouse_dy || st.mouse_wheel) {
            ESP_LOGI(TAG, "  ruch myszy od ostatniego odczytu: dx=%" PRId32 " dy=%" PRId32 " wheel=%" PRId32,
                     st.mouse_dx, st.mouse_dy, st.mouse_wheel);
        }
        if (st.modifiers || st.keys[0]) {
            ESP_LOGI(TAG, "  klawiatura: mod=0x%02x keys=%02x %02x %02x %02x %02x %02x",
                     st.modifiers, st.keys[0], st.keys[1], st.keys[2], st.keys[3], st.keys[4], st.keys[5]);
        }
        if (ble_hid_host_device_count() > 0) {
            ble_hid_host_log_devices();
        }
#else
        ESP_LOGI(TAG, "alive %" PRIu32 " s | heap %" PRIu32 " B | pad %s",
                 tick, (uint32_t)esp_get_free_heap_size(), pad);
#endif
    }
}
