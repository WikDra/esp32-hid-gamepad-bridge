/*
 * Rola central: NimBLE + esp_hidh. Patrz ble_hid_host.h i AGENTS.md 4.3/4.4.
 *
 * Wlasny skaner zamiast esp_hid_gap.c z przykladu IDF: tamten plik ma 1100 linii,
 * z czego wiekszosc to martwy kod Bluetooth Classic i Bluedroid, ktorych na C3
 * z NimBLE nie da sie uzyc. Tu jest tylko sciezka BLE.
 */

#include "ble_hid_host.h"

#include <inttypes.h>
#include <string.h>

#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Dostarczane przez komponent bt (store/config), ale bez publicznego naglowka -
 * tak samo robia przyklady w IDF. */
extern void ble_store_config_init(void);

static const char *TAG = "hid_host";

#define HID_HOST_MAX_DEVICES 2       /* klawiatura + mysz */
#define CANDIDATES_MAX       24      /* w bloku mieszkalnym slychac kilkanascie urzadzen BLE */
#define SCAN_DURATION_MS     6000
#define RETRY_COOLDOWN_US    (15 * 1000 * 1000) /* nie dobijamy sie do tego samego adresu co runde */
#define HID_SERVICE_UUID16   0x1812

#define EV_SYNCED    BIT0
#define EV_DISC_DONE BIT1

/* Jeden spinlock na caly stan modulu. Sekcje krytyczne sa krotkie (kopiowanie
 * kilku bajtow, petle po 2-12 elementach), wiec nie ma po co mieszac muteksow. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static EventGroupHandle_t s_events;
static uint8_t s_own_addr_type;

static hid_input_state_t s_state;

typedef struct {
    bool in_use;
    ble_addr_t addr;
    esp_hid_usage_t usage;
    esp_hidh_dev_t *dev;
} open_dev_t;
static open_dev_t s_devs[HID_HOST_MAX_DEVICES];

typedef struct {
    bool in_use;
    ble_addr_t addr;
    char name[32];
    uint16_t appearance;
    int8_t rssi;
    bool looks_like_hid;
    int64_t cooldown_until_us; /* 0 = mozna probowac */
} candidate_t;
static candidate_t s_cands[CANDIDATES_MAX];

/* ------------------------------------------------------------------ pomocnicze */

#define ADDR_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define ADDR_ARG(a) (a)[5], (a)[4], (a)[3], (a)[2], (a)[1], (a)[0]

static bool addr_eq(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, 6) == 0;
}

/* Bezpieczne, bo nazwy z ADV nie sa zerowane - kopiujemy z jawna dlugoscia. */
static bool name_contains(const char *haystack, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) {
        return false;
    }
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               (p[i] | 0x20) == (needle[i] | 0x20)) {
            i++;
        }
        if (i == nl) {
            return true;
        }
    }
    return false;
}

/* Nasze urzadzenia: AULA F99 Pro, AJAZZ AJ159 Pro. Nazwa to tylko fallback -
 * normalnie wystarcza UUID 0x1812 albo appearance. */
static bool name_is_known_device(const char *name)
{
    static const char *patterns[] = {"aula", "f99", "ajazz", "aj159"};
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (name_contains(name, patterns[i])) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------- tabela urzadzen */

static bool device_is_open(const ble_addr_t *addr)
{
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (s_devs[i].in_use && addr_eq(&s_devs[i].addr, addr)) {
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

static void device_register(const ble_addr_t *addr, esp_hid_usage_t usage, esp_hidh_dev_t *dev)
{
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (!s_devs[i].in_use) {
            s_devs[i].in_use = true;
            s_devs[i].addr = *addr;
            s_devs[i].usage = usage;
            s_devs[i].dev = dev;
            break;
        }
    }
    /* Flagi "co mamy podlaczone" trzymamy w stanie wejsc, zeby Etap 3 mial je
     * w jednym snapshocie razem z danymi. */
    s_state.keyboard_connected = false;
    s_state.mouse_connected = false;
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (!s_devs[i].in_use) {
            continue;
        }
        if (s_devs[i].usage == ESP_HID_USAGE_KEYBOARD) {
            s_state.keyboard_connected = true;
        } else if (s_devs[i].usage == ESP_HID_USAGE_MOUSE) {
            s_state.mouse_connected = true;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void device_unregister(esp_hidh_dev_t *dev)
{
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (s_devs[i].in_use && s_devs[i].dev == dev) {
            s_devs[i].in_use = false;
            s_devs[i].dev = NULL;
        }
    }
    s_state.keyboard_connected = false;
    s_state.mouse_connected = false;
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (!s_devs[i].in_use) {
            continue;
        }
        if (s_devs[i].usage == ESP_HID_USAGE_KEYBOARD) {
            s_state.keyboard_connected = true;
        } else if (s_devs[i].usage == ESP_HID_USAGE_MOUSE) {
            s_state.mouse_connected = true;
        }
    }
    /* Klawiatura odpadla -> zwolnic klawisze, inaczej pad zostanie z wcisnietym W. */
    if (!s_state.keyboard_connected) {
        s_state.modifiers = 0;
        memset(s_state.keys, 0, sizeof(s_state.keys));
    }
    if (!s_state.mouse_connected) {
        s_state.mouse_buttons = 0;
    }
    taskEXIT_CRITICAL(&s_mux);
}

int ble_hid_host_device_count(void)
{
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (s_devs[i].in_use) {
            n++;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

void ble_hid_host_log_devices(void)
{
    open_dev_t snapshot[HID_HOST_MAX_DEVICES];
    taskENTER_CRITICAL(&s_mux);
    memcpy(snapshot, s_devs, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (snapshot[i].in_use) {
            ESP_LOGI(TAG, "  [%d] " ADDR_FMT " usage=%s", i, ADDR_ARG(snapshot[i].addr.val),
                     esp_hid_usage_str(snapshot[i].usage));
        }
    }
}

/* ----------------------------------------------------------------- stan wejsc */

void ble_hid_host_take_state(hid_input_state_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_state;
    s_state.mouse_dx = 0;
    s_state.mouse_dy = 0;
    s_state.mouse_wheel = 0;
    taskEXIT_CRITICAL(&s_mux);
}

/* --------------------------------------------------------------- raporty HID */

static void log_hex(const char *prefix, const uint8_t *data, uint16_t len)
{
    char buf[3 * 16 + 1];
    uint16_t n = len > 16 ? 16 : len;
    for (uint16_t i = 0; i < n; i++) {
        snprintf(&buf[i * 3], 4, "%02x ", data[i]);
    }
    buf[n ? n * 3 - 1 : 0] = '\0';
    ESP_LOGI(TAG, "%s len=%u [%s]%s", prefix, len, buf, len > n ? " ..." : "");
}

static void handle_keyboard_report(const uint8_t *d, uint16_t len)
{
    /* Boot protocol: d[0] modyfikatory, d[1] rezerwa, d[2..7] keycody. */
    taskENTER_CRITICAL(&s_mux);
    s_state.modifiers = d[0];
    memcpy(s_state.keys, &d[2], HID_KEYS_MAX);
    taskEXIT_CRITICAL(&s_mux);

    /* Raportow klawiatury jest malo (tylko na zmianie stanu), wiec logujemy kazdy. */
    log_hex("KBD", d, len);
}

static void handle_mouse_report(const uint8_t *d, uint16_t len)
{
    int32_t dx, dy, wheel = 0;

    /*
     * UWAGA: uklad raportu myszy nie jest jeszcze potwierdzony na sprzecie.
     * Klasyczny boot protocol ma 8-bitowe X/Y, ale myszy gamingowe (a AJ159 Pro
     * to gamingowa, wysokie DPI) czesto raportuja 12/16-bitowo. Rozpoznajemy po
     * dlugosci raportu, a rowolegle logujemy surowe bajty, zeby dalo sie to
     * zweryfikowac z logu i poprawic w Etapie 3.
     */
    if (len >= 5) {
        /* buttons, X lo, X hi, Y lo, Y hi, [wheel] - little endian, ze znakiem */
        dx = (int16_t)((uint16_t)d[1] | ((uint16_t)d[2] << 8));
        dy = (int16_t)((uint16_t)d[3] | ((uint16_t)d[4] << 8));
        if (len >= 6) {
            wheel = (int8_t)d[5];
        }
    } else {
        /* buttons, dx, dy, [wheel] */
        dx = (int8_t)d[1];
        dy = (int8_t)d[2];
        if (len >= 4) {
            wheel = (int8_t)d[3];
        }
    }

    taskENTER_CRITICAL(&s_mux);
    s_state.mouse_buttons = d[0];
    s_state.mouse_dx += dx;
    s_state.mouse_dy += dy;
    s_state.mouse_wheel += wheel;
    taskEXIT_CRITICAL(&s_mux);

    /* Mysz sypie setkami raportow na sekunde - logujemy pierwsze kilka (zeby
     * zobaczyc uklad bajtow) i potem co ~1 s. */
    static uint32_t seen;
    static int64_t last_log_us;
    int64_t now = esp_timer_get_time();
    if (seen < 8 || (now - last_log_us) > 1000000) {
        last_log_us = now;
        log_hex("MOU", d, len);
        ESP_LOGI(TAG, "MOU dekodowanie: buttons=0x%02x dx=%" PRId32 " dy=%" PRId32 " wheel=%" PRId32,
                 d[0], dx, dy, wheel);
    }
    seen++;
}

static void hidh_callback(void *args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(p->open.dev);
        const char *name = esp_hidh_dev_name_get(p->open.dev);
        /* Uwaga: esp_hidh_dev_appearance_get() istnieje tylko w wariancie Bluedroid -
         * przy NimBLE linker go nie znajdzie. Appearance mamy z pakietu ADV. */
        ESP_LOGI(TAG, "OPEN " ADDR_FMT " '%s' usage=%s vid=0x%04x pid=0x%04x",
                 ADDR_ARG(bda), name ? name : "?",
                 esp_hid_usage_str(esp_hidh_dev_usage_get(p->open.dev)),
                 esp_hidh_dev_vendor_id_get(p->open.dev),
                 esp_hidh_dev_product_id_get(p->open.dev));
        break;
    }
    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "bateria %u%%", p->battery.level);
        break;

    case ESP_HIDH_INPUT_EVENT: {
        const uint8_t *d = p->input.data;
        uint16_t len = p->input.length;
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD && len >= 8) {
            handle_keyboard_report(d, len);
        } else if (p->input.usage == ESP_HID_USAGE_MOUSE && len >= 3) {
            handle_mouse_report(d, len);
        } else {
            /* Np. consumer control (klawisze multimedialne) albo raport, ktorego
             * jeszcze nie rozumiemy - warto zobaczyc, co przychodzi. */
            static int64_t last_other_us;
            int64_t now = esp_timer_get_time();
            if ((now - last_other_us) > 1000000) {
                last_other_us = now;
                ESP_LOGW(TAG, "raport nieobslugiwany: usage=%s report_id=%u map=%u",
                         esp_hid_usage_str(p->input.usage), p->input.report_id, p->input.map_index);
                log_hex("  RAW", d, len);
            }
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        esp_hidh_dev_t *dev = p->close.dev;
        const uint8_t *bda = esp_hidh_dev_bda_get(dev);
        ESP_LOGW(TAG, "CLOSE " ADDR_FMT " reason=%d", ADDR_ARG(bda), p->close.reason);
        device_unregister(dev);
        /* Wymagane przez API: dopiero to zwalnia pamiec urzadzenia. */
        esp_hidh_dev_free(dev);
        break;
    }

    default:
        ESP_LOGD(TAG, "zdarzenie hidh %" PRId32, id);
        break;
    }
}

/* ------------------------------------------------------------------- skaner */

static void candidate_seen(const struct ble_gap_disc_desc *disc, const struct ble_hs_adv_fields *f,
                           bool looks_like_hid, const char *name)
{
    taskENTER_CRITICAL(&s_mux);
    candidate_t *slot = NULL;
    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (s_cands[i].in_use && addr_eq(&s_cands[i].addr, &disc->addr)) {
            slot = &s_cands[i];
            break;
        }
    }
    if (slot == NULL) {
        for (int i = 0; i < CANDIDATES_MAX; i++) {
            if (!s_cands[i].in_use) {
                slot = &s_cands[i];
                slot->in_use = true;
                slot->addr = disc->addr;
                slot->name[0] = '\0';
                slot->appearance = 0;
                slot->looks_like_hid = false;
                slot->cooldown_until_us = 0;
                break;
            }
        }
    }
    if (slot == NULL && looks_like_hid) {
        /* Lista pelna, a to jest urzadzenie HID - wypieramy pierwszy wpis, ktory
         * HID-em nie jest. Bez tego gesty eter moglby wypchnac klawiature. */
        for (int i = 0; i < CANDIDATES_MAX; i++) {
            if (!s_cands[i].looks_like_hid && s_cands[i].cooldown_until_us == 0) {
                slot = &s_cands[i];
                slot->addr = disc->addr;
                slot->name[0] = '\0';
                slot->appearance = 0;
                slot->looks_like_hid = false;
                break;
            }
        }
    }
    if (slot != NULL) {
        /* Pakiety ADV i SCAN_RSP przychodza osobno i kazdy nosi czesc informacji -
         * dlatego scalamy, a nie nadpisujemy (AGENTS.md 4.4: filter_duplicates=0). */
        slot->rssi = disc->rssi;
        slot->looks_like_hid = slot->looks_like_hid || looks_like_hid;
        if (f->appearance_is_present) {
            slot->appearance = f->appearance;
        }
        if (name[0] != '\0' && slot->name[0] == '\0') {
            strlcpy(slot->name, name, sizeof(slot->name));
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void handle_adv_report(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, disc->data, disc->length_data) != 0) {
        return;
    }

    char name[32] = {0};
    if (f.name != NULL && f.name_len > 0) {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, n);
        name[n] = '\0';
    }

    bool has_hid_uuid = false;
    for (int i = 0; i < f.num_uuids16; i++) {
        if (f.uuids16[i].value == HID_SERVICE_UUID16) {
            has_hid_uuid = true;
            break;
        }
    }

    /* Kategoria "Human Interface Device" to appearance 0x03Cx. */
    bool hid_appearance = f.appearance_is_present && ((f.appearance & 0xFFC0) == ESP_HID_APPEARANCE_GENERIC);

    bool looks_like_hid = has_hid_uuid || hid_appearance || name_is_known_device(name);
    candidate_seen(disc, &f, looks_like_hid, name);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_adv_report(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xEventGroupSetBits(s_events, EV_DISC_DONE);
        break;
    default:
        break;
    }
    return 0;
}

static void candidates_clear_stale(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CANDIDATES_MAX; i++) {
        /* Wpisy bez aktywnego cooldownu wyrzucamy, zeby lista nie zaklinowala sie
         * na urzadzeniach, ktore juz nie rozglaszaja. */
        if (s_cands[i].in_use && s_cands[i].cooldown_until_us < now) {
            s_cands[i].in_use = false;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void log_scan_results(void)
{
    candidate_t list[CANDIDATES_MAX];
    taskENTER_CRITICAL(&s_mux);
    memcpy(list, s_cands, sizeof(list));
    taskEXIT_CRITICAL(&s_mux);

    int total = 0, hid = 0;
    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (list[i].in_use) {
            total++;
            if (list[i].looks_like_hid) {
                hid++;
            }
        }
    }
    ESP_LOGI(TAG, "skan: %d urzadzen, z tego %d wyglada na HID", total, hid);
    /* Logujemy wszystko, nie tylko HID - jak klawiatura nie chce sie polaczyc,
     * pierwsze pytanie jest "czy w ogole ja slyszymy". */
    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (!list[i].in_use) {
            continue;
        }
        ESP_LOGI(TAG, "  %s " ADDR_FMT " type=%u rssi=%4d appearance=0x%04x '%s'",
                 list[i].looks_like_hid ? "HID" : "   ", ADDR_ARG(list[i].addr.val),
                 list[i].addr.type, list[i].rssi, list[i].appearance,
                 list[i].name[0] ? list[i].name : "?");
    }
}

static void try_connect_candidates(void)
{
    candidate_t list[CANDIDATES_MAX];
    taskENTER_CRITICAL(&s_mux);
    memcpy(list, s_cands, sizeof(list));
    taskEXIT_CRITICAL(&s_mux);

    int64_t now = esp_timer_get_time();

    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (ble_hid_host_device_count() >= HID_HOST_MAX_DEVICES) {
            return;
        }
        candidate_t *c = &list[i];
        if (!c->in_use || !c->looks_like_hid) {
            continue;
        }
        if (c->cooldown_until_us > now || device_is_open(&c->addr)) {
            continue;
        }

        ESP_LOGI(TAG, "kandydat " ADDR_FMT " type=%u '%s' appearance=0x%04x rssi=%d",
                 ADDR_ARG(c->addr.val), c->addr.type, c->name[0] ? c->name : "?",
                 c->appearance, c->rssi);

        /* Nie da sie jednoczesnie skanowac i inicjowac polaczenia. */
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(200));

        uint8_t bda[6];
        memcpy(bda, c->addr.val, sizeof(bda));
        /* Blokujace: w srodku jest ble_gap_connect + odczyt uslug (i ewentualne
         * auto-parowanie). Dlatego to zadanie, a nie callback stacku. */
        esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, c->addr.type);
        if (dev == NULL) {
            ESP_LOGW(TAG, "  nie udalo sie otworzyc, cooldown %d s", RETRY_COOLDOWN_US / 1000000);
            taskENTER_CRITICAL(&s_mux);
            for (int j = 0; j < CANDIDATES_MAX; j++) {
                if (s_cands[j].in_use && addr_eq(&s_cands[j].addr, &c->addr)) {
                    s_cands[j].cooldown_until_us = esp_timer_get_time() + RETRY_COOLDOWN_US;
                }
            }
            taskEXIT_CRITICAL(&s_mux);
            continue;
        }

        esp_hid_usage_t usage = esp_hidh_dev_usage_get(dev);
        device_register(&c->addr, usage, dev);
        ESP_LOGI(TAG, "  podlaczone: usage=%s, razem %d/%d urzadzen",
                 esp_hid_usage_str(usage), ble_hid_host_device_count(), HID_HOST_MAX_DEVICES);
    }
}

static void scan_round(void)
{
    struct ble_gap_disc_params dp = {
        .itvl = 96,   /* 60 ms w jednostkach 0,625 ms */
        .window = 48, /* 30 ms - polowa okna, agresywnie, ale skanujemy tylko do czasu polaczenia */
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        /* KLUCZOWE: AULA F99 Pro nie daje pelnej nazwy w pierwszym pakiecie ADV,
         * a filtr duplikatow ucinal kolejne (AGENTS.md 4.4). */
        .filter_duplicates = 0,
    };

    candidates_clear_stale();
    xEventGroupClearBits(s_events, EV_DISC_DONE);

    int rc = ble_gap_disc(s_own_addr_type, SCAN_DURATION_MS, &dp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc nie wystartowal: rc=%d", rc);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    xEventGroupWaitBits(s_events, EV_DISC_DONE, pdTRUE, pdTRUE,
                        pdMS_TO_TICKS(SCAN_DURATION_MS + 2000));
    log_scan_results();
    try_connect_candidates();
}

static void scan_task(void *arg)
{
    xEventGroupWaitBits(s_events, EV_SYNCED, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "stack gotowy (own_addr_type=%u), zaczynam skanowanie", s_own_addr_type);

    while (true) {
        if (ble_hid_host_device_count() >= HID_HOST_MAX_DEVICES) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        scan_round();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------------------------------------ start / callbacki */

static void on_stack_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr: rc=%d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto: rc=%d, zostaje public", rc);
        s_own_addr_type = 0;
    }
    xEventGroupSetBits(s_events, EV_SYNCED);
}

static void on_stack_reset(int reason)
{
    ESP_LOGE(TAG, "reset stacku NimBLE, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run(); /* wraca dopiero przy nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_hid_host_start(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    esp_hidh_config_t cfg = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    err = esp_hidh_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * KOLEJNOSC MA ZNACZENIE: esp_hidh_init() ustawia wlasne ble_hs_cfg.sync_cb
     * i reset_cb, ktore w wersji NimBLE sa puste (AGENTS.md 4.2). Nadpisujemy je
     * PO nim, zeby dostac informacje o gotowosci stacku.
     */
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Just Works: ani klawiatura, ani mysz nie maja jak wyswietlic ani wpisac kodu. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();

    if (xTaskCreate(scan_task, "hid_scan", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "nie moge utworzyc zadania skanujacego");
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "start: szukam do %d urzadzen HID", HID_HOST_MAX_DEVICES);
    return ESP_OK;
}
