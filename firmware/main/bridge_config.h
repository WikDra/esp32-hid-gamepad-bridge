/*
 * Runtime configuration: the tunables that used to be compile-time constants, plus the binding
 * table, stored as named profiles in NVS.
 *
 * WHY THIS EXISTS AT ALL: changing mouse sensitivity meant editing Kconfig, rebuilding and
 * reflashing. The values worth tuning are exactly the ones you can only judge by feel, so that
 * loop was the wrong shape - you cannot A/B a divisor you have to reflash.
 *
 * RELATIONSHIP TO KCONFIG. The Kconfig options are not replaced, they become the DEFAULTS. With
 * an empty NVS - a fresh chip, or a build that never writes one - the active configuration is
 * bit-for-bit what the Kconfig values describe, so behaviour is unchanged. There is one code
 * path through the tunables, not one for "configured" and one for "not configured": two
 * descriptions of the same thing is the shape of the bug that kept the mapper from starting at
 * all (AGENTS.md 4.38).
 *
 * THREADING. The mapping task reads the active configuration up to 1000 times per second and
 * never locks. Writers build a complete struct, validate it, and publish it by swapping a
 * pointer, so a reader sees either the whole old configuration or the whole new one. A
 * generation counter lets the reader notice a change and recompute its derived values without
 * doing that work every tick.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "input_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bumped whenever the struct layout changes. A stored blob with a different version is IGNORED
 * rather than migrated or partially read: a configuration that decides how a gamepad behaves is
 * not worth guessing at, and falling back to the documented defaults is always safe.
 *
 * 2: added mouse_curve.
 */
#define BRIDGE_CONFIG_VERSION 2

#define BRIDGE_PROFILE_COUNT    4
#define BRIDGE_PROFILE_NAME_MAX 16

/* Limits. Enforced by bridge_config_validate() on everything that arrives from outside, so a
 * malformed profile cannot divide by zero or index past an array. */
#define BRIDGE_MOUSE_DIV_MIN 1
#define BRIDGE_MOUSE_DIV_MAX 512
#define BRIDGE_MOUSE_TAU_MIN 5
#define BRIDGE_MOUSE_TAU_MAX 500
#define BRIDGE_ANTI_DZ_MAX   80 /* percent; above this the usable range gets silly */

/*
 * Response curve exponent, in hundredths. 100 is linear and is the default, so the feature costs
 * nothing until asked for. 25..400 covers everything useful: below 100 boosts small movements,
 * above 100 damps them.
 */
#define BRIDGE_CURVE_MIN     25
#define BRIDGE_CURVE_MAX     400
#define BRIDGE_CURVE_LINEAR  100

/* Smoothing time constant default: the value tuned by hand at 100 Hz (AGENTS.md 4.22). */
#define BRIDGE_MOUSE_TAU_DEFAULT 80

typedef struct {
    uint16_t version;
    char name[BRIDGE_PROFILE_NAME_MAX];

    /*
     * Mouse to right stick. Divisors are per axis: full deflection at div * 400 counts per
     * SECOND, the same unit the Kconfig option documents, so a value carries over unchanged.
     * Separate X and Y because a lower vertical sensitivity is a common preference and the cost
     * of supporting it is one extra field.
     */
    uint16_t mouse_div_x;
    uint16_t mouse_div_y;

    /* Smoothing time constant in milliseconds. Was the compile-time MOUSE_TAU_MS. */
    uint16_t mouse_tau_ms;

    /*
     * Anti-deadzone, as a percentage of full deflection. Games discard stick magnitudes below
     * an inner deadzone, so a small mouse movement produces a deflection the game throws away.
     * This lifts any non-zero deflection to at least this magnitude and compresses the rest of
     * the range into what is left, which keeps the mapping continuous and still reaches full
     * scale.
     *
     * Applied RADIALLY, to the magnitude of the vector, not per axis. Per-axis is simpler and
     * is what a lot of converters do, but it overshoots on diagonals by up to 41 % and that
     * shows up as the stick pulling towards the corners.
     */
    uint8_t mouse_anti_deadzone;

    uint8_t mouse_invert_y;

    /*
     * Response curve exponent in hundredths, applied to the magnitude of the stick vector:
     * out = full * (in / full) ^ (curve / 100).
     *
     * WHAT THIS IS FOR. Several games apply their own curve to stick input and do not let you turn
     * it off - small deflections are damped so that a thumbstick feels controllable. A mouse does
     * not need that help, and the damping fights precise aiming. Setting the exponent below 1.0
     * pre-compensates: if the game raises input to the power 2, an exponent of 0.5 here makes the
     * combination linear again.
     *
     * WHAT IT IS NOT. It cannot undo acceleration that builds up over TIME, the kind where holding
     * a direction makes the turn rate ramp. That is a function of history, not of the current
     * value, and inverting it would mean modelling the game's ramp.
     *
     * 100 means linear, which is the default, and the code skips the whole step in that case - so
     * the arithmetic that the 997 Hz and 830 Hz measurements were taken with is untouched unless
     * somebody asks for a curve.
     */
    uint16_t mouse_curve;

    /* Binding table. bind_count rows of binds[] are in force. */
    uint8_t bind_count;
    input_bind_t binds[INPUT_BIND_MAX];
} bridge_config_t;

/*
 * Loads the active profile from NVS and publishes it. If nothing is stored, or what is stored
 * does not validate, the defaults are published instead and that is logged. Never fails in a
 * way that leaves the bridge without a configuration.
 *
 * Call after nvs_flash_init() and before input_mapper_start().
 */
esp_err_t bridge_config_init(void);

/* The configuration in force. The pointer may change on a write, so read it once per use. */
const bridge_config_t *bridge_config_get(void);

/* Increments on every successful apply. Readers cache derived values against it. */
uint32_t bridge_config_generation(void);

/* The documented defaults, built from the Kconfig values and the mapper's default table. */
void bridge_config_defaults(bridge_config_t *out);

/*
 * Clamps every field into range and repairs the binding table, in place. Returns false if
 * something had to be changed, which callers can report, but the struct is usable either way.
 */
bool bridge_config_validate(bridge_config_t *cfg);

/*
 * Validates and publishes cfg, and pushes the binding table into the mapper. Does NOT write to
 * NVS - live tuning should not cost a flash erase per slider movement.
 */
esp_err_t bridge_config_apply(const bridge_config_t *cfg);

/* Persist the active configuration into a slot, and remember that slot as the active one. */
esp_err_t bridge_config_save(uint8_t slot);

/* Load a slot and apply it. */
esp_err_t bridge_config_load(uint8_t slot);

/* Which slot was last saved or loaded. */
uint8_t bridge_config_active_slot(void);

/*
 * Asks for a profile to be loaded. Cheap and non-blocking: it only records the request.
 *
 * Called from the link receive task on every mode frame - four times a second - so it must not read
 * flash there. Reading NVS in that task would stall frame reception for as long as the read takes,
 * which on a path carrying 1 kHz mouse reports is the wrong place to spend milliseconds.
 *
 * bridge_config_service() does the actual work, from the mapping task, which is the same place the
 * USB identity switch happens for the same reason.
 */
void bridge_config_request_profile(uint8_t slot);

/*
 * Applies a pending profile request, if any. Call once per tick from the mapping task. Returns true
 * when a profile was actually loaded, which is rare - only when the request differs from what is
 * already in force.
 */
bool bridge_config_service(void);

/* Whether a slot holds a valid stored profile, and its name if so. Used by the UI to list them. */
bool bridge_config_slot_name(uint8_t slot, char out[BRIDGE_PROFILE_NAME_MAX]);

/* Forget a stored slot. */
esp_err_t bridge_config_erase(uint8_t slot);

/*
 * Restores a slot to the built-in defaults, persistently, and applies them live if that slot is the
 * one currently in force.
 *
 * One operation rather than "reset the live settings" followed by "save": the two-step version was
 * what the panel offered and it misled - resetting without saving looks like it worked until the next
 * reboot brings the old profile back. Which slot is active does NOT change, so resetting a profile
 * you are not using cannot move you onto it.
 */
esp_err_t bridge_config_reset_slot(uint8_t slot);

/*
 * Replaces just the binding table with the built-in one, leaving the tunables alone. Live only.
 *
 * Exists because the panel's mapping editor needs an escape that means what it says: a button in the
 * mapping tab labelled "defaults" that silently also reset sensitivity and smoothing would be a
 * second description of a different thing.
 */
esp_err_t bridge_config_reset_binds(void);

#ifdef __cplusplus
}
#endif
