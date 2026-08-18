#include "ble_stack.h"

#include "esp_bt.h"
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

/* Provided by the bt component (store/config) without a public header - the ESP-IDF
 * examples declare it the same way. */
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
        ESP_LOGE(TAG, "ble_hs_id_infer_auto: rc=%d, falling back to public address", rc);
        s_own_addr_type = 0;
    }
    /*
     * NOTE (AGENTS.md 4.35): a random static address was registered here at one point, so
     * that connections could be INITIATED from it - Windows dials the troublesome keyboard
     * from a private address and succeeds. It changed nothing for that keyboard and it broke
     * reconnection of bonded devices, because a bonded peer advertises DIRECTED at the
     * address it bonded with; an initiator using a different own address cannot answer.
     * Do not bring it back without solving that first.
     */
    ESP_LOGI(TAG, "stack ready, own_addr_type=%u", s_own_addr_type);
    xEventGroupSetBits(s_events, EV_SYNCED);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE stack reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run(); /* returns only on nimble_port_stop() */
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

    /*
     * Transmit power to maximum, for advertising, scanning and connections alike.
     *
     * The default is only +3 dBm (esp_bt.h: "If none of power type is set, system will
     * use ESP_PWR_LVL_P3 as default for ADV/SCAN/CONN0-9"), and we were leaving it there.
     * That is a poor default for this project, where the bridge has to reach three peers
     * at once and, unlike them, is not the device the user holds in their hand.
     *
     * What made this visible: a keyboard advertising at rssi -76 could be heard only once
     * or twice per six-second scan round - a Microsoft beacon at a comparable rssi gave
     * 21-28 packets in the same round - and every attempt to connect to it timed out,
     * while a mouse at rssi -62 answered in 310 ms. Losing nearly all of a peer's
     * advertising packets says the link is marginal in the direction we can measure; the
     * connection request we send travels the same path, and nothing answered it.
     *
     * Raising the level costs current, which is irrelevant here because the board is
     * USB powered. The achieved level is read back and logged rather than assumed - the
     * setter can clamp to what the chip and the calibration data actually allow.
     */
    esp_err_t pwr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
    if (pwr != ESP_OK) {
        ESP_LOGW(TAG, "cannot set TX power: %s", esp_err_to_name(pwr));
    }
    ESP_LOGI(TAG, "TX power level: %d (0=-24dBm .. 15=+20dBm)",
             (int)esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT));

    /* Just Works both ways: neither the keyboard, the mouse nor the pad has any way
     * to display or enter a passkey. Bonding is on because the keyboard will not hand
     * over reports without encryption, and Windows will not remember the pad without
     * a bond. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* GAP service (name, appearance) and GATT service (Service Changed) - needed on
     * the peripheral side, harmless for the central. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Pairing key store. With CONFIG_BT_NIMBLE_NVS_PERSIST=y the keys go to NVS. */
    ble_store_config_init();

    return ESP_OK;
}

esp_err_t ble_stack_start(void)
{
    /*
     * ORDER MATTERS. esp_hidh_init() (called earlier by the central role) installs its
     * own ble_hs_cfg.sync_cb and reset_cb, and in the NimBLE variant those are empty -
     * log only. If we left them in place we would never learn that the stack is ready,
     * and neither scanning nor advertising would ever start. So our callbacks are
     * installed here, last, right before the host task comes up.
     */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
