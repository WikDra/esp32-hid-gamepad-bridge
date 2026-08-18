/*
 * BLE HID bridge: keyboard + mouse -> gamepad, on an ESP32-C3 SuperMini.
 *
 * Both roles run at once: two central links (keyboard, mouse) and one peripheral link
 * (the pad presented to the PC). Roles are Kconfig switches so each side can be tested
 * on its own. See AGENTS.md for the engineering notes behind every design decision.
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_cpu.h"
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
#if CONFIG_APP_ENABLE_HID_HOST && CONFIG_APP_ENABLE_GAMEPAD && !CONFIG_APP_GAMEPAD_SELFTEST
#include "input_mapper.h"
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
    ESP_LOGI(TAG, "IDF %s, target %s rev v%d.%d, %d core(s)",
             esp_get_idf_version(), CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "flash %" PRIu32 " kB, PSRAM %s",
             flash_size / 1024,
             (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "present" : "none");
    ESP_LOGI(TAG, "heap before BLE: free %" PRIu32 " B, largest block %" PRIu32 " B",
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    /* Roles are menuconfig switches so that each side can be tested separately. */
    ESP_LOGI(TAG, "roles: hid_host=%s gamepad=%s selftest=%s",
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
        ESP_LOGW(TAG, "NVS needs erasing (%s), wiping the partition", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    log_boot_banner();

    /*
     * The order is forced by ble_hs_cfg being global and esp_hidh_init() overwriting it.
     * ble_stack_start() restores our callbacks at the very end (AGENTS.md 4.2).
     */
    ESP_ERROR_CHECK(ble_stack_init());

#if CONFIG_APP_ENABLE_HID_HOST
    ESP_ERROR_CHECK(ble_hid_host_start());
#endif
#if CONFIG_APP_ENABLE_GAMEPAD
    ESP_ERROR_CHECK(ble_gamepad_start());
#endif

#if CONFIG_APP_ENABLE_HID_HOST && CONFIG_APP_ENABLE_GAMEPAD && !CONFIG_APP_GAMEPAD_SELFTEST
    /* The glue between the roles. With the selftest enabled the pad drives a test
     * pattern, so the mapper would fight it over the same report. */
    ESP_ERROR_CHECK(input_mapper_start());
#elif CONFIG_APP_GAMEPAD_SELFTEST
    ESP_LOGW(TAG, "pad selftest enabled - input mapping is INACTIVE");
#endif

    ESP_ERROR_CHECK(ble_stack_start());

#if CONFIG_APP_DEBUG_WATCH_ADDR
    /*
     * Diagnostic tool for the crash in AGENTS.md 4.21. That crash shows up as a read
     * from a garbage address inside ble_gap_update_next_exp(), i.e. we see the VICTIM
     * but not the culprit. A write watchpoint inverts that: the panic fires at the
     * moment the structure is corrupted, with the culprit's backtrace.
     *
     * ARM IT AFTER THE STACK HAS SYNCED. Earlier it caught the legitimate write from
     * ble_gap_init() (ble_gap.c:9113, SLIST_INIT), which produced a reboot loop right
     * at startup.
     */
    ble_stack_wait_synced(5000);
    {
        void *addr = (void *)(uintptr_t)CONFIG_APP_DEBUG_WATCH_ADDR;
        esp_err_t werr = esp_cpu_set_watchpoint(0, addr, 4, ESP_CPU_WATCHPOINT_STORE);
        ESP_LOGW(TAG, "write watchpoint at %p: %s", addr, esp_err_to_name(werr));
    }
#endif

    /* Heartbeat: tells "the firmware is idle and waiting" apart from "the firmware
     * died". It also reports memory, which matters on a C3 without PSRAM. */
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        tick += 5;

#if CONFIG_APP_ENABLE_GAMEPAD
        const char *pad = ble_gamepad_is_ready() ? "ready" : "no PC";
#else
        const char *pad = "off";
#endif

#if CONFIG_APP_ENABLE_HID_HOST
        hid_input_state_t st;
        ble_hid_host_take_state(&st);
        ESP_LOGI(TAG, "alive %" PRIu32 " s | heap %" PRIu32 " B (min %" PRIu32 " B) | inputs %d (kbd=%d mouse=%d) | pad %s",
                 tick, (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size(),
                 ble_hid_host_device_count(), st.keyboard_connected, st.mouse_connected, pad);
        if (st.mouse_dx || st.mouse_dy || st.mouse_wheel) {
            ESP_LOGI(TAG, "  mouse motion since last read: dx=%" PRId32 " dy=%" PRId32 " wheel=%" PRId32,
                     st.mouse_dx, st.mouse_dy, st.mouse_wheel);
        }
        if (st.modifiers || st.keys[0]) {
            ESP_LOGI(TAG, "  keyboard: mod=0x%02x keys=%02x %02x %02x %02x %02x %02x",
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
