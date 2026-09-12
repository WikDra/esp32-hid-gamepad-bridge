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

#if CONFIG_APP_USB_PASSTHROUGH

/* ------------------------------------------------ passthrough identity (HID) */

/*
 * The second identity: a plain HID keyboard and mouse, so the real devices reach the PC
 * unchanged. See usb_pad.h for why this cannot be the same USB device as the pad.
 *
 * VID 0x303A is Espressif's, and 0x4004 is what esp_tinyusb itself computes for a HID-only
 * device (0x4000 with the HID class bit set). Picking a value from that scheme rather than
 * inventing one avoids squatting on somebody else's product ID.
 */
#define PT_EP_IN            0x81 /* same address as the pad's IN endpoint - only one identity
                                    is ever on the bus, so they cannot collide */
#define PT_REPORT_ID_KBD    1
#define PT_REPORT_ID_MOUSE  2

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(PT_REPORT_ID_KBD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(PT_REPORT_ID_MOUSE)),
};

static const tusb_desc_device_t s_hid_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* Class is declared per interface, not per device, so the host reads the HID interface. */
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4004,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

/*
 * Endpoint interval 1 ms, unlike the pad's 4 ms. Here nothing forces us to copy a real
 * device byte for byte, and the FreeRTOS tick is 1 kHz on this variant, so 1 ms is both
 * expressible and useful: a mouse passed through to the desktop benefits from it directly,
 * whereas a stick position is a filtered value that does not.
 */
#define PT_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_hid_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, PT_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), PT_EP_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 1),
};

static const char *s_hid_strings[] = {
    (const char[]){0x09, 0x04}, /* 0x0409 English (US) */
    "esp32-hid-gamepad-bridge",
    "Bridge passthrough",
    "08FEC93",
};

/* TinyUSB asks for the report descriptor by instance; we declare exactly one. */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

/* No feature reports are implemented - answering zero length is the correct refusal. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)reqlen;
    return 0;
}

/* The host writes keyboard LED state here (Caps Lock and friends). We have no LEDs to set. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)bufsize;
}

/* -------------------------------------------------------- mode switching */

static volatile bool s_want_passthrough;
static bool s_passthrough;

/* Mouse motion that did not fit into a report yet. Carried over rather than dropped, so a
 * fast flick is not truncated by the one-report-in-flight limit. */
static int32_t s_pt_dx, s_pt_dy, s_pt_wheel;
static uint8_t s_pt_buttons;
static uint8_t s_pt_mods;
static uint8_t s_pt_keys[HID_KEYS_MAX];
static bool s_pt_kbd_dirty;

void usb_pad_request_mode(bool passthrough)
{
    s_want_passthrough = passthrough;
}

#endif /* CONFIG_APP_USB_PASSTHROUGH - descriptors, callbacks and the mode request */

/* ------------------------------------------------------------------ installation */

/*
 * The ONLY place that installs the USB device stack, used both at startup and by the identity
 * switch. Deliberately one function: two copies of this configuration would be two descriptions
 * of one thing, which is the exact shape of the bug in AGENTS.md 4.38 - the mapper that was
 * compiled by one condition and started by another.
 */
static esp_err_t install_identity(bool passthrough)
{
#if !CONFIG_APP_USB_PASSTHROUGH
    (void)passthrough;
#endif
    const tinyusb_config_t cfg = {
#if CONFIG_APP_USB_PASSTHROUGH
        .device_descriptor = passthrough ? &s_hid_device_desc : &s_device_desc,
        .string_descriptor = passthrough ? s_hid_strings : s_strings,
        .string_descriptor_count =
            passthrough ? (int)(sizeof(s_hid_strings) / sizeof(s_hid_strings[0]))
                        : (int)(sizeof(s_strings) / sizeof(s_strings[0])),
        .configuration_descriptor = passthrough ? s_hid_config_desc : s_config_desc,
#else
        .device_descriptor = &s_device_desc,
        .string_descriptor = s_strings,
        .string_descriptor_count = (int)(sizeof(s_strings) / sizeof(s_strings[0])),
        .configuration_descriptor = s_config_desc,
#endif
        .external_phy = false,
        .self_powered = false,
    };
    return tinyusb_driver_install(&cfg);
}

#if CONFIG_APP_USB_PASSTHROUGH

bool usb_pad_service_mode(void)
{
    if (s_want_passthrough == s_passthrough) {
        return s_passthrough;
    }

    const bool target = s_want_passthrough;
    ESP_LOGI(TAG, "switching USB identity to %s", target ? "passthrough (HID)" : "gamepad (XInput)");

    /*
     * No neutral report is sent before tearing the pad down, and that is deliberate: a
     * disconnect makes XInput report the controller as gone, so there is no last-known state
     * for a game to act on. A report queued microseconds before the teardown would most likely
     * not reach the host anyway, and pretending otherwise would be misleading.
     */
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_uninstall failed: %s - staying in the current mode",
                 esp_err_to_name(err));
        s_want_passthrough = s_passthrough; /* do not retry every tick */
        return s_passthrough;
    }

    s_mounted = false;
    err = install_identity(target);
    if (err != ESP_OK) {
        /*
         * Both identities must not be down: that would leave the PC with no device at all and
         * no way to ask for one, because the hotkey arrives over the link into this very chip.
         * Fall back to the pad, which is the identity verified on hardware.
         */
        ESP_LOGE(TAG, "install of the %s identity failed: %s - falling back to the pad",
                 target ? "passthrough" : "gamepad", esp_err_to_name(err));
        if (install_identity(false) == ESP_OK) {
            s_passthrough = false;
            s_want_passthrough = false;
        }
        return s_passthrough;
    }

    s_passthrough = target;
    memset(s_pt_keys, 0, sizeof(s_pt_keys));
    s_pt_mods = 0;
    s_pt_buttons = 0;
    s_pt_dx = s_pt_dy = s_pt_wheel = 0;
    s_pt_kbd_dirty = false;
    return s_passthrough;
}

/* ------------------------------------------------------ passthrough reports */

static int8_t clamp8(int32_t v)
{
    if (v > 127) {
        return 127;
    }
    if (v < -127) {
        return -127;
    }
    return (int8_t)v;
}

bool usb_pad_send_passthrough(const hid_input_state_t *state)
{
    /* Accumulate first, so nothing is lost while the endpoint is busy. */
    s_pt_dx += state->mouse_dx;
    s_pt_dy += state->mouse_dy;
    s_pt_wheel += state->mouse_wheel;
    if (state->mouse_buttons != s_pt_buttons) {
        s_pt_buttons = state->mouse_buttons;
    }
    if (state->modifiers != s_pt_mods ||
        memcmp(state->keys, s_pt_keys, sizeof(s_pt_keys)) != 0) {
        s_pt_mods = state->modifiers;
        memcpy(s_pt_keys, state->keys, sizeof(s_pt_keys));
        s_pt_kbd_dirty = true;
    }

    if (!tud_mounted() || !tud_hid_ready()) {
        return false;
    }

    /*
     * One report may be in flight at a time, so the two classes take turns: the keyboard goes
     * first because a keypress is an event that must not wait, while mouse motion is
     * cumulative and loses nothing by being a tick late.
     */
    if (s_pt_kbd_dirty) {
        s_pt_kbd_dirty = false;
        return tud_hid_keyboard_report(PT_REPORT_ID_KBD, s_pt_mods, s_pt_keys);
    }

    static uint8_t last_buttons;
    if (s_pt_dx || s_pt_dy || s_pt_wheel || s_pt_buttons != last_buttons) {
        const int8_t dx = clamp8(s_pt_dx);
        const int8_t dy = clamp8(s_pt_dy);
        const int8_t wheel = clamp8(s_pt_wheel);
        last_buttons = s_pt_buttons;
        if (!tud_hid_mouse_report(PT_REPORT_ID_MOUSE, s_pt_buttons, dx, dy, wheel, 0)) {
            return false;
        }
        /* Subtract only what was actually sent; a clamped remainder rides the next tick. */
        s_pt_dx -= dx;
        s_pt_dy -= dy;
        s_pt_wheel -= wheel;
    }
    return true;
}

#endif /* CONFIG_APP_USB_PASSTHROUGH */

/* ----------------------------------------------------------------------- start */

esp_err_t usb_pad_start(void)
{
    esp_err_t err = install_identity(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB XInput pad up: VID 0x045E PID 0x028E (Xbox 360 wired)");
    ESP_LOGI(TAG, "  config descriptor %u B, report %u B on EP 0x%02x every 4 ms",
             (unsigned)sizeof(s_config_desc), XINPUT_IN_REPORT_LEN, XINPUT_EP_IN);
#if CONFIG_APP_USB_PASSTHROUGH
    ESP_LOGI(TAG, "  passthrough identity available: VID 0x303A PID 0x4004 (HID kbd+mouse), "
                  "hotkey Ctrl+Alt+0x%02x on the input chip",
             CONFIG_APP_PASSTHROUGH_KEYCODE);
#endif
    return ESP_OK;
}
