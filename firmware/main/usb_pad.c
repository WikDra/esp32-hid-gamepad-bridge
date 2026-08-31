/* USB XInput gamepad (Xbox 360 wired) - see usb_pad.h for why this model and not the Series X. */

#include "usb_pad.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "tinyusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"

static const char *TAG = "usb_pad";

/* ------------------------------------------------------------------ descriptors */

/*
 * Byte-exact from a Wireshark capture of a genuine Microsoft Xbox 360 wired controller
 * (partsnotincluded.com, "Understanding the Xbox 360 Wired Controller's USB Data"), and
 * cross-checked against the Linux xpad driver, which matches on
 * bInterfaceClass 0xFF / bInterfaceSubClass 93 (0x5D) / bInterfaceProtocol 1.
 *
 * ONE DELIBERATE DEVIATION: the real pad reports bMaxPacketSize0 = 8, we report 64.
 * TinyUSB's control endpoint is CFG_TUD_ENDPOINT0_SIZE (64 by default) and the descriptor
 * has to agree with the hardware or enumeration breaks. Windows reads the value rather than
 * assuming one, and xusb22.inf matches on VID/PID only, so this is safe. If a host ever
 * objects, the fix is to force CFG_TUD_ENDPOINT0_SIZE to 8 and put 8 here.
 */
#define XINPUT_EP_IN  0x81
#define XINPUT_EP_OUT 0x01

#define XINPUT_IN_REPORT_LEN  20
#define XINPUT_EP_SIZE        32

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xFF,
    .bDeviceSubClass = 0xFF,
    .bDeviceProtocol = 0xFF,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x045E,
    .idProduct = 0x028E,
    .bcdDevice = 0x0114,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

/*
 * Only interface 0 (control data) is declared. The real pad has four - headset, an unknown
 * one, and the Xbox Security Method - none of which Windows needs to bind XInput, and the
 * security interface is not implemented by the Windows driver anyway (it has no endpoints).
 * Declaring one interface also keeps us inside the S3's endpoint budget.
 *
 * The 17-byte descriptor of type 0x21 is NOT optional folklore: it repeats the endpoint
 * addresses and their payload sizes, and the source above reports that changing the endpoint
 * numbers in the endpoint descriptors WITHOUT changing them here stops the Windows driver
 * from communicating. So it is kept verbatim, with 0x81/0x14 and 0x01/0x08 in place.
 */
#define XINPUT_CONFIG_TOTAL_LEN (9 + 9 + 17 + 7 + 7)

static const uint8_t s_config_desc[] = {
    /* Configuration */
    0x09, TUSB_DESC_CONFIGURATION,
    U16_TO_U8S_LE(XINPUT_CONFIG_TOTAL_LEN),
    0x01,        /* bNumInterfaces - 1 here, 4 on the real pad */
    0x01,        /* bConfigurationValue */
    0x00,        /* iConfiguration */
    0xA0,        /* bmAttributes: bus powered + remote wakeup, as the real pad reports */
    0xFA,        /* bMaxPower: 500 mA, as the real pad reports */

    /* Interface 0: control data */
    0x09, TUSB_DESC_INTERFACE,
    0x00,        /* bInterfaceNumber */
    0x00,        /* bAlternateSetting */
    0x02,        /* bNumEndpoints */
    0xFF,        /* bInterfaceClass  - vendor specific */
    0x5D,        /* bInterfaceSubClass */
    0x01,        /* bInterfaceProtocol - 1 = wired (129 would be wireless) */
    0x00,        /* iInterface */

    /* Vendor descriptor 0x21 (17 B), verbatim from the real controller */
    0x11, 0x21,
    0x00, 0x01, 0x01, 0x25,
    XINPUT_EP_IN,           /* endpoint that carries control data to the host */
    XINPUT_IN_REPORT_LEN,   /* its payload size, 0x14 = 20 */
    0x00, 0x00, 0x00, 0x00, 0x13,
    XINPUT_EP_OUT,          /* endpoint that carries rumble and LED from the host */
    0x08,                   /* its payload size */
    0x00, 0x00,

    /* Endpoint IN: 32 B interrupt every 4 ms (250 Hz) */
    0x07, TUSB_DESC_ENDPOINT, XINPUT_EP_IN, TUSB_XFER_INTERRUPT,
    U16_TO_U8S_LE(XINPUT_EP_SIZE), 0x04,

    /* Endpoint OUT: 32 B interrupt every 8 ms */
    0x07, TUSB_DESC_ENDPOINT, XINPUT_EP_OUT, TUSB_XFER_INTERRUPT,
    U16_TO_U8S_LE(XINPUT_EP_SIZE), 0x08,
};

/* Strings 1..3 as the real pad reports them; index 0 is the language ID. */
static const char *s_strings[] = {
    (const char[]){0x09, 0x04}, /* 0x0409 English (US) */
    "\xc2\xa9Microsoft Corporation",
    "Controller",
    "08FEC93",
};

/* --------------------------------------------------------------- report state */

static uint8_t s_report[XINPUT_IN_REPORT_LEN];
static uint8_t s_out_buf[XINPUT_EP_SIZE];
static bool s_mounted;
static uint8_t s_rumble_left;
static uint8_t s_rumble_right;
static uint8_t s_led_pattern;

/* --------------------------------------------------- XInput class driver (TinyUSB) */

static void xinput_init(void)
{
    memset(s_report, 0, sizeof(s_report));
    s_report[0] = 0x00;                   /* message type: control surface data */
    s_report[1] = XINPUT_IN_REPORT_LEN;   /* length, 0x14 */
}

static bool xinput_deinit(void)
{
    return true;
}

static void xinput_reset(uint8_t rhport)
{
    (void)rhport;
    s_mounted = false;
    s_rumble_left = 0;
    s_rumble_right = 0;
}

static uint16_t xinput_open(uint8_t rhport, tusb_desc_interface_t const *desc_intf,
                            uint16_t max_len)
{
    /* Claim only our own interface signature, so this driver cannot swallow someone else's. */
    if (desc_intf->bInterfaceClass != 0xFF || desc_intf->bInterfaceSubClass != 0x5D ||
        desc_intf->bInterfaceProtocol != 0x01) {
        return 0;
    }

    /*
     * Consume: interface (9) + vendor 0x21 (17) + two endpoints (7 each). The vendor
     * descriptor sits between the interface and the endpoints, so it has to be skipped
     * explicitly - tu_desc_next() walks by bLength, which handles it.
     */
    uint16_t drv_len = sizeof(tusb_desc_interface_t) + 17 +
                       2 * sizeof(tusb_desc_endpoint_t);
    TU_VERIFY(max_len >= drv_len, 0);

    uint8_t const *p = (uint8_t const *)desc_intf;
    p = tu_desc_next(p);   /* past the interface descriptor */
    p = tu_desc_next(p);   /* past the vendor 0x21 descriptor */

    for (int i = 0; i < 2; i++) {
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
        TU_ASSERT(ep->bDescriptorType == TUSB_DESC_ENDPOINT, 0);
        TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
        p = tu_desc_next(p);
    }

    s_mounted = true;

    /* Arm the OUT endpoint so the host can send rumble and LED packets immediately. */
    usbd_edpt_xfer(rhport, XINPUT_EP_OUT, s_out_buf, sizeof(s_out_buf), false);

    ESP_LOGI(TAG, "XInput interface open: IN 0x%02x, OUT 0x%02x", XINPUT_EP_IN,
             XINPUT_EP_OUT);
    return drv_len;
}

static bool xinput_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                   tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;

    /*
     * The 360 pad answers a vendor control IN request (bRequest 0x01, wValue 0x0100) with a
     * 20-byte blob; the Linux driver calls it "the magic message" and only third-party pads
     * need it. Windows has never been observed to require it, and answering with zeros is
     * indistinguishable from a pad that has nothing to say - so we accept the setup stage
     * and let the stack complete a zero-length reply rather than stalling, which some hosts
     * treat as an error worth retrying.
     */
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    return false; /* not handled -> the stack stalls it, which is what a real pad does for
                   * requests it does not implement */
}

static bool xinput_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                           uint32_t xferred_bytes)
{
    if (ep_addr != XINPUT_EP_OUT || result != XFER_RESULT_SUCCESS) {
        return true;
    }

    /*
     * Host -> device packets share the type/length framing of the input report:
     *   0x00 0x08 ... : rumble, index 3 = left (big) motor, index 4 = right (small) motor
     *   0x01 0x03 x   : LED animation id in index 2
     */
    if (xferred_bytes >= 5 && s_out_buf[0] == 0x00 && s_out_buf[1] == 0x08) {
        if (s_rumble_left != s_out_buf[3] || s_rumble_right != s_out_buf[4]) {
            s_rumble_left = s_out_buf[3];
            s_rumble_right = s_out_buf[4];
            ESP_LOGI(TAG, "rumble from host: left=%u right=%u", s_rumble_left,
                     s_rumble_right);
        }
    } else if (xferred_bytes >= 3 && s_out_buf[0] == 0x01 && s_out_buf[1] == 0x03) {
        s_led_pattern = s_out_buf[2];
        ESP_LOGI(TAG, "LED pattern from host: %u", s_led_pattern);
    }

    /* Re-arm; the host keeps this endpoint polled. */
    usbd_edpt_xfer(rhport, XINPUT_EP_OUT, s_out_buf, sizeof(s_out_buf), false);
    return true;
}

static const usbd_class_driver_t s_xinput_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "XINPUT",
#endif
    .init = xinput_init,
    .deinit = xinput_deinit,
    .reset = xinput_reset,
    .open = xinput_open,
    .control_xfer_cb = xinput_control_xfer_cb,
    .xfer_cb = xinput_xfer_cb,
    .sof = NULL,
};

/*
 * TinyUSB calls this weak hook while initialising and adds whatever we return to its driver
 * list. This is the documented way to add a class the stack does not know, without forking
 * it (tinyusb README, "usbd_app_driver_get_cb").
 */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_xinput_driver;
}

/* ---------------------------------------------------------------------- public */

bool usb_pad_is_ready(void)
{
    return s_mounted && tud_mounted();
}

void usb_pad_get_rumble(uint8_t *left, uint8_t *right)
{
    if (left) {
        *left = s_rumble_left;
    }
    if (right) {
        *right = s_rumble_right;
    }
}

/* Our mapper produces -127..127 with Y positive DOWNWARDS (screen convention, which is what
 * a mouse and the WASD mapping give). The Xbox report wants int16 with north-east positive,
 * so Y is negated here. Getting this backwards is the classic "stick inverted" bug, so the
 * sign flip lives in exactly one place. */
static int16_t axis_to_xbox(int8_t v, bool invert)
{
    int32_t scaled = (int32_t)v * 32767 / 127;
    if (invert) {
        scaled = -scaled;
    }
    if (scaled > 32767) {
        scaled = 32767;
    }
    if (scaled < -32768) {
        scaled = -32768;
    }
    return (int16_t)scaled;
}

static void put_le16(uint8_t *dst, int16_t v)
{
    dst[0] = (uint8_t)((uint16_t)v & 0xFF);
    dst[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFF);
}

bool usb_pad_send(const gamepad_state_t *state)
{
    if (!usb_pad_is_ready()) {
        return false;
    }

    uint8_t r[XINPUT_IN_REPORT_LEN];
    memset(r, 0, sizeof(r));
    r[0] = 0x00;
    r[1] = XINPUT_IN_REPORT_LEN;

    /*
     * Byte 2: D-pad (bits 0..3 = up, down, left, right), Start, Back, L3, R3.
     * Opposite D-pad directions cancel, so a pad never reports left+right at once - the
     * same rule the BLE hat switch uses.
     */
    uint8_t b2 = 0;
    const bool up = (state->dpad & GAMEPAD_DPAD_UP) && !(state->dpad & GAMEPAD_DPAD_DOWN);
    const bool down = (state->dpad & GAMEPAD_DPAD_DOWN) && !(state->dpad & GAMEPAD_DPAD_UP);
    const bool left = (state->dpad & GAMEPAD_DPAD_LEFT) && !(state->dpad & GAMEPAD_DPAD_RIGHT);
    const bool right = (state->dpad & GAMEPAD_DPAD_RIGHT) && !(state->dpad & GAMEPAD_DPAD_LEFT);
    if (up) {
        b2 |= 0x01;
    }
    if (down) {
        b2 |= 0x02;
    }
    if (left) {
        b2 |= 0x04;
    }
    if (right) {
        b2 |= 0x08;
    }

    /*
     * Button mapping. Kept identical in meaning to the BLE Xbox profile's s_xbox_ctrl table
     * so that switching transport does not change what a key does:
     *   1 left mouse  -> RT (analog)      7  E     -> X
     *   2 right mouse -> LT (analog)      8  Q     -> Y
     *   3 middle      -> R3               9  R     -> LB
     *   4 Space       -> A                10 F     -> RB
     *   5 LShift      -> L3               11 Tab   -> Back (View)
     *   6 LCtrl       -> B                12 Esc   -> Start (Menu)
     */
    const uint16_t btn = state->buttons;
    if (btn & (1u << 4)) {  /* 5: LShift  */
        b2 |= 0x40;         /* L3 */
    }
    if (btn & (1u << 2)) {  /* 3: middle mouse */
        b2 |= 0x80;         /* R3 */
    }
    if (btn & (1u << 11)) { /* 12: Esc */
        b2 |= 0x10;         /* Start */
    }
    if (btn & (1u << 10)) { /* 11: Tab */
        b2 |= 0x20;         /* Back */
    }
    r[2] = b2;

    uint8_t b3 = 0;
    if (btn & (1u << 8)) {  /* 9: R  */
        b3 |= 0x01;         /* LB */
    }
    if (btn & (1u << 9)) {  /* 10: F */
        b3 |= 0x02;         /* RB */
    }
    if (btn & (1u << 3)) {  /* 4: Space */
        b3 |= 0x10;         /* A */
    }
    if (btn & (1u << 5)) {  /* 6: LCtrl */
        b3 |= 0x20;         /* B */
    }
    if (btn & (1u << 6)) {  /* 7: E */
        b3 |= 0x40;         /* X */
    }
    if (btn & (1u << 7)) {  /* 8: Q */
        b3 |= 0x80;         /* Y */
    }
    r[3] = b3;

    /* Triggers are ANALOG, 0..255. A mouse click drives one to full deflection, which shows
     * up as an axis and not as a lit button in joy.cpl - worth knowing when testing. */
    r[4] = (btn & (1u << 1)) ? 255 : 0; /* 2: right mouse -> LT */
    r[5] = (btn & (1u << 0)) ? 255 : 0; /* 1: left mouse  -> RT */

    put_le16(&r[6], axis_to_xbox(state->lx, false));
    put_le16(&r[8], axis_to_xbox(state->ly, true));
    put_le16(&r[10], axis_to_xbox(state->rx, false));
    put_le16(&r[12], axis_to_xbox(state->ry, true));

    /* Bytes 14..19 stay zero, as on the real pad. */

    if (memcmp(r, s_report, sizeof(r)) == 0) {
        return true; /* nothing changed - do not spend bus bandwidth on it */
    }
    if (usbd_edpt_busy(0, XINPUT_EP_IN)) {
        return false; /* previous report still in flight; the next tick will carry this one */
    }

    memcpy(s_report, r, sizeof(r));
    return usbd_edpt_xfer(0, XINPUT_EP_IN, s_report, sizeof(s_report), false);
}

/* ----------------------------------------------------------------------- start */

esp_err_t usb_pad_start(void)
{
    const tinyusb_config_t cfg = {
        .device_descriptor = &s_device_desc,
        .string_descriptor = s_strings,
        .string_descriptor_count = sizeof(s_strings) / sizeof(s_strings[0]),
        .external_phy = false,
        .configuration_descriptor = s_config_desc,
        .self_powered = false,
    };

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB XInput pad up: VID 0x045E PID 0x028E (Xbox 360 wired)");
    ESP_LOGI(TAG, "  config descriptor %u B, report %u B on EP 0x%02x every 4 ms",
             (unsigned)sizeof(s_config_desc), XINPUT_IN_REPORT_LEN, XINPUT_EP_IN);
    return ESP_OK;
}
