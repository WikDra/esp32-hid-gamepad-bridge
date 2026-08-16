/*
 * Rola central: NimBLE + esp_hidh. Patrz ble_hid_host.h i AGENTS.md 4.3/4.4.
 *
 * Wlasny skaner zamiast esp_hid_gap.c z przykladu IDF: tamten plik ma 1100 linii,
 * z czego wiekszosc to martwy kod Bluetooth Classic i Bluedroid, ktorych na C3
 * z NimBLE nie da sie uzyc. Tu jest tylko sciezka BLE.
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
    /* Suma bitowa ESP_HID_USAGE_* ze wszystkich raportow wejsciowych urzadzenia.
     * NIE uzywamy esp_hidh_dev_usage_get(): na sciezce NimBLE zwraca on zawsze
     * GENERIC, bo nimble_hidh.c nigdy nie ustawia dev->usage (potwierdzone w logu
     * z plytki i grepem po IDF 5.5.1) - patrz AGENTS.md 4.15. */
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
    bool bonded;               /* adres jest na liscie sparowanych w NVS */
    uint8_t event_type;        /* BLE_HCI_ADV_RPT_EVTYPE_* - DIR_IND znaczy "wracam do konkretnego hosta" */
    /* Pierwsze bajty ADV. Sluzy do rozpoznawania urzadzen, ktore nie podaja ani
     * nazwy, ani appearance - bez tego w logu widac tylko adres i nie da sie
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

/* Przelicza flagi "co mamy podlaczone" z tablicy urzadzen. Wolac TYLKO z sekcji
 * krytycznej. Flagi siedza w stanie wejsc, zeby Etap 3 dostawal je w jednym
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

    /* Urzadzenie odpadlo -> wyczyscic jego wejscia, inaczej pad zostanie
     * z wcisnietym klawiszem albo przyciskiem myszy na zawsze. */
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

static void device_unregister(esp_hidh_dev_t *dev)
{
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (s_devs[i].in_use && s_devs[i].dev == dev) {
            s_devs[i].in_use = false;
            s_devs[i].usage_mask = 0;
            s_devs[i].dev = NULL;
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

void ble_hid_host_log_devices(void)
{
    open_dev_t snapshot[HID_HOST_MAX_DEVICES];
    taskENTER_CRITICAL(&s_mux);
    memcpy(snapshot, s_devs, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < HID_HOST_MAX_DEVICES; i++) {
        if (snapshot[i].in_use) {
            ESP_LOGI(TAG, "  [%d] " ADDR_FMT " raporty:%s%s%s (maska 0x%02x)",
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
 * Lista adresow, z ktorymi mamy juz bond w NVS. Po co: urzadzenie, ktore raz sie
 * sparowalo, przy powrocie (np. po wybudzeniu ze snu) nie musi juz rozglaszac
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
         * a w skanie moze przyjsc ten sam adres z innym oznaczeniem typu. */
        if (memcmp(s_bonds[i].val, addr->val, 6) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------ raporty urzadzenia */

/*
 * Klasyfikacja urzadzenia i diagnostyka. Lista raportow jest jedynym rzetelnym
 * zrodlem informacji "czym to urzadzenie jest": esp_hidh_dev_usage_get() na
 * sciezce NimBLE zwraca zawsze GENERIC (AGENTS.md 4.15).
 *
 * Dump tabeli raportow jest tez kluczowy przy diagnozie dziwnych raportow -
 * AULA F99 Pro wystawia wiecej niz jeden raport wejsciowy o dlugosci 8 B.
 */
static uint8_t device_usage_mask(esp_hidh_dev_t *dev, bool log_table)
{
    size_t num = 0;
    esp_hid_report_item_t *reports = NULL;
    uint8_t mask = 0;

    if (esp_hidh_dev_reports_get(dev, &num, &reports) != ESP_OK || reports == NULL) {
        ESP_LOGW(TAG, "nie moge odczytac listy raportow");
        return 0;
    }

    if (log_table) {
        ESP_LOGI(TAG, "  raportow: %u", (unsigned)num);
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
         * 0x01 ErrorRollOver, 0x02 POSTFail, 0x03 ErrorUndefined - to nie sa klawisze,
         * tylko kody bledu z tabeli USB HID Keyboard/Keypad. AULA F99 Pro wysyla
         * serie raportow z 0x01 po kazdym nacisnieciu (widoczne w logu z plytki),
         * wiec bez tego filtra pad dostawalby fantomowy przycisk.
         */
        if (k >= 0x01 && k <= 0x03) {
            rollover = true;
            k = 0;
        }
        keys[i] = k;
    }

    /* Raport skladajacy sie wylacznie z kodow bledu ignorujemy calkowicie -
     * nadpisanie stanu zerami zgubiloby klawisz naprawde trzymany. */
    bool only_errors = rollover && keys[0] == 0 && d[0] == 0;
    if (!only_errors) {
        taskENTER_CRITICAL(&s_mux);
        s_state.modifiers = d[0];
        memcpy(s_state.keys, keys, HID_KEYS_MAX);
        taskEXIT_CRITICAL(&s_mux);
    }

    /* Raport samego rollovera nic nie wnosi, a AULA wysyla trzy takie po kazdym
     * nacisnieciu klawisza (§4.17) - w logu to trzy czwarte linii. Zliczamy je
     * i raportujemy zbiorczo, zeby log nadawal sie do czytania. */
    if (only_errors) {
        static uint32_t rollover_count;
        static int64_t last_us;
        int64_t now = esp_timer_get_time();
        rollover_count++;
        if ((now - last_us) > 1000000) {
            last_us = now;
            ESP_LOGD(TAG, "odfiltrowanych raportow ErrorRollOver: %" PRIu32, rollover_count);
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
     *   d[6]      zawsze 0 w naszych probach (prawdopodobnie kolko poziome)
     *
     * Rozstrzygajaca byla probka [00 ff ff 01 00 00 00] przy powolnym ruchu w lewo:
     * odczyt 16-bitowy daje (-1, +1), a 8-bitowy (-1, -1) - ten drugi bierze za Y
     * gorny bajt X-a. Do tego przy ruchu wylacznie w pion bajty X byly zerowe,
     * a 8-bitowy odczyt Y pokazywalby wtedy 0 mimo realnego ruchu.
     *
     * Krotszy raport (3 B) to klasyczny boot protocol z 8-bitowymi osiami - AJ159
     * deklaruje taki wariant w tabeli raportow, wiec obsluga zostaje.
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

    /* Uklad jest ustalony, wiec surowe bajty sa potrzebne juz tylko przy diagnozie
     * nowego urzadzenia. Zostaje jedna linia na sekunde, zeby dalo sie zobaczyc,
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
         * przy NimBLE linker go nie znajdzie. Appearance mamy z pakietu ADV.
         * esp_hidh_dev_usage_get() tez nie ma sensu (zawsze GENERIC), dlatego
         * klasyfikacje bierzemy z listy raportow. */
        ESP_LOGI(TAG, "OPEN " ADDR_FMT " '%s' vid=0x%04x pid=0x%04x",
                 ADDR_ARG(bda), name ? name : "?",
                 esp_hidh_dev_vendor_id_get(p->open.dev),
                 esp_hidh_dev_product_id_get(p->open.dev));
        device_usage_mask(p->open.dev, true); /* tylko log tabeli raportow */
        break;
    }
    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "bateria %u%%", p->battery.level);
        break;

    case ESP_HIDH_INPUT_EVENT: {
        uint16_t len = p->input.length;
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD && len >= 8) {
            handle_keyboard_report(p);
        } else if (p->input.usage == ESP_HID_USAGE_MOUSE && len >= 3) {
            handle_mouse_report(p);
        } else {
            /* Np. consumer control (klawisze multimedialne) albo raport, ktorego
             * jeszcze nie rozumiemy - warto zobaczyc, co przychodzi. */
            static int64_t last_other_us;
            int64_t now = esp_timer_get_time();
            if ((now - last_other_us) > 1000000) {
                last_other_us = now;
                ESP_LOGW(TAG, "raport nieobslugiwany: usage=%s map=%u id=%u",
                         esp_hid_usage_str(p->input.usage), p->input.map_index, p->input.report_id);
                log_hex("  RAW", p->input.data, len);
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

/* ------------------------------------------------- otwieranie z ograniczeniem czasu */

/*
 * esp_hidh_dev_open() potrafi zablokowac sie NA ZAWSZE. To blad w IDF, nie u nas:
 *
 *   nimble_hidh.c:49   WAIT_CB() = xSemaphoreTake(sem, portMAX_DELAY)
 *   nimble_hidh.c:353  rc = ble_gattc_disc_all_chrs(...); WAIT_CB();
 *
 * czyli kod czeka bez timeoutu i NIE sprawdza rc. Gdy link padnie w trakcie
 * odkrywania uslug, kolejne operacje GATT zwracaja BLE_HS_ENOTCONN synchronicznie,
 * zaden callback juz nie przyjdzie i WAIT_CB() nie ma kto obudzic.
 *
 * Zaobserwowane na sprzecie: polaczenie z klawiatura zerwalo sie w trakcie odczytu
 * uslug (disconnect reason=520, czyli HCI 0x08 Connection Timeout), zadanie hid_scan
 * zawislo i mostek przestal skanowac - a wiec mysz po zasnieciu nie miala jak wrocic.
 * W logu bylo to widoczne po braku linii "skan:" i braku "po esp_hidh_dev_open ...",
 * ktora normalnie leci bezwarunkowo po wywolaniu.
 *
 * Dlatego wywolanie idzie do osobnego zadania, a hid_scan czeka z limitem czasu.
 * Po przekroczeniu limitu wiemy, ze zadanie utknelo w IDF. Nie da sie go bezpiecznie
 * ubic: siedzi na prywatnym semaforze esp_hidh, a kolejne proby otwarcia
 * konkurowalyby z nim o ten sam semafor (czyli obudzilyby jego, nie nas). Dlatego
 * jedynym uczciwym wyjsciem jest kontrolowany restart - bondy sa w NVS, wiec PC
 * wraca po ~2 s, a klawiatura i mysz same sie podlaczaja.
 */
#define OPEN_TIMEOUT_MS 45000 /* dev_open ma w sobie 30 s timeoutu na samo polaczenie */

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

    ESP_LOGI(TAG, "  po esp_hidh_dev_open zostalo %u B stosu",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    if (ctx->generation == s_open_generation) {
        s_open_result = dev;
        xSemaphoreGive(s_open_done);
    } else {
        /* Nas juz nikt nie slucha - hid_scan uznal te probe za zawieszona. */
        ESP_LOGW(TAG, "  spozniony wynik otwarcia (generacja %" PRIu32 "), pomijam", ctx->generation);
        if (dev != NULL) {
            esp_hidh_dev_close(dev);
        }
    }
    free(ctx);
    vTaskDelete(NULL);
}

/* Zwraca urzadzenie, NULL przy zwyklym niepowodzeniu. Przy zawieszeniu nie wraca -
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

    if (xTaskCreate(open_task, "hid_open", 8192, ctx, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "nie moge utworzyc zadania otwierajacego");
        free(ctx);
        return NULL;
    }

    if (xSemaphoreTake(s_open_done, pdMS_TO_TICKS(OPEN_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "esp_hidh_dev_open() nie wrocilo w %d s - zadanie utknelo w IDF",
                 OPEN_TIMEOUT_MS / 1000);
        ESP_LOGE(TAG, "to znany blad: WAIT_CB() w nimble_hidh.c czeka bez timeoutu");
        ESP_LOGE(TAG, "restartuje uklad - bondy sa w NVS, wszystko wroci samo");
        vTaskDelay(pdMS_TO_TICKS(500)); /* zeby log zdazyl wyjsc na konsole */
        esp_restart();
    }
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
        /* Lista pelna, a to jest urzadzenie HID - wypieramy pierwszy wpis, ktory
         * HID-em nie jest. Bez tego gesty eter moglby wypchnac klawiature. */
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
         * dlatego scalamy, a nie nadpisujemy (AGENTS.md 4.4: filter_duplicates=0). */
        slot->rssi = disc->rssi;
        slot->looks_like_hid = slot->looks_like_hid || looks_like_hid;
        slot->bonded = is_bonded_peer(&disc->addr);
        /* SCAN_RSP nadpisalby typ pakietu glownego, a chcemy wiedziec, czy
         * urzadzenie rozglasza sie kierunkowo (DIR_IND = wraca do znanego hosta). */
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
     * Trzy niezalezne przeslanki, bo zadna nie wystarcza sama:
     *  - UUID 0x1812 / appearance 0x03Cx / znana nazwa: normalny przypadek, urzadzenie
     *    w trybie parowania rozglasza pelny payload,
     *  - bond w NVS: urzadzenie, ktore juz raz sie z nami sparowalo, przy powrocie
     *    (np. po wybudzeniu ze snu) moze rozglaszac minimalny payload bez UUID,
     *  - DIR_IND: rozgloszenie kierunkowe, czyli urzadzenie celuje w konkretny host.
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
    ESP_LOGI(TAG, "skan: %d urzadzen, z tego %d wyglada na HID (bondow w NVS: %d)",
             total, hid, s_bonds_len);
    /* Logujemy wszystko, nie tylko HID - jak klawiatura nie chce sie polaczyc,
     * pierwsze pytanie jest "czy w ogole ja slyszymy". */
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
        /* Bez nazwy i bez appearance zostaje tylko surowy ADV - bez tego nie da sie
         * powiedziec, co siedzi pod danym adresem. */
        if (c.name[0] == '\0' && c.appearance == 0 && c.adv_len > 0) {
            log_hex("      adv", c.adv, c.adv_len);
        }
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
        /*
         * Tylko ADV_IND i DIR_IND sa rozgloszeniami, na ktore mozna odpowiedziec
         * polaczeniem. SCAN_IND i NONCONN_IND to z definicji broadcast - proba
         * polaczenia skonczylaby sie timeoutem 30 s w blokujacym dev_open.
         * Praktyczny przypadek: Windows rozglasza beacon Swift Pair
         * (Manufacturer Specific Data, company ID 0x0006) wlasnie jako NONCONN.
         */
        if (c.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            c.event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
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

        esp_hidh_dev_t *dev = open_device_guarded(&c.addr);
        if (dev == NULL) {
            ESP_LOGW(TAG, "  nie udalo sie otworzyc, cooldown %d s", RETRY_COOLDOWN_US / 1000000);
            candidate_set_cooldown(&c.addr);
            continue;
        }

        uint8_t mask = device_usage_mask(dev, false);
        device_register(&c.addr, mask, dev);
        ESP_LOGI(TAG, "  podlaczone (maska usage 0x%02x), razem %d/%d urzadzen",
                 mask, ble_hid_host_device_count(), HID_HOST_MAX_DEVICES);
        if (mask == 0) {
            ESP_LOGW(TAG, "  brak raportow wejsciowych - urzadzenie nic nam nie da");
        }

        /*
         * Odstep przed kolejna proba. Swiezo podlaczone urzadzenie wlasnie zapisalo
         * sie na kilka charakterystyk i zaczyna nadawac raporty; odkrywanie uslug
         * drugiego urzadzenia w tym samym momencie to trzy aktywne linki walczace
         * o antene. Na sprzecie skonczylo sie to zerwaniem linku z klawiatura
         * (disconnect reason=520, czyli HCI 0x08 Connection Timeout) 1,5 s po
         * podlaczeniu myszy - a to wlasnie zerwanie wywolalo blokade z IDF.
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
        /* KLUCZOWE: AULA F99 Pro nie daje pelnej nazwy w pierwszym pakiecie ADV,
         * a filtr duplikatow ucinal kolejne (AGENTS.md 4.4). */
        .filter_duplicates = 0,
    };

    candidates_clear_stale();
    refresh_bonded_peers();
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

    /*
     * 4 kB wystarcza: samo esp_hidh_dev_open(), ktore potrzebuje ~2,6 kB, wykonuje
     * sie teraz w osobnym zadaniu "hid_open" z 8 kB (patrz open_device_guarded).
     */
    if (xTaskCreate(scan_task, "hid_scan", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "nie moge utworzyc zadania skanujacego");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
