/*
 * Wi-Fi configuration panel: lifecycle and network side.
 *
 * WI-FI IS OFF UNTIL ASKED FOR, and that is the whole design, not a power-saving afterthought.
 * This chip runs a 1 kHz mapping task feeding a USB endpoint polled every millisecond, and the
 * measured 997 Hz ceiling was taken with the radio silent. Bringing Wi-Fi up permanently would
 * put an unquantified cost under every future measurement; bringing it up only while somebody is
 * configuring keeps the number meaningful and makes the feature free the rest of the time.
 *
 * The request arrives over the inter-chip link, because the hotkey is on the keyboard and the
 * keyboard is on the other chip.
 *
 * NETWORK MODES. With credentials stored the bridge joins that network and drops its own access
 * point, which is what makes the panel reachable at a stable address from a phone already on
 * your Wi-Fi. Without them, or when the join fails, or when the AP hotkey forces it, it serves
 * its own access point instead. There is always something to connect to - a configuration panel
 * you cannot reach is worse than none, because you would have to reach for a serial adapter to
 * find out why.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the panel is currently doing. Reported in the heartbeat so the console says where to
 * point a browser without anyone having to guess. */
typedef enum {
    WEBUI_OFF = 0,
    WEBUI_STA_CONNECTING,
    WEBUI_STA_UP,
    WEBUI_AP_UP,
} webui_state_t;

/*
 * THE PANEL RUNS ON THE CHIP THAT OWNS THE PAD, because that is where the mapper and the
 * configuration it reads already live. On the input chip this header compiles to nothing but
 * no-ops, so callers do not need to know: every call site below is UNCONDITIONAL.
 *
 * That is deliberate and it is the lesson of AGENTS.md 4.38. When a call was wrapped in a
 * condition that had to mirror a CMake condition, the two drifted, the mapper was compiled and
 * never started, and the build reported success. An empty inline cannot drift.
 */
#if CONFIG_APP_WEBUI && CONFIG_APP_USB_PAD

/* Starts the control task. Wi-Fi itself stays down until webui_request() asks for it. */
esp_err_t webui_start(void);

/*
 * Desired state, as absolute values rather than a toggle.
 *
 * Called for every mode frame that arrives - which is every keepalive, four times a second - so
 * it is idempotent and cheap: it records what is wanted and lets the control task notice that it
 * differs from what is already in force.
 */
void webui_request(bool on, bool force_ap);

/* Resets the idle timer. Called by the HTTP layer on every request, so an open panel keeps
 * itself alive and a forgotten one shuts down. */
void webui_touch(void);

webui_state_t webui_state(void);

/* Human-readable address to point a browser at, or an empty string when Wi-Fi is down. */
const char *webui_url(void);

#else /* the panel is not built on this chip */

static inline esp_err_t webui_start(void)
{
    return ESP_OK;
}
static inline void webui_request(bool on, bool force_ap)
{
    (void)on;
    (void)force_ap;
}
static inline void webui_touch(void)
{
}
static inline webui_state_t webui_state(void)
{
    return WEBUI_OFF;
}
static inline const char *webui_url(void)
{
    return "";
}

#endif

#ifdef __cplusplus
}
#endif
