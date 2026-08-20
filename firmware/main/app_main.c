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

/* For reading each link's negotiated connection interval in the heartbeat. */
#include "host/ble_gap.h"

#if CONFIG_APP_ENABLE_HID_HOST
#include "ble_hid_host.h"
#endif
#if CONFIG_APP_ENABLE_GAMEPAD
#include "ble_gamepad.h"
#endif
#if CONFIG_APP_ENABLE_HID_HOST && CONFIG_APP_ENABLE_GAMEPAD && !CONFIG_APP_GAMEPAD_SELFTEST
#include "input_mapper.h"
#endif
#if CONFIG_APP_ROLE_FAKE_KEYBOARD
#include "fake_keyboard.h"
#endif
#if !CONFIG_APP_LINK_DISABLED
#include "chip_link.h"
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

/*
 * Reports the connection interval of EVERY active link, because that interval is the hard
 * upper bound on how often each hop can carry a report - and because it is negotiated, not
 * chosen: the pad link is set by Windows, an input link by whichever side asks for less.
 * The values are only logged on change by the GAP listener, so without this line the steady
 * state is invisible and the question "what rate are we actually running at" cannot be
 * answered from a log.
 */
static void log_link_intervals(void)
{
    char line[160];
    size_t n = 0;
    int found = 0;

    for (uint16_t h = 0; h <= 9; h++) {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(h, &d) != 0) {
            continue;
        }
        const unsigned us = (unsigned)d.conn_itvl * 1250u; /* units of 1.25 ms */
        if (n < sizeof(line)) {
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%s[%u] %s %u.%02u ms = %u Hz",
                                  found ? " | " : "", h,
                                  d.role == BLE_GAP_ROLE_MASTER ? "central" : "peripheral",
                                  us / 1000u, (us % 1000u) / 10u, 1000000u / us);
        }
        found++;
    }

    if (found) {
        ESP_LOGI(TAG, "  links: %s", line);
    }
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

#if !CONFIG_APP_LINK_DISABLED
    /*
     * Split bridge. Started before the stack because it is pure UART - it does not touch
     * BLE and must be ready to receive the moment the other chip starts talking, which on
     * a shared-power board is roughly when we boot.
     */
    ESP_ERROR_CHECK(chip_link_start());
#endif

    ESP_ERROR_CHECK(ble_stack_start());

#if CONFIG_APP_ROLE_FAKE_KEYBOARD
    /*
     * Started AFTER the stack, unlike the other roles, because it needs to register a random
     * static address and that only works once the host has synced with the controller
     * (AGENTS.md 4.35). Nothing else runs in this mode - the Kconfig options for the two
     * normal roles depend on this being off.
     */
    ESP_ERROR_CHECK(fake_keyboard_start());
#endif

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
        log_link_intervals();
#else
        ESP_LOGI(TAG, "alive %" PRIu32 " s | heap %" PRIu32 " B | pad %s",
                 tick, (uint32_t)esp_get_free_heap_size(), pad);
        log_link_intervals();
#endif
    }
}
