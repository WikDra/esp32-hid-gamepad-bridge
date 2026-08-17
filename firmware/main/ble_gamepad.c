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

/* ------------------------------------------------- profil: pad Xbox One S */

#define GAMEPAD_REPORT_ID  0x01
#define GAMEPAD_REPORT_LEN XBOX_INPUT_REPORT_LEN

/*
 * Maski przyciskow z prawdziwego pada Xbox. Deskryptor deklaruje 15 przyciskow
 * (Button 1..15), ale pad uzywa ich z dziurami - bity 2, 5, 8 i 9 zostaja puste.
 * Nie wygladzamy tego, bo celem jest zgodnosc z oryginalem.
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

#define XBOX_TRIGGER_MAX 1023  /* spusty sa 10-bitowe */

/*
 * Tozsamosc urzadzenia. To ona decyduje, czy Windows zaladuje sterownik XInput -
 * deskryptor sam nie wystarcza.
 *
 * PnP ID (charakterystyka 0x2A50) ma 7 bajtow: zrodlo VID, VID, PID, wersja.
 * Zrodlo 0x02 znaczy "rejestr USB Implementers Forum" i tak wlasnie jest
 * w padzie, bo VID 0x045E to numer USB Microsoftu.
 *
 * PID NIE JEST DOWOLNY. Sterownik XInput dla BLE w Windows (xinputhid.inf, sekcja
 * Btle_Bus = "Bluetooth LE XINPUT compatible input device") wiaze sie wylacznie
 * z PID 0x0B13 oraz 0x0B20..0x0B27. Pada Xbox One S (model 1708, PID 0x02FD) na
 * tej liscie NIE MA - sprawdzone na tej maszynie i opisane w AGENTS.md 4.32.
 * Dlatego udajemy pada Xbox Series X (model 1914, PID 0x0B13).
 *
 * DLACZEGO WLASNA USLUGA, A NIE ble_svc_dis Z NimBLE: tamta implementacja
 * wpisuje pierwszy bajt na sztywno jako 0x01 (zrodlo = Bluetooth SIG) i dokleja
 * do niego nasza wartosc jako string, a jej domyslna wartosc to szesc znakow
 * ASCII "000000". Nie da sie przez nia ustawic zrodla 0x02 - patrz AGENTS.md 4.30.
 */
#define DIS_UUID16              0x180A
#define DIS_CHR_UUID16_PNP_ID   0x2A50
#define DIS_CHR_UUID16_MANUF    0x2A29

static const uint8_t s_pnp_id[7] = {
    0x02,        /* Vendor ID Source: USB Implementers Forum   */
    0x5E, 0x04,  /* Vendor ID: 0x045E Microsoft                */
    0x13, 0x0B,  /* Product ID: 0x0B13 pad Xbox Series X (1914) */
    0x09, 0x05,  /* Product Version: 0x0509                    */
};

static const char s_manufacturer[] = "Microsoft";

/*
 * Mapowanie NASZYCH przyciskow 1..12 (te z tabeli w README) na kontrolki pada
 * Xbox. Wejscia zostaja bez zmian, tlumaczenie jest tutaj, zeby input_mapper
 * nie musial wiedziec, jaki profil jest aktywny.
 *
 * Wybor jest moj i da sie go zmienic w jednym miejscu. Kierowalem sie tym, jak
 * te klawisze dzialaja w grach: lewy przycisk myszy to strzal (prawy spust),
 * prawy to celowanie (lewy spust), Shift to sprint (klik lewej galki).
 */
enum xbox_ctrl_kind {
    XBOX_CTRL_BUTTON,
    XBOX_CTRL_TRIGGER_L,
    XBOX_CTRL_TRIGGER_R,
};

struct xbox_ctrl {
    enum xbox_ctrl_kind kind;
    uint16_t mask;                  /* uzywane tylko dla XBOX_CTRL_BUTTON */
    const char *name;               /* tylko do logu przy starcie */
};

static const struct xbox_ctrl s_xbox_ctrl[12] = {
    /*  1 mysz lewy    */ { XBOX_CTRL_TRIGGER_R, 0,              "RT" },
    /*  2 mysz prawy   */ { XBOX_CTRL_TRIGGER_L, 0,              "LT" },
    /*  3 mysz srodk.  */ { XBOX_CTRL_BUTTON,    XBOX_BTN_RS,    "RS" },
    /*  4 Spacja       */ { XBOX_CTRL_BUTTON,    XBOX_BTN_A,     "A" },
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

/* --------------------------------------- profil: generyczny pad DirectInput */

#define GAMEPAD_REPORT_ID  1
#define GAMEPAD_REPORT_LEN 6

/*
 * Deskryptor raportu HID pada: 4 osie 8-bitowe ze znakiem + 12 przyciskow.
 *
 * Osie X/Y (lewy analog) i Z/Rz (prawy analog) - taki uklad uzywaja typowe pady
 * DirectInput, wiec joy.cpl pokaze krzyzyk (X/Y) oraz suwaki "os Z" i "obrot Z".
 *
 * UWAGA: kazda zmiana tej tablicy wymaga usuniecia pada z listy urzadzen
 * Bluetooth w Windows i sparowania na nowo - Windows cache'uje Report Map
 * per bond (AGENTS.md 4.7).
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
    0x81, 0x03,       /*   Input (Const, Var, Abs) - dopelnienie do 6 bajtow */
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
 * Wlasna usluga Device Information (0x180A) z PnP ID. Nie uzywamy ble_svc_dis
 * z NimBLE, bo ta nie potrafi ustawic zrodla VID na 0x02 (AGENTS.md 4.30).
 *
 * Usluga z NimBLE nie jest w naszym buildzie zarejestrowana - ble_svc_dis_init()
 * wola wylacznie nimble_hidd.c, ktorego nie uzywamy - wiec nie ma ryzyka, ze
 * powstana dwie uslugi 0x180A.
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
                0, /* koniec listy charakterystyk */
            },
        },
    },
    {
        0, /* koniec listy uslug */
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
    ESP_LOGI(TAG, "PnP ID: zrodlo=0x%02x VID=0x%04x PID=0x%04x wersja=0x%04x",
             s_pnp_id[0],
             (unsigned)(s_pnp_id[1] | (s_pnp_id[2] << 8)),
             (unsigned)(s_pnp_id[3] | (s_pnp_id[4] << 8)),
             (unsigned)(s_pnp_id[5] | (s_pnp_id[6] << 8)));
    return ESP_OK;
}
#endif /* CONFIG_APP_GAMEPAD_PROFILE_XBOX */

/* ------------------------------------------------------- wlasna usluga HID */

/*
 * DLACZEGO NIE ble_svc_hid Z NimBLE, ktorej uzywalismy do Etapu 3:
 *
 * struct ble_svc_hid_params trzyma dlugosc Report Map w polu uint8_t, mimo ze sam
 * bufor ma 512 B - i dokladnie tym polem karmi hosta:
 *
 *   os_mbuf_append(ctxt->om, &hid_instances[i].report_map,
 *                  hid_instances[i].report_map_len);      <- uint8_t
 *
 * Deskryptor pada Xbox ma 334 B, wiec Windows dostalby 78 B (334 modulo 256),
 * czyli obcieta, bezsensowna Report Map. Kompilator zlapal to jako
 * "conversion ... changes value from '334' to '78'". Komponentu bt nie da sie
 * sforkowac tak jak esp_hid (sa w nim prekompilowane biblioteki kontrolera),
 * dlatego usluga jest tu napisana wprost na GATT - AGENTS.md 4.30.
 *
 * Efekt uboczny na plus: uchwyty charakterystyk dostajemy wprost przez val_handle,
 * bez zgadywania, ktora z kilku charakterystyk o UUID 0x2A4D jest nasza.
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

/* HID Information: bcdHID=0x0111, kraj=0, flagi=0x02 (NormallyConnectable).
 * Na drucie little-endian, czyli dokladnie te bajty. */
static const uint8_t s_hid_info[4] = {0x11, 0x01, 0x00, 0x02};

/* Protocol Mode: 1 = Report (a nie Boot). Windows moze to nadpisac. */
static uint8_t s_proto_mode = 0x01;

#define HID_RPT_MAX_LEN 16

/*
 * Jeden wpis na kazda charakterystyke Report. Trzymamy tu ostatnia wartosc, bo
 * host ma prawo odczytac raport, a nie tylko dostawac notyfikacje.
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
 * Ten deskryptor deklaruje dwa raporty: wejsciowy pada i wyjsciowy z wibracjami.
 * Identyfikatory i dlugosci NIE sa tu wpisane liczbami - wypelniamy je w
 * register_hid_service() z tablicy xbox_reports[], ktora generator policzyl
 * wprost z deskryptora. Inaczej dalo by sie po cichu rozjechac z Report Map.
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
        /* Tu jest cala rzecz, ktorej nie dawala usluga z NimBLE: pelna dlugosc. */
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
                ESP_LOGI(TAG, "PC ustawil Protocol Mode = %u (1 = Report)", buf);
            }
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;

    case HID_CHR_UUID16_CTRL_PT:
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_hs_mbuf_to_flat(ctxt->om, &buf, 1, &got) == 0 && got == 1) {
            ESP_LOGI(TAG, "HID Control Point: %u (0 = suspend, 1 = wyjscie z suspend)", buf);
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
             * Raport wyjsciowy to u pada Xbox polecenie wibracji. Silnikow nie
             * mamy, ale ten log jest cenny sam w sobie: jesli Windows tu pisze,
             * to znaczy, ze obsluguje nas swoim sterownikiem pada, a nie jako
             * zwykle HID.
             */
            ESP_LOGI(TAG, "raport wyjsciowy id=%u (%u B): %02x %02x %02x %02x %02x %02x %02x %02x",
                     rpt->id, got, rpt->data[0], rpt->data[1], rpt->data[2], rpt->data[3],
                     rpt->data[4], rpt->data[5], rpt->data[6], rpt->data[7]);
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* Report Reference (0x2908): dwa bajty - identyfikator raportu i jego typ.
 * Bez tego host nie wie, ktora charakterystyka odpowiada ktoremu Report ID. */
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

/* Profil HOGP wymaga szyfrowania na charakterystykach HID. Windows i tak szyfruje
 * link, ale deklarujemy to jawnie, bo tak robi prawdziwe urzadzenie. */
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
                /* Raport wejsciowy pada - ten notyfikujemy. */
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
                0, /* koniec listy charakterystyk */
            },
        },
    },
    {
        0, /* koniec listy uslug */
    },
};

static esp_err_t register_hid_service(void)
{
#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
    /*
     * Identyfikatory i dlugosci raportow bierzemy z tablicy policzonej przez
     * generator wprost z deskryptora, zeby kod nie mogl sie z nim rozjechac.
     * Jesli deskryptor kiedys zadeklaruje inny zestaw raportow, chcemy o tym
     * wiedziec od razu przy starcie, a nie z zachowania Windows.
     */
    const size_t nrpts = sizeof(xbox_reports) / sizeof(xbox_reports[0]);
    if (nrpts != 2 || xbox_reports[0].type != HID_RPT_TYPE_INPUT ||
        xbox_reports[1].type != HID_RPT_TYPE_OUTPUT) {
        ESP_LOGE(TAG, "deskryptor deklaruje %u raportow w nieoczekiwanym ukladzie",
                 (unsigned)nrpts);
        return ESP_ERR_INVALID_STATE;
    }
    if (xbox_reports[0].len > HID_RPT_MAX_LEN || xbox_reports[1].len > HID_RPT_MAX_LEN) {
        ESP_LOGE(TAG, "raport dluzszy niz bufor %d B", HID_RPT_MAX_LEN);
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
    ESP_LOGI(TAG, "usluga HID: Report Map %u B, raporty 0x%02x INPUT %u B + 0x%02x OUTPUT %u B",
             (unsigned)REPORT_MAP_BYTES, s_rpt_input.id, s_rpt_input.len,
             s_rpt_rumble.id, s_rpt_rumble.len);
#else
    ESP_LOGI(TAG, "usluga HID: Report Map %u B, raport wejsciowy 0x%02x %u B",
             (unsigned)REPORT_MAP_BYTES, s_rpt_input.id, s_rpt_input.len);
#endif
    return ESP_OK;
}

/* ------------------------------------------------------------ advertising */

static int gap_event(struct ble_gap_event *event, void *arg);

static esp_err_t start_advertising(void)
{
    /*
     * Rozdzielenie na ADV i SCAN_RSP: w 31 bajtach ADV nie ma pewnosci, ze zmiesci
     * sie nazwa razem z flagami, UUID i appearance (a dlugosc nazwy jest z Kconfig).
     * Nazwa idzie wiec w odpowiedzi na skan - Windows skanuje aktywnie i tak ja
     * przeczyta.
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
    ESP_LOGI(TAG, "rozglaszam jako '%s' (appearance 0x%04x)", name, APPEARANCE_GAMEPAD);
    return ESP_OK;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "polaczenie nieudane, status=%d - wracam do rozglaszania",
                     event->connect.status);
            start_advertising();
            return 0;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0) {
            return 0;
        }
        /*
         * Sprawdzenie, ktorego brakuje w nimble_hidd.c (AGENTS.md 4.2). Ten callback
         * jest przypiazany do naszego advertisingu, wiec rola i tak powinna byc SLAVE,
         * ale jesli kiedys zostanie podpiety globalnie, to zabezpieczenie zostaje.
         */
        if (desc.role != BLE_GAP_ROLE_SLAVE) {
            ESP_LOGW(TAG, "ignoruje polaczenie w roli %d (to nie PC)", desc.role);
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        s_subscribed = false;
        s_have_last = false;
        ESP_LOGI(TAG, "PC podlaczony, conn_handle=%u", s_conn_handle);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "PC rozlaczony, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_have_last = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_report_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "PC %s notyfikacje raportu",
                     s_subscribed ? "wlaczyl" : "wylaczyl");
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "szyfrowanie/parowanie: status=%d", event->enc_change.status);
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

/* Osie pada Xbox sa 16-bitowe BEZ znaku (deskryptor: logical 0..65535).
 * Skalowanie z naszego zakresu -127..127; dodane 127 zaokragla tak, ze zero
 * wypada dokladnie na 32768, a konce zakresu na 0 i 65535. */
static uint16_t axis_to_u16(int8_t v)
{
    int32_t t = (int32_t)v + 127;   /* 0..254 */
    return (uint16_t)((t * 65535 + 127) / 254);
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
    rpt[9]  = (uint8_t)(trigger_l >> 8);   /* gorne 6 bitow to dopelnienie */
    rpt[10] = (uint8_t)(trigger_r & 0xFF);
    rpt[11] = (uint8_t)(trigger_r >> 8);
    rpt[12] = 0;                            /* hat switch: 0 = wysrodkowany */
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
    /* Tylko na zmianie stanu - inaczej przy 100 Hz zasypywalibysmy link
     * identycznymi raportami. */
    if (s_have_last && memcmp(rpt, s_last_report, sizeof(rpt)) == 0) {
        return false;
    }

#if CONFIG_APP_GAMEPAD_PROFILE_XBOX
    /*
     * Logujemy tylko czesc "cyfrowa" raportu - spusty, hat i przyciski (bajty 8..15).
     * Osi nie, bo przy ruchu myszy zmieniaja sie kilkanascie razy na sekunde i zalalyby
     * konsole. Dzieki temu widac wprost, czy klik myszy wyszedl jako spust: w joy.cpl
     * spusty NIE sa przyciskami, wiec inaczej trzeba by to zgadywac.
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
        ESP_LOGW(TAG, "brak mbuf na raport");
        return false;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_report_handle, om);
    if (rc != 0) {
        /* mbuf jest zwalniany przez stack niezaleznie od wyniku */
        ESP_LOGW(TAG, "ble_gatts_notify_custom: rc=%d", rc);
        return false;
    }

    memcpy(s_last_report, rpt, sizeof(rpt));
    s_have_last = true;
    /* Host ma prawo odczytac raport, nie tylko dostawac notyfikacje - trzymamy
     * wiec ostatnia wartosc pod charakterystyka. */
    memcpy(s_rpt_input.data, rpt, sizeof(rpt));
    return true;
}

/* ------------------------------------------------------------- test wlasny */

#if CONFIG_APP_GAMEPAD_SELFTEST
/* Osiem krokow po okregu o promieniu 100 - wystarcza, zeby w joy.cpl bylo widac
 * ruch, i nie wymaga arytmetyki zmiennoprzecinkowej. */
static const int8_t s_circle_x[8] = {100, 71, 0, -71, -100, -71, 0, 71};
static const int8_t s_circle_y[8] = {0, 71, 100, 71, 0, -71, -100, -71};

static void selftest_step(uint32_t tick)
{
    gamepad_state_t st = {0};
    uint32_t phase = (tick / 6) % 8;   /* pelny okrag w ~0,5 s przy 100 Hz */
    st.lx = s_circle_x[phase];
    st.ly = s_circle_y[phase];
    st.rx = s_circle_x[(phase + 2) % 8];
    st.ry = s_circle_y[(phase + 2) % 8];
    /* Po kolei jeden przycisk, zmiana raz na sekunde. */
    st.buttons = (uint16_t)(1u << ((tick / CONFIG_APP_REPORT_RATE_HZ) % 12));
    ble_gamepad_send(&st);
}
#endif

static void gamepad_task(void *arg)
{
    ble_stack_wait_synced(portMAX_DELAY);

    /*
     * Uchwyt wypelnia NimBLE przez wskaznik val_handle w chwili startu GATT,
     * czyli po synchronizacji stacku. Nie musimy go szukac (tak bylo, gdy usluge
     * dawal ble_svc_hid) - wystarczy sprawdzic, ze jest.
     */
    if (s_report_handle == 0) {
        ESP_LOGE(TAG, "raport nie dostal uchwytu - pad nie ma sensu, koncze zadanie");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "charakterystyka Report ma handle %u", s_report_handle);
    start_advertising();

    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_APP_REPORT_RATE_HZ);
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(period > 0 ? period : 1);
        tick++;
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
    ESP_LOGI(TAG, "profil: pad Xbox Series X (XInput), PID 0x%02x%02x",
             s_pnp_id[4], s_pnp_id[3]);
    /* Mapowanie w logu, bo inaczej trzeba je zgadywac z kodu przy kazdym tescie. */
    ESP_LOGI(TAG, "przyciski -> Xbox: 1=%s 2=%s 3=%s 4=%s 5=%s 6=%s",
             s_xbox_ctrl[0].name, s_xbox_ctrl[1].name, s_xbox_ctrl[2].name,
             s_xbox_ctrl[3].name, s_xbox_ctrl[4].name, s_xbox_ctrl[5].name);
    ESP_LOGI(TAG, "               7=%s 8=%s 9=%s 10=%s 11=%s 12=%s",
             s_xbox_ctrl[6].name, s_xbox_ctrl[7].name, s_xbox_ctrl[8].name,
             s_xbox_ctrl[9].name, s_xbox_ctrl[10].name, s_xbox_ctrl[11].name);
#else
    ESP_LOGI(TAG, "profil: generyczny pad HID (DirectInput)");
#endif
    return ESP_OK;
}
