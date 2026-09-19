/* USB host for a keyboard and a mouse - see usb_hid_host.h. */

#include "usb_hid_host.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "input_state.h"
#if CONFIG_APP_LINK_SENDER
#include "chip_link.h"
#endif

static const char *TAG = "usb_host";

/*
 * How many HID INTERFACES can be tracked at once - interfaces, not devices. The distinction
 * matters: a 2.4 GHz receiver dongle typically exposes three or four HID interfaces on its own
 * (boot keyboard, boot mouse, consumer control, plus a vendor one), so two dongles can easily
 * present six to eight. Four was enough for a plain keyboard and a plain mouse and nothing
 * more.
 *
 * Overflowing this array is not fatal - the interface is opened and started before it is
 * recorded, and the report callback dispatches on params.proto rather than on this table, so
 * input keeps flowing. What breaks is the bookkeeping: count_proto() under-counts, and a
 * disconnect can then declare a class gone while another interface of it is still alive,
 * which zeroes that class's state. Cheap to avoid, so avoid it.
 */
#define USB_HID_MAX_IFACES 8

typedef struct {
    hid_host_device_handle_t handle;
    uint8_t proto; /* HID_PROTOCOL_KEYBOARD / HID_PROTOCOL_MOUSE */
    bool in_use;
} iface_t;

static iface_t s_ifaces[USB_HID_MAX_IFACES];
static int s_open_count;

static int count_proto(uint8_t proto)
{
    int n = 0;
    for (int i = 0; i < USB_HID_MAX_IFACES; i++) {
        if (s_ifaces[i].in_use && s_ifaces[i].proto == proto) {
            n++;
        }
    }
    return n;
}

int usb_hid_host_device_count(void)
{
    return s_open_count;
}

/* ------------------------------------------------------------------ decoding */

static void handle_keyboard(const uint8_t *data, size_t len)
{
    /*
     * Boot protocol: data[0] modifiers, data[1] reserved, data[2..7] up to six keycodes.
     *
     * Keycodes 0x01..0x03 are ErrorRollOver / POSTFail / ErrorUndefined, not keys. Our BLE
     * keyboard emits bursts of ErrorRollOver after every press (AGENTS.md 4.17) and without
     * this filter the pad would get a phantom button each time. USB keyboards do the same
     * thing when more keys are held than the report can carry, so the filter belongs here
     * too rather than being a quirk of one device.
     */
    if (len < 8) {
        return;
    }

    uint8_t keys[HID_KEYS_MAX];
    bool any_real = false;
    for (int i = 0; i < HID_KEYS_MAX; i++) {
        uint8_t k = data[2 + i];
        if (k >= 0x01 && k <= 0x03) {
            k = 0;
        }
        keys[i] = k;
        if (k) {
            any_real = true;
        }
    }

    /* A report made only of rollover codes says nothing about what is held; overwriting the
     * state with zeros would drop a key that really is down. */
    if (!any_real && data[0] == 0) {
        bool was_rollover = false;
        for (int i = 0; i < HID_KEYS_MAX; i++) {
            if (data[2 + i] >= 0x01 && data[2 + i] <= 0x03) {
                was_rollover = true;
            }
        }
        if (was_rollover) {
            return;
        }
    }

    input_state_set_keyboard(data[0], keys);
#if CONFIG_APP_LINK_SENDER
    chip_link_send_keyboard(data[0], keys);
#endif
}

/*
 * Hotkeys live on this chip because this is where the keyboard is, even though two of the three
 * act on the OTHER chip. What travels is the resulting mode bitmask, as absolute state, so a
 * lost frame repairs itself - see chip_link.h.
 *
 * ONE NAME FOR THE CONDITION, tested in two places below.
 */
#if (CONFIG_APP_USB_PASSTHROUGH || CONFIG_APP_WEBUI) && CONFIG_APP_LINK_SENDER
#define HOST_HAS_HOTKEYS 1
#else
#define HOST_HAS_HOTKEYS 0
#endif

#if HOST_HAS_HOTKEYS
/*
 * Ctrl+Alt+<key>, either side of the keyboard.
 *
 * The caller DROPS a report that fired a hotkey instead of forwarding it. That matters: without
 * it the chosen key would also reach the PC, and in passthrough mode that means a stray
 * character in whatever has focus.
 *
 * Edge-triggered per hotkey, each with its own state. A held combination must not fire
 * repeatedly - the keyboard resends the same report while a key is held, and acting per report
 * would flip identity dozens of times a second, re-enumerating USB each time.
 */
enum {
    HOTKEY_NONE = 0,
    HOTKEY_PASSTHROUGH,
    HOTKEY_WEBUI,
    HOTKEY_WEBUI_AP,
};

static const struct {
    uint8_t keycode;
    uint8_t id;
} s_hotkeys[] = {
#if CONFIG_APP_USB_PASSTHROUGH
    { CONFIG_APP_PASSTHROUGH_KEYCODE, HOTKEY_PASSTHROUGH },
#endif
#if CONFIG_APP_WEBUI
    { CONFIG_APP_WEBUI_HOTKEY_KEYCODE, HOTKEY_WEBUI },
    { CONFIG_APP_WEBUI_AP_HOTKEY_KEYCODE, HOTKEY_WEBUI_AP },
#endif
};

#define HOTKEY_COUNT (sizeof(s_hotkeys) / sizeof(s_hotkeys[0]))

static uint8_t hotkey_fired(const uint8_t *data)
{
    const uint8_t mods = data[0];
    const bool ctrl = (mods & 0x11) != 0; /* LCtrl | RCtrl */
    const bool alt = (mods & 0x44) != 0;  /* LAlt  | RAlt  */

    static bool was_down[HOTKEY_COUNT];
    uint8_t fired = HOTKEY_NONE;

    for (unsigned h = 0; h < HOTKEY_COUNT; h++) {
        bool down = false;
        if (ctrl && alt) {
            for (int i = 0; i < HID_KEYS_MAX; i++) {
                if (data[2 + i] == s_hotkeys[h].keycode) {
                    down = true;
                    break;
                }
            }
        }
        /*
         * The edge state is updated for every hotkey even after one has fired, so releasing a
         * combination always clears its own flag. Returning early would leave a stale "still
         * held" for the others.
         */
        if (down && !was_down[h] && fired == HOTKEY_NONE) {
            fired = s_hotkeys[h].id;
        }
        was_down[h] = down;
    }
    return fired;
}

static void hotkey_act(uint8_t id)
{
    uint8_t bits = chip_link_mode_bits();

    switch (id) {
#if CONFIG_APP_USB_PASSTHROUGH
    case HOTKEY_PASSTHROUGH:
        bits ^= LINK_MODE_PASSTHROUGH;
        ESP_LOGI(TAG, "hotkey: pad chip -> %s",
                 (bits & LINK_MODE_PASSTHROUGH) ? "PASSTHROUGH (keyboard + mouse)" : "GAMEPAD");
        break;
#endif
#if CONFIG_APP_WEBUI
    case HOTKEY_WEBUI:
        bits ^= LINK_MODE_WEBUI;
        if (!(bits & LINK_MODE_WEBUI)) {
            /* Switching the panel off also forgets a forced access point, so turning it back on
             * retries the stored network. Otherwise one emergency use of the AP hotkey would
             * quietly stick for the rest of the session. */
            bits &= (uint8_t)~LINK_MODE_WEBUI_AP;
        }
        ESP_LOGI(TAG, "hotkey: config panel -> %s", (bits & LINK_MODE_WEBUI) ? "ON" : "OFF");
        break;

    case HOTKEY_WEBUI_AP:
        bits |= LINK_MODE_WEBUI | LINK_MODE_WEBUI_AP;
        ESP_LOGI(TAG, "hotkey: config panel -> ON, forced to its own access point");
        break;
#endif
    default:
        return;
    }

    chip_link_set_mode_bits(bits);
}
#endif /* HOST_HAS_HOTKEYS */

static void handle_mouse(const uint8_t *data, size_t len)
{
    /*
     * Boot protocol: data[0] buttons, data[1] dx, data[2] dy, both int8, and data[3] is the
     * wheel when the device sends it. Plenty of mice report more than the boot protocol
     * promises even after SET_PROTOCOL(0) - a 16-bit variant is common - so the length
     * decides how to read it, exactly as on the BLE side (AGENTS.md 4.10).
     */
    if (len < 3) {
        return;
    }

    int32_t dx, dy, wheel = 0;
    if (len >= 5 && len != 4) {
        dx = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
        dy = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
        if (len >= 6) {
            wheel = (int8_t)data[5];
        }
    } else {
        dx = (int8_t)data[1];
        dy = (int8_t)data[2];
        if (len >= 4) {
            wheel = (int8_t)data[3];
        }
    }

    input_state_accum_mouse(data[0], dx, dy, wheel);
#if CONFIG_APP_LINK_SENDER
    chip_link_send_mouse(data[0], dx, dy, wheel);
#endif
}

/* ------------------------------------------------------------------- callbacks */

static void iface_event_cb(hid_host_device_handle_t dev, const hid_host_interface_event_t event,
                          void *arg)
{
    (void)arg;
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(dev, &params) != ESP_OK) {
        return;
    }

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        if (hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &len) != ESP_OK) {
            return;
        }
        if (params.proto == HID_PROTOCOL_KEYBOARD) {
#if HOST_HAS_HOTKEYS
            if (len >= 8) {
                const uint8_t hk = hotkey_fired(data);
                if (hk != HOTKEY_NONE) {
                    hotkey_act(hk);
                    break; /* consumed - the combination itself must not reach the PC */
                }
            }
#endif
            handle_keyboard(data, len);
        } else if (params.proto == HID_PROTOCOL_MOUSE) {
            handle_mouse(data, len);
        }

        /* One line per second at most, so a moving mouse cannot flood the console. */
        static int64_t last_log_us;
        int64_t now = esp_timer_get_time();
        if (now - last_log_us > 1000000) {
            last_log_us = now;
            ESP_LOGI(TAG, "%s report len=%u [%02x %02x %02x %02x]",
                     params.proto == HID_PROTOCOL_KEYBOARD ? "KBD" : "MOU", (unsigned)len,
                     len > 0 ? data[0] : 0, len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
                     len > 3 ? data[3] : 0);
        }
        break;
    }

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "%s disconnected (addr %u iface %u)",
                 params.proto == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse", params.addr,
                 params.iface_num);
        for (int i = 0; i < USB_HID_MAX_IFACES; i++) {
            if (s_ifaces[i].in_use && s_ifaces[i].handle == dev) {
                s_ifaces[i].in_use = false;
                s_open_count--;
                break;
            }
        }
        /* Only clear the class when the LAST interface of that class went away - a combo
         * device or a second keyboard must not release the other one's keys. */
        if (params.proto == HID_PROTOCOL_KEYBOARD && count_proto(HID_PROTOCOL_KEYBOARD) == 0) {
            input_state_set_keyboard_present(false);
#if CONFIG_APP_LINK_SENDER
            chip_link_set_presence(count_proto(HID_PROTOCOL_MOUSE) > 0, false);
#endif
        } else if (params.proto == HID_PROTOCOL_MOUSE && count_proto(HID_PROTOCOL_MOUSE) == 0) {
            input_state_set_mouse_present(false);
#if CONFIG_APP_LINK_SENDER
            chip_link_set_presence(false, count_proto(HID_PROTOCOL_KEYBOARD) > 0);
#endif
        }
        hid_host_device_close(dev);
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "transfer error on addr %u iface %u", params.addr, params.iface_num);
        break;

    default:
        break;
    }
}

static void driver_event_cb(hid_host_device_handle_t dev, const hid_host_driver_event_t event,
                           void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    hid_host_dev_params_t params;
    if (hid_host_device_get_params(dev, &params) != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG, "HID connected: addr %u iface %u sub_class %u proto %u", params.addr,
             params.iface_num, params.sub_class, params.proto);

    const hid_host_device_config_t dev_cfg = {
        .callback = iface_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(dev, &dev_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }

    /*
     * Ask for the boot protocol explicitly. Interfaces that advertise the boot subclass
     * usually start there anyway, but a device left in report mode by a previous host would
     * otherwise hand us a layout we have not parsed. This request is allowed to fail: a
     * device with no boot protocol at all still delivers reports, and the decoder above
     * branches on length.
     */
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        err = hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SET_PROTOCOL(boot) failed: %s - carrying on", esp_err_to_name(err));
        }
    }

    err = hid_host_device_start(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_start failed: %s", esp_err_to_name(err));
        hid_host_device_close(dev);
        return;
    }

    bool tracked = false;
    for (int i = 0; i < USB_HID_MAX_IFACES; i++) {
        if (!s_ifaces[i].in_use) {
            s_ifaces[i].in_use = true;
            s_ifaces[i].handle = dev;
            s_ifaces[i].proto = params.proto;
            s_open_count++;
            tracked = true;
            break;
        }
    }
    if (!tracked) {
        /* Reports from this interface will still arrive - see the comment on
         * USB_HID_MAX_IFACES - but its class bookkeeping is now wrong. Say so, because the
         * symptom (a class going dead when an unrelated interface disconnects) looks like
         * anything but a full table. */
        ESP_LOGW(TAG, "interface table full (%d) - addr %u iface %u not tracked",
                 USB_HID_MAX_IFACES, params.addr, params.iface_num);
    }

    if (params.proto == HID_PROTOCOL_KEYBOARD) {
        input_state_set_keyboard_present(true);
    } else if (params.proto == HID_PROTOCOL_MOUSE) {
        input_state_set_mouse_present(true);
    }
#if CONFIG_APP_LINK_SENDER
    chip_link_set_presence(count_proto(HID_PROTOCOL_MOUSE) > 0,
                           count_proto(HID_PROTOCOL_KEYBOARD) > 0);
#endif
}

/* ------------------------------------------------------------------------ start */

static void usb_events_task(void *arg)
{
    (void)arg;
    while (true) {
        /*
         * usb_host_lib_handle_events() must be pumped by somebody. The HID driver creates its
         * own task for HID events, but the library's own event loop is ours to run - without
         * it devices never finish enumerating.
         */
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

esp_err_t usb_hid_host_start(void)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(usb_events_task, "usb_events", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = driver_event_cb,
        .callback_arg = NULL,
    };
    err = hid_host_install(&hid_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB host up, waiting for a keyboard and a mouse");
#if CONFIG_USB_HOST_HUBS_SUPPORTED
    ESP_LOGI(TAG, "  external hubs: supported%s",
             CONFIG_USB_HOST_HUB_MULTI_LEVEL ? " (multi-level)" : " (single)");
#else
    ESP_LOGW(TAG, "  external hubs: NOT enabled - only one device can be attached directly");
#endif
    return ESP_OK;
}
