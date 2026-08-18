/*
 * Rola central: NimBLE + esp_hidh. Patrz ble_hid_host.h i AGENTS.md 4.3/4.4.
 *
 * Wlasny skaner zamiast esp_hid_gap.c z przykladu IDF: tamten plik ma 1100 linii,
 * most of which is dead Bluetooth Classic and Bluedroid code that cannot be used
 * on a C3 with NimBLE at all. Only the BLE path lives here.
 */

#include "ble_hid_host.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_stack.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "esp_private/esp_hidh_private.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"
#include "nimble/hci_common.h"

static const char *TAG = "hid_host";

#define HID_HOST_MAX_DEVICES 2       /* klawiatura + mysz */
#define CANDIDATES_MAX       24      /* w bloku mieszkalnym slychac kilkanascie urzadzen BLE */
#define SCAN_DURATION_MS     6000
#define RETRY_COOLDOWN_US    (15 * 1000 * 1000) /* do not hammer the same address every round */
#define HID_SERVICE_UUID16   0x1812

#define EV_DISC_DONE   BIT0
#define EV_OPEN_DONE   BIT1
#define EV_OPEN_DOOMED BIT2

/* Jeden spinlock na caly stan modulu. Sekcje krytyczne sa krotkie (kopiowanie
 * a few bytes, loops over 2-12 elements), so mutexes would be overkill. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static EventGroupHandle_t s_events;

static hid_input_state_t s_state;

/* State of an in-progress device open. Declared here because the disconnect
 * listener uses it too, and that appears earlier in the file than open_device_guarded(). */
static volatile bool s_opening;
static ble_addr_t s_opening_addr;

bool ble_hid_host_is_opening(void)
{
    return s_opening;
}

typedef struct {
    bool in_use;
    ble_addr_t addr;
    /* Suma bitowa ESP_HID_USAGE_* ze wszystkich raportow wejsciowych urzadzenia.
     * We do NOT use esp_hidh_dev_usage_get(): on the NimBLE path it always returns
     * GENERIC, because nimble_hidh.c never sets dev->usage (confirmed both in the
     * device log and by grepping IDF 5.5.1) - see AGENTS.md 4.15. */
    uint8_t usage_mask;
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
    bool bonded;               /* the address is in the NVS bond list */
    uint8_t event_type;        /* BLE_HCI_ADV_RPT_EVTYPE_* - DIR_IND znaczy "wracam do konkretnego hosta" */
    /* First bytes of the ADV packet. Used to recognise devices that advertise neither
     * a name nor an appearance - without it the log shows only an address and there is
     * powiedziec, co to jest. */
    uint8_t adv[16];
    uint8_t adv_len;
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

/* Safe, because ADV names are not NUL-terminated - we copy with an explicit length. */
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
 * normally UUID 0x1812 or the appearance is enough. */
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

/* Przelicza flagi "co mamy podlaczone" z tablicy urzadzen. Wolac TYLKO z sekcji
 * section. The flags live in the input state so that the mapper receives them in one
 * snapshocie razem z danymi. */
static void refresh_connected_flags_locked(void)
{
    uint8_t mask = 0;
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (s_devs[i].in_use) {
            mask |= s_devs[i].usage_mask;
        }
    }
    s_state.keyboard_connected = (mask & ESP_HID_USAGE_KEYBOARD) != 0;
    s_state.mouse_connected = (mask & ESP_HID_USAGE_MOUSE) != 0;

    /* The device dropped out -> clear its inputs, otherwise the pad would be left
     * with a key or mouse button held down forever. */
    if (!s_state.keyboard_connected) {
        s_state.modifiers = 0;
        memset(s_state.keys, 0, sizeof(s_state.keys));
    }
    if (!s_state.mouse_connected) {
        s_state.mouse_buttons = 0;
    }
}

static void device_register(const ble_addr_t *addr, uint8_t usage_mask, esp_hidh_dev_t *dev)
{
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (!s_devs[i].in_use) {
            s_devs[i].in_use = true;
            s_devs[i].addr = *addr;
            s_devs[i].usage_mask = usage_mask;
            s_devs[i].dev = dev;
            break;
        }
    }
    refresh_connected_flags_locked();
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

/*
 * Removes a device from the table by address and returns its esp_hidh pointer.
 * By address rather than by pointer, because that is how the GAP event identifies it.
 */
static esp_hidh_dev_t *device_take_by_addr(const uint8_t *addr_val)
{
    esp_hidh_dev_t *dev = NULL;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (s_devs[i].in_use && memcmp(s_devs[i].addr.val, addr_val, 6) == 0) {
            dev = s_devs[i].dev;
            s_devs[i].in_use = false;
            s_devs[i].usage_mask = 0;
            s_devs[i].dev = NULL;
            break;
        }
    }
    refresh_connected_flags_locked();
    taskEXIT_CRITICAL(&s_mux);
    return dev;
}

/*
 * Disconnected devices awaiting resource release. We do not free them in the NimBLE
 * host task (where the disconnect event arrives), because free_inner takes the
 * esp_hidh device list mutex - the scanning task does it instead.
 */
static esp_hidh_dev_t *s_dead[HID_HOST_MAX_DEVICES];

/* Device from the given table slot, or NULL. Needed to retry the interval request
 * once the scanner stops. */
static esp_hidh_dev_t *device_at(int idx)
{
    esp_hidh_dev_t *dev = NULL;
    if (idx < 0 || idx >= HID_HOST_MAX_DEVICES) {
        return NULL;
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_devs[idx].in_use) {
        dev = s_devs[idx].dev;
    }
    taskEXIT_CRITICAL(&s_mux);
    return dev;
}

static void device_mark_dead(esp_hidh_dev_t *dev)
{
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (s_dead[i] == NULL) {
            s_dead[i] = dev;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void reap_dead_devices(void)
{
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        esp_hidh_dev_t *dev;
        taskENTER_CRITICAL(&s_mux);
        dev = s_dead[i];
        s_dead[i] = NULL;
        taskEXIT_CRITICAL(&s_mux);
        if (dev == NULL) {
            continue;
        }
        /*
         * NOTE: esp_hidh_dev_free() from the public API is EMPTY in IDF
         * (esp_hidh.c: "return ESP_OK;"). Zasoby zwalnia esp_hidh_dev_free_inner(),
         * which is normally called by the internal ESP_HIDH_CLOSE_EVENT handler - and that
         * event never arrives on the NimBLE path (see AGENTS.md 4.25).
         * Bez tego wywolania kazdy cykl uspienia urzadzenia zostawialby wpis
         * w liscie esp_hidh i jego bufory na stercie.
         */
        esp_hidh_dev_free_inner(dev);
        ESP_LOGI(TAG, "resources of the disconnected device released");
    }
}

void ble_hid_host_log_devices(void)
{
    open_dev_t snapshot[HID_HOST_MAX_DEVICES];
    taskENTER_CRITICAL(&s_mux);
    memcpy(snapshot, s_devs, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (snapshot[i].in_use) {
            ESP_LOGI(TAG, "  [%d] " ADDR_FMT " reports:%s%s%s (mask 0x%02x)",
                     i, ADDR_ARG(snapshot[i].addr.val),
                     (snapshot[i].usage_mask & ESP_HID_USAGE_KEYBOARD) ? " keyboard" : "",
                     (snapshot[i].usage_mask & ESP_HID_USAGE_MOUSE) ? " mouse" : "",
                     (snapshot[i].usage_mask & ~(ESP_HID_USAGE_KEYBOARD | ESP_HID_USAGE_MOUSE)) ? " inne" : "",
                     snapshot[i].usage_mask);
        }
    }
}

/* ---------------------------------------------------------- sparowani peerzy */

/*
 * Addresses we already have a bond with in NVS. Why: a device that has paired once
 * does not have to advertise its name, appearance or service UUID when it comes back
 * pelnego payloadu z UUID 0x1812 ani appearance - wystarcza mu, ze host go zna.
 * Bez tej listy taki powrot przelecialby przez filtr kandydatow.
 */
#define BONDS_MAX 3
static ble_addr_t s_bonds[BONDS_MAX];
static int s_bonds_len;

static void refresh_bonded_peers(void)
{
    int num = 0;
    if (ble_store_util_bonded_peers(s_bonds, &num, BONDS_MAX) != 0) {
        num = 0;
    }
    s_bonds_len = num;
}

static bool is_bonded_peer(const ble_addr_t *addr)
{
    for (int i = 0; i < s_bonds_len; i++) {
        /* Porownujemy same bajty adresu, bez typu: bond trzyma adres tozsamosci,
         * and the scan may report the same address with a different type marker. */
        if (memcmp(s_bonds[i].val, addr->val, 6) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------ raporty urzadzenia */

/*
 * Device classification and diagnostics. The report list is the only reliable source
 * of "what this device is": esp_hidh_dev_usage_get() on the NimBLE path always
 * sciezce NimBLE zwraca zawsze GENERIC (AGENTS.md 4.15).
 *
 * Dumping the report table is also key when diagnosing odd reports -
 * AULA F99 Pro wystawia wiecej niz jeden raport wejsciowy o dlugosci 8 B.
 */
static uint8_t device_usage_mask(esp_hidh_dev_t *dev, bool log_table)
{
    size_t num = 0;
    esp_hid_report_item_t *reports = NULL;
    uint8_t mask = 0;

    if (esp_hidh_dev_reports_get(dev, &num, &reports) != ESP_OK || reports == NULL) {
        ESP_LOGW(TAG, "cannot read the report list");
        return 0;
    }

    if (log_table) {
        ESP_LOGI(TAG, "  reports: %u", (unsigned)num);
    }
    for (size_t i = 0; i < num; i++) {
        if (reports[i].report_type == ESP_HID_REPORT_TYPE_INPUT) {
            mask |= (uint8_t)reports[i].usage;
        }
        if (log_table) {
            ESP_LOGI(TAG, "    map=%u id=%u typ=%s usage=%s len=%u",
                     reports[i].map_index, reports[i].report_id,
                     esp_hid_report_type_str(reports[i].report_type),
                     esp_hid_usage_str(reports[i].usage), reports[i].value_len);
        }
    }
    free(reports); /* API alokuje tablice mallociem i oddaje ja nam */
    return mask;
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

static void handle_keyboard_report(const esp_hidh_event_data_t *p)
{
    const uint8_t *d = p->input.data;
    uint16_t len = p->input.length;

    /* Boot protocol: d[0] modyfikatory, d[1] rezerwa, d[2..7] keycody. */
    uint8_t keys[HID_KEYS_MAX];
    bool rollover = false;
    for (int i = 0; i < HID_KEYS_MAX; i++) {
        uint8_t k = d[2 + i];
        /*
         * 0x01 ErrorRollOver, 0x02 POSTFail, 0x03 ErrorUndefined - these are not keys,
         * tylko kody bledu z tabeli USB HID Keyboard/Keypad. AULA F99 Pro wysyla
         * serie raportow z 0x01 po kazdym nacisnieciu (widoczne w logu z plytki),
         * so without this filter the pad would receive a phantom button press.
         */
        if (k >= 0x01 && k <= 0x03) {
            rollover = true;
            k = 0;
        }
        keys[i] = k;
    }

    /* A report consisting solely of error codes is ignored entirely - overwriting
     * the state with zeros would lose a key that is genuinely held. */
    bool only_errors = rollover && keys[0] == 0 && d[0] == 0;
    if (!only_errors) {
        taskENTER_CRITICAL(&s_mux);
        s_state.modifiers = d[0];
        memcpy(s_state.keys, keys, HID_KEYS_MAX);
        taskEXIT_CRITICAL(&s_mux);
    }

    /* A pure rollover report carries no information, and the keyboard sends three of
     * nacisnieciu klawisza (§4.17) - w logu to trzy czwarte linii. Zliczamy je
     * them after every keypress, so we count them and report in bulk to keep the log readable. */
    if (only_errors) {
        static uint32_t rollover_count;
        static int64_t last_us;
        int64_t now = esp_timer_get_time();
        rollover_count++;
        if ((now - last_us) > 1000000) {
            last_us = now;
            ESP_LOGD(TAG, "ErrorRollOver reports filtered out: %" PRIu32, rollover_count);
        }
        return;
    }

    char prefix[40];
    snprintf(prefix, sizeof(prefix), "KBD map=%u id=%u", p->input.map_index, p->input.report_id);
    log_hex(prefix, d, len);
}

static void handle_mouse_report(const esp_hidh_event_data_t *p)
{
    const uint8_t *d = p->input.data;
    uint16_t len = p->input.length;
    int32_t dx, dy, wheel = 0;

    /*
     * Uklad POTWIERDZONY na sprzecie (AGENTS.md 4.10), AJAZZ AJ159 Pro, raport
     * map=0 id=5 len=7:
     *
     *   d[0]      przyciski (bit0 lewy, bit1 prawy, bit2 srodkowy)
     *   d[1..2]   X, int16 little-endian
     *   d[3..4]   Y, int16 little-endian
     *   d[5]      kolko, int8
     *   d[6]      always 0 in our samples (probably a horizontal wheel)
     *
     * The decisive sample was [00 ff ff 01 00 00 00] during slow leftward motion:
     * odczyt 16-bitowy daje (-1, +1), a 8-bitowy (-1, -1) - ten drugi bierze za Y
     * the high byte of X. On top of that, moving purely vertically left the X bytes zero,
     * a 8-bitowy odczyt Y pokazywalby wtedy 0 mimo realnego ruchu.
     *
     * Krotszy raport (3 B) to klasyczny boot protocol z 8-bitowymi osiami - AJ159
     * declares such a variant in its report table, so the handling stays.
     */
    if (len >= 5) {
        dx = (int16_t)((uint16_t)d[1] | ((uint16_t)d[2] << 8));
        dy = (int16_t)((uint16_t)d[3] | ((uint16_t)d[4] << 8));
        if (len >= 6) {
            wheel = (int8_t)d[5];
        }
    } else {
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

    /* The layout is settled, so the raw bytes are only needed when diagnosing a new
     * device. One line per second is kept so that it is possible to see that reports
     * ze raporty w ogole plyna, i wychwycic raport o nieznanej dlugosci. */
    static int64_t last_log_us;
    static uint16_t last_len;
    int64_t now = esp_timer_get_time();
    if ((now - last_log_us) > 1000000 || len != last_len) {
        last_log_us = now;
        last_len = len;
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "MOU map=%u id=%u", p->input.map_index, p->input.report_id);
        log_hex(prefix, d, len);
        ESP_LOGI(TAG, "  btn=0x%02x dx=%" PRId32 " dy=%" PRId32 " wheel=%" PRId32,
                 d[0], dx, dy, wheel);
    }
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
         * the linker will not find it with NimBLE. The appearance comes from the ADV packet.
         * esp_hidh_dev_usage_get() is useless too (always GENERIC), hence
         * klasyfikacje bierzemy z listy raportow. */
        ESP_LOGI(TAG, "OPEN " ADDR_FMT " '%s' vid=0x%04x pid=0x%04x",
                 ADDR_ARG(bda), name ? name : "?",
                 esp_hidh_dev_vendor_id_get(p->open.dev),
                 esp_hidh_dev_product_id_get(p->open.dev));
        device_usage_mask(p->open.dev, true); /* tylko log tabeli raportow */
        break;
    }
    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "battery %u%%", p->battery.level);
        break;

    case ESP_HIDH_INPUT_EVENT: {
        uint16_t len = p->input.length;
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD && len >= 8) {
            handle_keyboard_report(p);
        } else if (p->input.usage == ESP_HID_USAGE_MOUSE && len >= 3) {
            handle_mouse_report(p);
        } else {
            /* E.g. consumer control (media keys) or a report we do not understand yet -
             * worth seeing what actually arrives. */
            static int64_t last_other_us;
            int64_t now = esp_timer_get_time();
            if ((now - last_other_us) > 1000000) {
                last_other_us = now;
                ESP_LOGW(TAG, "unhandled report: usage=%s map=%u id=%u",
                         esp_hid_usage_str(p->input.usage), p->input.map_index, p->input.report_id);
                log_hex("  RAW", p->input.data, len);
            }
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        /*
         * On the NimBLE path this event NEVER ARRIVES (AGENTS.md 4.25) - the handler
         * stays because it is correct and will work if IDF ever fixes it.
         * In practice gap_disconnect_listener() is what detects a disconnect.
         */
        esp_hidh_dev_t *dev = p->close.dev;
        const uint8_t *bda = esp_hidh_dev_bda_get(dev);
        ESP_LOGW(TAG, "CLOSE " ADDR_FMT " reason=%d", ADDR_ARG(bda), p->close.reason);
        if (device_take_by_addr(bda) != NULL) {
            device_mark_dead(dev);
        }
        break;
    }

    default:
        ESP_LOGD(TAG, "hidh event %" PRId32, id);
        break;
    }
}

/*
 * We detect disconnects ourselves, from the global GAP event.
 *
 * Why, given that esp_hidh has ESP_HIDH_CLOSE_EVENT: because on the NimBLE path that
 * event does not arrive. nimble_hidh.c:687 posts it only if dev->connected is set,
 * and that field is NEVER set to true anywhere in that file (AGENTS.md 4.25).
 *
 * The consequence was exactly the reported symptom: the mouse falls asleep, closes
 * the link, and the device table still holds it. The counter stays at 2/2, so the
 * scanner stops looking and the mouse has no way back - even though the firmware runs fine.
 *
 * The listener is global, so it also sees the PC disconnect. That is fine: we look the
 * address up in OUR input table, and the PC is not in it. That also avoids the bug
 * nimble_hidd falls over on (no check whose connection it is).
 */
static struct ble_gap_event_listener s_gap_listener;

static int gap_disconnect_listener(struct ble_gap_event *event, void *arg)
{
    /*
     * Uchwyt polaczenia trafia w NimBLE do tablic wymiarowanych liczba polaczen
     * (ble_gap.c:323, indexed by conn_handle - see AGENTS.md 4.28).
     * We log it to show the range the controller hands out; if it ever exceeded
     * przekroczyl CONFIG_BT_NIMBLE_MAX_CONNECTIONS, wrocilby crash z 4.21.
     */
    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
        ESP_LOGI(TAG, "connection established, conn_handle=%u (NimBLE array limit: %d)",
                 event->connect.conn_handle, CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
        /*
         * The HOGP profile requires the central to encrypt the link before using the
         * HID service. esp_hidh NEVER DOES THIS (grepping for security_initiate in
         * components/esp_hid: zero hits) - it relies solely on
         * GATTC_AUTO_PAIR zareaguje na odmowe odczytu charakterystyki.
         *
         * For the keyboard that works by accident: it refuses reads without
         * authentication, so AUTO_PAIR kicks in. The mouse allows reads without
         * encryption, so nothing triggers it - the link stays in the clear,
         * and the mouse stays silent, because it will not send HID reports over a clear
         * link. Symptom: "connected but unresponsive", with the pairing LED blinking.
         *
         * So we do what any HOGP central should. With an existing bond that is just
         * encryption with the key from NVS; without one - pairing.
         */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0 &&
            desc.role == BLE_GAP_ROLE_MASTER && !desc.sec_state.encrypted) {
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "cannot initiate encryption: rc=%d", rc);
            }
        }
        return 0;
    }

    /*
     * Raporty HID przez BLE wymagaja zaszyfrowanego linku. Bez tego urzadzenie
     * connects, serves GATT reads and sends no reports - a symptom that looks like
     * "connected but unresponsive". So we log the encryption and pairing result for
     * EVERY link, so that it does not have to be guessed.
     */
    if (event->type == BLE_GAP_EVENT_ENC_CHANGE) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "encryption: conn_handle=%u status=%d | enc=%d auth=%d bond=%d | itvl=%u (%u ms)",
                     event->enc_change.conn_handle, event->enc_change.status,
                     desc.sec_state.encrypted, desc.sec_state.authenticated,
                     desc.sec_state.bonded,
                     desc.conn_itvl, (unsigned)(desc.conn_itvl * 125 / 100));
        } else {
            ESP_LOGW(TAG, "encryption: conn_handle=%u status=%d (no descriptor)",
                     event->enc_change.conn_handle, event->enc_change.status);
        }
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_PARING_COMPLETE) {
        ESP_LOGI(TAG, "pairing complete: conn_handle=%u status=%d",
                 event->pairing_complete.conn_handle, event->pairing_complete.status);
        return 0;
    }

    /*
     * The connection interval is the UPPER LIMIT on report rate: a peripheral can only
     * moze wyslac notyfikacje tylko w zdarzeniu polaczenia. esp_hidh wola
     * ble_gap_connect() with NULL parameters, so NimBLE's defaults apply,
     * czyli 30-50 ms - stad mysz raportowala tylko ~20-25 razy na sekunde.
     * We log the value so that what the controller actually negotiated is visible.
     */
    if (event->type == BLE_GAP_EVENT_CONN_UPDATE) {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
            ESP_LOGI(TAG, "link %u parameters: status=%d itvl=%u (%u.%02u ms) latency=%u timeout=%u",
                     event->conn_update.conn_handle, event->conn_update.status,
                     d.conn_itvl, (unsigned)(d.conn_itvl * 125 / 100),
                     (unsigned)(d.conn_itvl * 125 % 100),
                     d.conn_latency, d.supervision_timeout);
        }
        return 0;
    }

    if (event->type != BLE_GAP_EVENT_DISCONNECT) {
        return 0;
    }

    const struct ble_gap_conn_desc *conn = &event->disconnect.conn;

    /*
     * If the link to the device being opened has dropped, esp_hidh_dev_open() will hang
     * in WAIT_CB() (AGENTS.md 4.23). We signal that immediately so as not to wait
     * pelnego limitu 45 s.
     */
    if (s_opening &&
        (memcmp(s_opening_addr.val, conn->peer_id_addr.val, 6) == 0 ||
         memcmp(s_opening_addr.val, conn->peer_ota_addr.val, 6) == 0)) {
        xEventGroupSetBits(s_events, EV_OPEN_DOOMED);
    }

    esp_hidh_dev_t *dev = device_take_by_addr(conn->peer_id_addr.val);
    if (dev == NULL) {
        /* The identity address and the address used on air may differ. */
        dev = device_take_by_addr(conn->peer_ota_addr.val);
    }
    if (dev != NULL) {
        ESP_LOGW(TAG, "input disconnected " ADDR_FMT " reason=%d, back to scanning",
                 ADDR_ARG(conn->peer_id_addr.val), event->disconnect.reason);
        device_mark_dead(dev);
    }
    return 0;
}

/* -------------------------------------------------------- time-limited device open */

/*
 * esp_hidh_dev_open() can block FOREVER. That is an IDF bug, not ours:
 *
 *   nimble_hidh.c:49   WAIT_CB() = xSemaphoreTake(sem, portMAX_DELAY)
 *   nimble_hidh.c:353  rc = ble_gattc_disc_all_chrs(...); WAIT_CB();
 *
 * i.e. the code waits without a timeout and does NOT check rc. When the link drops
 * odkrywania uslug, kolejne operacje GATT zwracaja BLE_HS_ENOTCONN synchronicznie,
 * no callback will ever arrive and there is nobody left to release WAIT_CB().
 *
 * Observed on hardware: the keyboard link dropped during service discovery
 * (disconnect reason=520, i.e. HCI 0x08 Connection Timeout), the hid_scan task hung
 * and the bridge stopped scanning - so the mouse had no way back after falling asleep.
 * W logu bylo to widoczne po braku linii "skan:" i braku "po esp_hidh_dev_open ...",
 * which is normally logged unconditionally after the call returns.
 *
 * Hence the call runs in a separate task while hid_scan waits with a timeout.
 * Once the limit passes we know the task is stuck inside IDF. It cannot be safely
 * ubic: siedzi na prywatnym semaforze esp_hidh, a kolejne proby otwarcia
 * would compete with it for the same semaphore (waking it, not us). So the only
 * honest way out is a controlled restart - bonds live in NVS, so the PC comes back
 * in ~2 s and the keyboard and mouse reconnect by themselves.
 */
#define OPEN_TIMEOUT_MS 45000 /* dev_open already has a 30 s timeout on the connect itself */

typedef struct {
    ble_addr_t addr;
    uint32_t generation;
} open_ctx_t;

static SemaphoreHandle_t s_open_done;
static esp_hidh_dev_t *volatile s_open_result;
static volatile uint32_t s_open_generation;

static void open_task(void *arg)
{
    open_ctx_t *ctx = (open_ctx_t *)arg;
    uint8_t bda[6];

    memcpy(bda, ctx->addr.val, sizeof(bda));
    esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, ctx->addr.type);

    ESP_LOGI(TAG, "  %u B of stack left after esp_hidh_dev_open",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    if (ctx->generation == s_open_generation) {
        s_open_result = dev;
        xEventGroupSetBits(s_events, EV_OPEN_DONE);
        xSemaphoreGive(s_open_done);
    } else {
        /* Nobody is listening any more - hid_scan gave up on this attempt. */
        ESP_LOGW(TAG, "  spozniony wynik otwarcia (generacja %" PRIu32 "), pomijam", ctx->generation);
        if (dev != NULL) {
            esp_hidh_dev_close(dev);
        }
    }
    free(ctx);
    vTaskDelete(NULL);
}

/* Returns the device, or NULL on ordinary failure. On a hang it does not return -
 * restartuje uklad. */
static esp_hidh_dev_t *open_device_guarded(const ble_addr_t *addr)
{
    open_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->addr = *addr;
    ctx->generation = ++s_open_generation;

    s_open_result = NULL;
    xSemaphoreTake(s_open_done, 0); /* wyczyscic ewentualny stary sygnal */
    xEventGroupClearBits(s_events, EV_OPEN_DONE | EV_OPEN_DOOMED);
    s_opening_addr = *addr;
    s_opening = true;

    if (xTaskCreate(open_task, "hid_open", 8192, ctx, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the opening task");
        s_opening = false;
        free(ctx);
        return NULL;
    }

    /*
     * We wait for one of two things: the open result, or word that the link to this very
     * device has dropped. The latter means the open cannot possibly succeed - further
     * GATT operations will return ENOTCONN synchronously and IDF's WAIT_CB() will hang
     * na zawsze (AGENTS.md 4.23). Bez tej sciezki czekalibysmy pelne 45 s; na sprzecie
     * zaobserwowane 36 s bezczynnosci, zanim zadzialal limit.
     */
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_OPEN_DONE | EV_OPEN_DOOMED,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(OPEN_TIMEOUT_MS));

    if (!(bits & EV_OPEN_DONE) && (bits & EV_OPEN_DOOMED)) {
        /* Link padl. Dajemy jeszcze chwile - otwarcie moglo byc na ostatniej prostej. */
        bits = xEventGroupWaitBits(s_events, EV_OPEN_DONE, pdFALSE, pdTRUE,
                                   pdMS_TO_TICKS(2000));
        if (!(bits & EV_OPEN_DONE)) {
            ESP_LOGE(TAG, "the link to the device being opened dropped - esp_hidh_dev_open() will hang");
            ESP_LOGE(TAG, "this is an IDF bug: WAIT_CB() in nimble_hidh.c waits without a timeout");
            ESP_LOGE(TAG, "restarting the chip - bonds live in NVS, everything comes back on its own");
            vTaskDelay(pdMS_TO_TICKS(500)); /* give the log time to reach the console */
            esp_restart();
        }
    }

    if (!(bits & EV_OPEN_DONE)) {
        ESP_LOGE(TAG, "esp_hidh_dev_open() did not return within %d s - the task is stuck inside IDF",
                 OPEN_TIMEOUT_MS / 1000);
        ESP_LOGE(TAG, "a known bug: WAIT_CB() in nimble_hidh.c waits without a timeout");
        ESP_LOGE(TAG, "restarting the chip - bonds live in NVS, everything comes back on its own");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    s_opening = false;
    return s_open_result;
}

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
                memset(slot, 0, sizeof(*slot));
                slot->in_use = true;
                slot->addr = disc->addr;
                break;
            }
        }
    }
    if (slot == NULL && looks_like_hid) {
        /* The list is full and this is a HID device - evict the first entry that is not
         * HID. Without this a busy airspace could push the keyboard out. */
        for (int i = 0; i < CANDIDATES_MAX; i++) {
            if (!s_cands[i].looks_like_hid && s_cands[i].cooldown_until_us == 0) {
                slot = &s_cands[i];
                memset(slot, 0, sizeof(*slot));
                slot->in_use = true;
                slot->addr = disc->addr;
                break;
            }
        }
    }
    if (slot != NULL) {
        /* Pakiety ADV i SCAN_RSP przychodza osobno i kazdy nosi czesc informacji -
         * so we merge rather than overwrite (AGENTS.md 4.4: filter_duplicates=0). */
        slot->rssi = disc->rssi;
        slot->looks_like_hid = slot->looks_like_hid || looks_like_hid;
        slot->bonded = is_bonded_peer(&disc->addr);
        /* SCAN_RSP nadpisalby typ pakietu glownego, a chcemy wiedziec, czy
         * the device advertises directed (DIR_IND = returning to a known host). */
        if (disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            slot->event_type = disc->event_type;
        }
        if (disc->length_data > 0 && slot->adv_len == 0) {
            slot->adv_len = disc->length_data < sizeof(slot->adv) ? disc->length_data
                                                                 : sizeof(slot->adv);
            memcpy(slot->adv, disc->data, slot->adv_len);
        }
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

    /*
     * Three independent signals, because none of them is sufficient on its own:
     *  - UUID 0x1812 / appearance 0x03Cx / znana nazwa: normalny przypadek, urzadzenie
     *    w trybie parowania rozglasza pelny payload,
     *  - a bond in NVS: a device that has paired with us once may come back advertising
     *    (np. po wybudzeniu ze snu) moze rozglaszac minimalny payload bez UUID,
     *  - DIR_IND: directed advertising, i.e. the device is aiming at a specific host.
     */
    bool looks_like_hid = has_hid_uuid || hid_appearance || name_is_known_device(name) ||
                          is_bonded_peer(&disc->addr) ||
                          disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
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
        /* Entries without an active cooldown are dropped so the list does not get stuck
         * on devices that no longer advertise. */
        if (s_cands[i].in_use && s_cands[i].cooldown_until_us < now) {
            s_cands[i].in_use = false;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

/* Copies ONE entry from the candidate table. Deliberately there is no variant that
 * cala tablice: sizeof(candidate_t) * CANDIDATES_MAX to ~1,5 kB, a te petle wolaja
 * esp_hidh_dev_open() afterwards, which itself eats a few kilobytes of stack. The
 * first version copied the whole table and ended in a "Stack protection fault"
 * in the hid_scan task on the first keyboard connection. */
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
    ESP_LOGI(TAG, "scan: %d devices, %d of them look like HID (bonds in NVS: %d)",
             total, hid, s_bonds_len);
    /* We log everything, not just HID - when a keyboard refuses to connect the first
     * question is "do we hear it at all". */
    static const char *evt[] = {"ADV_IND", "DIR_IND", "SCAN_IND", "NONCONN", "SCAN_RSP"};
    for (int i = 0; i < CANDIDATES_MAX; i++) {
        if (!candidate_get(i, &c)) {
            continue;
        }
        ESP_LOGI(TAG, "  %s " ADDR_FMT " type=%u rssi=%4d %s%s appearance=0x%04x '%s'",
                 c.looks_like_hid ? "HID" : "   ", ADDR_ARG(c.addr.val),
                 c.addr.type, c.rssi,
                 c.event_type < 5 ? evt[c.event_type] : "?",
                 c.bonded ? " BOND" : "", c.appearance, c.name[0] ? c.name : "?");
        /* With no name and no appearance only the raw ADV is left - without it there is no
         * powiedziec, co siedzi pod danym adresem. */
        if (c.name[0] == '\0' && c.appearance == 0 && c.adv_len > 0) {
            log_hex("      adv", c.adv, c.adv_len);
        }
    }
}

/*
 * Kontroler C3 odrzuca aktualizacje parametrow polaczenia z HCI 0x12 (Invalid HCI
 * Command Parameters). This is the same thing we had been seeing in the log all along
 * "ocf=0x0013, hci_err=0x212" - jeszcze zanim sami cokolwiek aktualizowalismy,
 * czyli odrzucane sa takze aktualizacje inicjowane przez NimBLE lub przez peera.
 *
 * We do not know WHICH parameter it dislikes, so we try several sets in turn and log
 * the result of each. The order is deliberate: if only a longer interval gets through,
 * the interval itself is the limit (three links on one antenna). If the variant with
 * ce_len or with a longer supervision timeout gets through, that parameter was the
 * problem. And if even the "no interval change" attempt is rejected, the controller
 * does not accept this command at all.
 */
/* Definicja nizej - skraca interwal polaczenia swiezo otwartego urzadzenia. */
static void request_fast_interval(esp_hidh_dev_t *dev);

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
        /*
         * Only ADV_IND and DIR_IND are advertisements that can be answered with a connection
         * polaczeniem. SCAN_IND i NONCONN_IND to z definicji broadcast - proba
         * attempt would end in a 30 s timeout inside the blocking dev_open.
         * Praktyczny przypadek: Windows rozglasza beacon Swift Pair
         * (Manufacturer Specific Data, company ID 0x0006) exactly as NONCONN.
         */
        if (c.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            c.event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
            continue;
        }
        if (c.cooldown_until_us > esp_timer_get_time() || device_is_open(&c.addr)) {
            continue;
        }

        ESP_LOGI(TAG, "candidate " ADDR_FMT " type=%u '%s' appearance=0x%04x rssi=%d",
                 ADDR_ARG(c.addr.val), c.addr.type, c.name[0] ? c.name : "?",
                 c.appearance, c.rssi);

        /* Scanning and initiating a connection cannot happen at the same time. */
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(200));

        esp_hidh_dev_t *dev = open_device_guarded(&c.addr);
        if (dev == NULL) {
            ESP_LOGW(TAG, "  open failed, cooldown %d s", RETRY_COOLDOWN_US / 1000000);
            candidate_set_cooldown(&c.addr);
            continue;
        }

        uint8_t mask = device_usage_mask(dev, false);
        device_register(&c.addr, mask, dev);
        ESP_LOGI(TAG, "  connected (usage mask 0x%02x), %d/%d devices total",
                 mask, ble_hid_host_device_count(), HID_HOST_MAX_DEVICES);
        if (mask == 0) {
            ESP_LOGW(TAG, "  no input reports - this device is of no use to us");
        }

        request_fast_interval(dev);

        /*
         * A gap before the next attempt. A freshly connected device has just subscribed to
         * several characteristics and is starting to stream reports; discovering services on
         * drugiego urzadzenia w tym samym momencie to trzy aktywne linki walczace
         * for the antenna. On hardware that ended with the keyboard link dropping
         * (disconnect reason=520, czyli HCI 0x08 Connection Timeout) 1,5 s po
         * after the mouse connected - and that drop is what triggered the IDF hang.
         */
        vTaskDelay(pdMS_TO_TICKS(3000));
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
        /* CRITICAL: the keyboard does not put its full name in the first ADV packet,
         * a filtr duplikatow ucinal kolejne (AGENTS.md 4.4). */
        .filter_duplicates = 0,
    };

    candidates_clear_stale();
    refresh_bonded_peers();
    xEventGroupClearBits(s_events, EV_DISC_DONE);

    int rc = ble_gap_disc(ble_stack_own_addr_type(), SCAN_DURATION_MS, &dp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc did not start: rc=%d", rc);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    xEventGroupWaitBits(s_events, EV_DISC_DONE, pdTRUE, pdTRUE,
                        pdMS_TO_TICKS(SCAN_DURATION_MS + 2000));
    log_scan_results();
    try_connect_candidates();
}

/*
 * Skracamy interwal polaczenia swiezo otwartego urzadzenia wejsciowego.
 *
 * DLACZEGO: peryferial moze wyslac notyfikacje tylko w zdarzeniu polaczenia, czyli
 * the interval is the upper limit on report rate. esp_hidh calls
 * ble_gap_connect() with NULL parameters (nimble_hidh.c:991), so NimBLE's defaults
 * domyslne z NimBLE: BLE_GAP_INITIAL_CONN_ITVL_MIN/MAX to 30 i 50 ms. Stad mysz
 * reported only ~20-25 times per second even though it samples far faster.
 *
 * WHY ONLY HERE and not right after connecting: service discovery is the heaviest
 * moment for the stack (dozens of GATT procedures with two other links active) and
 * both crashes in this project's history landed exactly then (4.21, 4.26). Packing
 * connection events more tightly at that point buys nothing - there are no reports yet.
 *
 * WHY NOT ALWAYS THE MINIMUM: one antenna carries three links plus a periodic scan,
 * a krotszy interwal to wiecej zdarzen polaczenia w tej samej sekundzie, takze pustych.
 * The value therefore lives in Kconfig (APP_INPUT_CONN_ITVL) so it can be backed off
 * w kodzie. Domyslne 6 jednostek, czyli 7,5 ms, to minimum ze specyfikacji BLE i tyle
 * potrzebuje AJ159 Pro na swoje 125 Hz.
 */
#define FAST_ITVL_MIN CONFIG_APP_INPUT_CONN_ITVL
/* Upper bound with slack: the controller has room to manoeuvre with three links, and
 * we still get no less than FAST_ITVL_MIN. */
#define FAST_ITVL_MAX (CONFIG_APP_INPUT_CONN_ITVL + 2)

static void request_fast_interval(esp_hidh_dev_t *dev)
{
    if (dev == NULL || dev->ble.conn_id < 0) {
        return;
    }
    const uint16_t conn_handle = (uint16_t)dev->ble.conn_id;

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return;
    }
    if (desc.conn_itvl <= FAST_ITVL_MAX) {
        ESP_LOGI(TAG, "  interval on link %u is already %u (%u ms), leaving it",
                 conn_handle, desc.conn_itvl, (unsigned)(desc.conn_itvl * 125 / 100));
        return;
    }

    /*
     * Ustalone eksperymentem na sprzecie (log z 2026-08-17):
     *  - 7,5 ms odrzucone z HCI 0x12, TAKZE w wariantach z ce_len i z dluzszym
     *    supervision timeout - so the interval itself is the problem, not another parameter,
     *  - 15-20 ms przyjete, a kontroler wybral GORNA granice zakresu (dostalismy 20 ms).
     *
     * So we ask for a specific value (min = max) rather than a range, and walk a ladder
     * from the shortest upwards. The first accepted value wins, so we end up with the
     * shortest interval this controller allows with three links.
     */
    const uint16_t ladder[] = {CONFIG_APP_INPUT_CONN_ITVL, 8, 10, 12, 16, 24};

    ESP_LOGI(TAG, "  link %u: interval %u (%u ms), latency=%u, supervision timeout=%u (%u ms)",
             conn_handle, desc.conn_itvl, (unsigned)(desc.conn_itvl * 125 / 100),
             desc.conn_latency, desc.supervision_timeout,
             (unsigned)(desc.supervision_timeout * 10));

    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        const uint16_t itvl = ladder[i];
        if (i > 0 && itvl <= CONFIG_APP_INPUT_CONN_ITVL) {
            continue; /* juz probowane jako wartosc z Kconfig */
        }
        if (itvl >= desc.conn_itvl) {
            break; /* longer than the current one makes no sense */
        }
        const struct ble_gap_upd_params params = {
            .itvl_min = itvl,
            .itvl_max = itvl,
            .latency = 0,
            .supervision_timeout = desc.supervision_timeout,
            .min_ce_len = 0,
            .max_ce_len = 0,
        };
        int rc = ble_gap_update_params(conn_handle, &params);
        if (rc == 0) {
            ESP_LOGI(TAG, "  ACCEPTED: interval %u (%u.%02u ms = %u Hz)",
                     itvl, (unsigned)(itvl * 125 / 100), (unsigned)(itvl * 125 % 100),
                     (unsigned)(1000 * 100 / (itvl * 125)));
            return;
        }
        /* rc >= 0x200 to blad HCI; kod z listy Bluetooth to rc - 0x200. */
        ESP_LOGW(TAG, "  interval %u (%u.%02u ms) rejected: rc=%d (HCI 0x%02x)",
                 itvl, (unsigned)(itvl * 125 / 100), (unsigned)(itvl * 125 % 100),
                 rc, rc >= 0x200 ? (unsigned)(rc - 0x200) : 0u);
    }
    ESP_LOGW(TAG, "  no interval was accepted - keeping %u ms",
             (unsigned)(desc.conn_itvl * 125 / 100));
}

static void scan_task(void *arg)
{
    ble_stack_wait_synced(portMAX_DELAY);
    ESP_LOGI(TAG, "starting scan (up to %d HID devices, target interval %u.%02u ms = %u Hz)",
             HID_HOST_MAX_DEVICES,
             (unsigned)(FAST_ITVL_MIN * 125 / 100), (unsigned)(FAST_ITVL_MIN * 125 % 100),
             (unsigned)(1000 * 100 / (FAST_ITVL_MIN * 125)));

    bool retried_without_scan = false;

    while (true) {
        reap_dead_devices();
        if (ble_hid_host_device_count() >= HID_HOST_MAX_DEVICES) {
            /*
             * The device limit is reached, so scanning stops - and a scan reserves radio
             * time. That is the only suspect left: the controller rejects a master-initiated
             * master-initiated interwal ponizej 15 ms, mimo ze rownolegle utrzymuje
             * link 7,5 ms w roli peryferiala (pad) i mimo ze odrzucal to takze przy
             * ONE central link. If the scanner is the cause then now - with the radio free of
             * wolnym od skanowania - krotszy interwal powinien przejsc.
             *
             * A one-shot attempt, so as not to hammer the controller in a loop.
             */
            if (!retried_without_scan) {
                retried_without_scan = true;
                vTaskDelay(pdMS_TO_TICKS(3000)); /* let the radio settle */
                ESP_LOGI(TAG, "scan stopped (device limit reached) - retrying the interval request");
                for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
                    esp_hidh_dev_t *dev = device_at(i);
                    if (dev != NULL) {
                        request_fast_interval(dev);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        retried_without_scan = false;
        scan_round();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------------------------------------------------ start */

esp_err_t ble_hid_host_start(void)
{
    s_events = xEventGroupCreate();
    s_open_done = xSemaphoreCreateBinary();
    if (s_events == NULL || s_open_done == NULL) {
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

    /* Disconnect detection. It has to be ours, because ESP_HIDH_CLOSE_EVENT never
     * arrives on NimBLE (AGENTS.md 4.25). */
    int rc = ble_gap_event_listener_register(&s_gap_listener, gap_disconnect_listener, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_event_listener_register: rc=%d", rc);
        return ESP_FAIL;
    }

    /*
     * 4 kB is enough: esp_hidh_dev_open() itself, which needs ~2.6 kB, now runs in a
     * separate "hid_open" task with 8 kB (see open_device_guarded).
     */
    if (xTaskCreate(scan_task, "hid_scan", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the scanning task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
