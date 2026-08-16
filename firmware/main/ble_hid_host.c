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

#include "ble_stack.h"
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

static const char *TAG = "hid_host";

#define HID_HOST_MAX_DEVICES 2       /* klawiatura + mysz */
#define CANDIDATES_MAX       24      /* w bloku mieszkalnym slychac kilkanascie urzadzen BLE */
#define SCAN_DURATION_MS     6000
#define RETRY_COOLDOWN_US    (15 * 1000 * 1000) /* nie dobijamy sie do tego samego adresu co runde */
#define HID_SERVICE_UUID16   0x1812

#define EV_DISC_DONE BIT0

/* Jeden spinlock na caly stan modulu. Sekcje krytyczne sa krotkie (kopiowanie
 * kilku bajtow, petle po 2-12 elementach), wiec nie ma po co mieszac muteksow. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static EventGroupHandle_t s_events;

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

/* Kopiuje JEDEN wpis z tablicy kandydatow. Swiadomie nie ma tu wersji kopiujacej
 * cala tablice: sizeof(candidate_t) * CANDIDATES_MAX to ~1,5 kB, a te petle wolaja
 * potem esp_hidh_dev_open(), ktore samo zjada kilka kilobajtow stosu. Pierwsza
 * wersja kopiowala cala tablice i konczylo sie to "Stack protection fault"
 * w zadaniu hid_scan przy pierwszym polaczeniu z klawiatura. */
static bool candidate_get(int idx, candidate_t *out)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_cands[idx].in_use) {
        *out = s_cands[idx];
        ok = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

static void candidate_set_cooldown(const ble_addr_t *addr)
{
    int64_t until = esp_timer_get_time() + RETRY_COOLDOWN_US;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (s_cands[i].in_use && addr_eq(&s_cands[i].addr, addr)) {
            s_cands[i].cooldown_until_us = until;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void log_scan_results(void)
{
    candidate_t c;
    int total = 0, hid = 0;

    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (candidate_get(i, &c)) {
            total++;
            if (c.looks_like_hid) {
                hid++;
            }
        }
    }
    ESP_LOGI(TAG, "skan: %d urzadzen, z tego %d wyglada na HID", total, hid);
    /* Logujemy wszystko, nie tylko HID - jak klawiatura nie chce sie polaczyc,
     * pierwsze pytanie jest "czy w ogole ja slyszymy". */
    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (!candidate_get(i, &c)) {
            continue;
        }
        ESP_LOGI(TAG, "  %s " ADDR_FMT " type=%u rssi=%4d appearance=0x%04x '%s'",
                 c.looks_like_hid ? "HID" : "   ", ADDR_ARG(c.addr.val),
                 c.addr.type, c.rssi, c.appearance, c.name[0] ? c.name : "?");
    }
}

static void try_connect_candidates(void)
{
    candidate_t c;

    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (ble_hid_host_device_count() >= HID_HOST_MAX_DEVICES) {
            return;
        }
        if (!candidate_get(i, &c) || !c.looks_like_hid) {
            continue;
        }
        if (c.cooldown_until_us > esp_timer_get_time() || device_is_open(&c.addr)) {
            continue;
        }

        ESP_LOGI(TAG, "kandydat " ADDR_FMT " type=%u '%s' appearance=0x%04x rssi=%d",
                 ADDR_ARG(c.addr.val), c.addr.type, c.name[0] ? c.name : "?",
                 c.appearance, c.rssi);

        /* Nie da sie jednoczesnie skanowac i inicjowac polaczenia. */
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(200));

        uint8_t bda[6];
        memcpy(bda, c.addr.val, sizeof(bda));
        /*
         * Blokujace i bardzo grube na stosie: w srodku jest ble_gap_connect, pelne
         * odkrywanie uslug GATT i parsowanie Report Map. read_device_services()
         * w nimble_hidh.c trzyma na stosie WOLAJACEGO trzy tablice naraz
         * (service_result[10], char_result[20], descr_result[20]) - patrz rozmiar
         * stosu przy xTaskCreate na koncu pliku.
         */
        esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, c.addr.type);
        ESP_LOGI(TAG, "  po esp_hidh_dev_open zostalo %u B stosu",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        if (dev == NULL) {
            ESP_LOGW(TAG, "  nie udalo sie otworzyc, cooldown %d s", RETRY_COOLDOWN_US / 1000000);
            candidate_set_cooldown(&c.addr);
            continue;
        }

        esp_hid_usage_t usage = esp_hidh_dev_usage_get(dev);
        device_register(&c.addr, usage, dev);
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

    int rc = ble_gap_disc(ble_stack_own_addr_type(), SCAN_DURATION_MS, &dp, gap_event_cb, NULL);
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
    ble_stack_wait_synced(portMAX_DELAY);
    ESP_LOGI(TAG, "zaczynam skanowanie (do %d urzadzen HID)", HID_HOST_MAX_DEVICES);

    while (true) {
        if (ble_hid_host_device_count() >= HID_HOST_MAX_DEVICES) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        scan_round();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------------------------------------------------ start */

esp_err_t ble_hid_host_start(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_hidh_config_t cfg = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    /* Uwaga: to nadpisuje ble_hs_cfg.sync_cb i reset_cb pustymi wersjami z
     * nimble_hidh.c. Naprawia to ble_stack_start(), wolane po wszystkich rolach. */
    esp_err_t err = esp_hidh_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 8 kB, nie 4 kB. Powod: esp_hidh_dev_open() wykonuje sie w TYM zadaniu i przez
     * read_device_services() klada na naszym stosie service_result[10] +
     * char_result[20] + descr_result[20] naraz, plus parser Report Map. Przy 4 kB
     * konczylo sie to "Stack protection fault" dokladnie w momencie polaczenia
     * z klawiatura (potwierdzone na sprzecie). Przyklad esp_hid_host z IDF daje
     * swojemu zadaniu 6 kB; bierzemy 8 kB i logujemy high water mark.
     */
    if (xTaskCreate(scan_task, "hid_scan", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "nie moge utworzyc zadania skanujacego");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
