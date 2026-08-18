#include "ble_gamepad.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "ble_stack.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
#include "xbox_report_map.h"
#endif

static const char *TAG = "gamepad";

#define APPEARANCE_GAMEPAD 0x03C4

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX

/* ------------------------------------------------ profile: Xbox controller */

#define GAMEPAD_REPORT_ID  0x01
#define GAMEPAD_REPORT_LEN XBOX_INPUT_REPORT_LEN

/*
 * Button masks from a real Xbox pad. The descriptor declares 15 buttons
 * (Button 1..15), but the pad uses them with gaps - bits 2, 5, 8 and 9 stay empty.
 * We do not tidy that up, because the goal is compatibility with the original.
 */
#define XBOX_BTN_A     0x0001
#define XBOX_BTN_B     0x0002
#define XBOX_BTN_X     0x0008
#define XBOX_BTN_Y     0x0010
#define XBOX_BTN_LB    0x0040
#define XBOX_BTN_RB    0x0080
#define XBOX_BTN_VIEW  0x0400
#define XBOX_BTN_MENU  0x0800
#define XBOX_BTN_GUIDE 0x1000
#define XBOX_BTN_LS    0x2000
#define XBOX_BTN_RS    0x4000

#define XBOX_TRIGGER_MAX 1023  /* triggers are 10-bit */

/*
 * Device identity. This is what decides whether Windows loads the XInput driver -
 * the report descriptor alone is not enough.
 *
 * PnP ID (characteristic 0x2A50) is 7 bytes: VID source, VID, PID, version.
 * Source 0x02 means "USB Implementers Forum registry", which is what a real pad
 * uses, because VID 0x045E is Microsoft's USB vendor number.
 *
 * THE PID IS NOT ARBITRARY. Windows' XInput driver for BLE (xinputhid.inf, section
 * Btle_Bus = "Bluetooth LE XINPUT compatible input device") binds only to PIDs
 * 0x0B13 and 0x0B20..0x0B27. The Xbox One S pad (model 1708, PID 0x02FD) is NOT on
 * that list - verified on this machine and written up in AGENTS.md 4.32. Hence we
 * impersonate an Xbox Series X pad (model 1914, PID 0x0B13).
 *
 * WHY OUR OWN SERVICE RATHER THAN NimBLE's ble_svc_dis: that implementation hardcodes
 * the first byte as 0x01 (source = Bluetooth SIG) and appends our value as a string,
 * its own default being the six ASCII characters "000000". There is no way to set
 * source 0x02 through it - see AGENTS.md 4.30.
 */
#define DIS_UUID16              0x180A
#define DIS_CHR_UUID16_PNP_ID   0x2A50
#define DIS_CHR_UUID16_MANUF    0x2A29

static const uint8_t s_pnp_id[7] = {
    0x02,        /* Vendor ID Source: USB Implementers Forum      */
    0x5E, 0x04,  /* Vendor ID: 0x045E Microsoft                   */
    0x13, 0x0B,  /* Product ID: 0x0B13 Xbox Series X pad (1914)   */
    0x09, 0x05,  /* Product Version: 0x0509                       */
};

static const char s_manufacturer[] = "Microsoft";

/*
 * Mapping of OUR buttons 1..12 (the ones in the README table) onto Xbox controls.
 * The inputs stay as they are; the translation lives here so that input_mapper does
 * not need to know which profile is active.
 *
 * The choice is a judgement call and can be changed in this one place. It follows how
 * those keys behave in games: left mouse button is fire (right trigger), right mouse
 * button is aim (left trigger), Shift is sprint (left stick click).
 */
enum xbox_ctrl_kind {
    XBOX_CTRL_BUTTON,
    XBOX_CTRL_TRIGGER_L,
    XBOX_CTRL_TRIGGER_R,
};

struct xbox_ctrl {
    enum xbox_ctrl_kind kind;
    uint16_t mask;                  /* used only for XBOX_CTRL_BUTTON */
    const char *name;               /* startup log only */
};

static const struct xbox_ctrl s_xbox_ctrl[12] = {
    /*  1 mouse left   */ { XBOX_CTRL_TRIGGER_R, 0,              "RT" },
    /*  2 mouse right  */ { XBOX_CTRL_TRIGGER_L, 0,              "LT" },
    /*  3 mouse middle */ { XBOX_CTRL_BUTTON,    XBOX_BTN_RS,    "RS" },
    /*  4 Space        */ { XBOX_CTRL_BUTTON,    XBOX_BTN_A,     "A" },
    /*  5 LShift       */ { XBOX_CTRL_BUTTON,    XBOX_BTN_LS,    "LS" },
    /*  6 LCtrl        */ { XBOX_CTRL_BUTTON,    XBOX_BTN_B,     "B" },
    /*  7 E            */ { XBOX_CTRL_BUTTON,    XBOX_BTN_X,     "X" },
    /*  8 Q            */ { XBOX_CTRL_BUTTON,    XBOX_BTN_Y,     "Y" },
    /*  9 R            */ { XBOX_CTRL_BUTTON,    XBOX_BTN_LB,    "LB" },
    /* 10 F            */ { XBOX_CTRL_BUTTON,    XBOX_BTN_RB,    "RB" },
    /* 11 Tab          */ { XBOX_CTRL_BUTTON,    XBOX_BTN_VIEW,  "View" },
    /* 12 Esc          */ { XBOX_CTRL_BUTTON,    XBOX_BTN_MENU,  "Menu" },
};

#define s_report_map     xbox_report_map
#define REPORT_MAP_BYTES sizeof(xbox_report_map)

#else /* CONFIG_APP_GAMEPAD_PROFILE_GENERIC */

/* -------------------------------------- profile: generic DirectInput gamepad */

#define GAMEPAD_REPORT_ID  1
#define GAMEPAD_REPORT_LEN 6

/*
 * HID report descriptor for the generic pad: 4 signed 8-bit axes + 12 buttons.
 *
 * Axes X/Y (left stick) and Z/Rz (right stick) - the layout typical DirectInput pads
 * use, so games recognise it. joy.cpl draws the crosshair from the first pair (X/Y)
 * and shows Z and Rz as two separate sliders.
 *
 * NOTE: any change to this table requires removing the pad from the Windows Bluetooth
 * device list and pairing again - Windows caches the Report Map per bond
 * (AGENTS.md 4.7).
 */
static const uint8_t s_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)      */
    0x09, 0x05,       /* Usage (Game Pad)                  */
    0xA1, 0x01,       /* Collection (Application)          */
    0x85, GAMEPAD_REPORT_ID, /*   Report ID (1)            */

    0x05, 0x01,       /*   Usage Page (Generic Desktop)    */
    0x09, 0x01,       /*   Usage (Pointer)                 */
    0xA1, 0x00,       /*   Collection (Physical)           */
    0x09, 0x30,       /*     Usage (X)                     */
    0x09, 0x31,       /*     Usage (Y)                     */
    0x09, 0x32,       /*     Usage (Z)                     */
    0x09, 0x35,       /*     Usage (Rz)                    */
    0x15, 0x81,       /*     Logical Minimum (-127)        */
    0x25, 0x7F,       /*     Logical Maximum (127)         */
    0x75, 0x08,       /*     Report Size (8)               */
    0x95, 0x04,       /*     Report Count (4)              */
    0x81, 0x02,       /*     Input (Data, Var, Abs)        */
    0xC0,             /*   End Collection                  */

    0x05, 0x09,       /*   Usage Page (Button)             */
    0x19, 0x01,       /*   Usage Minimum (Button 1)        */
    0x29, 0x0C,       /*   Usage Maximum (Button 12)       */
    0x15, 0x00,       /*   Logical Minimum (0)             */
    0x25, 0x01,       /*   Logical Maximum (1)             */
    0x75, 0x01,       /*   Report Size (1)                 */
    0x95, 0x0C,       /*   Report Count (12)               */
    0x81, 0x02,       /*   Input (Data, Var, Abs)          */

    0x75, 0x01,       /*   Report Size (1)                 */
    0x95, 0x04,       /*   Report Count (4)                */
    0x81, 0x03,       /*   Input (Const, Var, Abs) - padding to 6 bytes */
    0xC0              /* End Collection                    */
};

#define REPORT_MAP_BYTES sizeof(s_report_map)

#endif /* CONFIG_APP_GAMEPAD_PROFILE_* */

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;
static bool s_subscribed;
static uint8_t s_last_report[GAMEPAD_REPORT_LEN];
static bool s_have_last;

/* ------------------------------------------------------------------- GATT */

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
/*
 * Our own Device Information service (0x180A) with the PnP ID. NimBLE's ble_svc_dis is
 * not used because it cannot set the VID source to 0x02 (AGENTS.md 4.30).
 *
 * NimBLE's own service is not registered in this build - ble_svc_dis_init() is called
 * only from nimble_hidd.c, which we do not use - so there is no risk of ending up with
 * two 0x180A services.
 */
static int dis_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const void *data;
    uint16_t len;

    switch (ble_uuid_u16(ctxt->chr->uuid)) {
    case DIS_CHR_UUID16_PNP_ID:
        data = s_pnp_id;
        len = sizeof(s_pnp_id);
        break;
    case DIS_CHR_UUID16_MANUF:
        data = s_manufacturer;
        len = (uint16_t)strlen(s_manufacturer);
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }

    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_dis_defs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(DIS_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(DIS_CHR_UUID16_PNP_ID),
                .access_cb = dis_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = BLE_UUID16_DECLARE(DIS_CHR_UUID16_MANUF),
                .access_cb = dis_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                0, /* end of characteristic list */
            },
        },
    },
    {
        0, /* end of service list */
    },
};

static esp_err_t register_dis_service(void)
{
    int rc = ble_gatts_count_cfg(s_dis_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg (DIS): rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_dis_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs (DIS): rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PnP ID: source=0x%02x VID=0x%04x PID=0x%04x version=0x%04x",
             s_pnp_id[0],
             (unsigned)(s_pnp_id[1] | (s_pnp_id[2] << 8)),
             (unsigned)(s_pnp_id[3] | (s_pnp_id[4] << 8)),
             (unsigned)(s_pnp_id[5] | (s_pnp_id[6] << 8)));
    return ESP_OK;
}
#endif /* CONFIG_APP_GAMEPAD_PROFILE_XBOX */

/* ---------------------------------------------------------- own HID service */

/*
 * WHY NOT NimBLE's ble_svc_hid, which this project used up to the generic-pad stage:
 *
 * struct ble_svc_hid_params stores the Report Map length in a uint8_t field even though
 * the buffer itself is 512 B - and that is exactly the field it feeds to the host:
 *
 *   os_mbuf_append(ctxt->om, &hid_instances[i].report_map,
 *                  hid_instances[i].report_map_len);      <- uint8_t
 *
 * The Xbox pad descriptor is 334 B, so Windows would receive 78 B (334 modulo 256):
 * a truncated, meaningless Report Map. The compiler caught it as
 * "conversion ... changes value from '334' to '78'". The bt component cannot be forked
 * the way esp_hid was (it ships precompiled controller libraries), so the service is
 * written directly on GATT here - AGENTS.md 4.30.
 *
 * Welcome side effect: characteristic handles arrive directly through val_handle, with
 * no guessing which of several characteristics with UUID 0x2A4D is ours.
 */
#define HID_UUID16                 0x1812
#define HID_CHR_UUID16_HID_INFO    0x2A4A
#define HID_CHR_UUID16_REPORT_MAP  0x2A4B
#define HID_CHR_UUID16_CTRL_PT     0x2A4C
#define HID_CHR_UUID16_REPORT      0x2A4D
#define HID_CHR_UUID16_PROTO_MODE  0x2A4E
#define HID_DSC_UUID16_RPT_REF     0x2908

#define HID_RPT_TYPE_INPUT   0x01
#define HID_RPT_TYPE_OUTPUT  0x02

/* HID Information: bcdHID=0x0111, country=0, flags=0x02 (NormallyConnectable).
 * Little-endian on the wire, i.e. exactly these bytes. */
static const uint8_t s_hid_info[4] = {0x11, 0x01, 0x00, 0x02};

/* Protocol Mode: 1 = Report (not Boot). Windows may overwrite it. */
static uint8_t s_proto_mode = 0x01;

#define HID_RPT_MAX_LEN 16

/*
 * One entry per Report characteristic. The last value is kept here because the host
 * is allowed to read a report, not just receive notifications.
 */
struct hid_report_def {
    uint8_t id;
    uint8_t type;
    uint8_t len;
    uint8_t data[HID_RPT_MAX_LEN];
};

static struct hid_report_def s_rpt_input = {
    .id = GAMEPAD_REPORT_ID,
    .type = HID_RPT_TYPE_INPUT,
    .len = GAMEPAD_REPORT_LEN,
};

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
/*
 * This descriptor declares two reports: the pad input report and the rumble output
 * report. The IDs and lengths are NOT written out as numbers here - they are filled in
 * by register_hid_service() from the xbox_reports[] table, which the generator computed
 * straight from the descriptor. Otherwise the code could silently drift from the
 * Report Map.
 */
static struct hid_report_def s_rpt_rumble;
#endif

static int hid_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    struct hid_report_def *rpt = (struct hid_report_def *)arg;
    uint8_t buf;
    uint16_t got;

    switch (ble_uuid_u16(ctxt->chr->uuid)) {
    case HID_CHR_UUID16_REPORT_MAP:
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        /* This is the whole point NimBLE's service could not deliver: the full length. */
        return os_mbuf_append(ctxt->om, s_report_map, REPORT_MAP_BYTES) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

    case HID_CHR_UUID16_HID_INFO:
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

    case HID_CHR_UUID16_PROTO_MODE:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, &s_proto_mode, 1) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            if (ble_hs_mbuf_to_flat(ctxt->om, &buf, 1, &got) == 0 && got == 1) {
                s_proto_mode = buf;
                ESP_LOGI(TAG, "PC set Protocol Mode = %u (1 = Report)", buf);
            }
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;

    case HID_CHR_UUID16_CTRL_PT:
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_hs_mbuf_to_flat(ctxt->om, &buf, 1, &got) == 0 && got == 1) {
            ESP_LOGI(TAG, "HID Control Point: %u (0 = suspend, 1 = exit suspend)", buf);
        }
        return 0;

    case HID_CHR_UUID16_REPORT:
        if (rpt == NULL) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, rpt->data, rpt->len) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            got = 0;
            ble_hs_mbuf_to_flat(ctxt->om, rpt->data, rpt->len, &got);
            /*
             * On an Xbox pad the output report is a rumble command. We have no motors,
             * but this log is valuable in itself: if Windows writes here, it is
             * handling us with its own pad driver rather than as a plain HID device.
             */
            ESP_LOGI(TAG, "output report id=%u (%u B): %02x %02x %02x %02x %02x %02x %02x %02x",
                     rpt->id, got, rpt->data[0], rpt->data[1], rpt->data[2], rpt->data[3],
                     rpt->data[4], rpt->data[5], rpt->data[6], rpt->data[7]);
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* Report Reference (0x2908): two bytes - the report ID and its type.
 * Without it the host cannot tell which characteristic maps to which Report ID. */
static int hid_rpt_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const struct hid_report_def *rpt = (const struct hid_report_def *)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC || rpt == NULL) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const uint8_t ref[2] = {rpt->id, rpt->type};
    return os_mbuf_append(ctxt->om, ref, sizeof(ref)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* The HOGP profile requires encryption on the HID characteristics. Windows encrypts
 * the link anyway, but we declare it explicitly because a real device does. */
#define HID_RPT_REF_DSC(rptdef)                                    \
    (struct ble_gatt_dsc_def[])                                    \
    {                                                              \
        {                                                          \
            .uuid = BLE_UUID16_DECLARE(HID_DSC_UUID16_RPT_REF),    \
            .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,      \
            .access_cb = hid_rpt_ref_access,                       \
            .arg = (void *)&(rptdef),                              \
        },                                                         \
        {                                                          \
            0,                                                     \
        },                                                         \
    }

static const struct ble_gatt_svc_def s_hid_defs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(HID_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(HID_CHR_UUID16_REPORT_MAP),
                .access_cb = hid_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            }, {
                .uuid = BLE_UUID16_DECLARE(HID_CHR_UUID16_HID_INFO),
                .access_cb = hid_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = BLE_UUID16_DECLARE(HID_CHR_UUID16_CTRL_PT),
                .access_cb = hid_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            }, {
                .uuid = BLE_UUID16_DECLARE(HID_CHR_UUID16_PROTO_MODE),
                .access_cb = hid_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
            }, {
                /* The pad input report - this is the one we notify. */
                .uuid = BLE_UUID16_DECLARE(HID_CHR_UUID16_REPORT),
                .access_cb = hid_access,
                .arg = (void *)&s_rpt_input,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &s_report_handle,
                .descriptors = HID_RPT_REF_DSC(s_rpt_input),
            },
#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
            {
                /* Wibracje - tutaj pisze sterownik pada Xbox. */
                .uuid = BLE_UUID16_DECLARE(HID_CHR_UUID16_REPORT),
                .access_cb = hid_access,
                .arg = (void *)&s_rpt_rumble,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_WRITE_ENC,
                .descriptors = HID_RPT_REF_DSC(s_rpt_rumble),
            },
#endif
            {
                0, /* end of characteristic list */
            },
        },
    },
    {
        0, /* end of service list */
    },
};

static esp_err_t register_hid_service(void)
{
#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
    /*
     * Report IDs and lengths come from the table the generator computed straight from
     * the descriptor, so the code cannot drift away from it. If the descriptor ever
     * declares a different set of reports we want to know at startup, not from the
     * way Windows behaves.
     */
    const size_t nrpts = sizeof(xbox_reports) / sizeof(xbox_reports[0]);
    if (nrpts != 2 || xbox_reports[0].type != HID_RPT_TYPE_INPUT ||
        xbox_reports[1].type != HID_RPT_TYPE_OUTPUT) {
        ESP_LOGE(TAG, "descriptor declares %u reports in an unexpected layout",
                 (unsigned)nrpts);
        return ESP_ERR_INVALID_STATE;
    }
    if (xbox_reports[0].len > HID_RPT_MAX_LEN || xbox_reports[1].len > HID_RPT_MAX_LEN) {
        ESP_LOGE(TAG, "report longer than the %d B buffer", HID_RPT_MAX_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    s_rpt_input.id = xbox_reports[0].id;
    s_rpt_input.type = xbox_reports[0].type;
    s_rpt_input.len = xbox_reports[0].len;

    s_rpt_rumble.id = xbox_reports[1].id;
    s_rpt_rumble.type = xbox_reports[1].type;
    s_rpt_rumble.len = xbox_reports[1].len;
#endif

    int rc = ble_gatts_count_cfg(s_hid_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg (HID): rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_hid_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs (HID): rc=%d", rc);
        return ESP_FAIL;
    }

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
    ESP_LOGI(TAG, "HID service: Report Map %u B, reports 0x%02x INPUT %u B + 0x%02x OUTPUT %u B",
             (unsigned)REPORT_MAP_BYTES, s_rpt_input.id, s_rpt_input.len,
             s_rpt_rumble.id, s_rpt_rumble.len);
#else
    ESP_LOGI(TAG, "HID service: Report Map %u B, input report 0x%02x %u B",
             (unsigned)REPORT_MAP_BYTES, s_rpt_input.id, s_rpt_input.len);
#endif
    return ESP_OK;
}

/* ------------------------------------------------------------ advertising */

static int gap_event(struct ble_gap_event *event, void *arg);

static esp_err_t start_advertising(void)
{
    /*
     * Split between ADV and SCAN_RSP: 31 bytes of ADV cannot be guaranteed to hold the
     * name together with flags, UUID and appearance (and the name length comes from
     * Kconfig). The name therefore goes into the scan response - Windows scans actively
     * and will read it anyway.
     */
    struct ble_hs_adv_fields adv = {0};
    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(HID_UUID16);

    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids16 = &hid_uuid;
    adv.num_uuids16 = 1;
    adv.uuids16_is_complete = 1;
    adv.appearance = APPEARANCE_GAMEPAD;
    adv.appearance_is_present = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields: rc=%d", rc);
        return ESP_FAIL;
    }

    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields: rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(ble_stack_own_addr_type(), NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start: rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "advertising as '%s' (appearance 0x%04x)", name, APPEARANCE_GAMEPAD);
    return ESP_OK;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed, status=%d - back to advertising",
                     event->connect.status);
            start_advertising();
            return 0;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0) {
            return 0;
        }
        /*
         * The check that nimble_hidd.c is missing (AGENTS.md 4.2). This callback is tied
         * to our own advertising, so the role should be SLAVE anyway, but the guard stays
         * in case it is ever registered globally.
         */
        if (desc.role != BLE_GAP_ROLE_SLAVE) {
            ESP_LOGW(TAG, "ignoring connection in role %d (not the PC)", desc.role);
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        s_subscribed = false;
        s_have_last = false;
        ESP_LOGI(TAG, "PC connected, conn_handle=%u", s_conn_handle);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "PC disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_have_last = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_report_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "PC %s report notifications",
                     s_subscribed ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        /* The outcome of our request for a shorter interval. Windows is the central on
         * this link, so we can only ask - this shows what it granted. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
            ESP_LOGI(TAG, "link do PC: status=%d itvl=%u (%u.%02u ms = %u Hz) latency=%u timeout=%u",
                     event->conn_update.status, d.conn_itvl,
                     (unsigned)(d.conn_itvl * 125 / 100), (unsigned)(d.conn_itvl * 125 % 100),
                     (unsigned)(1000 * 100 / (d.conn_itvl * 125)),
                     d.conn_latency, d.supervision_timeout);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption/pairing: status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ wysylka */

bool ble_gamepad_is_ready(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_subscribed;
}

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX

/* Xbox pad axes are UNSIGNED 16-bit (descriptor: logical 0..65535). Scaled from our
 * -127..127 range; the added 127 rounds so that zero lands exactly on 32768 and the
 * range ends on 0 and 65535. */
static uint16_t axis_to_u16(int8_t v)
{
    int32_t t = (int32_t)v + 127;   /* 0..254 */
    return (uint16_t)((t * 65535 + 127) / 254);
}

/*
 * Xbox pad hat switch: 0 = centred, then 1..8 clockwise from the top
 * (N, NE, E, SE, S, SW, W, NW). The descriptor declares it as a 4-bit field with a
 * Null State, so 0 means "no direction".
 *
 * A table rather than a chain of conditions, because there are eight combinations plus
 * the cases where opposite directions are held. Those cancel out - otherwise the result
 * would depend on the order the conditions are checked in. Index: bit0 up, bit1 right,
 * bit2 down, bit3 left.
 */
static uint8_t hat_from_dpad(uint8_t dpad)
{
    static const uint8_t hat[16] = {
        0, /* ----  none          */
        1, /* U     up            */
        3, /* R     right         */
        2, /* UR    up-right      */
        5, /* D     down          */
        0, /* UD    cancel out    */
        4, /* RD    down-right    */
        3, /* URD   right wins    */
        7, /* L     left          */
        8, /* UL    up-left       */
        0, /* RL    cancel out    */
        1, /* URL   up wins       */
        6, /* DL    down-left     */
        7, /* UDL   left wins     */
        5, /* RDL   down wins     */
        0, /* URDL  cancel out    */
    };
    return hat[dpad & 0x0F];
}

static void build_report(const gamepad_state_t *state, uint8_t *rpt)
{
    uint16_t buttons = 0;
    uint16_t trigger_l = 0;
    uint16_t trigger_r = 0;

    for (unsigned i = 0; i < 12; i++) {
        if ((state->buttons & (1u << i)) == 0) {
            continue;
        }
        switch (s_xbox_ctrl[i].kind) {
        case XBOX_CTRL_BUTTON:
            buttons |= s_xbox_ctrl[i].mask;
            break;
        case XBOX_CTRL_TRIGGER_L:
            trigger_l = XBOX_TRIGGER_MAX;
            break;
        case XBOX_CTRL_TRIGGER_R:
            trigger_r = XBOX_TRIGGER_MAX;
            break;
        }
    }

    const uint16_t x = axis_to_u16(state->lx);
    const uint16_t y = axis_to_u16(state->ly);
    const uint16_t z = axis_to_u16(state->rx);
    const uint16_t rz = axis_to_u16(state->ry);

    /* Uklad 16 bajtow zgodny z deskryptorem, wszystko little-endian:
     * X, Y, Z, Rz, hamulec (LT), gaz (RT), hat, przyciski, przycisk Share. */
    rpt[0]  = (uint8_t)(x & 0xFF);
    rpt[1]  = (uint8_t)(x >> 8);
    rpt[2]  = (uint8_t)(y & 0xFF);
    rpt[3]  = (uint8_t)(y >> 8);
    rpt[4]  = (uint8_t)(z & 0xFF);
    rpt[5]  = (uint8_t)(z >> 8);
    rpt[6]  = (uint8_t)(rz & 0xFF);
    rpt[7]  = (uint8_t)(rz >> 8);
    rpt[8]  = (uint8_t)(trigger_l & 0xFF);
    rpt[9]  = (uint8_t)(trigger_l >> 8);   /* the top 6 bits are padding */
    rpt[10] = (uint8_t)(trigger_r & 0xFF);
    rpt[11] = (uint8_t)(trigger_r >> 8);
    rpt[12] = hat_from_dpad(state->dpad);   /* D-pad, 0 = centred */
    rpt[13] = (uint8_t)(buttons & 0xFF);
    rpt[14] = (uint8_t)((buttons >> 8) & 0x7F);
    rpt[15] = 0;                            /* przycisk Share */
}

#else /* profil generyczny */

static void build_report(const gamepad_state_t *state, uint8_t *rpt)
{
    rpt[0] = (uint8_t)state->lx;
    rpt[1] = (uint8_t)state->ly;
    rpt[2] = (uint8_t)state->rx;
    rpt[3] = (uint8_t)state->ry;
    rpt[4] = (uint8_t)(state->buttons & 0xFF);
    rpt[5] = (uint8_t)((state->buttons >> 8) & 0x0F);
}

#endif

bool ble_gamepad_send(const gamepad_state_t *state)
{
    uint8_t rpt[GAMEPAD_REPORT_LEN];
    build_report(state, rpt);

    if (!ble_gamepad_is_ready()) {
        return false;
    }
    /* Only on a state change - at 100 Hz we would otherwise flood the link with
     * identical reports. */
    if (s_have_last && memcmp(rpt, s_last_report, sizeof(rpt)) == 0) {
        return false;
    }

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
    /*
     * Log only the "digital" part of the report - triggers, hat and buttons (bytes 8..15).
     * Not the axes: during mouse movement they change a dozen times a second and would
     * flood the console. This makes it directly visible whether a mouse click went out as
     * a trigger: in joy.cpl triggers are NOT buttons, so otherwise it would be guesswork.
     */
    if (!s_have_last || memcmp(rpt + 8, s_last_report + 8, GAMEPAD_REPORT_LEN - 8) != 0) {
        ESP_LOGI(TAG, "xbox: LT=%u RT=%u hat=%u btn=0x%04x share=%u",
                 (unsigned)(rpt[8] | (rpt[9] << 8)),
                 (unsigned)(rpt[10] | (rpt[11] << 8)),
                 rpt[12],
                 (unsigned)(rpt[13] | (rpt[14] << 8)),
                 rpt[15]);
    }
#endif

    struct os_mbuf *om = ble_hs_mbuf_from_flat(rpt, sizeof(rpt));
    if (om == NULL) {
        ESP_LOGW(TAG, "no mbuf available for the report");
        return false;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_report_handle, om);
    if (rc != 0) {
        /* the stack frees the mbuf regardless of the result */
        ESP_LOGW(TAG, "ble_gatts_notify_custom: rc=%d", rc);
        return false;
    }

    memcpy(s_last_report, rpt, sizeof(rpt));
    s_have_last = true;
    /* The host may read the report, not just receive notifications - so the last value
     * is kept behind the characteristic. */
    memcpy(s_rpt_input.data, rpt, sizeof(rpt));
    return true;
}

/* ---------------------------------------------------------------- selftest */

#if CONFIG_APP_GAMEPAD_SELFTEST
/* Eight steps around a circle of radius 100 - enough to see movement in joy.cpl, and
 * it needs no floating-point arithmetic. */
static const int8_t s_circle_x[8] = {100, 71, 0, -71, -100, -71, 0, 71};
static const int8_t s_circle_y[8] = {0, 71, 100, 71, 0, -71, -100, -71};

static void selftest_step(uint32_t tick)
{
    gamepad_state_t st = {0};
    uint32_t phase = (tick / 6) % 8;   /* full circle in ~0.5 s at 100 Hz */
    st.lx = s_circle_x[phase];
    st.ly = s_circle_y[phase];
    st.rx = s_circle_x[(phase + 2) % 8];
    st.ry = s_circle_y[(phase + 2) % 8];
    /* One button at a time, changing once per second. */
    st.buttons = (uint16_t)(1u << ((tick / CONFIG_APP_REPORT_RATE_HZ) % 12));
    ble_gamepad_send(&st);
}
#endif

/*
 * Request a shorter connection interval on the link to the PC.
 *
 * This link is the NARROWEST POINT of the whole chain: the mouse can report as often as
 * it likes, but only what fits through here reaches the game. And until now we never
 * asked for anything on it - we simply accepted whatever Windows chose.
 *
 * Difference from the central side: here Windows is the central, so we cannot dictate
 * parameters. ble_gap_update_params() then sends either an LL Connection Parameters
 * Request or - if the peer does not support that - an L2CAP request. The answer arrives
 * ASYNCHRONOUSLY as BLE_GAP_EVENT_CONN_UPDATE, so rc == 0 only means "sent", not
 * "accepted". That is why we read back what was actually applied after every attempt.
 *
 * Starting point: a real Xbox pad does 125 Hz over BT, so Windows is capable of holding
 * a 7.5 ms link with a device of this class. We check whether it will grant us one.
 */
static void negotiate_pad_interval(void)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn_handle, &d) != 0) {
        return;
    }

    ESP_LOGI(TAG, "link to PC: interval %u (%u.%02u ms = %u Hz), supervision timeout=%u",
             d.conn_itvl, (unsigned)(d.conn_itvl * 125 / 100),
             (unsigned)(d.conn_itvl * 125 % 100),
             (unsigned)(1000 * 100 / (d.conn_itvl * 125)), d.supervision_timeout);

    /*
     * Right after enabling notifications Windows is still finishing its own procedures
     * (feature exchange, its own parameter update). A request sent at that moment comes
     * back with HCI status 0x2A (Different Transaction Collision) - measured on hardware.
     * So we let it finish before asking for anything.
     */
    vTaskDelay(pdMS_TO_TICKS(1500));

    const uint16_t ladder[] = {CONFIG_APP_PAD_CONN_ITVL, 8, 10, 12};
    /* A procedure collision is transient, so each value is tried a few times. */
    const int tries_per_step = 3;

    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        const uint16_t itvl = ladder[i];
        if (i > 0 && itvl <= CONFIG_APP_PAD_CONN_ITVL) {
            continue; /* already tried as the Kconfig value */
        }

        struct ble_gap_conn_desc cur;
        if (ble_gap_conn_find(s_conn_handle, &cur) != 0) {
            return; /* link dropped */
        }
        if (itvl >= cur.conn_itvl) {
            break; /* longer than the current one makes no sense */
        }

        for (int t = 0; t < tries_per_step; t++) {
            const struct ble_gap_upd_params params = {
                .itvl_min = itvl,
                .itvl_max = itvl,
                .latency = 0,
                .supervision_timeout = cur.supervision_timeout,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            int rc = ble_gap_update_params(s_conn_handle, &params);
            if (rc != 0) {
                /* Refused by OUR side (host or the C3 controller), before the air. */
                ESP_LOGW(TAG, "  %u.%02u ms: our side did not send the request, rc=%d (HCI 0x%02x)",
                         (unsigned)(itvl * 125 / 100), (unsigned)(itvl * 125 % 100),
                         rc, rc >= 0x200 ? (unsigned)(rc - 0x200) : 0u);
                break; /* retrying the same value would change nothing */
            }

            ESP_LOGI(TAG, "  %u.%02u ms: request %d/%d sent, waiting for Windows to decide",
                     (unsigned)(itvl * 125 / 100), (unsigned)(itvl * 125 % 100),
                     t + 1, tries_per_step);
            vTaskDelay(pdMS_TO_TICKS(2500));

            struct ble_gap_conn_desc now;
            if (ble_gap_conn_find(s_conn_handle, &now) != 0) {
                return; /* link padl w trakcie */
            }
            if (now.conn_itvl <= itvl) {
                ESP_LOGI(TAG, "  UZYSKANE: interwal %u (%u.%02u ms = %u Hz)",
                         now.conn_itvl, (unsigned)(now.conn_itvl * 125 / 100),
                         (unsigned)(now.conn_itvl * 125 % 100),
                         (unsigned)(1000 * 100 / (now.conn_itvl * 125)));
                return;
            }
        }
        ESP_LOGW(TAG, "  %u.%02u ms: Windows did not shorten the interval after %d attempts",
                 (unsigned)(itvl * 125 / 100), (unsigned)(itvl * 125 % 100), tries_per_step);
    }

    struct ble_gap_conn_desc fin;
    if (ble_gap_conn_find(s_conn_handle, &fin) == 0) {
        ESP_LOGW(TAG, "  keeping interval %u (%u.%02u ms = %u Hz)",
                 fin.conn_itvl, (unsigned)(fin.conn_itvl * 125 / 100),
                 (unsigned)(fin.conn_itvl * 125 % 100),
                 (unsigned)(1000 * 100 / (fin.conn_itvl * 125)));
    }
}

static void gamepad_task(void *arg)
{
    ble_stack_wait_synced(portMAX_DELAY);

    /*
     * NimBLE fills the handle in through the val_handle pointer when GATT starts, i.e.
     * after the stack has synced. There is no need to search for it (which was the case
     * when ble_svc_hid provided the service) - checking that it exists is enough.
     */
    if (s_report_handle == 0) {
        ESP_LOGE(TAG, "the report got no handle - the pad is pointless, ending the task");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Report characteristic has handle %u", s_report_handle);
    start_advertising();

    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_APP_REPORT_RATE_HZ);
    uint32_t tick = 0;
    bool itvl_negotiated = false;
    while (true) {
        vTaskDelay(period > 0 ? period : 1);
        tick++;

        /*
         * Only after the PC enables notifications - by then the link is established and
         * encrypted, and Windows has finished service discovery. Once per connection.
         */
        if (ble_gamepad_is_ready() && !itvl_negotiated) {
            itvl_negotiated = true;
            negotiate_pad_interval();
        }
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            itvl_negotiated = false;
        }
#if CONFIG_APP_GAMEPAD_SELFTEST
        selftest_step(tick);
#endif
    }
}

esp_err_t ble_gamepad_start(void)
{
    int rc = ble_svc_gap_device_name_set(CONFIG_APP_GAMEPAD_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set: rc=%d", rc);
        return ESP_FAIL;
    }

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
    /* Tozsamosc musi byc zarejestrowana przed startem GATT, tak jak usluga HID. */
    esp_err_t dis_err = register_dis_service();
    if (dis_err != ESP_OK) {
        return dis_err;
    }
#endif

    esp_err_t err = register_hid_service();
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(gamepad_task, "gamepad", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
    ESP_LOGI(TAG, "profile: Xbox Series X pad (XInput), PID 0x%02x%02x",
             s_pnp_id[4], s_pnp_id[3]);
    /* The mapping goes into the log, otherwise every test means digging through code. */
    ESP_LOGI(TAG, "buttons -> Xbox: 1=%s 2=%s 3=%s 4=%s 5=%s 6=%s",
             s_xbox_ctrl[0].name, s_xbox_ctrl[1].name, s_xbox_ctrl[2].name,
             s_xbox_ctrl[3].name, s_xbox_ctrl[4].name, s_xbox_ctrl[5].name);
    ESP_LOGI(TAG, "               7=%s 8=%s 9=%s 10=%s 11=%s 12=%s",
             s_xbox_ctrl[6].name, s_xbox_ctrl[7].name, s_xbox_ctrl[8].name,
             s_xbox_ctrl[9].name, s_xbox_ctrl[10].name, s_xbox_ctrl[11].name);
#else
    ESP_LOGI(TAG, "profile: generic HID pad (DirectInput)");
#endif
    return ESP_OK;
}
