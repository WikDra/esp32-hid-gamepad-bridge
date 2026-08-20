/*
 * Inter-chip link for the split bridge on an ESP Thread Border Router board.
 *
 * That board carries an ESP32-S3 and an ESP32-H2 wired together with UART and SPI. We use
 * it to split the three BLE links across two radios:
 *
 *   H2  (sender)   : BLE central for the mouse          -> UART -> S3
 *   S3  (receiver) : BLE central for the keyboard + BLE peripheral pad for the PC
 *
 * Why UART and not SPI. A mouse frame is 10 bytes; at 921600 baud that is ~108 us on the
 * wire, against a 15 ms BLE connection interval - four orders of magnitude apart, so the
 * transport is not the latency term. SPI would add a master that has to poll, or a
 * handshake line, to carry the same 10 bytes; the events here are asynchronous and tiny,
 * which is exactly what a UART is for. The gain of this split is radio time, not baud.
 *
 * The link is deliberately one-directional. The receiver never needs to talk back: it owns
 * the pad and makes every decision. Nothing to arbitrate, nothing to time out on the
 * sender.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialises the UART and starts the task for the configured mode. Call once. */
esp_err_t chip_link_start(void);

/*
 * Sender only: queue one decoded mouse report for the host chip. Safe to call from the
 * NimBLE host task - it only posts to a queue and never blocks, because blocking that
 * task would stall the very BLE link the report came from.
 *
 * Deltas are per report, not accumulated: the receiver does the accumulating, exactly as
 * a local mouse would have it done in ble_hid_host.
 */
void chip_link_send_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel);

/* Receiver only: whether a frame arrived recently enough (APP_LINK_PEER_TIMEOUT_MS). */
bool chip_link_peer_alive(void);

#ifdef __cplusplus
}
#endif
