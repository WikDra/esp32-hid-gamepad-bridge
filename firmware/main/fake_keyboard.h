/*
 * A synthetic BLE keyboard used to bisect the C6/H2 connection failure (AGENTS.md 4.35).
 * See fake_keyboard.c for what it advertises and which properties are adjustable.
 */
#pragma once

#include "esp_err.h"

/*
 * Starts advertising as a copy of the AULA F99 Pro in pairing mode. Call AFTER the stack has
 * synced, from the same place the other roles are started.
 */
esp_err_t fake_keyboard_start(void);
