/*
 * Wi-Fi lifecycle for the configuration panel. See webui.h for why it is on demand.
 */

#include "webui.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "webui_http.h"

static const char *TAG = "webui";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"

/* How often the control task looks at what is wanted versus what is running. Wi-Fi coming up is
 * a multi-second affair, so there is nothing to gain from a tighter loop. */
#define TICK_MS 250

/* Reconnect attempts before giving up on the stored network and serving an access point. The
 * timeout in Kconfig is the real limit; this only stops us hammering a network that answers
 * quickly and rejects us, which would otherwise never reach that timeout. */
#define STA_MAX_RETRY 5

static volatile bool s_want_on;
static volatile bool s_want_ap;
static volatile uint8_t s_wire_bits;  /* what the link last asked for */
static volatile bool s_suppressed;    /* the idle timeout has overridden the link */
static volatile webui_state_t s_state;
static volatile int64_t s_last_touch_us;

static esp_netif_t *s_netif_ap;
static esp_netif_t *s_netif_sta;
static esp_event_handler_instance_t s_h_wifi;
static esp_event_handler_instance_t s_h_ip;
static bool s_wifi_inited;
static int s_sta_retry;
static volatile bool s_sta_got_ip;

static char s_url[32];
static char s_sta_ssid[33];
static volatile bool s_want_restart;
static char s_ssid[33];
static char s_password[33];

void webui_request(bool on, bool force_ap)
{
    /*
     * The link carries ABSOLUTE state and repeats it with every keepalive, four times a second.
     * That is what makes a lost frame harmless - and it is also why the idle timeout needs the
     * suppression flag below.
     *
     * WITHOUT IT THIS OSCILLATES. The input chip has no idea we shut down, so it keeps asserting
     * "panel on"; a plain idle shutdown would be undone within 250 ms, forever. The link is
     * one-directional, so there is no way to answer "clear that bit" - it has to be resolved
     * here.
     *
     * Resolution: the link wins again as soon as the USER does something, which is any change in
     * the bits. So after an idle shutdown, Ctrl+Alt+W once clears the bit and once more sets it,
     * and Ctrl+Alt+P recovers in a single press because it changes the AP bit. What it will not
     * do is fight a timeout that already fired.
     */
    const uint8_t bits = (uint8_t)((on ? 1u : 0u) | (force_ap ? 2u : 0u));
    if (bits != s_wire_bits) {
        s_wire_bits = bits;
        s_suppressed = false;
    }

    s_want_on = on && !s_suppressed;
    s_want_ap = force_ap;
}

void webui_touch(void)
{
    s_last_touch_us = esp_timer_get_time();
}

webui_state_t webui_state(void)
{
    return s_state;
}

const char *webui_url(void)
{
    return s_url;
}

const char *webui_sta_ssid(void)
{
    return s_sta_ssid;
}

/* Refreshes the cached copy of the stored network name. Cached rather than read on demand because
 * the panel polls the state several times a second and this is in that response. */
static void refresh_sta_ssid(void)
{
    s_sta_ssid[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_sta_ssid);
    if (nvs_get_str(h, NVS_KEY_STA_SSID, s_sta_ssid, &len) != ESP_OK) {
        s_sta_ssid[0] = '\0';
    }
    nvs_close(h);
}

void webui_network_changed(void)
{
    refresh_sta_ssid();
    /*
     * Handled by the control task rather than here, so the HTTP response has already left before
     * the interface it came in on is torn down. wifi_down() does not set the suppression flag, so
     * the next tick brings Wi-Fi straight back up - this time consulting the credentials.
     */
    s_want_restart = true;
    ESP_LOGI(TAG, "network settings changed - restarting Wi-Fi to apply them");
}

/*
 * Device credentials.
 *
 * An empty APP_WEBUI_PASSWORD means "derive one", which is deliberately the default: a fixed
 * password in a public repository is the same password on every board built from it, and this
 * panel can both change what the pad reports and flash new firmware. Eight hex digits of the MAC
 * is unique per board, needs no configuration, and meets WPA2's eight-character minimum exactly.
 */
static void build_identity(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    snprintf(s_ssid, sizeof(s_ssid), "%s-%02x%02x", CONFIG_APP_WEBUI_AP_SSID_PREFIX, mac[4],
             mac[5]);

    if (strlen(CONFIG_APP_WEBUI_PASSWORD) >= 8) {
        snprintf(s_password, sizeof(s_password), "%s", CONFIG_APP_WEBUI_PASSWORD);
    } else {
        if (strlen(CONFIG_APP_WEBUI_PASSWORD) > 0) {
            ESP_LOGW(TAG, "APP_WEBUI_PASSWORD is shorter than the 8 characters WPA2 requires - "
                          "deriving one from the MAC instead");
        }
        snprintf(s_password, sizeof(s_password), "%02x%02x%02x%02x", mac[2], mac[3], mac[4],
                 mac[5]);
    }
}

static bool sta_credentials(char ssid[33], char pass[65])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t len = 33;
    esp_err_t err = nvs_get_str(h, NVS_KEY_STA_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = 65;
        if (nvs_get_str(h, NVS_KEY_STA_PASS, pass, &len) != ESP_OK) {
            pass[0] = '\0'; /* an open network is unusual but legal */
        }
    }
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_got_ip = false;
        if (s_sta_retry < STA_MAX_RETRY) {
            s_sta_retry++;
            esp_wifi_connect();
        }
        /* Not an error yet: the control task decides when to give up, because only it knows how
         * long the attempt has been running. */
        break;

    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client joined the access point: " MACSTR, MAC2STR(e->mac));
        /* A client joining counts as activity, or the idle timeout could drop the network out
         * from under somebody who has only just connected. */
        webui_touch();
        break;
    }

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *e = data;
    snprintf(s_url, sizeof(s_url), "http://" IPSTR, IP2STR(&e->ip_info.ip));
    s_sta_got_ip = true;
}

static esp_err_t wifi_common_init(void)
{
    /*
     * The default event loop and the netif layer are process-wide and other components may rely
     * on them, so they are created once and never torn down. Only the Wi-Fi driver and our own
     * netifs are released when the panel goes away, which is where the memory is.
     */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Marked as initialised HERE, not at the end of this function, and that is a bug fix rather
     * than a style choice: every failure below goes to wifi_down(), which does nothing unless this
     * flag is set. Setting it last meant a failure halfway through left the driver initialised and
     * its event handlers registered, with nothing able to clean either up - a leak per attempt, on
     * the path that retries.
     */
    s_wifi_inited = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL,
                                             &s_h_wifi);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL,
                                             &s_h_ip);
    if (err != ESP_OK) {
        return err;
    }

    /* Credentials are not persisted to flash: they are ours to decide every time, and an NVS write
     * per Wi-Fi start would be wear for nothing. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    return ESP_OK;
}

/*
 * The SSID and password fields of wifi_config_t are fixed-size arrays that are NOT required to be
 * NUL-terminated, so a bounded copy is both the correct way to fill them and the way to avoid
 * gcc's format-truncation warning: snprintf from a NUL-terminated buffer one byte larger than the
 * destination is exactly the case it flags, and it is right to.
 *
 * The caller zero-initialises the struct, so whatever is not copied is already zero.
 */
static size_t copy_field(uint8_t *dst, size_t dst_size, const char *src)
{
    const size_t n = strnlen(src, dst_size);
    memcpy(dst, src, n);
    return n;
}

static esp_err_t start_ap(void)
{
    s_netif_ap = esp_netif_create_default_wifi_ap();
    if (!s_netif_ap) {
        return ESP_ERR_NO_MEM;
    }

    wifi_config_t wc = {0};
    wc.ap.ssid_len = (uint8_t)copy_field(wc.ap.ssid, sizeof(wc.ap.ssid), s_ssid);
    copy_field(wc.ap.password, sizeof(wc.ap.password), s_password);
    wc.ap.max_connection = 2;
    wc.ap.channel = 1;
    /*
     * WPA2, never open. The panel can rewrite the mapping and flash firmware, so an open access
     * point would hand both to anyone in range. The password is printed on the console because
     * a derived one has to be discoverable somehow.
     */
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_netif_ap, &ip);
    snprintf(s_url, sizeof(s_url), "http://" IPSTR, IP2STR(&ip.ip));

    ESP_LOGI(TAG, "access point '%s' up, password '%s' -> %s", s_ssid, s_password, s_url);
    return webui_http_start(s_password);
}

static esp_err_t start_sta(const char *ssid, const char *pass)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();
    if (!s_netif_sta) {
        return ESP_ERR_NO_MEM;
    }

    wifi_config_t wc = {0};
    copy_field(wc.sta.ssid, sizeof(wc.sta.ssid), ssid);
    copy_field(wc.sta.password, sizeof(wc.sta.password), pass);

    s_sta_retry = 0;
    s_sta_got_ip = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start(); /* WIFI_EVENT_STA_START triggers the connect */
    }
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "joining '%s' (up to %d s, then falling back to an access point)", ssid,
             CONFIG_APP_WEBUI_STA_TIMEOUT_S);
    return ESP_OK;
}

static void wifi_down(void)
{
    if (!s_wifi_inited) {
        return;
    }

    /* Server first: stopping it closes its sockets while the interface they belong to still
     * exists. The other order leaves lwIP tearing down under a task that is still accepting. */
    webui_http_stop();

    esp_wifi_stop();

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);

    /*
     * deinit, not just stop: stopping leaves the driver's buffers allocated, and the point of
     * doing this on demand is to give the memory back. Netifs are destroyed too, or a second
     * start would create duplicates.
     */
    esp_wifi_deinit();

    if (s_netif_ap) {
        esp_netif_destroy_default_wifi(s_netif_ap);
        s_netif_ap = NULL;
    }
    if (s_netif_sta) {
        esp_netif_destroy_default_wifi(s_netif_sta);
        s_netif_sta = NULL;
    }

    s_wifi_inited = false;
    s_sta_got_ip = false;
    s_url[0] = '\0';
    s_state = WEBUI_OFF;
    ESP_LOGI(TAG, "Wi-Fi down, %u B heap free", (unsigned)esp_get_free_heap_size());
}

/*
 * A failed start must not be retried four times a second for the rest of the session, which is
 * what the repeating link request would otherwise cause. Same mechanism as the idle timeout: stop
 * wanting it until the user asks again.
 */
static void fail_and_stop(const char *why, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s - giving up until the hotkey is pressed again", why,
             esp_err_to_name(err));
    s_suppressed = true;
    s_want_on = false;
    wifi_down();
}

static void wifi_up(void)
{
    const uint32_t heap_before = esp_get_free_heap_size();

    esp_err_t err = wifi_common_init();
    if (err != ESP_OK) {
        fail_and_stop("Wi-Fi init failed", err);
        return;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    const bool have_sta = !s_want_ap && sta_credentials(ssid, pass);

    if (have_sta) {
        err = start_sta(ssid, pass);
        s_state = WEBUI_STA_CONNECTING;
    } else {
        err = start_ap();
        s_state = WEBUI_AP_UP;
    }

    if (err != ESP_OK) {
        fail_and_stop("starting Wi-Fi failed", err);
        return;
    }

    webui_touch();
    ESP_LOGI(TAG, "Wi-Fi cost %d B of heap", (int)heap_before - (int)esp_get_free_heap_size());
}

/* Falls back from a failed join to an access point without tearing the driver down. */
static void sta_to_ap(void)
{
    ESP_LOGW(TAG, "could not join the stored network - serving an access point instead");

    esp_wifi_stop();
    if (s_netif_sta) {
        esp_netif_destroy_default_wifi(s_netif_sta);
        s_netif_sta = NULL;
    }

    const esp_err_t err = start_ap();
    if (err == ESP_OK) {
        s_state = WEBUI_AP_UP;
        webui_touch();
    } else {
        fail_and_stop("the access point would not start either", err);
    }
}

static void webui_task(void *arg)
{
    int64_t attempt_started_us = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        const bool want_on = s_want_on;
        const bool want_ap = s_want_ap;
        const webui_state_t state = s_state;

        /*
         * Network settings changed. Taken before everything else: bringing the interface down here
         * leaves state OFF, and the check below then brings it straight back up - this time
         * consulting the stored credentials. One transition expressed as two, rather than a second
         * path that could disagree with the first.
         */
        if (s_want_restart) {
            s_want_restart = false;
            if (state != WEBUI_OFF) {
                wifi_down();
                continue;
            }
        }

        /* Asked to stop. */
        if (!want_on && state != WEBUI_OFF) {
            wifi_down();
            continue;
        }

        /* Asked to start. */
        if (want_on && state == WEBUI_OFF) {
            attempt_started_us = esp_timer_get_time();
            wifi_up();
            continue;
        }

        /* Asked to force an access point while joined to a network, or the other way round. */
        if (want_on && want_ap && (state == WEBUI_STA_UP || state == WEBUI_STA_CONNECTING)) {
            sta_to_ap();
            continue;
        }

        if (state == WEBUI_STA_CONNECTING) {
            if (s_sta_got_ip) {
                s_state = WEBUI_STA_UP;
                webui_touch();
                /*
                 * The access point is not brought up alongside: the whole point of joining a
                 * network is that the panel sits at a stable address on it, and running both
                 * would keep a second, weaker network advertised for no reason. The AP hotkey is
                 * the way back.
                 */
                ESP_LOGI(TAG, "joined the network -> %s", s_url);
                if (webui_http_start(s_password) != ESP_OK) {
                    fail_and_stop("the HTTP server would not start", ESP_FAIL);
                }
            } else {
                const int64_t elapsed = esp_timer_get_time() - attempt_started_us;
                const bool timed_out =
                    elapsed > (int64_t)CONFIG_APP_WEBUI_STA_TIMEOUT_S * 1000000;
                if (timed_out || s_sta_retry >= STA_MAX_RETRY) {
                    sta_to_ap();
                }
            }
            continue;
        }

        /* Idle shutdown. Zero disables it, which is what somebody leaving the panel open on a
         * bench wants. */
        if (state != WEBUI_OFF && CONFIG_APP_WEBUI_IDLE_TIMEOUT_MIN > 0) {
            const int64_t idle_us = esp_timer_get_time() - s_last_touch_us;
            if (idle_us > (int64_t)CONFIG_APP_WEBUI_IDLE_TIMEOUT_MIN * 60 * 1000000) {
                ESP_LOGI(TAG, "no requests for %d min - shutting Wi-Fi down. The link still asks "
                              "for it, so press the hotkey again to override",
                         CONFIG_APP_WEBUI_IDLE_TIMEOUT_MIN);
                /* Suppression, not just want_on = false: see webui_request() for why the wire
                 * would otherwise bring it straight back up. */
                s_suppressed = true;
                s_want_on = false;
                wifi_down();
            }
        }
    }
}

esp_err_t webui_start(void)
{
    build_identity();
    refresh_sta_ssid();

    /*
     * Say so at boot if firmware updates are not actually possible, rather than letting the panel
     * discover it when somebody tries.
     *
     * This exact misconfiguration happened while writing the feature and is easy to repeat: a
     * value already present in a generated sdkconfig wins over a defaults file, so adding the
     * partition choice to a tree that had been built before changed nothing. The build succeeded
     * and OTA was silently absent. Remedy is in sdkconfig.defaults.s3pad.
     */
    if (esp_ota_get_next_update_partition(NULL) == NULL) {
        ESP_LOGW(TAG, "no spare OTA partition - firmware updates from the panel will FAIL. The "
                      "partition table has only one application slot; delete the variant's "
                      "generated sdkconfig so it picks up partitions.csv");
    }

    if (xTaskCreate(webui_task, "webui", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "panel ready but Wi-Fi is DOWN - press Ctrl+Alt+%c on the keyboard to bring it "
                  "up (Ctrl+Alt+%c forces an access point)",
             'A' + CONFIG_APP_WEBUI_HOTKEY_KEYCODE - 0x04,
             'A' + CONFIG_APP_WEBUI_AP_HOTKEY_KEYCODE - 0x04);
    return ESP_OK;
}
