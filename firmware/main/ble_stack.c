#include "ble_stack.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Dostarczane przez komponent bt (store/config), bez publicznego naglowka -
 * tak samo deklaruja to przyklady w IDF. */
extern void ble_store_config_init(void);

static const char *TAG = "ble_stack";

#define EV_SYNCED BIT0

static EventGroupHandle_t s_events;
static uint8_t s_own_addr_type;

uint8_t ble_stack_own_addr_type(void)
{
    return s_own_addr_type;
}

bool ble_stack_wait_synced(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_SYNCED, pdFALSE, pdTRUE,
                                          timeout_ms == portMAX_DELAY ? portMAX_DELAY
                                                                      : pdMS_TO_TICKS(timeout_ms));
    return (bits & EV_SYNCED) != 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr: rc=%d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto: rc=%d, zostaje adres public", rc);
        s_own_addr_type = 0;
    }
    ESP_LOGI(TAG, "stack gotowy, own_addr_type=%u", s_own_addr_type);
    xEventGroupSetBits(s_events, EV_SYNCED);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "reset stacku NimBLE, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run(); /* wraca dopiero przy nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_stack_init(void)
{
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    /* Just Works w obie strony: ani klawiatura, ani mysz, ani pad nie maja jak
     * wyswietlic albo przyjac kodu. Bondowanie wlaczone, bo klawiatura nie odda
     * raportow bez szyfrowania, a Windows nie zapamieta pada bez bonda. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Usluga GAP (nazwa, appearance) i GATT (Service Changed) - potrzebne po
     * stronie peryferialu, nieszkodliwe dla centrala. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Magazyn kluczy parowania. Z CONFIG_BT_NIMBLE_NVS_PERSIST=y trafiaja do NVS. */
    ble_store_config_init();

    return ESP_OK;
}

esp_err_t ble_stack_start(void)
{
    /*
     * KOLEJNOSC MA ZNACZENIE. esp_hidh_init() (wolane wczesniej przez rola
     * central) ustawia wlasne ble_hs_cfg.sync_cb i reset_cb, a w wariancie NimBLE
     * sa one puste - tylko log. Gdybysmy na nich zostali, nigdy nie dowiedzielibysmy
     * sie, ze stack jest gotowy, i ani skan, ani advertising by nie wystartowaly.
     * Dlatego nasze callbacki ustawiamy tu, jako ostatnie przed startem hosta.
     */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
