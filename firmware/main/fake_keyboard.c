/*
 * fake_keyboard.c - a synthetic BLE keyboard, byte-for-byte impersonating the AULA F99 Pro
 * in pairing mode.
 *
 * WHY THIS EXISTS. The AULA F99 Pro pairs with our bridge on the ESP32-C3 and ESP32-S3 and
 * never pairs on the ESP32-C6 or ESP32-H2, with the same firmware, the same room and a signal
 * 42 dB in favour of the boards that fail (AGENTS.md 4.35). Twelve hypotheses were eliminated
 * by measurement and the difference narrowed to the controller library, which is a prebuilt
 * blob. That left one honest way forward: stop investigating a device we cannot change, and
 * build an advertiser we CAN change, then bisect which of its properties the C6/H2 initiator
 * chokes on.
 *
 * WHAT IT ADVERTISES. The exact 31 bytes captured from the keyboard, so the first run tests
 * the real thing rather than an approximation:
 *
 *   02 01 05                -> Flags: LE Limited Discoverable + BR/EDR not supported
 *   03 03 12 18             -> Complete list of 16-bit UUIDs: 0x1812 (HID)
 *   03 19 c1 03             -> Appearance 0x03C1 (keyboard)
 *   06 ff 06 00 03 00 80    -> Manufacturer Specific, company 0x0006 (Microsoft Swift Pair)
 *   0c 08 'AULA-F99Pro'     -> Shortened local name
 *
 * The bytes are written with ble_gap_adv_set_data() rather than assembled from NimBLE's
 * field struct, because "identical to the captured payload" is the whole point and letting
 * the host encode the fields would risk a different-but-equivalent encoding.
 *
 * WHAT IS ADJUSTABLE, i.e. the bisection knobs (all in menuconfig):
 *   - advertising interval          APP_FAKE_KBD_ADV_ITVL_MS
 *   - own address type              APP_FAKE_KBD_RANDOM_ADDR (static random vs public)
 *   - the AD flags byte             APP_FAKE_KBD_FLAGS
 *
 * The measured original, for reference (CONFIG_APP_DEBUG_SCAN_ONLY on a second board): a
 * STABLE static random address held for at least 13.2 s, packets every 50-900 ms, often
 * several per second. Nothing pathological - which is why the defaults here are simply the
 * measured values.
 */

#include "fake_keyboard.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "nimble/nimble_port.h"

#include "ble_stack.h"

static const char *TAG = "fake_kbd";

/*
 * The captured advertising payload. Only the flags byte is patched at runtime, so that the
 * discoverability bits can be bisected without touching anything else.
 */
static uint8_t s_adv_data[] = {
    0x02, 0x01, 0x05,                                     /* Flags */
    0x03, 0x03, 0x12, 0x18,                               /* UUID16 complete: 0x1812 HID */
    0x03, 0x19, 0xc1, 0x03,                               /* Appearance: keyboard 0x03C1 */
    0x06, 0xff, 0x06, 0x00, 0x03, 0x00, 0x80,             /* Microsoft Swift Pair beacon */
    0x0c, 0x08, 'A', 'U', 'L', 'A', '-', 'F', '9', '9', 'P', 'r', 'o', /* shortened name */
};
#define ADV_FLAGS_OFFSET 2

static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    int rc = ble_gap_adv_set_data(s_adv_data, sizeof(s_adv_data));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data: rc=%d", rc);
        return;
    }

    /*
     * itvl_min == itvl_max: we are measuring, so the interval must be what we asked for and
     * not something the controller picked out of a range.
     */
    uint16_t itvl = (uint16_t)((CONFIG_APP_FAKE_KBD_ADV_ITVL_MS * 1000) / 625);
    struct ble_gap_adv_params advp = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,   /* ADV_IND: connectable, undirected */
        .disc_mode = BLE_GAP_DISC_MODE_LTD,   /* matches the captured flags 0x05 */
        .itvl_min = itvl,
        .itvl_max = itvl,
    };

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start: rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising: ADV_IND every %d ms, flags=0x%02x, %u bytes of payload",
             CONFIG_APP_FAKE_KBD_ADV_ITVL_MS, s_adv_data[ADV_FLAGS_OFFSET],
             (unsigned)sizeof(s_adv_data));
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /*
         * This line is the whole experiment. If it appears, the C6/H2 initiator CAN establish
         * a link with an advertiser that looks exactly like the keyboard - and the trigger
         * lies somewhere else, in what the real keyboard does after CONNECT_IND. If it never
         * appears while a C3 connects to this same synthetic keyboard, we have a reproducer
         * that needs no third-party hardware.
         */
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
                ESP_LOGW(TAG, "CONNECTED, conn_handle=%u itvl=%u (%u.%02u ms) latency=%u"
                              " timeout=%u",
                         s_conn_handle, desc.conn_itvl,
                         (unsigned)(desc.conn_itvl * 125 / 100),
                         (unsigned)(desc.conn_itvl * 125 % 100),
                         desc.conn_latency, desc.supervision_timeout);
            } else {
                ESP_LOGW(TAG, "CONNECTED, conn_handle=%u", s_conn_handle);
            }
        } else {
            ESP_LOGW(TAG, "connection attempt failed, status=%d - advertising again",
                     event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected, reason=%d - advertising again",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change: status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr_handle=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

esp_err_t fake_keyboard_start(void)
{
    s_adv_data[ADV_FLAGS_OFFSET] = (uint8_t)CONFIG_APP_FAKE_KBD_FLAGS;

    /*
     * Everything below needs the host synced with the controller: setting an address or
     * advertising data before that returns BLE_HS_ENOTSYNCED (22), which is exactly what the
     * first version of this did. The gamepad role waits the same way.
     */
    ble_stack_wait_synced(portMAX_DELAY);

    /*
     * Override the transmit power the stack set for us. ble_stack.c puts it at maximum,
     * which is right for a bridge but wrong for a device whose job is to be as loud as a
     * keyboard across a room: on the same PCB as the board under test that gives rssi -7 dBm,
     * a signal no real peripheral would ever present. Lowering it is our only way to simulate
     * distance without moving hardware.
     */
    esp_err_t pwr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT,
                                         (esp_power_level_t)CONFIG_APP_FAKE_KBD_TX_POWER);
    if (pwr != ESP_OK) {
        ESP_LOGW(TAG, "cannot set TX power: %s", esp_err_to_name(pwr));
    }
    ESP_LOGI(TAG, "TX power level: %d (15 = +20 dBm, 0 = -24 dBm)",
             (int)esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT));

#if CONFIG_APP_FAKE_KBD_RANDOM_ADDR
    /*
     * The real keyboard advertises from a static random address, so by default so do we.
     * Addresses can only be set once the host has synced with the controller, which is why
     * this runs from the start function rather than from init (AGENTS.md 4.35: doing it
     * earlier returns BLE_HS_ENOTSYNCED).
     */
    ble_addr_t rnd;
    int rc = ble_hs_id_gen_rnd(0, &rnd);
    if (rc == 0) {
        rc = ble_hs_id_set_rnd(rnd.val);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot set a random static address: rc=%d - falling back to public", rc);
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    } else {
        s_own_addr_type = BLE_OWN_ADDR_RANDOM;
        ESP_LOGI(TAG, "static random address %02x:%02x:%02x:%02x:%02x:%02x",
                 rnd.val[5], rnd.val[4], rnd.val[3], rnd.val[2], rnd.val[1], rnd.val[0]);
    }
#else
    s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    ESP_LOGI(TAG, "advertising from the public address");
#endif

    advertise();
    return ESP_OK;
}
