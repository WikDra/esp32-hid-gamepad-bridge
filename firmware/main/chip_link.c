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

#if CONFIG_APP_LINK_RECEIVER || CONFIG_APP_LINK_SENDER
#include "ble_hid_host.h"
#endif

#if !CONFIG_APP_LINK_DISABLED

static const char *TAG = "link";

/*
 * Framing. The receiver has to survive garbage on the wire: the H2's ROM bootloader prints
 * on UART0 at every reset, and on this board that pin goes straight to the S3. So a frame
 * is self-delimiting - two sync bytes, then a CRC over everything that follows them.
 */
#define LINK_SYNC0 0xA5
#define LINK_SYNC1 0x5A

#define LINK_TYPE_MOUSE     0x01 /* buttons u8, dx i16, dy i16, wheel i8         */
#define LINK_TYPE_KEEPALIVE 0x02 /* mouse_present u8                             */

#define LINK_PAYLOAD_MAX 8
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
    esp_err_t err = uart_driver_install(CONFIG_APP_LINK_UART_PORT, 512, 512, 0, NULL, 0);
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

/* ------------------------------------------------------------------------ sender */

#if CONFIG_APP_LINK_SENDER

typedef struct {
    uint8_t buttons;
    int16_t dx;
    int16_t dy;
    int8_t wheel;
} mouse_evt_t;

static QueueHandle_t s_tx_queue;
static uint32_t s_sent_frames;
static uint32_t s_dropped;

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

void chip_link_send_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel)
{
    if (!s_tx_queue) {
        return;
    }
    const mouse_evt_t evt = {
        .buttons = buttons,
        .dx = clamp16(dx),
        .dy = clamp16(dy),
        .wheel = (int8_t)(wheel > 127 ? 127 : (wheel < -128 ? -128 : wheel)),
    };
    /* Never block the caller: it is the NimBLE host task. A full queue means the link is
     * wedged, and one lost delta matters far less than a stalled BLE link. */
    if (xQueueSend(s_tx_queue, &evt, 0) != pdTRUE) {
        s_dropped++;
    }
}

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

static void sender_task(void *arg)
{
    (void)arg;
    int64_t last_keepalive_us = 0;
    int64_t last_stat_us = esp_timer_get_time();
    uint32_t stat_frames = 0;

    for (;;) {
        mouse_evt_t evt;
        /* Wake either on an event or on the keepalive deadline, whichever comes first. */
        if (xQueueReceive(s_tx_queue, &evt, pdMS_TO_TICKS(LINK_KEEPALIVE_MS)) == pdTRUE) {
            uint8_t p[6];
            p[0] = evt.buttons;
            p[1] = (uint8_t)(evt.dx & 0xff);
            p[2] = (uint8_t)((evt.dx >> 8) & 0xff);
            p[3] = (uint8_t)(evt.dy & 0xff);
            p[4] = (uint8_t)((evt.dy >> 8) & 0xff);
            p[5] = (uint8_t)evt.wheel;
            frame_send(LINK_TYPE_MOUSE, p, sizeof(p));
        }

        int64_t now = esp_timer_get_time();
        if (now - last_keepalive_us >= LINK_KEEPALIVE_MS * 1000) {
            last_keepalive_us = now;
            /* Tells the host chip that we are alive even while the mouse is still, and
             * carries whether the mouse itself is connected here. */
            uint8_t present = ble_hid_host_device_count() > 0 ? 1 : 0;
            frame_send(LINK_TYPE_KEEPALIVE, &present, 1);
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

#if CONFIG_APP_LINK_RECEIVER

static volatile int64_t s_last_frame_us;
static uint32_t s_rx_frames;
#if !CONFIG_APP_LINK_PROBE_RX
static uint32_t s_crc_errors; /* only the real receive path counts these */
#endif
static bool s_peer_mouse;

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
            ble_hid_host_inject_mouse(p[0], dx, dy, wheel);
        }
        break;

    case LINK_TYPE_KEEPALIVE:
        if (len != 1) {
            return;
        }
        if ((p[0] != 0) != s_peer_mouse) {
            s_peer_mouse = (p[0] != 0);
            ESP_LOGI(TAG, "peer reports mouse %s", s_peer_mouse ? "connected" : "gone");
            /* No mouse on the peer means no more deltas will come; drop the state so the
             * stick centres instead of holding its last deflection. */
            if (!s_peer_mouse) {
                ble_hid_host_set_remote_mouse(false);
            }
        }
        break;

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
            ESP_LOGW(TAG, "peer silent for %d ms - clearing mouse state",
                     CONFIG_APP_LINK_PEER_TIMEOUT_MS);
            ble_hid_host_set_remote_mouse(false);
            s_peer_mouse = false;
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

#endif /* CONFIG_APP_LINK_RECEIVER */

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
    s_tx_queue = xQueueCreate(16, sizeof(mouse_evt_t));
    if (!s_tx_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sender_task, "link_tx", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mode: sender (mouse -> host chip)");
#endif

#if CONFIG_APP_LINK_RECEIVER
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
