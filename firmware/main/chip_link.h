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

/*
 * Sender only: absolute keyboard state, boot-protocol layout (modifier bitmap + up to six
 * keycodes). Absolute rather than incremental, so a lost frame self-heals on the next one.
 *
 * Only the USB build uses this: on the BLE split bridge the keyboard is local to the host
 * chip and never crosses the wire.
 */
void chip_link_send_keyboard(uint8_t modifiers, const uint8_t keys[6]);

/*
 * Sender only: tell the host chip which classes we currently serve. Carried by the keepalive,
 * so the host learns about a device going away even if it was idle at the time.
 */
void chip_link_set_presence(bool mouse, bool keyboard);

/*
 * Sender only: ask the pad chip to change mode - which USB identity it presents, and whether the
 * configuration panel is up.
 *
 * ONE OCTET OF ABSOLUTE STATE, not a toggle command, and that is what makes it robust: a frame
 * lost to noise, or a reset of either chip, corrects itself within one keepalive period instead
 * of leaving the two sides disagreeing. A toggle would do the opposite - a lost toggle stays
 * wrong forever, and both chips would report success.
 *
 * It carries a bitmask rather than a boolean because passthrough and the panel are independent:
 * the panel is useful in either identity, and a single "mode" value would have to enumerate
 * every combination.
 *
 * WIRE COMPATIBILITY: an older build treated this octet as "non-zero means passthrough", so a
 * new input chip talking to an old pad chip would switch identity when asked for the panel.
 * Both ends come out of the same tree, so this only matters if they are flashed from different
 * commits.
 */
#define LINK_MODE_PASSTHROUGH 0x01 /* present as a HID keyboard + mouse instead of a pad */
#define LINK_MODE_WEBUI       0x02 /* bring Wi-Fi and the configuration panel up */
#define LINK_MODE_WEBUI_AP    0x04 /* serve an access point rather than joining a network */

/*
 * Which configuration profile is in force, as a two-bit index in bits 3..4.
 *
 * It rides the mode octet rather than getting a frame of its own because it is the same KIND of
 * thing: absolute state that the input chip owns and repeats. The payoff is the same too - the
 * profile survives a reset of the pad chip, because the input chip re-asserts it within a keepalive.
 * Two bits is exactly BRIDGE_PROFILE_COUNT, so nothing is wasted and nothing is missing.
 */
#define LINK_MODE_PROFILE_MASK  0x18
#define LINK_MODE_PROFILE_SHIFT 3

void chip_link_set_mode_bits(uint8_t bits);

/* Sender only: the bits last set, so a caller can toggle one without keeping its own copy. */
uint8_t chip_link_mode_bits(void);

/* Receiver only: whether a frame arrived recently enough (APP_LINK_PEER_TIMEOUT_MS). */
bool chip_link_peer_alive(void);

/*
 * Link counters and the pins in force, for diagnosis without a serial adapter.
 *
 * Added because a firmware push over the link failed and there was no way to tell "this chip is not
 * transmitting" from "the wire does not carry it" - the only console available was the other chip's.
 * sent counts frames this chip wrote, received counts frames it parsed, and crc_errors counts frames
 * that arrived damaged. A sender that climbs while the peer's receiver stays at zero points at the
 * cable; both at zero points at this chip.
 */
void chip_link_stats(uint32_t *sent, uint32_t *received, uint32_t *crc_errors, int *tx_pin,
                     int *rx_pin);

/*
 * Pushes a firmware image to the peer chip, for the end that has a network.
 *
 * Exists because the input chip's USB port is the host side and its console needs the USB-UART
 * adapter, so a firmware change there otherwise means holding BOOT and RESET on the board. The cable
 * was already crossed on both pairs, so the reverse direction cost no rewiring.
 *
 * Call begin, then data as many times as needed, then end. All three BLOCK: the peer acknowledges
 * each window of frames and that is what stops the sender outrunning its flash writes. Any of them
 * returning an error means the transfer is over - abandon it and let the operator upload again. The
 * peer verifies the image's SHA256 before switching to it, so a failed transfer cannot brick it.
 *
 * Only built where both link directions exist and this chip is the pad; elsewhere these are absent.
 */
#if CONFIG_APP_USB_PAD && CONFIG_APP_LINK_TX_GPIO >= 0 && CONFIG_APP_LINK_RX_GPIO >= 0
esp_err_t chip_link_fw_begin(uint32_t total);
esp_err_t chip_link_fw_data(const uint8_t *data, size_t len);
esp_err_t chip_link_fw_end(void);
#define CHIP_LINK_HAS_FW_PUSH 1
#else
#define CHIP_LINK_HAS_FW_PUSH 0
#endif

#ifdef __cplusplus
}
#endif
