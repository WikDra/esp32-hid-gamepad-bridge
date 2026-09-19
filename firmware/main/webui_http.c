/*
 * HTTP side of the configuration panel: one page and a small API.
 *
 * SERVED AS A SINGLE EMBEDDED FILE, uncompressed. Three reasons, in order of how much they
 * matter: one request cannot leave the page and its script out of step with each other; there is
 * no filesystem partition to corrupt or to keep in sync with the firmware version, so an update
 * replaces the panel and the code it talks to together; and gzipping would need a build-time tool
 * that has to exist on every machine that builds this. The page costs a fraction of the free space
 * in the application partition and is fetched once over a link doing nothing else.
 *
 * NO JSON PARSER ON THE DEVICE, and that is a deliberate choice rather than a shortcut. cJSON left
 * ESP-IDF in 6.x, so using it would mean a downloaded dependency - but the better argument is that
 * we only ever need to PRODUCE JSON. Requests arrive form-encoded, which esp_http_server already
 * splits, and the one place a real parser would be needed - importing a profile file - is done in
 * the browser, where JSON.parse is free and cannot corrupt anything on this side. The result is a
 * parsing surface of two small functions instead of a general parser reachable from the network.
 *
 * AUTHENTICATION IS HTTP BASIC, and it is not decoration. This API changes what the gamepad
 * reports and accepts firmware images, so without it anyone in range could move your sticks
 * mid-game or replace the firmware. Basic is chosen because the browser implements it, it works
 * for the OTA upload with no JavaScript, and it has no session state to get wrong.
 *
 * What it does NOT do is encrypt: the credentials are base64, not ciphertext. On the access point
 * WPA2 covers the air. Joined to somebody's network, the traffic is in the clear on that network,
 * and that is a real limitation rather than one to paper over - see README.
 */

#include "webui_http.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_config.h"
#include "chip_link.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_mapper.h"
#include "input_state.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "usb_pad.h"
#include "webui.h"

static const char *TAG = "webui-http";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"

/*
 * Body limit for the form endpoints. A full configuration with 40 bindings encodes to about 500
 * bytes; 2 kB leaves room without letting a request decide how much to allocate.
 */
#define BODY_MAX 2048

/* Response buffer. The largest response is a configuration with 40 bindings, around 1.4 kB. */
#define JSON_MAX 3072

/* OTA receive chunk: big enough not to thrash, small enough to be a predictable allocation. */
#define OTA_CHUNK 4096

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;
static char s_expected_auth[128];

/* --------------------------------------------------------- JSON production */

/*
 * Append-with-truncation writer. Every add() checks the remaining room, so a response that would
 * not fit is cut short rather than overrunning - and because the only strings that ever reach it
 * are sanitised names and fixed literals, a cut response is a bug in a size constant, not
 * something a request can provoke.
 */
typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool overflow;
} jw_t;

static void jw_raw(jw_t *w, const char *s)
{
    const size_t n = strlen(s);
    if (w->len + n + 1 > w->size) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_fmt(jw_t *w, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    jw_raw(w, tmp);
}

/*
 * Strings are emitted with escaping anyway, even though bridge_config_validate() already reduces
 * names to a safe alphabet. Two defences for one property, because the failure mode if the
 * sanitiser is ever loosened is a panel that executes whatever is in a profile name.
 */
static void jw_str(jw_t *w, const char *s)
{
    jw_raw(w, "\"");
    char esc[2] = {0, 0};
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            jw_raw(w, esc);
            esc[0] = (char)c;
            jw_raw(w, esc);
        } else if (c < 0x20 || c > 0x7e) {
            jw_fmt(w, "\\u%04x", c);
        } else {
            esc[0] = (char)c;
            jw_raw(w, esc);
        }
    }
    jw_raw(w, "\"");
}

/* ------------------------------------------------------------------ helpers */

static esp_err_t send_jw(httpd_req_t *req, jw_t *w)
{
    if (w->overflow) {
        ESP_LOGE(TAG, "response did not fit in %u B - raise JSON_MAX", (unsigned)w->size);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    /* The panel reads live state several times a second; a cached response would freeze it. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, w->buf, w->len);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[192];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, body);
}

/*
 * Every handler calls this first. Returning false means the response has already been sent, so the
 * handler must return ESP_OK and do nothing else.
 */
static bool authorised(httpd_req_t *req)
{
    webui_touch(); /* any request, authorised or not, means somebody is there */

    char header[160];
    const esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header));
    if (err == ESP_OK && strcmp(header, s_expected_auth) == 0) {
        return true;
    }

    /* WWW-Authenticate makes the browser prompt, which is the whole reason for choosing Basic: no
     * login page, no token handling, and it works for a file upload too. */
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"hid-bridge\"");
    httpd_resp_send(req, NULL, 0);
    return false;
}

/* Reads a request body into buf, NUL-terminated. Returns false on failure, response already
 * sent. */
static bool read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len == 0 || req->content_len >= buf_size) {
        send_err(req, "400 Bad Request", "body missing or too large");
        return false;
    }

    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            send_err(req, "400 Bad Request", "could not read the body");
            return false;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return true;
}

/*
 * Percent-decoding, in place.
 *
 * httpd_query_key_value() splits on '&' and '=' but does NOT decode, so without this a Wi-Fi
 * password containing a space, a '%' or an '&' would be stored wrong - and the symptom would be a
 * network the bridge silently cannot join, which is a miserable thing to debug over a link that
 * only exists while the bridge is reachable.
 */
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            const char hex[3] = {s[1], s[2], 0};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

/*
 * Looks up a form key and decodes it. Returns false when absent, which callers treat as "leave
 * unchanged".
 *
 * THE SCRATCH BUFFER HOLDS THE ENCODED FORM, WHICH IS LONGER THAN THE DECODED ONE, and getting
 * this wrong cost two silent bugs in a row - both found on hardware, neither visible to the
 * compiler:
 *
 *   1. Reading straight into the caller's buffer. httpd_query_key_value() copies the value STILL
 *      PERCENT-ENCODED and fails when it does not fit, and every byte can become three characters
 *      ("%20"). So any value needing escapes was rejected: the name "abc def" (9 encoded) was
 *      stored while "aa bb cc dd ee" (22 encoded) vanished, and the panel allows 15 characters.
 *
 *   2. Guarding with out_size * 3 >= sizeof(scratch). That looks careful and silently disabled the
 *      largest field: the binding table's destination is 560 bytes, so the guard rejected it
 *      outright and the mapping editor did nothing at all.
 *
 * The scratch only has to fit what a client can legitimately send. The largest field is the binding
 * table: 40 rows of "255:255:255," where encodeURIComponent turns ':' into "%3A" and ',' into
 * "%2C", so 40 x 18 = 720 characters. 1024 leaves room; anything beyond it is refused OUT LOUD.
 */
#define FORM_VALUE_MAX 1024

static bool form_str(const char *body, const char *key, char *out, size_t out_size)
{
    if (out_size == 0) {
        return false;
    }

    char enc[FORM_VALUE_MAX];
    const esp_err_t err = httpd_query_key_value(body, key, enc, sizeof(enc));
    if (err == ESP_ERR_NOT_FOUND) {
        return false; /* absent: the caller keeps whatever it had */
    }
    if (err != ESP_OK) {
        /*
         * Logged rather than ignored. Both bugs above were invisible precisely because a rejected
         * field is indistinguishable from an absent one, and the symptom - "the panel ignores that
         * box" - points nowhere near the cause.
         */
        ESP_LOGW(TAG, "form field '%s' does not fit in %u B - ignored", key, (unsigned)sizeof(enc));
        return false;
    }

    url_decode(enc);
    snprintf(out, out_size, "%s", enc);
    return true;
}

static int form_int(const char *body, const char *key, int fallback)
{
    char value[16];
    if (!form_str(body, key, value, sizeof(value))) {
        return fallback;
    }
    return atoi(value);
}

static uint8_t slot_from_query(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return 0xFF;
    }
    char value[8];
    if (httpd_query_key_value(query, "slot", value, sizeof(value)) != ESP_OK) {
        return 0xFF;
    }
    const int slot = atoi(value);
    return (slot >= 0 && slot < BRIDGE_PROFILE_COUNT) ? (uint8_t)slot : 0xFF;
}

/* ------------------------------------------------------------- config codec */

static void write_config(jw_t *w, const bridge_config_t *cfg)
{
    jw_raw(w, "{\"name\":");
    jw_str(w, cfg->name);
    jw_fmt(w, ",\"div_x\":%u,\"div_y\":%u,\"tau_ms\":%u,\"anti_dz\":%u,\"invert_y\":%u,\"curve\":%u",
           cfg->mouse_div_x, cfg->mouse_div_y, cfg->mouse_tau_ms, cfg->mouse_anti_deadzone,
           cfg->mouse_invert_y, cfg->mouse_curve);
    jw_raw(w, ",\"binds\":[");
    for (unsigned i = 0; i < cfg->bind_count; i++) {
        jw_fmt(w, "%s{\"src\":%u,\"code\":%u,\"action\":%u}", i ? "," : "", cfg->binds[i].src,
               cfg->binds[i].code, cfg->binds[i].action);
    }
    jw_raw(w, "]}");
}

/*
 * Bindings arrive as "src:code:action,src:code:action,..." - one field, no nesting, and parsed
 * with strtol. Rows that do not have three numbers are skipped rather than failing the request:
 * bridge_config_validate() has the final word on every value anyway, and losing one malformed row
 * is better than losing the other nineteen.
 *
 * Returns the number of rows parsed.
 */
static uint8_t parse_binds(const char *s, input_bind_t *out, uint8_t max)
{
    uint8_t n = 0;
    while (*s && n < max) {
        char *end = NULL;
        const long src = strtol(s, &end, 10);
        if (end == s || *end != ':') {
            break;
        }
        s = end + 1;
        const long code = strtol(s, &end, 10);
        if (end == s || *end != ':') {
            break;
        }
        s = end + 1;
        const long action = strtol(s, &end, 10);
        if (end == s) {
            break;
        }
        s = end;

        out[n].src = (uint8_t)src;
        out[n].code = (uint8_t)code;
        out[n].action = (uint8_t)action;
        n++;

        if (*s == ',') {
            s++;
        } else {
            break;
        }
    }
    return n;
}

/*
 * Amends cfg from a form body. Starts from what is already in force rather than from zero, so a
 * request carrying only the sliders does not silently wipe the binding table.
 */
static void config_from_form(const char *body, bridge_config_t *cfg)
{
    char name[BRIDGE_PROFILE_NAME_MAX];
    if (form_str(body, "name", name, sizeof(name))) {
        snprintf(cfg->name, sizeof(cfg->name), "%s", name);
    }

    cfg->mouse_div_x = (uint16_t)form_int(body, "div_x", cfg->mouse_div_x);
    cfg->mouse_div_y = (uint16_t)form_int(body, "div_y", cfg->mouse_div_y);
    cfg->mouse_tau_ms = (uint16_t)form_int(body, "tau_ms", cfg->mouse_tau_ms);
    cfg->mouse_anti_deadzone = (uint8_t)form_int(body, "anti_dz", cfg->mouse_anti_deadzone);
    cfg->mouse_invert_y = form_int(body, "invert_y", cfg->mouse_invert_y) ? 1 : 0;
    cfg->mouse_curve = (uint16_t)form_int(body, "curve", cfg->mouse_curve);

    /* Decoded table: 40 rows of "255:255:255," is 480 characters plus a terminator. */
    char binds[520];
    if (form_str(body, "binds", binds, sizeof(binds))) {
        cfg->bind_count = parse_binds(binds, cfg->binds, INPUT_BIND_MAX);
    }
}

/* ------------------------------------------------------------------ handlers */

static esp_err_t h_index(httpd_req_t *req)
{
    /* The page itself is not behind the password: it holds no secrets and every endpoint it calls
     * is protected. Prompting before the page can explain itself would be worse. */
    webui_touch();
    /* Charset stated in the HEADER as well as in the document's meta tag: the page contains
     * typographic dashes and quotes, and leaving the encoding to a browser default is how those
     * turn into mojibake on somebody else's machine. */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t h_get_config(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }
    char buf[JSON_MAX];
    jw_t w = {.buf = buf, .size = sizeof(buf)};
    write_config(&w, bridge_config_get());
    return send_jw(req, &w);
}

static esp_err_t h_post_config(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }

    char body[BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    /* A copy of what is in force, amended by the request. Applying is live only - saving is a
     * separate call, so dragging a slider does not erase a flash page per pixel. */
    bridge_config_t cfg = *bridge_config_get();
    config_from_form(body, &cfg);

    if (bridge_config_apply(&cfg) != ESP_OK) {
        return send_err(req, "500 Internal Server Error", "could not apply");
    }
    return send_ok(req);
}

static esp_err_t h_defaults(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }
    bridge_config_t cfg;
    bridge_config_defaults(&cfg);
    if (bridge_config_apply(&cfg) != ESP_OK) {
        return send_err(req, "500 Internal Server Error", "could not apply");
    }
    ESP_LOGI(TAG, "configuration reset to built-in defaults");
    return send_ok(req);
}

static esp_err_t h_profiles(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }

    char buf[JSON_MAX];
    jw_t w = {.buf = buf, .size = sizeof(buf)};
    jw_fmt(&w, "{\"active\":%u,\"slots\":[", bridge_config_active_slot());
    for (uint8_t i = 0; i < BRIDGE_PROFILE_COUNT; i++) {
        char name[BRIDGE_PROFILE_NAME_MAX];
        const bool stored = bridge_config_slot_name(i, name);
        jw_fmt(&w, "%s{\"slot\":%u,\"stored\":%s,\"name\":", i ? "," : "", i,
               stored ? "true" : "false");
        jw_str(&w, stored ? name : "");
        jw_raw(&w, "}");
    }
    jw_raw(&w, "]}");
    return send_jw(req, &w);
}

static esp_err_t h_save(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }
    const uint8_t slot = slot_from_query(req);
    if (slot == 0xFF) {
        return send_err(req, "400 Bad Request", "slot must be 0..3");
    }
    if (bridge_config_save(slot) != ESP_OK) {
        return send_err(req, "500 Internal Server Error", "could not write NVS");
    }
    return send_ok(req);
}

static esp_err_t h_load(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }
    const uint8_t slot = slot_from_query(req);
    if (slot == 0xFF) {
        return send_err(req, "400 Bad Request", "slot must be 0..3");
    }
    if (bridge_config_load(slot) != ESP_OK) {
        return send_err(req, "404 Not Found", "that slot is empty");
    }
    return send_ok(req);
}

static esp_err_t h_erase(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }
    const uint8_t slot = slot_from_query(req);
    if (slot == 0xFF) {
        return send_err(req, "400 Bad Request", "slot must be 0..3");
    }
    bridge_config_erase(slot);
    return send_ok(req);
}

static esp_err_t h_export(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }
    char buf[JSON_MAX];
    jw_t w = {.buf = buf, .size = sizeof(buf)};
    write_config(&w, bridge_config_get());
    /* Content-Disposition so the browser saves a file instead of rendering it. */
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"hid-bridge-profile.json\"");
    return send_jw(req, &w);
}

static esp_err_t h_state(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }

    gamepad_state_t pad;
    hid_input_state_t in;
    bool passthrough = false;
    input_mapper_snapshot(&pad, &in, &passthrough);

    char buf[JSON_MAX];
    jw_t w = {.buf = buf, .size = sizeof(buf)};

    jw_fmt(&w, "{\"pad\":{\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"buttons\":%u,\"dpad\":%u}",
           pad.lx, pad.ly, pad.rx, pad.ry, pad.buttons, pad.dpad);

    jw_fmt(&w, ",\"in\":{\"mods\":%u,\"mbtn\":%u,\"kbd\":%s,\"mouse\":%s,\"keys\":[",
           in.modifiers, in.mouse_buttons, in.keyboard_connected ? "true" : "false",
           in.mouse_connected ? "true" : "false");
    for (int k = 0; k < HID_KEYS_MAX; k++) {
        jw_fmt(&w, "%s%u", k ? "," : "", in.keys[k]);
    }
    jw_raw(&w, "]}");

    const webui_state_t net = webui_state();
    const esp_app_desc_t *app = esp_app_get_description();

    jw_fmt(&w, ",\"sys\":{\"heap\":%u,\"heap_min\":%u,\"uptime_ms\":%lld",
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
           esp_timer_get_time() / 1000);
    /*
     * TWO counters, two meanings, and keeping them apart matters. "ticks" is how often the mapping
     * task ran - operator-independent, so it shows whether Wi-Fi or a polling browser is stealing
     * time from the 1 kHz loop. "reports" is how many actually went on the wire, which only
     * changes when the pad state does and therefore depends on the hand on the mouse.
     *
     * Reported as monotonic counts rather than rates: the panel samples twice and divides, so the
     * window belongs to whoever is measuring instead of being one we guessed at.
     */
    jw_fmt(&w, ",\"ticks\":%u,\"reports\":%u,\"pad_ready\":%s,\"slot\":%u,\"rate_hz\":%d",
           (unsigned)input_mapper_ticks(), (unsigned)usb_pad_reports_sent(),
           usb_pad_is_ready() ? "true" : "false", bridge_config_active_slot(),
           CONFIG_APP_REPORT_RATE_HZ);
    /* Without this the panel cannot tell "passthrough is on, so there is no pad" from "the pad has
     * stopped working" - both look like pad_ready=false. */
    jw_fmt(&w, ",\"passthrough\":%s", passthrough ? "true" : "false");
    jw_raw(&w, ",\"net\":");
    jw_str(&w, net == WEBUI_STA_UP           ? "station"
               : net == WEBUI_AP_UP          ? "ap"
               : net == WEBUI_STA_CONNECTING ? "joining"
                                             : "off");
    jw_raw(&w, ",\"url\":");
    jw_str(&w, webui_url());
    /* The stored network name, so the panel can show what it will try to join. The password is not
     * exposed: it has no use to the panel and every reason not to leave the device. */
    jw_raw(&w, ",\"sta_ssid\":");
    jw_str(&w, webui_sta_ssid());
    jw_raw(&w, ",\"version\":");
    jw_str(&w, app->version);
    jw_raw(&w, ",\"built\":");
    jw_str(&w, app->date);
    jw_raw(&w, ",\"idf\":");
    jw_str(&w, app->idf_ver);
    jw_raw(&w, "}}");

    return send_jw(req, &w);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }

    char body[BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    const bool have_ssid = form_str(body, "ssid", ssid, sizeof(ssid)) && ssid[0];
    form_str(body, "pass", pass, sizeof(pass));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return send_err(req, "500 Internal Server Error", "could not open NVS");
    }

    esp_err_t err;
    if (have_ssid) {
        err = nvs_set_str(h, NVS_KEY_STA_SSID, ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, NVS_KEY_STA_PASS, pass);
        }
        ESP_LOGI(TAG, "stored credentials for '%s' - in effect the next time Wi-Fi starts", ssid);
    } else {
        /* An empty SSID means "forget the network", which is how to get back to always serving an
         * access point without needing the hotkey. */
        nvs_erase_key(h, NVS_KEY_STA_SSID);
        nvs_erase_key(h, NVS_KEY_STA_PASS);
        err = ESP_OK;
        ESP_LOGI(TAG, "stored network forgotten - the access point will be served from now on");
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        return send_err(req, "500 Internal Server Error", "could not write NVS");
    }

    /*
     * Apply them now rather than at the next hotkey. Credentials are only consulted when the
     * interface comes up, so without this "I typed my password and it did not join" was the
     * correct description of what happened. The restart is deferred to the control task, so this
     * response still reaches the browser.
     */
    webui_network_changed();
    return send_ok(req);
}

/*
 * Firmware update. A raw POST of the .bin, written straight into the inactive OTA slot.
 *
 * The image is NOT marked valid here. The bootloader starts it as pending and app_main confirms it
 * only after the firmware has stayed up for half a minute, so an image that crashes or boot-loops
 * is rolled back with nobody touching the board. Confirming at the end of the upload would only
 * prove that the transfer finished.
 */
static esp_err_t h_ota(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return send_err(req, "500 Internal Server Error",
                        "no spare OTA partition in this build");
    }
    if (req->content_len == 0) {
        return send_err(req, "400 Bad Request", "empty image");
    }

    ESP_LOGW(TAG, "OTA: receiving %d B into '%s'", req->content_len, target->label);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(target, req->content_len, &ota);
    if (err != ESP_OK) {
        return send_err(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    char *chunk = malloc(OTA_CHUNK);
    if (!chunk) {
        esp_ota_abort(ota);
        return send_err(req, "500 Internal Server Error", "out of memory");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        const int want = (remaining < OTA_CHUNK) ? remaining : OTA_CHUNK;
        const int n = httpd_req_recv(req, chunk, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, chunk, (size_t)n);
        if (err != ESP_OK) {
            break;
        }
        remaining -= n;
        webui_touch(); /* a long upload must not trip the idle timeout */
    }
    free(chunk);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "OTA failed while receiving: %s", esp_err_to_name(err));
        return send_err(req, "500 Internal Server Error", "transfer failed - nothing was changed");
    }

    /* esp_ota_end() is where the image is verified: a truncated or corrupt one fails here and the
     * running firmware is untouched. */
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image rejected: %s", esp_err_to_name(err));
        return send_err(req, "400 Bad Request", "the image did not verify");
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return send_err(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "OTA written to '%s' - rebooting into it", target->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    /* Let the response reach the browser before the reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* not reached */
}

#if CHIP_LINK_HAS_FW_PUSH
/*
 * Firmware update for the OTHER chip, relayed over the inter-chip link.
 *
 * The input chip has no network of its own: its USB port is the host side and its console needs the
 * USB-UART adapter, so a firmware change there used to mean holding BOOT and RESET on the board.
 * The cable was already crossed on both pairs, so the reverse direction was free.
 *
 * Streamed rather than buffered - a 300 kB image would not fit in RAM alongside Wi-Fi, and it does
 * not need to. Backpressure comes from the link acknowledging each window, so reading from the socket
 * only as fast as the wire drains is automatic.
 */
static esp_err_t h_ota_peer(httpd_req_t *req)
{
    if (!authorised(req)) {
        return ESP_OK;
    }
    if (req->content_len == 0) {
        return send_err(req, "400 Bad Request", "empty image");
    }
    if (!chip_link_peer_alive()) {
        return send_err(req, "503 Service Unavailable",
                        "the other chip is not answering on the link");
    }

    ESP_LOGW(TAG, "peer OTA: relaying %d B over the link", req->content_len);

    esp_err_t err = chip_link_fw_begin((uint32_t)req->content_len);
    if (err != ESP_OK) {
        return send_err(req, "502 Bad Gateway", "the other chip refused to start");
    }

    char *chunk = malloc(OTA_CHUNK);
    if (!chunk) {
        return send_err(req, "500 Internal Server Error", "out of memory");
    }

    int remaining = req->content_len;
    while (remaining > 0 && err == ESP_OK) {
        const int want = (remaining < OTA_CHUNK) ? remaining : OTA_CHUNK;
        const int n = httpd_req_recv(req, chunk, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = chip_link_fw_data((const uint8_t *)chunk, (size_t)n);
        remaining -= n;
        webui_touch(); /* a multi-second relay must not trip the idle timeout */
    }
    free(chunk);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "peer OTA failed mid-transfer: %s", esp_err_to_name(err));
        return send_err(req, "502 Bad Gateway", "transfer failed - the other chip is unchanged");
    }

    err = chip_link_fw_end();
    if (err != ESP_OK) {
        return send_err(req, "400 Bad Request", "the other chip rejected the image");
    }

    ESP_LOGW(TAG, "peer OTA accepted - the other chip is rebooting into it");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"peer_rebooting\":true}");
}
#endif /* CHIP_LINK_HAS_FW_PUSH */

/* ---------------------------------------------------------------- lifecycle */
static void build_auth(const char *password)
{
    /*
     * Sizes are chosen so the WORST case fits, not the typical one: a 32-character password gives
     * "admin:" + 32 = 38 bytes of plaintext, which is 52 bytes of base64, and "Basic " + 52 = 58.
     * The compiler reasons about the worst case too, and rightly refused a combination where it
     * could not prove that.
     */
    char plain[80];
    snprintf(plain, sizeof(plain), "admin:%s", password);

    unsigned char b64[96];
    size_t written = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &written, (const unsigned char *)plain,
                              strlen(plain)) != 0) {
        /* Cannot happen at these sizes, but an unset expectation must never be one that a missing
         * or empty header could match. */
        snprintf(s_expected_auth, sizeof(s_expected_auth), "Basic \x01");
        return;
    }
    b64[written] = '\0';
    snprintf(s_expected_auth, sizeof(s_expected_auth), "Basic %s", (const char *)b64);
}

esp_err_t webui_http_start(const char *password)
{
    if (s_server) {
        return ESP_OK; /* idempotent: called again when STA replaces AP */
    }

    build_auth(password);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Handlers keep a JSON_MAX buffer on the stack, so the default 4 kB is not enough. */
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    /* A phone that walks out of range leaves a socket behind; without these the server runs out of
     * them and stops answering, which from outside is indistinguishable from a crash. */
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    /* Below the mapper's priority (5), deliberately: a browser polling the state page must never
     * delay a pad report. */
    cfg.task_priority = 3;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/state", .method = HTTP_GET, .handler = h_state},
        {.uri = "/api/config", .method = HTTP_GET, .handler = h_get_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_post_config},
        {.uri = "/api/defaults", .method = HTTP_POST, .handler = h_defaults},
        {.uri = "/api/profiles", .method = HTTP_GET, .handler = h_profiles},
        {.uri = "/api/save", .method = HTTP_POST, .handler = h_save},
        {.uri = "/api/load", .method = HTTP_POST, .handler = h_load},
        {.uri = "/api/erase", .method = HTTP_POST, .handler = h_erase},
        {.uri = "/api/export", .method = HTTP_GET, .handler = h_export},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
#if CHIP_LINK_HAS_FW_PUSH
        {.uri = "/api/ota/peer", .method = HTTP_POST, .handler = h_ota_peer},
#endif
    };

    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", routes[i].uri, esp_err_to_name(err));
            webui_http_stop();
            return err;
        }
    }

    ESP_LOGI(TAG, "panel served on port 80, user 'admin'");
    return ESP_OK;
}

void webui_http_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

bool webui_http_running(void)
{
    return s_server != NULL;
}
