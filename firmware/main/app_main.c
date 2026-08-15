/*
 * Mostek BLE HID: klawiatura + mysz -> pad, na ESP32-C3 SuperMini.
 *
 * Etap 0 (ten plik w obecnej formie): szkielet. Sprawdza, ze projekt sie buduje,
 * wgrywa i startuje, oraz ze konsola po USB Serial/JTAG dziala. Nic wiecej.
 *
 * Kolejne etapy (patrz AGENTS.md, sekcja 5) dodaja tutaj:
 *   Etap 1 - ble_hid_host_start()  (rola central, esp_hidh)
 *   Etap 2 - ble_gamepad_start()   (rola peripheral, wlasny serwer GATT)
 *   Etap 3 - zadanie mapujace wejscia na raport pada
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "bridge";

static void log_boot_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    ESP_LOGI(TAG, "=== esp32-hid-gamepad-bridge, etap 0 (szkielet) ===");
    ESP_LOGI(TAG, "IDF %s, target %s rev v%d.%d, %d rdzen(i)",
             esp_get_idf_version(), CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "flash %" PRIu32 " kB, PSRAM %s",
             flash_size / 1024,
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "jest" : "brak");
    ESP_LOGI(TAG, "heap: free %" PRIu32 " B, najwiekszy blok %" PRIu32 " B, minimum od bootu %" PRIu32 " B",
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (uint32_t)esp_get_minimum_free_heap_size());

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
    ESP_LOGW(TAG, "etap 0: zadna rola BLE nie jest jeszcze zaimplementowana");
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

    /* Heartbeat: pozwala odroznic "firmware stoi" od "firmware sie wysypal i restartuje". */
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive %" PRIu32 " s, free_heap %" PRIu32 " B",
                 (tick += 5), (uint32_t)esp_get_free_heap_size());
    }
}
