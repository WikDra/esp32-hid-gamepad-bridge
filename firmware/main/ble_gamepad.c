#include "ble_gamepad.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "ble_stack.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/hid/ble_svc_hid.h"

static const char *TAG = "gamepad";

#define GAMEPAD_REPORT_ID  1
#define GAMEPAD_REPORT_LEN 6

#define APPEARANCE_GAMEPAD 0x03C4

/*
 * Deskryptor raportu HID pada: 4 osie 8-bitowe ze znakiem + 12 przyciskow.
 *
 * Osie X/Y (lewy analog) i Z/Rz (prawy analog) - taki uklad uzywaja typowe pady
 * DirectInput, wiec joy.cpl pokaze krzyzyk (X/Y) oraz suwaki "os Z" i "obrot Z".
 *
 * UWAGA: kazda zmiana tej tablicy wymaga usuniecia pada z listy urzadzen
 * Bluetooth w Windows i sparowania na nowo - Windows cache'uje Report Map
 * per bond (AGENTS.md 4.7).
 */
static const uint8_t s_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)      */
    0x09, 0x05,       /* Usage (Game Pad)                  */
    0xA1, 0x01,       /* Collection (Application)          */
    0x85, GAMEPAD_REPORT_ID, /*   Report ID (1)            */

    0x05, 0x01,       /*   Usage Page (Generic Desktop)    */
    0x09, 0x01,       /*   Usage (Pointer)                 */
    0xA1, 0x00,       /*   Collection (Physical)           */
    0x09, 0x30,       /*     Usage (X)                     */
    0x09, 0x31,       /*     Usage (Y)                     */
    0x09, 0x32,       /*     Usage (Z)                     */
    0x09, 0x35,       /*     Usage (Rz)                    */
    0x15, 0x81,       /*     Logical Minimum (-127)        */
    0x25, 0x7F,       /*     Logical Maximum (127)         */
    0x75, 0x08,       /*     Report Size (8)               */
    0x95, 0x04,       /*     Report Count (4)              */
    0x81, 0x02,       /*     Input (Data, Var, Abs)        */
    0xC0,             /*   End Collection                  */

    0x05, 0x09,       /*   Usage Page (Button)             */
    0x19, 0x01,       /*   Usage Minimum (Button 1)        */
    0x29, 0x0C,       /*   Usage Maximum (Button 12)       */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x01,       /*   Logical Maximum (1)             */
    0x75, 0x01,       /*   Report Size (1)                 */
    0x95, 0x0C,       /*   Report Count (12)               */
    0x81, 0x02,       /*   Input (Data, Var, Abs)          */

    0x75, 0x01,       /*   Report Size (1)                 */
    0x95, 0x04,       /*   Report Count (4)                */
    0x81, 0x03,       /*   Input (Const, Var, Abs) - dopelnienie do 6 bajtow */
    0xC0              /* End Collection                    */
};

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;
static bool s_subscribed;
static uint8_t s_last_report[GAMEPAD_REPORT_LEN];
static bool s_have_last;

/* ------------------------------------------------------------------- GATT */

static esp_err_t register_hid_service(void)
{
    /* struct ble_svc_hid_params ma w sobie report_map[512] i rpts[] po 256 B,
     * a ble_svc_hid_add() bierze ja PRZEZ WARTOSC. Trzymamy ja na stercie, zeby
     * nie miec dwoch kopii na stosie (AGENTS.md 4.9). */
    struct ble_svc_hid_params *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }

    p->proto_mode_present = 1;
    p->proto_mode = BLE_SVC_HID_PROTO_MODE_REPORT;

    /* Boot reporty sa dla hostow, ktore nie potrafia czytac Report Map. Pada
     * i tak nie da sie opisac boot protokolem, wiec ich nie wystawiamy. */
    p->kbd_inp_present = 0;
    p->kbd_out_present = 0;
    p->mouse_inp_present = 0;

    memcpy(p->report_map, s_report_map, sizeof(s_report_map));
    p->report_map_len = sizeof(s_report_map);

    p->rpts_len = 1;
    p->rpts[0].type = BLE_SVC_HID_RPT_TYPE_INPUT;
    p->rpts[0].id = GAMEPAD_REPORT_ID;
    p->rpts[0].len = GAMEPAD_REPORT_LEN;
    memset(p->rpts[0].data, 0, GAMEPAD_REPORT_LEN);

    /* HID Information: bcdHID=0x0111, kraj=0, flagi=0x02 (NormallyConnectable).
     * Na drucie leci little-endian, czyli 11 01 00 02. */
    p->hid_info = 0x02000111;

    int rc = ble_svc_hid_add(*p);
    free(p);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_hid_add: rc=%d", rc);
        return ESP_FAIL;
    }

    ble_svc_hid_init();
    return ESP_OK;
}

static esp_err_t find_report_handle(void)
{
    const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(BLE_SVC_HID_UUID16);
    const ble_uuid16_t chr_uuid = BLE_UUID16_INIT(BLE_SVC_HID_CHR_UUID16_RPT);

    /* Handle'e sa przypisywane dopiero gdy stack wystartuje GATT, czyli po
     * synchronizacji - dlatego szukamy tutaj, a nie w register_hid_service(). */
    int rc = ble_gatts_find_chr(&svc_uuid.u, &chr_uuid.u, NULL, &s_report_handle);
    if (rc != 0) {
        ESP_LOGE(TAG, "nie znalazlem charakterystyki Report: rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "charakterystyka Report ma handle %u", s_report_handle);
    return ESP_OK;
}

/* ------------------------------------------------------------ advertising */

static int gap_event(struct ble_gap_event *event, void *arg);

static esp_err_t start_advertising(void)
{
    /*
     * Rozdzielenie na ADV i SCAN_RSP: w 31 bajtach ADV nie ma pewnosci, ze zmiesci
     * sie nazwa razem z flagami, UUID i appearance (a dlugosc nazwy jest z Kconfig).
     * Nazwa idzie wiec w odpowiedzi na skan - Windows skanuje aktywnie i tak ja
     * przeczyta.
     */
    struct ble_hs_adv_fields adv = {0};
    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(BLE_SVC_HID_UUID16);

    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids16 = &hid_uuid;
    adv.num_uuids16 = 1;
    adv.uuids16_is_complete = 1;
    adv.appearance = APPEARANCE_GAMEPAD;
    adv.appearance_is_present = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields: rc=%d", rc);
        return ESP_FAIL;
    }

    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields: rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(ble_stack_own_addr_type(), NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start: rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "rozglaszam jako '%s' (appearance 0x%04x)", name, APPEARANCE_GAMEPAD);
    return ESP_OK;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "polaczenie nieudane, status=%d - wracam do rozglaszania",
                     event->connect.status);
            start_advertising();
            return 0;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0) {
            return 0;
        }
        /*
         * Sprawdzenie, ktorego brakuje w nimble_hidd.c (AGENTS.md 4.2). Ten callback
         * jest przypiazany do naszego advertisingu, wiec rola i tak powinna byc SLAVE,
         * ale jesli kiedys zostanie podpiety globalnie, to zabezpieczenie zostaje.
         */
        if (desc.role != BLE_GAP_ROLE_SLAVE) {
            ESP_LOGW(TAG, "ignoruje polaczenie w roli %d (to nie PC)", desc.role);
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        s_subscribed = false;
        s_have_last = false;
        ESP_LOGI(TAG, "PC podlaczony, conn_handle=%u", s_conn_handle);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "PC rozlaczony, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_have_last = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_report_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "PC %s notyfikacje raportu",
                     s_subscribed ? "wlaczyl" : "wylaczyl");
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "szyfrowanie/parowanie: status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ wysylka */

bool ble_gamepad_is_ready(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_subscribed;
}

bool ble_gamepad_send(const gamepad_state_t *state)
{
    uint8_t rpt[GAMEPAD_REPORT_LEN];
    rpt[0] = (uint8_t)state->lx;
    rpt[1] = (uint8_t)state->ly;
    rpt[2] = (uint8_t)state->rx;
    rpt[3] = (uint8_t)state->ry;
    rpt[4] = (uint8_t)(state->buttons & 0xFF);
    rpt[5] = (uint8_t)((state->buttons >> 8) & 0x0F);

    if (!ble_gamepad_is_ready()) {
        return false;
    }
    /* Tylko na zmianie stanu - inaczej przy 100 Hz zasypywalibysmy link
     * identycznymi raportami. */
    if (s_have_last && memcmp(rpt, s_last_report, sizeof(rpt)) == 0) {
        return false;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(rpt, sizeof(rpt));
    if (om == NULL) {
        ESP_LOGW(TAG, "brak mbuf na raport");
        return false;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_report_handle, om);
    if (rc != 0) {
        /* mbuf jest zwalniany przez stack niezaleznie od wyniku */
        ESP_LOGW(TAG, "ble_gatts_notify_custom: rc=%d", rc);
        return false;
    }

    memcpy(s_last_report, rpt, sizeof(rpt));
    s_have_last = true;
    return true;
}

/* ------------------------------------------------------------- test wlasny */

#if CONFIG_APP_GAMEPAD_SELFTEST
/* Osiem krokow po okregu o promieniu 100 - wystarcza, zeby w joy.cpl bylo widac
 * ruch, i nie wymaga arytmetyki zmiennoprzecinkowej. */
static const int8_t s_circle_x[8] = {100, 71, 0, -71, -100, -71, 0, 71};
static const int8_t s_circle_y[8] = {0, 71, 100, 71, 0, -71, -100, -71};

static void selftest_step(uint32_t tick)
{
    gamepad_state_t st = {0};
    uint32_t phase = (tick / 6) % 8;   /* pelny okrag w ~0,5 s przy 100 Hz */
    st.lx = s_circle_x[phase];
    st.ly = s_circle_y[phase];
    st.rx = s_circle_x[(phase + 2) % 8];
    st.ry = s_circle_y[(phase + 2) % 8];
    /* Po kolei jeden przycisk, zmiana raz na sekunde. */
    st.buttons = (uint16_t)(1u << ((tick / CONFIG_APP_REPORT_RATE_HZ) % 12));
    ble_gamepad_send(&st);
}
#endif

static void gamepad_task(void *arg)
{
    ble_stack_wait_synced(portMAX_DELAY);

    if (find_report_handle() != ESP_OK) {
        ESP_LOGE(TAG, "bez handle raportu pad nie ma sensu, koncze zadanie");
        vTaskDelete(NULL);
        return;
    }
    start_advertising();

    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_APP_REPORT_RATE_HZ);
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(period > 0 ? period : 1);
        tick++;
#if CONFIG_APP_GAMEPAD_SELFTEST
        selftest_step(tick);
#endif
    }
}

esp_err_t ble_gamepad_start(void)
{
    int rc = ble_svc_gap_device_name_set(CONFIG_APP_GAMEPAD_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set: rc=%d", rc);
        return ESP_FAIL;
    }

    esp_err_t err = register_hid_service();
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(gamepad_task, "gamepad", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "usluga HID zarejestrowana, Report Map %u B, raport %u B",
             (unsigned)sizeof(s_report_map), GAMEPAD_REPORT_LEN);
    return ESP_OK;
}
