/*
 * Runtime configuration store. See bridge_config.h for the reasoning; this file is the
 * mechanics.
 */

#include "bridge_config.h"

#include <string.h>

#include "esp_log.h"
#include "input_mapper.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_ACTIVE "active"

/*
 * Double buffered, published by pointer swap. The mapping task reads through s_active without
 * locking, so the struct it is reading must never be the one being written - hence two of them
 * and never writing the one currently published.
 *
 * volatile so the compiler reloads it rather than caching a stale pointer for the lifetime of
 * the task.
 */
static bridge_config_t s_slots[2];
static const bridge_config_t *volatile s_active;
static volatile uint32_t s_generation;
static uint8_t s_active_slot;

void bridge_config_defaults(bridge_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = BRIDGE_CONFIG_VERSION;
    strncpy(out->name, "default", BRIDGE_PROFILE_NAME_MAX - 1);

    /*
     * Straight from Kconfig, which is the point: a chip with an empty NVS behaves exactly as it
     * did before this module existed. CONFIG_APP_MOUSE_SCALE_DIV applies to both axes, because
     * that is what a single divisor meant.
     */
    out->mouse_div_x = CONFIG_APP_MOUSE_SCALE_DIV;
    out->mouse_div_y = CONFIG_APP_MOUSE_SCALE_DIV;

    /* 80 ms is the value tuned by hand at 100 Hz (AGENTS.md 4.22) and expressed in time rather
     * than ticks since 4.40, so it means the same thing at every report rate. */
    out->mouse_tau_ms = BRIDGE_MOUSE_TAU_DEFAULT;

    /* Off by default. It changes the feel and the right value depends on the game, so it is not
     * something to switch on behind the user's back. */
    out->mouse_anti_deadzone = 0;
    out->mouse_invert_y = 0;

    /* Linear. The mapper skips the curve step entirely at this value, so the default path is the
     * arithmetic the 997 Hz and 830 Hz figures were measured with. */
    out->mouse_curve = BRIDGE_CURVE_LINEAR;

    input_bind_t binds[INPUT_BIND_MAX];
    size_t n = input_mapper_default_binds(binds, INPUT_BIND_MAX);
    if (n > INPUT_BIND_MAX) {
        n = INPUT_BIND_MAX;
    }
    out->bind_count = (uint8_t)n;
    memcpy(out->binds, binds, n * sizeof(input_bind_t));
}

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

bool bridge_config_validate(bridge_config_t *cfg)
{
    bool ok = true;

    if (cfg->version != BRIDGE_CONFIG_VERSION) {
        cfg->version = BRIDGE_CONFIG_VERSION;
        ok = false;
    }

    /* A name that is not terminated would be read past its end by every consumer. */
    cfg->name[BRIDGE_PROFILE_NAME_MAX - 1] = '\0';

    /*
     * Names are reduced to a safe alphabet rather than merely length-checked. This string arrives
     * over the network, is stored, is put into a JSON response and is then rendered by the panel -
     * so quotes, backslashes, angle brackets and control characters each have somewhere they could
     * do harm. Letters, digits, space, dash and underscore name a profile perfectly well.
     *
     * The JSON writer escapes as well. Two defences for one property is right here: the cost is a
     * loop, and the failure mode if either is loosened on its own is a panel that runs whatever
     * somebody put in a profile name.
     */
    for (char *p = cfg->name; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        if (!safe) {
            *p = '_';
            ok = false;
        }
    }
    if (cfg->name[0] == '\0') {
        strncpy(cfg->name, "unnamed", BRIDGE_PROFILE_NAME_MAX - 1);
        ok = false;
    }

    uint16_t v;
    v = clamp_u16(cfg->mouse_div_x, BRIDGE_MOUSE_DIV_MIN, BRIDGE_MOUSE_DIV_MAX);
    if (v != cfg->mouse_div_x) {
        cfg->mouse_div_x = v;
        ok = false;
    }
    v = clamp_u16(cfg->mouse_div_y, BRIDGE_MOUSE_DIV_MIN, BRIDGE_MOUSE_DIV_MAX);
    if (v != cfg->mouse_div_y) {
        cfg->mouse_div_y = v;
        ok = false;
    }
    v = clamp_u16(cfg->mouse_tau_ms, BRIDGE_MOUSE_TAU_MIN, BRIDGE_MOUSE_TAU_MAX);
    if (v != cfg->mouse_tau_ms) {
        cfg->mouse_tau_ms = v;
        ok = false;
    }
    if (cfg->mouse_anti_deadzone > BRIDGE_ANTI_DZ_MAX) {
        cfg->mouse_anti_deadzone = BRIDGE_ANTI_DZ_MAX;
        ok = false;
    }
    cfg->mouse_invert_y = cfg->mouse_invert_y ? 1 : 0;

    v = clamp_u16(cfg->mouse_curve, BRIDGE_CURVE_MIN, BRIDGE_CURVE_MAX);
    if (v != cfg->mouse_curve) {
        cfg->mouse_curve = v;
        ok = false;
    }

    if (cfg->bind_count > INPUT_BIND_MAX) {
        cfg->bind_count = INPUT_BIND_MAX;
        ok = false;
    }

    /*
     * Repair rather than reject individual rows. An unknown action or source is dropped to
     * BIND_SRC_NONE / ACT_NONE, which the mapper ignores; that way one bad row in an imported
     * profile does not cost the other nineteen.
     */
    for (unsigned i = 0; i < cfg->bind_count; i++) {
        input_bind_t *b = &cfg->binds[i];
        if (b->action >= ACT_COUNT) {
            b->action = ACT_NONE;
            ok = false;
        }
        if (b->src != BIND_SRC_KEY && b->src != BIND_SRC_MOD && b->src != BIND_SRC_MOUSE_BTN) {
            b->src = BIND_SRC_NONE;
            b->action = ACT_NONE;
            ok = false;
        }
    }

    return ok;
}

esp_err_t bridge_config_apply(const bridge_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Write into the buffer that is NOT currently published. */
    bridge_config_t *spare = (s_active == &s_slots[0]) ? &s_slots[1] : &s_slots[0];
    memcpy(spare, cfg, sizeof(*spare));

    if (!bridge_config_validate(spare)) {
        ESP_LOGW(TAG, "configuration had out-of-range fields - clamped");
    }

    /*
     * The mapper owns its binding lookups, so hand the table over before publishing: if the
     * order were reversed there would be a window where the tunables are new and the bindings
     * are old. Both are visible in one report, so that window would be visible too.
     */
    input_mapper_set_binds(spare->binds, spare->bind_count);

    s_active = spare;
    s_generation++;
    return ESP_OK;
}

const bridge_config_t *bridge_config_get(void)
{
    return s_active;
}

uint32_t bridge_config_generation(void)
{
    return s_generation;
}

uint8_t bridge_config_active_slot(void)
{
    return s_active_slot;
}

/* 0xFF means "nothing pending". */
static volatile uint8_t s_wanted_slot = 0xFF;

void bridge_config_request_profile(uint8_t slot)
{
    if (slot < BRIDGE_PROFILE_COUNT) {
        s_wanted_slot = slot;
    }
}

bool bridge_config_service(void)
{
    const uint8_t want = s_wanted_slot;
    if (want == 0xFF || want == s_active_slot) {
        return false;
    }

    /*
     * Cleared before the attempt, not after, so a slot that cannot be loaded is not retried on every
     * tick. The request repeats on the wire anyway - the input chip resends it with each keepalive -
     * but that is four times a second rather than a thousand.
     */
    s_wanted_slot = 0xFF;

    if (bridge_config_load(want) != ESP_OK) {
        ESP_LOGW(TAG, "profile %u requested but that slot is empty - keeping profile %u", want,
                 s_active_slot);
        return false;
    }
    return true;
}

static void slot_key(uint8_t slot, char out[8])
{
    out[0] = 'p';
    out[1] = (char)('0' + (slot % 10));
    out[2] = '\0';
}

static esp_err_t slot_read(uint8_t slot, bridge_config_t *out)
{
    if (slot >= BRIDGE_PROFILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[8];
    slot_key(slot, key);
    size_t len = sizeof(*out);
    err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }
    /*
     * A blob of the wrong size is a layout change, not a corrupt read. Treat it the same as
     * absent: the version field cannot be trusted if the struct it sits in is a different
     * shape.
     */
    if (len != sizeof(*out) || out->version != BRIDGE_CONFIG_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

bool bridge_config_slot_name(uint8_t slot, char out[BRIDGE_PROFILE_NAME_MAX])
{
    bridge_config_t tmp;
    if (slot_read(slot, &tmp) != ESP_OK) {
        return false;
    }
    tmp.name[BRIDGE_PROFILE_NAME_MAX - 1] = '\0';
    memcpy(out, tmp.name, BRIDGE_PROFILE_NAME_MAX);
    return true;
}

esp_err_t bridge_config_save(uint8_t slot)
{
    if (slot >= BRIDGE_PROFILE_COUNT || !s_active) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[8];
    slot_key(slot, key);
    err = nvs_set_blob(h, key, s_active, sizeof(*s_active));
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_ACTIVE, slot);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        s_active_slot = slot;
        ESP_LOGI(TAG, "saved profile %u '%s'", slot, s_active->name);
    } else {
        ESP_LOGE(TAG, "saving profile %u failed: %s", slot, esp_err_to_name(err));
    }
    return err;
}

esp_err_t bridge_config_load(uint8_t slot)
{
    bridge_config_t cfg;
    esp_err_t err = slot_read(slot, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = bridge_config_apply(&cfg);
    if (err == ESP_OK) {
        s_active_slot = slot;
        ESP_LOGI(TAG, "loaded profile %u '%s'", slot, cfg.name);
    }
    return err;
}

esp_err_t bridge_config_erase(uint8_t slot)
{
    if (slot >= BRIDGE_PROFILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[8];
    slot_key(slot, key);
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t bridge_config_init(void)
{
    /* Publish the defaults first, unconditionally. Everything after this point can fail without
     * leaving the bridge without a configuration. */
    bridge_config_t cfg;
    bridge_config_defaults(&cfg);
    esp_err_t err = bridge_config_apply(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored configuration - using built-in defaults "
                      "(mouse divisor %d, tau %d ms)",
                 CONFIG_APP_MOUSE_SCALE_DIV, BRIDGE_MOUSE_TAU_DEFAULT);
        return ESP_OK;
    }

    uint8_t slot = 0;
    if (nvs_get_u8(h, NVS_KEY_ACTIVE, &slot) != ESP_OK) {
        slot = 0;
    }
    nvs_close(h);

    if (bridge_config_load(slot) != ESP_OK) {
        ESP_LOGI(TAG, "profile %u is absent or from another layout - using built-in defaults",
                 slot);
        return ESP_OK;
    }

    const bridge_config_t *a = bridge_config_get();
    ESP_LOGI(TAG,
             "profile %u '%s': mouse div %u/%u, tau %u ms, anti-deadzone %u%%, curve %u.%02u, "
             "%u bindings",
             slot, a->name, a->mouse_div_x, a->mouse_div_y, a->mouse_tau_ms,
             a->mouse_anti_deadzone, a->mouse_curve / 100, a->mouse_curve % 100, a->bind_count);
    return ESP_OK;
}
