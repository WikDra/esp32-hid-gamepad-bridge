/* Inter-chip UART link - see chip_link.h for what this is and why it is a UART. */

#include "chip_link.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "webui.h"

#if CONFIG_APP_LINK_RECEIVER || CONFIG_APP_LINK_SENDER
#if CONFIG_APP_ENABLE_HID_HOST
#include "ble_hid_host.h"
#else
#include "input_state.h"
#endif
#endif

#if !CONFIG_APP_LINK_DISABLED

/*
 * DIRECTION IS DERIVED FROM THE PINS, ROLE FROM KCONFIG, and keeping those two apart is what makes
 * a bidirectional link possible without disturbing what already works.
 *
 *   - LINK_CAN_TX / LINK_CAN_RX say what the wiring permits. They gate the MACHINERY: the frame
 *     writer, the parser, the tasks.
 *   - APP_LINK_SENDER / APP_LINK_RECEIVER say what this chip is FOR. They gate the CONTENT: who
 *     produces mouse and keyboard frames, who generates the keepalive and the mode octet, and who
 *     feeds the input accumulator.
 *
 * The distinction matters because the pad chip must be able to SEND (firmware images to its peer)
 * without ever sending a mode octet - two chips asserting absolute state at each other would fight,
 * and the loser would be whichever spoke last.
 *
 * On the USB split both chips now have TX=GPIO4 and RX=GPIO5 with the cable crossed, so both
 * directions exist. On the BLE split the pins are one-way and these conditions reduce to exactly
 * the old behaviour.
 */
#define LINK_CAN_TX (CONFIG_APP_LINK_TX_GPIO >= 0)
#define LINK_CAN_RX (CONFIG_APP_LINK_RX_GPIO >= 0)

static const char *TAG = "link";

/*
 * Framing. The receiver has to survive garbage on the wire: the H2's ROM bootloader prints
 * on UART0 at every reset, and on this board that pin goes straight to the S3. So a frame
 * is self-delimiting - two sync bytes, then a CRC over everything that follows them.
 */
#define LINK_SYNC0 0xA5
#define LINK_SYNC1 0x5A

#define LINK_TYPE_MOUSE     0x01 /* buttons u8, dx i16, dy i16, wheel i8         */
#define LINK_TYPE_KEEPALIVE 0x02 /* presence bitmask u8: bit0 mouse, bit1 keyboard */
#define LINK_TYPE_KEYBOARD  0x03 /* modifiers u8, keycodes u8 x6                 */
#define LINK_TYPE_MODE      0x04 /* mode bitmask: passthrough, panel, profile     */

/*
 * Firmware transfer, from the chip that has a network to the one that does not.
 *
 * WHY THIS EXISTS. The input chip's USB port is the host side and its console needs the USB-UART
 * adapter, so every firmware change there meant holding BOOT and RESET. The cable was already
 * crossed on both pairs, so the reverse direction cost nothing to enable.
 *
 * NO RETRANSMISSION, deliberately. The ACK is FLOW CONTROL - it stops the sender outrunning the
 * receiver's flash writes - not reliability. Reliability comes from two places that are already
 * there: every frame carries a CRC, and esp_ota_end() verifies the image's SHA256 before it can
 * ever be booted. A transfer that goes wrong is abandoned and the browser can upload again; what
 * cannot happen is a corrupt image being booted.
 */
#define LINK_TYPE_FW_BEGIN 0x05 /* total size u32                                */
#define LINK_TYPE_FW_DATA  0x06 /* image bytes, in order                         */
#define LINK_TYPE_FW_END   0x07 /* no payload: finish, verify and switch          */
#define LINK_TYPE_FW_ACK   0x08 /* bytes accepted so far u32, status u8           */

#define LINK_PRESENT_MOUSE    0x01
#define LINK_PRESENT_KEYBOARD 0x02

/*
 * Raised from 8 to carry firmware chunks. The control frames need seven bytes at most; this size is
 * chosen for throughput, because at eight bytes a 300 kB image would be 37 500 frames and the
 * per-frame round trips, not the wire, would set the pace. At 192 it is about 1 600 frames and
 * roughly three and a half seconds at 921600 baud.
 *
 * The cost is stack: the parser and the writer each keep a buffer this size. The len field is a
 * single octet, so 255 is the ceiling regardless.
 */
#define LINK_PAYLOAD_MAX 192
#define LINK_FRAME_MAX   (2 + 1 + 1 + LINK_PAYLOAD_MAX + 1)

#define LINK_KEEPALIVE_MS 250

/* CRC-8, polynomial 0x07 (ATM/ITU). Small, table-free, and plenty for 10-byte frames on a
 * 10 cm PCB trace - we need to reject boot chatter, not correct bit errors. */
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t link_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_APP_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /*
     * A TX ring buffer is requested so that uart_write_bytes() copies and returns instead
     * of spinning until the FIFO drains. The sender task must never sit in the driver: it
     * is fed from the BLE event path.
     */
    esp_err_t err = uart_driver_install(CONFIG_APP_LINK_UART_PORT, 2048, 2048, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(CONFIG_APP_LINK_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(CONFIG_APP_LINK_UART_PORT, CONFIG_APP_LINK_TX_GPIO,
                       CONFIG_APP_LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin(tx=%d rx=%d) failed: %s", CONFIG_APP_LINK_TX_GPIO,
                 CONFIG_APP_LINK_RX_GPIO, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "UART%d up: tx=GPIO%d rx=GPIO%d %d baud", CONFIG_APP_LINK_UART_PORT,
             CONFIG_APP_LINK_TX_GPIO, CONFIG_APP_LINK_RX_GPIO, CONFIG_APP_LINK_BAUD);
    return ESP_OK;
}

#endif /* !CONFIG_APP_LINK_DISABLED */

/* ------------------------------------------------------------- frame writer (any direction) */

#if LINK_CAN_TX

static uint32_t s_sent_frames;

/*
 * Writes one framed, CRC-protected frame. Available to both roles, because sending is now a property
 * of the wiring rather than of what the chip is for: the pad chip uses this to push a firmware image
 * to its peer without taking on any of the sender role's content.
 */
static void frame_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[LINK_FRAME_MAX];
    buf[0] = LINK_SYNC0;
    buf[1] = LINK_SYNC1;
    buf[2] = type;
    buf[3] = len;
    if (len) {
        memcpy(&buf[4], payload, len);
    }
    buf[4 + len] = crc8(&buf[2], (size_t)len + 2);
    uart_write_bytes(CONFIG_APP_LINK_UART_PORT, (const char *)buf, (size_t)len + 5);
    s_sent_frames++;
}

#endif /* LINK_CAN_TX */

/* ----------------------------------------------------------- firmware over the link */

/*
 * Which end does what. Derived from the USB roles rather than from the link role, because that is
 * what it actually depends on: the pad chip is the one with a network and a panel, the input chip is
 * the one that is otherwise only reachable by holding BOOT and RESET.
 */
#define LINK_FW_SOURCE (CONFIG_APP_USB_PAD && LINK_CAN_TX && LINK_CAN_RX)
#define LINK_FW_TARGET (CONFIG_APP_USB_HID_HOST && LINK_CAN_RX && LINK_CAN_TX)

#if LINK_FW_TARGET
#include "esp_ota_ops.h"

/* One window of frames is acknowledged at a time. Four times 192 bytes is 768 in flight, against a
 * 2 kB UART receive buffer - enough margin that a slow flash write cannot overrun it. */
#define LINK_FW_ACK_EVERY 4

static esp_ota_handle_t s_fw_ota;
static const esp_partition_t *s_fw_part;
static uint32_t s_fw_total;
static uint32_t s_fw_got;
static uint32_t s_fw_frames;
static bool s_fw_failed;

static void fw_ack(uint8_t status)
{
    uint8_t p[5];
    p[0] = (uint8_t)(s_fw_got & 0xFF);
    p[1] = (uint8_t)((s_fw_got >> 8) & 0xFF);
    p[2] = (uint8_t)((s_fw_got >> 16) & 0xFF);
    p[3] = (uint8_t)((s_fw_got >> 24) & 0xFF);
    p[4] = status;
    frame_send(LINK_TYPE_FW_ACK, p, sizeof(p));
}

static void fw_abort(const char *why)
{
    if (s_fw_ota) {
        esp_ota_abort(s_fw_ota);
        s_fw_ota = 0;
    }
    s_fw_failed = true;
    ESP_LOGE(TAG, "firmware over the link failed: %s", why);
    fw_ack(1);
}

static void fw_begin(const uint8_t *p, uint8_t len)
{
    if (len != 4) {
        return;
    }
    if (s_fw_ota) {
        esp_ota_abort(s_fw_ota);
        s_fw_ota = 0;
    }

    s_fw_total = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                 ((uint32_t)p[3] << 24);
    s_fw_got = 0;
    s_fw_frames = 0;
    s_fw_failed = false;

    s_fw_part = esp_ota_get_next_update_partition(NULL);
    if (!s_fw_part) {
        fw_abort("no spare OTA partition on this chip");
        return;
    }
    if (esp_ota_begin(s_fw_part, s_fw_total, &s_fw_ota) != ESP_OK) {
        s_fw_ota = 0;
        fw_abort("esp_ota_begin");
        return;
    }

    ESP_LOGW(TAG, "firmware over the link: receiving %" PRIu32 " B into '%s'", s_fw_total,
             s_fw_part->label);
    fw_ack(0);
}

static void fw_data(const uint8_t *p, uint8_t len)
{
    if (s_fw_failed || !s_fw_ota) {
        return;
    }
    if (esp_ota_write(s_fw_ota, p, len) != ESP_OK) {
        fw_abort("esp_ota_write");
        return;
    }
    s_fw_got += len;

    /* Acknowledge a window, not every frame: the point is to bound how much the sender may have in
     * flight, and one round trip per 768 bytes costs far less than one per 192. */
    if ((++s_fw_frames % LINK_FW_ACK_EVERY) == 0) {
        fw_ack(0);
    }
}

static void fw_end(void)
{
    if (s_fw_failed || !s_fw_ota) {
        fw_ack(1);
        return;
    }

    /* This is where the image is verified. A truncated or corrupt transfer dies here and the running
     * firmware is untouched - which is why the transfer itself needs no retransmission. */
    const esp_err_t err = esp_ota_end(s_fw_ota);
    s_fw_ota = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware over the link rejected: %s", esp_err_to_name(err));
        fw_ack(1);
        return;
    }
    if (esp_ota_set_boot_partition(s_fw_part) != ESP_OK) {
        fw_ack(1);
        return;
    }

    ESP_LOGW(TAG, "firmware over the link accepted (%" PRIu32 " B) - rebooting into '%s'", s_fw_got,
             s_fw_part->label);
    fw_ack(0);
    /* Let the acknowledgement reach the other chip before the UART goes away. */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
#endif /* LINK_FW_TARGET */

#if LINK_FW_SOURCE
/* Set by the parser when an acknowledgement arrives, consumed by the push functions. */
static volatile uint32_t s_fw_ack_bytes;
static volatile uint8_t s_fw_ack_status;
static volatile bool s_fw_ack_seen;

/* Bytes handed to the wire so far, and how many frames since the last acknowledged window. */
static uint32_t s_fw_pushed;
static uint32_t s_fw_window;

static esp_err_t fw_wait_ack(uint32_t expect_bytes, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (s_fw_ack_seen) {
            s_fw_ack_seen = false;
            if (s_fw_ack_status != 0) {
                ESP_LOGE(TAG, "peer refused the firmware at %" PRIu32 " B", s_fw_ack_bytes);
                return ESP_FAIL;
            }
            if (s_fw_ack_bytes != expect_bytes) {
                ESP_LOGE(TAG, "peer accepted %" PRIu32 " B, expected %" PRIu32, s_fw_ack_bytes,
                         expect_bytes);
                return ESP_ERR_INVALID_STATE;
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGE(TAG, "peer did not acknowledge within %d ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t chip_link_fw_begin(uint32_t total)
{
    uint8_t p[4] = { (uint8_t)total, (uint8_t)(total >> 8), (uint8_t)(total >> 16),
                     (uint8_t)(total >> 24) };
    s_fw_ack_seen = false;
    s_fw_pushed = 0;
    s_fw_window = 0;
    frame_send(LINK_TYPE_FW_BEGIN, p, sizeof(p));
    return fw_wait_ack(0, 3000);
}

esp_err_t chip_link_fw_data(const uint8_t *data, size_t len)
{
    while (len) {
        const uint8_t chunk = (len > LINK_PAYLOAD_MAX) ? LINK_PAYLOAD_MAX : (uint8_t)len;
        frame_send(LINK_TYPE_FW_DATA, data, chunk);
        data += chunk;
        len -= chunk;
        s_fw_pushed += chunk;

        if ((++s_fw_window % 4) == 0) {
            const esp_err_t err = fw_wait_ack(s_fw_pushed, 2000);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

esp_err_t chip_link_fw_end(void)
{
    /* Drain whatever the last partial window left unacknowledged, so a failure is reported here
     * rather than looking like a rejected image. */
    if ((s_fw_window % 4) != 0) {
        s_fw_window = 0;
    }
    frame_send(LINK_TYPE_FW_END, NULL, 0);
    return fw_wait_ack(s_fw_pushed, 10000);
}
#endif /* LINK_FW_SOURCE */


/* ------------------------------------------------------------------------ sender */

#if CONFIG_APP_LINK_SENDER

typedef struct {
    uint8_t type; /* LINK_TYPE_MOUSE or LINK_TYPE_KEYBOARD */
    union {
        struct {
            uint8_t buttons;
            int16_t dx;
            int16_t dy;
            int8_t wheel;
        } mouse;
        struct {
            uint8_t modifiers;
            uint8_t keys[6];
        } kbd;
    };
} link_evt_t;

static QueueHandle_t s_tx_queue;
static uint32_t s_dropped;
static volatile uint8_t s_presence;

void chip_link_set_presence(bool mouse, bool keyboard)
{
    s_presence = (uint8_t)((mouse ? LINK_PRESENT_MOUSE : 0) |
                           (keyboard ? LINK_PRESENT_KEYBOARD : 0));
}

/*
 * The mode frame is needed by passthrough (which identity is on the bus) and by the web panel
 * (whether Wi-Fi is up). ONE NAME FOR THE CONDITION, because it is tested in four places and a
 * disjunction repeated four times is four chances to write it differently - which is the shape of
 * the bug in AGENTS.md 4.38.
 */
#if CONFIG_APP_USB_PASSTHROUGH || CONFIG_APP_WEBUI
#define LINK_HAS_MODE 1
#else
#define LINK_HAS_MODE 0
#endif

#if LINK_HAS_MODE
static volatile uint8_t s_mode; /* LINK_MODE_* bits */

void chip_link_set_mode_bits(uint8_t bits)
{
    s_mode = bits;
    /*
     * Sent immediately so the switch feels instant, and then repeated with every keepalive by
     * the sender task. The repetition is what makes it robust: the mode is ABSOLUTE state, so
     * a frame lost to line noise or to a reset of the other chip corrects itself within
     * 250 ms instead of leaving the two chips disagreeing about which identity is on the bus.
     */
    if (s_tx_queue) {
        link_evt_t evt = {.type = LINK_TYPE_MODE};
        evt.kbd.modifiers = s_mode; /* reuse the byte; the frame carries one payload octet */
        if (xQueueSend(s_tx_queue, &evt, 0) != pdTRUE) {
            s_dropped++;
        }
    }
}

uint8_t chip_link_mode_bits(void)
{
    return s_mode;
}
#endif /* LINK_HAS_MODE */

static int16_t clamp16(int32_t v)
{
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)v;
}

void chip_link_send_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    if (!s_tx_queue) {
        return;
    }
    link_evt_t evt = {.type = LINK_TYPE_KEYBOARD};
    evt.kbd.modifiers = modifiers;
    memcpy(evt.kbd.keys, keys, sizeof(evt.kbd.keys));
    if (xQueueSend(s_tx_queue, &evt, 0) != pdTRUE) {
        s_dropped++;
    }
}

void chip_link_send_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel)
{
    if (!s_tx_queue) {
        return;
    }
    link_evt_t evt = {.type = LINK_TYPE_MOUSE};
    evt.mouse.buttons = buttons;
    evt.mouse.dx = clamp16(dx);
    evt.mouse.dy = clamp16(dy);
    evt.mouse.wheel = (int8_t)(wheel > 127 ? 127 : (wheel < -128 ? -128 : wheel));
    /* Never block the caller: it is a USB or BLE driver callback. A full queue means the link
     * is wedged, and one lost delta matters far less than a stalled input path. */
    if (xQueueSend(s_tx_queue, &evt, 0) != pdTRUE) {
        s_dropped++;
    }
}

static void sender_task(void *arg)
{
    (void)arg;
    int64_t last_keepalive_us = 0;
    int64_t last_stat_us = esp_timer_get_time();
    uint32_t stat_frames = 0;

    for (;;) {
        link_evt_t evt;
        /* Wake either on an event or on the keepalive deadline, whichever comes first. */
        if (xQueueReceive(s_tx_queue, &evt, pdMS_TO_TICKS(LINK_KEEPALIVE_MS)) == pdTRUE) {
            if (evt.type == LINK_TYPE_MOUSE) {
                uint8_t p[6];
                p[0] = evt.mouse.buttons;
                p[1] = (uint8_t)(evt.mouse.dx & 0xff);
                p[2] = (uint8_t)((evt.mouse.dx >> 8) & 0xff);
                p[3] = (uint8_t)(evt.mouse.dy & 0xff);
                p[4] = (uint8_t)((evt.mouse.dy >> 8) & 0xff);
                p[5] = (uint8_t)evt.mouse.wheel;
                frame_send(LINK_TYPE_MOUSE, p, sizeof(p));
            } else if (evt.type == LINK_TYPE_KEYBOARD) {
                uint8_t p[7];
                p[0] = evt.kbd.modifiers;
                memcpy(&p[1], evt.kbd.keys, sizeof(evt.kbd.keys));
                frame_send(LINK_TYPE_KEYBOARD, p, sizeof(p));
#if LINK_HAS_MODE
            } else if (evt.type == LINK_TYPE_MODE) {
                const uint8_t p = evt.kbd.modifiers;
                frame_send(LINK_TYPE_MODE, &p, 1);
#endif
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - last_keepalive_us >= LINK_KEEPALIVE_MS * 1000) {
            last_keepalive_us = now;
            /*
             * Tells the host chip that we are alive even while nothing moves, and carries
             * which classes we serve.
             *
             * On the BLE split the mouse presence is still derived from the device table, so
             * that branch behaves exactly as before this file learned about keyboards.
             */
            uint8_t present = s_presence;
#if CONFIG_APP_ENABLE_HID_HOST
            present = (uint8_t)(ble_hid_host_device_count() > 0 ? LINK_PRESENT_MOUSE : 0);
#endif
            frame_send(LINK_TYPE_KEEPALIVE, &present, 1);
#if LINK_HAS_MODE
            /* Absolute state, resent with every keepalive - see chip_link_set_mode_bits(). */
            {
                const uint8_t m = s_mode;
                frame_send(LINK_TYPE_MODE, &m, 1);
            }
#endif
        }

        if (now - last_stat_us >= 10 * 1000 * 1000) {
            if (s_sent_frames != stat_frames) {
                ESP_LOGI(TAG, "sent %" PRIu32 " frames (dropped %" PRIu32 ")", s_sent_frames,
                         s_dropped);
                stat_frames = s_sent_frames;
            }
            last_stat_us = now;
        }
    }
}

#endif /* CONFIG_APP_LINK_SENDER */

/* ---------------------------------------------------------------------- receiver */

#if LINK_CAN_RX

static volatile int64_t s_last_frame_us;
static uint32_t s_rx_frames;
#if !CONFIG_APP_LINK_PROBE_RX
static uint32_t s_crc_errors; /* only the real receive path counts these */
#endif
static uint8_t s_peer_present;

/*
 * Where a received report goes depends on which bridge this is:
 *   - BLE split: into ble_hid_host's accumulator, next to its local keyboard.
 *   - USB split: into the shared accumulator, which is the only producer there.
 * Wrapped in macros so the frame handler below reads the same in both.
 */
#if CONFIG_APP_ENABLE_HID_HOST
#define LINK_ACCUM_MOUSE(b, dx, dy, w) ble_hid_host_inject_mouse((b), (dx), (dy), (w))
#define LINK_MOUSE_PRESENT(on)         ble_hid_host_set_remote_mouse((on))
#define LINK_SET_KEYBOARD(m, k)        ((void)0) /* keyboard is local on that chip */
#define LINK_KEYBOARD_PRESENT(on)      ((void)(on))
#else
#define LINK_ACCUM_MOUSE(b, dx, dy, w) input_state_accum_mouse((b), (dx), (dy), (w))
#define LINK_MOUSE_PRESENT(on)         input_state_set_mouse_present((on))
#define LINK_SET_KEYBOARD(m, k)        input_state_set_keyboard((m), (k))
#define LINK_KEYBOARD_PRESENT(on)      input_state_set_keyboard_present((on))
#endif

/*
 * The mode frame carries two independent things. Switching USB identity only means something on a
 * chip that owns a pad, so that one stays behind a condition; the panel handles its own absence
 * through empty inlines in webui.h, so that one does not need one.
 *
 * Both dispatches are called on EVERY mode frame, which arrives with every keepalive, so both
 * must be idempotent and act only on a change. That is the price of absolute state on the wire
 * and it is worth paying: a lost frame repairs itself instead of leaving the chips disagreeing.
 */
#if CONFIG_APP_USB_PASSTHROUGH && CONFIG_APP_USB_PAD
#include "usb_pad.h"
#define LINK_SET_IDENTITY(pt) usb_pad_request_mode((pt))
#else
#define LINK_SET_IDENTITY(pt) ((void)(pt))
#endif

/*
 * The profile index only means something where the configuration lives, which is the chip that runs
 * the mapper. The condition MIRRORS the one guarding input_mapper.c and bridge_config.c in
 * CMakeLists.txt; elsewhere the index is accepted and discarded, so an input chip can talk to a chip
 * built without profiles at all.
 */
#if !CONFIG_APP_GAMEPAD_SELFTEST && \
    ((CONFIG_APP_ENABLE_HID_HOST && CONFIG_APP_ENABLE_GAMEPAD) || CONFIG_APP_USB_PAD)
#include "bridge_config.h"
#define LINK_SET_PROFILE(s) bridge_config_request_profile((s))
#else
#define LINK_SET_PROFILE(s) ((void)(s))
#endif

bool chip_link_peer_alive(void)
{
    int64_t last = s_last_frame_us;
    if (last == 0) {
        return false;
    }
    return (esp_timer_get_time() - last) < (int64_t)CONFIG_APP_LINK_PEER_TIMEOUT_MS * 1000;
}

static void handle_frame(uint8_t type, const uint8_t *p, uint8_t len)
{
    s_last_frame_us = esp_timer_get_time();
    s_rx_frames++;

    switch (type) {
    case LINK_TYPE_MOUSE:
        if (len != 6) {
            return;
        }
        {
            int16_t dx = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
            int16_t dy = (int16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
            int8_t wheel = (int8_t)p[5];
            LINK_ACCUM_MOUSE(p[0], dx, dy, wheel);
        }
        break;

    case LINK_TYPE_KEYBOARD:
        if (len != 7) {
            return;
        }
        LINK_SET_KEYBOARD(p[0], &p[1]);
        break;

    case LINK_TYPE_MODE:
        if (len != 1) {
            return;
        }
        /* Tested BIT BY BIT, not for being non-zero: with two independent flags in one octet,
         * "non-zero means passthrough" would switch identity whenever the panel was asked for. */
        LINK_SET_IDENTITY((p[0] & LINK_MODE_PASSTHROUGH) != 0);
        webui_request((p[0] & LINK_MODE_WEBUI) != 0, (p[0] & LINK_MODE_WEBUI_AP) != 0);
        LINK_SET_PROFILE((uint8_t)((p[0] & LINK_MODE_PROFILE_MASK) >> LINK_MODE_PROFILE_SHIFT));
        break;

    case LINK_TYPE_KEEPALIVE:
        if (len != 1) {
            return;
        }
        if (p[0] != s_peer_present) {
            const uint8_t was = s_peer_present;
            s_peer_present = p[0];
            ESP_LOGI(TAG, "peer serves:%s%s",
                     (s_peer_present & LINK_PRESENT_MOUSE) ? " mouse" : "",
                     (s_peer_present & LINK_PRESENT_KEYBOARD) ? " keyboard" : "");
            /* A class that went away sends no more reports, so drop what it was holding -
             * otherwise a button or key held at that instant stays held forever. */
            if ((was & LINK_PRESENT_MOUSE) && !(s_peer_present & LINK_PRESENT_MOUSE)) {
                LINK_MOUSE_PRESENT(false);
            }
            if ((was & LINK_PRESENT_KEYBOARD) && !(s_peer_present & LINK_PRESENT_KEYBOARD)) {
                LINK_KEYBOARD_PRESENT(false);
            }
        }
        break;

#if LINK_FW_TARGET
    case LINK_TYPE_FW_BEGIN:
        fw_begin(p, len);
        break;
    case LINK_TYPE_FW_DATA:
        fw_data(p, len);
        break;
    case LINK_TYPE_FW_END:
        fw_end();
        break;
#endif

#if LINK_FW_SOURCE
    case LINK_TYPE_FW_ACK:
        if (len == 5) {
            s_fw_ack_bytes = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                             ((uint32_t)p[3] << 24);
            s_fw_ack_status = p[4];
            s_fw_ack_seen = true;
        }
        break;
#endif

    default:
        break;
    }
}

static void receiver_task(void *arg)
{
    (void)arg;
#if CONFIG_APP_LINK_PROBE_RX
    /*
     * Pin sweep. The wiring between the two SoCs on this board is not documented in the
     * places we looked (the ESP-IDF ot_br example hardcodes GPIO4/5, but its README shows
     * that as DevKit-to-DevKit wiring), so instead of guessing we listen.
     *
     * GPIO19/20 are the S3's USB pins and carry our console; 26..37 are flash and PSRAM.
     * Neither appears below. Everything here is configured as an input only.
     */
    static const int candidates[] = {4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                     16, 17, 18, 21, 38, 39, 40, 41, 42, 43, 44, 45,
                                     46, 47, 48, 1,  2,  3};
    ESP_LOGW(TAG, "RX PIN SWEEP: %d candidates, ~1 s each - the peer keepalives every %d ms",
             (int)(sizeof(candidates) / sizeof(candidates[0])), LINK_KEEPALIVE_MS);

    for (;;) {
        int best_pin = -1;
        uint32_t best_bytes = 0;
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            const int pin = candidates[i];
            if (uart_set_pin(CONFIG_APP_LINK_UART_PORT, CONFIG_APP_LINK_TX_GPIO, pin,
                             UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
                continue;
            }
            uart_flush_input(CONFIG_APP_LINK_UART_PORT);

            uint32_t bytes = 0, frames = 0, syncs = 0;
            int64_t until = esp_timer_get_time() + 1000 * 1000;
            uint8_t prev = 0;
            while (esp_timer_get_time() < until) {
                uint8_t b;
                if (uart_read_bytes(CONFIG_APP_LINK_UART_PORT, &b, 1, pdMS_TO_TICKS(20)) == 1) {
                    bytes++;
                    if (prev == LINK_SYNC0 && b == LINK_SYNC1) {
                        syncs++;
                        /* Read the rest of the frame and check the CRC, so that a random
                         * 0xA5 0x5A in noise cannot be mistaken for our peer. */
                        uint8_t hdr[2];
                        if (uart_read_bytes(CONFIG_APP_LINK_UART_PORT, hdr, 2,
                                            pdMS_TO_TICKS(20)) == 2 &&
                            hdr[1] <= LINK_PAYLOAD_MAX) {
                            uint8_t body[LINK_PAYLOAD_MAX + 1];
                            int need = hdr[1] + 1;
                            if (uart_read_bytes(CONFIG_APP_LINK_UART_PORT, body, need,
                                                pdMS_TO_TICKS(20)) == need) {
                                uint8_t chk[LINK_PAYLOAD_MAX + 2];
                                chk[0] = hdr[0];
                                chk[1] = hdr[1];
                                memcpy(&chk[2], body, hdr[1]);
                                if (crc8(chk, (size_t)hdr[1] + 2) == body[hdr[1]]) {
                                    frames++;
                                }
                            }
                        }
                    }
                    prev = b;
                }
            }

            if (bytes) {
                ESP_LOGW(TAG, "  GPIO%-2d: %" PRIu32 " bytes, %" PRIu32 " syncs, %" PRIu32
                              " VALID FRAMES%s",
                         pin, bytes, syncs, frames, frames ? "   <<< THIS IS THE PIN" : "");
            }
            if (bytes > best_bytes) {
                best_bytes = bytes;
                best_pin = pin;
            }
        }
        if (best_pin < 0) {
            ESP_LOGE(TAG, "sweep found nothing - is the peer sending, and is TX=GPIO%d right?",
                     CONFIG_APP_LINK_TX_GPIO);
        } else {
            ESP_LOGW(TAG, "sweep done, most traffic on GPIO%d (%" PRIu32 " bytes)", best_pin,
                     best_bytes);
        }
    }
#else
    /* Byte-wise state machine. Deliberately not uart_read_bytes() into a big buffer with
     * memchr for the sync word: at 66 frames/s the cost is irrelevant, and resynchronising
     * mid-stream after boot chatter is much easier to get right one byte at a time. */
    enum { WAIT_S0, WAIT_S1, WAIT_TYPE, WAIT_LEN, WAIT_PAYLOAD, WAIT_CRC } state = WAIT_S0;
    uint8_t type = 0, len = 0, got = 0;
    uint8_t payload[LINK_PAYLOAD_MAX];
    bool peer_was_alive = false;
    int64_t last_stat_us = esp_timer_get_time();
    uint32_t stat_frames = 0;

    for (;;) {
        uint8_t b;
        int n = uart_read_bytes(CONFIG_APP_LINK_UART_PORT, &b, 1, pdMS_TO_TICKS(100));
        if (n == 1) {
            switch (state) {
            case WAIT_S0:
                state = (b == LINK_SYNC0) ? WAIT_S1 : WAIT_S0;
                break;
            case WAIT_S1:
                /* A second 0xA5 could be the start of a real frame, so stay armed. */
                state = (b == LINK_SYNC1) ? WAIT_TYPE : (b == LINK_SYNC0 ? WAIT_S1 : WAIT_S0);
                break;
            case WAIT_TYPE:
                type = b;
                state = WAIT_LEN;
                break;
            case WAIT_LEN:
                len = b;
                if (len > LINK_PAYLOAD_MAX) {
                    state = WAIT_S0; /* not ours - resynchronise */
                    break;
                }
                got = 0;
                state = len ? WAIT_PAYLOAD : WAIT_CRC;
                break;
            case WAIT_PAYLOAD:
                payload[got++] = b;
                if (got == len) {
                    state = WAIT_CRC;
                }
                break;
            case WAIT_CRC: {
                uint8_t check[LINK_PAYLOAD_MAX + 2];
                check[0] = type;
                check[1] = len;
                memcpy(&check[2], payload, len);
                if (crc8(check, (size_t)len + 2) == b) {
                    handle_frame(type, payload, len);
                } else {
                    s_crc_errors++;
                }
                state = WAIT_S0;
                break;
            }
            }
        }

        /* Link watchdog: silence is meaningful, because the sender keepalives. */
        bool alive = chip_link_peer_alive();
        if (peer_was_alive && !alive) {
            ESP_LOGW(TAG, "peer silent for %d ms - clearing its input state",
                     CONFIG_APP_LINK_PEER_TIMEOUT_MS);
            LINK_MOUSE_PRESENT(false);
            LINK_KEYBOARD_PRESENT(false);
            s_peer_present = 0;
        } else if (!peer_was_alive && alive) {
            ESP_LOGI(TAG, "peer link up");
        }
        peer_was_alive = alive;

        int64_t now = esp_timer_get_time();
        if (now - last_stat_us >= 10 * 1000 * 1000) {
            if (s_rx_frames != stat_frames) {
                ESP_LOGI(TAG, "received %" PRIu32 " frames (CRC errors %" PRIu32 ")",
                         s_rx_frames, s_crc_errors);
                stat_frames = s_rx_frames;
            }
            last_stat_us = now;
        }
    }
#endif /* CONFIG_APP_LINK_PROBE_RX */
}

#endif /* LINK_CAN_RX */

/* -------------------------------------------------------------------------- start */

esp_err_t chip_link_start(void)
{
#if CONFIG_APP_LINK_DISABLED
    return ESP_OK;
#else
    esp_err_t err = link_uart_init();
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_APP_LINK_SENDER
    s_tx_queue = xQueueCreate(16, sizeof(link_evt_t));
    if (!s_tx_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sender_task, "link_tx", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mode: sender (mouse -> host chip)");
#endif

#if LINK_CAN_RX
    /* Priority above the mapper so an arriving frame is parsed before the next pad tick
     * consumes the state - that is the whole point of the split. */
    if (xTaskCreate(receiver_task, "link_rx", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mode: receiver (mouse arrives over UART)");
#endif

    return ESP_OK;
#endif
}
