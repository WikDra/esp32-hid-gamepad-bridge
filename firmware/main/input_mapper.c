/*
 * Maps inputs onto the gamepad.
 *
 * The task wakes CONFIG_APP_REPORT_RATE_HZ times per second, takes a snapshot of the
 * state from ble_hid_host and turns it into a gamepad report. Sending only happens on
 * a state change - ble_gamepad_send() takes care of that.
 *
 * Mapping is a TABLE, not a chain of conditions: s_default_binds below is the mapping the
 * documentation describes, and input_mapper_set_binds() replaces it at runtime.
 *
 *   left stick    <- WASD
 *   right stick   <- mouse motion (deltas, scaled and clamped) - not bindable, see input_map.h
 *   D-pad         <- arrow keys (Xbox profile only - the generic one has no hat switch)
 *   button 1..3   <- left / right / middle mouse button
 *   button 4      <- space
 *   button 5..6   <- left Shift / left Ctrl
 *   button 7..12  <- E, Q, R, F, Tab, Esc
 *
 * The button numbers are nominal: ble_gamepad.c translates them into Xbox controls (table
 * s_xbox_ctrl) so that this module does not need to know which profile is active.
 */

#include "input_mapper.h"

/*
 * Transport seam. The mapping logic below is identical for BLE and USB; only where the
 * state comes from and where the report goes differ, so those two are macros resolved at
 * compile time. Four symbols is the whole coupling.
 */
#if CONFIG_APP_ENABLE_HID_HOST
#include "ble_hid_host.h"
#define IO_TAKE_STATE(p) ble_hid_host_take_state(p)
#define IO_INPUT_BUSY()  ble_hid_host_is_opening()
#else
#include "input_state.h"
#define IO_TAKE_STATE(p) input_state_take(p)
#define IO_INPUT_BUSY()  false
#endif

#if CONFIG_APP_ENABLE_GAMEPAD
#include "ble_gamepad.h"
#define IO_PAD_SEND(p) ble_gamepad_send(p)
#elif CONFIG_APP_USB_PAD
#include "usb_pad.h"
#define IO_PAD_SEND(p) usb_pad_send(p)
#else
#error "input_mapper needs a pad transport: APP_ENABLE_GAMEPAD or APP_USB_PAD"
#endif

/*
 * Passthrough is the third end of the seam: in that mode the chip is a plain HID keyboard and
 * mouse instead of a pad, so the mapping below is skipped entirely and the raw state goes out
 * unchanged. Compiles to nothing when the feature or the USB pad is absent.
 */
#if CONFIG_APP_USB_PASSTHROUGH && CONFIG_APP_USB_PAD
#define IO_SERVICE_MODE()      usb_pad_service_mode()
#define IO_PASSTHROUGH_SEND(p) usb_pad_send_passthrough(p)
#else
#define IO_SERVICE_MODE()      false
#define IO_PASSTHROUGH_SEND(p) ((void)(p))
#endif


#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_gamepad.h"
#include "ble_hid_host.h"
#include "bridge_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_map.h"
#include "sdkconfig.h"

static const char *TAG = "mapper";

/*
 * What the mapper last produced, for the configuration panel to display.
 *
 * DOUBLE BUFFERED, and not because a torn pad state would matter - one odd frame in a
 * visualisation is invisible. It matters for the INPUT state: the panel's "press a key to bind
 * it" flow reads the held keys from here, and a half-written keys[] could show a key nobody is
 * pressing and bind the wrong thing. The writer fills the spare slot and then publishes the
 * index, which is a single aligned store.
 *
 * The reader can still be unlucky if it is descheduled for a whole millisecond mid-copy, so this
 * is cheap consistency rather than a guarantee. That is the right trade here: a lock in a loop
 * that runs 1000 times a second, taken for the benefit of a diagnostic, would be paying in the
 * wrong currency.
 */
typedef struct {
    gamepad_state_t pad;
    hid_input_state_t in;
} mapper_snapshot_t;

static mapper_snapshot_t s_snap[2];
static volatile uint8_t s_snap_idx;
static volatile uint32_t s_ticks;

/* USB HID Keyboard/Keypad usage IDs */
#define KEY_A     0x04
#define KEY_D     0x07
#define KEY_E     0x08
#define KEY_F     0x09
#define KEY_Q     0x14
#define KEY_R     0x15
#define KEY_S     0x16
#define KEY_W     0x1A
#define KEY_ESC   0x29
#define KEY_TAB   0x2B
#define KEY_SPACE 0x2C
#define KEY_RIGHT 0x4F
#define KEY_LEFT  0x50
#define KEY_DOWN  0x51
#define KEY_UP    0x52

/* Modifier bitmap from byte 0 of the keyboard report */
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02

/* Full axis deflection. The descriptor declares -127..127, but 127 is only used for
 * straight directions - see map_digital(). */
#define AXIS_MAX 127

/*
 * The default table: exactly the mapping that was hardcoded here before, and exactly the one
 * the README documents. Anything that changes the feel of the bridge has to change this table
 * or replace it at runtime - there is no second place where a key maps to a control.
 */
static const input_bind_t s_default_binds[] = {
    { BIND_SRC_KEY, KEY_A, ACT_LSTICK_LEFT },
    { BIND_SRC_KEY, KEY_D, ACT_LSTICK_RIGHT },
    { BIND_SRC_KEY, KEY_W, ACT_LSTICK_UP },
    { BIND_SRC_KEY, KEY_S, ACT_LSTICK_DOWN },

    { BIND_SRC_MOUSE_BTN, 0x01, ACT_BTN_1 }, /* left   -> RT in the Xbox profile */
    { BIND_SRC_MOUSE_BTN, 0x02, ACT_BTN_2 }, /* right  -> LT */
    { BIND_SRC_MOUSE_BTN, 0x04, ACT_BTN_3 }, /* middle -> right stick click */

    { BIND_SRC_KEY, KEY_SPACE, ACT_BTN_4 },
    { BIND_SRC_MOD, MOD_LSHIFT, ACT_BTN_5 },
    { BIND_SRC_MOD, MOD_LCTRL, ACT_BTN_6 },
    { BIND_SRC_KEY, KEY_E, ACT_BTN_7 },
    { BIND_SRC_KEY, KEY_Q, ACT_BTN_8 },
    { BIND_SRC_KEY, KEY_R, ACT_BTN_9 },
    { BIND_SRC_KEY, KEY_F, ACT_BTN_10 },
    { BIND_SRC_KEY, KEY_TAB, ACT_BTN_11 },
    { BIND_SRC_KEY, KEY_ESC, ACT_BTN_12 },

    { BIND_SRC_KEY, KEY_UP, ACT_DPAD_UP },
    { BIND_SRC_KEY, KEY_RIGHT, ACT_DPAD_RIGHT },
    { BIND_SRC_KEY, KEY_DOWN, ACT_DPAD_DOWN },
    { BIND_SRC_KEY, KEY_LEFT, ACT_DPAD_LEFT },
};

/*
 * Derived form of the table: a direct index from what a HID report contains to what it does.
 * 272 bytes, and it turns the hot path from "scan up to 40 rows per pressed key" into one array
 * read per pressed key.
 */
typedef struct {
    uint8_t key[256]; /* indexed by USB HID keycode */
    uint8_t mod[8];   /* indexed by modifier BIT, not mask */
    uint8_t mouse[8]; /* indexed by mouse button BIT */
} bind_lut_t;

/*
 * Two of them, and the task reads through a pointer. A table change builds the spare copy and
 * then swaps the pointer, which is a single aligned store: the mapping task sees either the
 * whole old table or the whole new one, never a half-applied mixture. That is the same
 * reasoning that puts the USB identity switch in this task rather than behind a lock - it is
 * cheaper than a mutex and there is less to get wrong.
 *
 * volatile, not plain, so the compiler cannot hoist the read out of the loop and keep using a
 * stale copy for the lifetime of the task.
 */
static bind_lut_t s_lut[2];
static const bind_lut_t *volatile s_lut_active = &s_lut[0];
static input_bind_t s_binds[INPUT_BIND_MAX];
static size_t s_bind_count;

static void lut_build(bind_lut_t *lut, const input_bind_t *binds, size_t count)
{
    memset(lut, 0, sizeof(*lut)); /* everything unbound is ACT_NONE, which is 0 */

    for (size_t i = 0; i < count; i++) {
        const uint8_t act = binds[i].action;
        if (act == ACT_NONE || act >= ACT_COUNT) {
            continue;
        }

        switch (binds[i].src) {
        case BIND_SRC_KEY:
            /* code is a uint8_t and the array has 256 entries, so every value is in range. */
            lut->key[binds[i].code] = act;
            break;

        case BIND_SRC_MOD:
        case BIND_SRC_MOUSE_BTN:
            /* Stored as a mask, because that is how the HID report carries it; looked up by
             * bit. A mask with several bits set binds all of them, which is harmless. */
            for (int b = 0; b < 8; b++) {
                if (binds[i].code & (1u << b)) {
                    if (binds[i].src == BIND_SRC_MOD) {
                        lut->mod[b] = act;
                    } else {
                        lut->mouse[b] = act;
                    }
                }
            }
            break;

        default:
            break;
        }
    }
}

void input_mapper_set_binds(const input_bind_t *binds, size_t count)
{
    if (count > INPUT_BIND_MAX) {
        count = INPUT_BIND_MAX;
    }

    bind_lut_t *spare = (s_lut_active == &s_lut[0]) ? &s_lut[1] : &s_lut[0];
    lut_build(spare, binds, count);

    memcpy(s_binds, binds, count * sizeof(input_bind_t));
    s_bind_count = count;
    s_lut_active = spare; /* the swap */
}

const input_bind_t *input_mapper_binds(size_t *count)
{
    if (count) {
        *count = s_bind_count;
    }
    return s_binds;
}

size_t input_mapper_default_binds(input_bind_t *out, size_t max)
{
    const size_t n = sizeof(s_default_binds) / sizeof(s_default_binds[0]);
    const size_t copy = (n < max) ? n : max;
    if (out) {
        memcpy(out, s_default_binds, copy * sizeof(input_bind_t));
    }
    return n;
}

static inline void act_apply(uint8_t action, int *lx, int *ly, uint8_t *dpad, uint16_t *btn)
{
    switch (action) {
    case ACT_NONE:
        return;
    case ACT_LSTICK_LEFT:
        *lx -= 1;
        return;
    case ACT_LSTICK_RIGHT:
        *lx += 1;
        return;
    case ACT_LSTICK_UP:
        *ly -= 1; /* in HID the Y axis grows downwards */
        return;
    case ACT_LSTICK_DOWN:
        *ly += 1;
        return;
    case ACT_DPAD_UP:
        *dpad |= GAMEPAD_DPAD_UP;
        return;
    case ACT_DPAD_RIGHT:
        *dpad |= GAMEPAD_DPAD_RIGHT;
        return;
    case ACT_DPAD_DOWN:
        *dpad |= GAMEPAD_DPAD_DOWN;
        return;
    case ACT_DPAD_LEFT:
        *dpad |= GAMEPAD_DPAD_LEFT;
        return;
    default:
        if (action >= ACT_BTN_1 && action <= ACT_BTN_12) {
            *btn |= 1u << (action - ACT_BTN_1);
        }
        return;
    }
}

/*
 * Everything digital in one pass: keys, modifiers and mouse buttons all resolve through the
 * same lookup and land on the same four outputs.
 *
 * WASD is a digital input while an analog axis expects a vector. With two keys held (e.g. W+D),
 * simply setting both axes to maximum would produce a vector of length 1.41 - in games that
 * shows up as moving faster diagonally. Hence the ~0.707 scaling.
 */
static void map_digital(const hid_input_state_t *st, gamepad_state_t *out)
{
    const bind_lut_t *lut = s_lut_active;
    int lx = 0, ly = 0;
    uint8_t dpad = 0;
    uint16_t btn = 0;

    for (int i = 0; i < HID_KEYS_MAX; i++) {
        const uint8_t kc = st->keys[i];
        if (kc != 0) {
            act_apply(lut->key[kc], &lx, &ly, &dpad, &btn);
        }
    }
    for (int b = 0; b < 8; b++) {
        if (st->modifiers & (1u << b)) {
            act_apply(lut->mod[b], &lx, &ly, &dpad, &btn);
        }
    }
    for (int b = 0; b < 8; b++) {
        if (st->mouse_buttons & (1u << b)) {
            act_apply(lut->mouse[b], &lx, &ly, &dpad, &btn);
        }
    }

    /*
     * Clamped because a user-editable table can do what the hardcoded version could not: bind
     * two keys to the same direction. Without this, holding both would scale the axis past
     * int8_t range and wrap the sign.
     */
    if (lx > 1) {
        lx = 1;
    } else if (lx < -1) {
        lx = -1;
    }
    if (ly > 1) {
        ly = 1;
    } else if (ly < -1) {
        ly = -1;
    }

    const int magnitude = (lx != 0 && ly != 0) ? 90 : AXIS_MAX; /* 90 ~= 127 * 0.707 */
    out->lx = (int8_t)(lx * magnitude);
    out->ly = (int8_t)(ly * magnitude);
    out->buttons = btn;
    out->dpad = dpad;
}

static int8_t clamp_axis(int32_t v)
{
    if (v > AXIS_MAX) {
        return AXIS_MAX;
    }
    if (v < -AXIS_MAX) {
        return -AXIS_MAX;
    }
    return (int8_t)v;
}

/*
 * A mouse reports deltas, while an analog stick has an absolute position.
 *
 * The trap, visible in the device log: at 100 Hz the mouse reported ~20-25 times per second
 * while this task ran at 100 Hz. With a naive "delta of this tick -> axis" mapping, three
 * ticks out of four see zero, so the stick jumps between deflected and centred ~20 times a
 * second. Since a pad report only goes out on a state change, the PC then receives an
 * alternating series of R(0,x) and R(0,0) - which feels like jitter, not movement.
 *
 * Hence an exponential moving average of the delta, with a time constant of MOUSE_TAU_MS.
 * Under steady motion the stick holds a stable deflection proportional to mouse speed, and
 * returns to centre about one time constant after the mouse stops.
 *
 * BOTH OF THESE ARE EXPRESSED IN TIME, NOT IN TICKS, and that is load-bearing rather than tidy.
 * They used to be per-tick: the time constant was "8 ticks" and full deflection was "div * 4
 * counts per tick". Raising APP_REPORT_RATE_HZ therefore changed the feel silently - going from
 * 100 Hz to 250 Hz made the filter 2.5x faster AND the mouse 2.5x less sensitive, quietly
 * undoing the tuning that AGENTS.md 4.22 arrived at by hand. Converted from time to ticks
 * against the actual rate, the numbers mean the same thing at 100 Hz and at 1 kHz.
 *
 * Both now live in bridge_config rather than in a macro, because they are the two settings you
 * can only judge by feel and therefore the two worst candidates for requiring a reflash.
 */

/*
 * Fractional resolution of the fixed-point accumulator. It has to be large relative to the
 * number of ticks in the time constant, or the filter STALLS: the step is
 * (sample * EMA_FRAC - ema) / ema_ticks in integer arithmetic, so once the difference falls
 * below ema_ticks the increment truncates to zero and the average stops moving.
 *
 * That is not hypothetical - it is a bug this file had for one iteration. With EMA_FRAC 256 and
 * a 1 kHz task the time constant is 80 ticks, leaving barely three units of headroom, and slow
 * mouse movement simply stopped registering. 4096 keeps at least 51 units even at 1 kHz, and at
 * 100 Hz it is 512, so nothing is lost at the low end either.
 */
#define EMA_FRAC 4096

static int32_t s_ema_x;
static int32_t s_ema_y;

/*
 * Values derived from the configuration, recomputed only when it changes.
 *
 * The point of caching is not the arithmetic - a multiply and a divide per tick would be free.
 * It is that the derivation belongs to the filter and not to the configuration store: this file
 * owns EMA_FRAC and the report rate, and nothing else should have to know they exist to be able
 * to offer a time constant in milliseconds.
 */
static struct {
    uint32_t gen;
    int32_t ema_ticks;
    int64_t denom_x;
    int64_t denom_y;
    int32_t anti_dz; /* in axis units, not percent */
    bool invert_y;
} s_tune = { .gen = UINT32_MAX, .ema_ticks = 1, .denom_x = 1, .denom_y = 1 };

static void tune_refresh(void)
{
    const uint32_t gen = bridge_config_generation();
    if (gen == s_tune.gen) {
        return;
    }
    const bridge_config_t *cfg = bridge_config_get();
    if (!cfg) {
        return;
    }

    /* Time constant expressed in ticks at the configured rate. At 100 Hz and 80 ms this is 8,
     * which is exactly what the hand-tuned version used. */
    int32_t ticks = ((int32_t)CONFIG_APP_REPORT_RATE_HZ * cfg->mouse_tau_ms) / 1000;
    s_tune.ema_ticks = (ticks > 0) ? ticks : 1;

    /*
     * Mouse speed that produces full deflection, expressed in counts per SECOND so that the
     * feel does not depend on the task rate. The tuned value was "div * 4 counts per tick at
     * 100 Hz", which is div * 400 counts per second; at div=24 that is 9600 counts/s, and
     * measurements on the reference mouse showed roughly 7900 counts/s during brisk movement,
     * so it lands around 80 % of the range.
     */
    s_tune.denom_x = (int64_t)cfg->mouse_div_x * 400 * EMA_FRAC;
    s_tune.denom_y = (int64_t)cfg->mouse_div_y * 400 * EMA_FRAC;

    s_tune.anti_dz = ((int32_t)cfg->mouse_anti_deadzone * AXIS_MAX) / 100;
    s_tune.invert_y = cfg->mouse_invert_y != 0;

    s_tune.gen = gen;
}

/*
 * Returns the accumulator in FIXED-POINT units, not whole counts per tick, and that matters
 * more the faster the task runs. Truncating here used to be the real resolution limit: at
 * 1 kHz the average delta per tick is about one count, so "counts per tick" had three usable
 * levels - 0, 1, 2 - and the stick moved in steps of a seventh of its range. The whole path to
 * the axis now stays in EMA_FRAC units.
 */
static int32_t ema_step(int32_t *ema, int32_t sample, int32_t ticks)
{
    *ema += ((sample * EMA_FRAC) - *ema) / ticks;
    /* Integer division never quite reaches zero for a small remainder, which would
     * leave the stick permanently off-centre. Below 1 count per tick there is nothing
     * worth carrying over anyway. */
    if (sample == 0 && *ema > -EMA_FRAC && *ema < EMA_FRAC) {
        *ema = 0;
    }
    return *ema;
}

/* Integer square root. No FPU is involved deliberately: this file is shared with the ESP32-C3
 * build, which has none, and a soft-float sqrt in a 1 kHz loop would be a silly way to pay for
 * a deadzone. */
static uint32_t isqrt32(uint32_t v)
{
    uint32_t rem = v, root = 0, bit = 1u << 30;
    while (bit > rem) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/*
 * Lifts a non-zero deflection to at least the anti-deadzone magnitude and compresses the rest
 * of the range into what is left, so the mapping stays continuous and still reaches full scale.
 *
 * Radial, on the magnitude of the vector: applying it per axis would add up to 41 % on
 * diagonals, which feels like the stick being pulled towards the corners.
 */
static void apply_anti_deadzone(int32_t *x, int32_t *y, int32_t dz)
{
    if (dz <= 0 || (*x == 0 && *y == 0)) {
        return;
    }

    const uint32_t mag = isqrt32((uint32_t)(*x * *x) + (uint32_t)(*y * *y));
    if (mag == 0) {
        return;
    }

    const int32_t scaled = dz + (int32_t)((mag * (uint32_t)(AXIS_MAX - dz)) / AXIS_MAX);
    *x = (int32_t)(((int64_t)*x * scaled) / (int32_t)mag);
    *y = (int32_t)(((int64_t)*y * scaled) / (int32_t)mag);
}

static void stick_from_mouse(const hid_input_state_t *st, int8_t *out_x, int8_t *out_y)
{
    tune_refresh();

    const int32_t ema_x = ema_step(&s_ema_x, st->mouse_dx, s_tune.ema_ticks);
    const int32_t ema_y = ema_step(&s_ema_y, st->mouse_dy, s_tune.ema_ticks);

    /*
     * The scaling is done in one expression, in 64-bit, rather than by first reducing the
     * accumulator to counts per tick: both intermediate divisions used to truncate, and at high
     * task rates almost nothing survived them.
     */
    int32_t x = (int32_t)(((int64_t)ema_x * AXIS_MAX * CONFIG_APP_REPORT_RATE_HZ) / s_tune.denom_x);
    int32_t y = (int32_t)(((int64_t)ema_y * AXIS_MAX * CONFIG_APP_REPORT_RATE_HZ) / s_tune.denom_y);

    if (s_tune.invert_y) {
        y = -y;
    }

    /* Clamp before the deadzone so the magnitude it works on is the one that will actually be
     * reported, and skip the whole thing when the feature is off - the default path then costs
     * exactly what it did before. */
    x = clamp_axis(x);
    y = clamp_axis(y);
    apply_anti_deadzone(&x, &y, s_tune.anti_dz);

    *out_x = clamp_axis(x);
    *out_y = clamp_axis(y);
}

static void mapper_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_APP_REPORT_RATE_HZ);
    TickType_t last_wake = xTaskGetTickCount();

    /*
     * pdMS_TO_TICKS() rounds DOWN to whole FreeRTOS ticks, so a rate faster than the tick
     * rate silently collapses to one report per tick. MEASURED CONSEQUENCE: with
     * APP_REPORT_RATE_HZ=250 and the default 100 Hz tick the pad updated at 100 Hz, not 250,
     * even though its USB endpoint is polled every 4 ms - the number in menuconfig was simply
     * unreachable. Say so in the log rather than let it lie; the fix is CONFIG_FREERTOS_HZ.
     */
    const unsigned achieved = (unsigned)configTICK_RATE_HZ / (unsigned)(period > 0 ? period : 1);
    if (achieved < CONFIG_APP_REPORT_RATE_HZ) {
        ESP_LOGW(TAG, "APP_REPORT_RATE_HZ=%d is not reachable with a %d Hz FreeRTOS tick - "
                      "reports go out at %u Hz; raise CONFIG_FREERTOS_HZ",
                 CONFIG_APP_REPORT_RATE_HZ, (int)configTICK_RATE_HZ, achieved);
    } else {
        ESP_LOGI(TAG, "mapping task at %u Hz (FreeRTOS tick %d Hz)", achieved,
                 (int)configTICK_RATE_HZ);
    }
    gamepad_state_t prev_logged = {0};
    int64_t last_log_us = 0;

    while (true) {
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);

        hid_input_state_t in;
        /* Taking the state clears the mouse accumulators, so every delta ends up in
         * exactly one gamepad report. */
        IO_TAKE_STATE(&in);

        /*
         * Applies a pending identity switch and tells us which mode is now in force. Done here,
         * in the task that also submits reports, so re-enumeration can never race a transfer -
         * that is cheaper and easier to reason about than a lock around both.
         */
        if (IO_SERVICE_MODE()) {
            IO_PASSTHROUGH_SEND(&in);
            continue;
        }

        gamepad_state_t out = {0};
        map_digital(&in, &out);
        stick_from_mouse(&in, &out.rx, &out.ry);

#if CONFIG_APP_DEBUG_PAD_RATE_PROBE
        /*
         * Measuring instrument, not a feature. Reports only go out on a state change, so
         * without this the packet rate seen by the host says as much about the smoothing filter
         * and the hand on the mouse as about the transport. One unit of alternation on one axis
         * guarantees every tick is distinct, and then --rate measures the endpoint interval,
         * the task rate and the host driver, and nothing else.
         */
        static bool probe_toggle;
        probe_toggle = !probe_toggle;
        out.lx = probe_toggle ? 1 : 0;
#endif

        /*
         * Opening a HID device is the heaviest moment for the stack: one link runs
         * dozens of GATT procedures (service discovery, Report Map read, subscriptions)
         * while the other two are active. Both crashes we ever saw landed exactly then,
         * and both were inside NimBLE's internal pools (AGENTS.md 4.21, 4.26). We do
         * not add ~16 pad notifications per second on top of that.
         *
         * One zero report is sent when entering that window, so the PC is not left with
         * a deflected stick or a held button for its duration.
         */
        static bool was_opening;
        bool opening = IO_INPUT_BUSY();
        if (opening) {
            if (!was_opening) {
                gamepad_state_t neutral = {0};
                IO_PAD_SEND(&neutral);
                ESP_LOGI(TAG, "device open in progress - suspending pad reports");
            }
            was_opening = true;
            continue;
        }
        if (was_opening) {
            ESP_LOGI(TAG, "device open finished - resuming pad reports");
            was_opening = false;
        }

        bool sent = IO_PAD_SEND(&out);

        /*
         * Publish after sending, so what the panel shows is what went to the host rather than
         * what we were about to send. The counter is only ever incremented here, and the panel
         * turns two samples of it into a rate - which is the same measurement
         * scripts/xinput_rumble.py --rate makes from the other end.
         */
        if (sent) {
            s_ticks++;
        }
        {
            const uint8_t spare = s_snap_idx ? 0 : 1;
            s_snap[spare].pad = out;
            s_snap[spare].in = in;
            s_snap_idx = spare;
        }

        /* Log only on a real change and no more than once per 250 ms - otherwise mouse
         * movement would flood the console. */
        if (sent) {
            int64_t now = esp_timer_get_time();
            bool changed = memcmp(&out, &prev_logged, sizeof(out)) != 0;
            if (changed && (now - last_log_us) > 250000) {
                last_log_us = now;
                prev_logged = out;
                ESP_LOGI(TAG, "pad: L(%4d,%4d) R(%4d,%4d) btn=0x%03x dpad=0x%x",
                         out.lx, out.ly, out.rx, out.ry, out.buttons, out.dpad);
            }
        }
    }
}

void input_mapper_snapshot(gamepad_state_t *pad, hid_input_state_t *in)
{
    const mapper_snapshot_t *s = &s_snap[s_snap_idx];
    if (pad) {
        *pad = s->pad;
    }
    if (in) {
        *in = s->in;
    }
}

uint32_t input_mapper_ticks(void)
{
    return s_ticks;
}

esp_err_t input_mapper_start(void)
{
    /*
     * bridge_config_init() publishes a configuration and hands the binding table over, so it has
     * to have run first. Failing loudly rather than quietly installing a second set of defaults
     * is deliberate: a second place that decides what the default mapping is would be a second
     * description of one thing, which in this project has twice meant a bug that builds, links,
     * reports success and behaves differently (AGENTS.md 4.38, 4.39).
     */
    const bridge_config_t *cfg = bridge_config_get();
    if (!cfg) {
        ESP_LOGE(TAG, "no configuration in force - call bridge_config_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(mapper_task, "mapper", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "input mapping enabled (%d Hz, mouse div %u/%u, tau %u ms, %u bindings)",
             CONFIG_APP_REPORT_RATE_HZ, cfg->mouse_div_x, cfg->mouse_div_y, cfg->mouse_tau_ms,
             cfg->bind_count);
    return ESP_OK;
}
