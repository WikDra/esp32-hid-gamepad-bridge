/*
 * Ties the central and peripheral roles together: reads state from ble_hid_host and
 * sends reports through ble_gamepad. Requires both roles enabled in Kconfig.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "gamepad_state.h"
#include "input_map.h"
#include "input_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the mapping task. Call after ble_hid_host_start() and ble_gamepad_start(). */
esp_err_t input_mapper_start(void);

/*
 * Replaces the binding table. Safe to call while the mapping task is running: the derived
 * lookups are built into a spare buffer and switched in with one pointer store, so no report is
 * ever produced from a half-applied table.
 *
 * Anything beyond INPUT_BIND_MAX rows is dropped rather than rejected - the caller is a
 * configuration layer that should have validated first, and silently doing less is better here
 * than leaving the previous table in force while reporting success.
 */
void input_mapper_set_binds(const input_bind_t *binds, size_t count);

/* The table currently in force. The pointer stays valid; the contents change on a set. */
const input_bind_t *input_mapper_binds(size_t *count);

/*
 * The compiled-in default table - the mapping the README documents. Returns the number of rows
 * it has, which may exceed max; only min(max, that) rows are written.
 */
size_t input_mapper_default_binds(input_bind_t *out, size_t max);

/*
 * What the mapper last produced, and the input state it produced it from. Either pointer may be
 * NULL. Non-destructive, unlike input_state_take(), which clears the mouse accumulators - reading
 * that from an HTTP handler would steal motion from the pad.
 */
void input_mapper_snapshot(gamepad_state_t *pad, hid_input_state_t *in);

/*
 * Reports actually sent since boot. Monotonic, and only incremented when a report went out - so
 * two samples and the time between them give the rate the host really sees, measured from this
 * end instead of with scripts/xinput_rumble.py from the other.
 */
uint32_t input_mapper_reports_sent(void);

#ifdef __cplusplus
}
#endif
