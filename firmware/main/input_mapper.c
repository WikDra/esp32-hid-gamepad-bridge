/*
 * Etap 3: mapowanie wejsc na pada.
 *
 * Zadanie budzi sie CONFIG_APP_REPORT_RATE_HZ razy na sekunde, zdejmuje snapshot
 * stanu z ble_hid_host i przelicza go na raport pada. Wysylka i tak idzie tylko
 * przy zmianie stanu - o to dba ble_gamepad_send().
 *
 * Mapowanie (klawisze podane jako USB HID keycodes):
 *   lewy analog   <- WASD
 *   prawy analog  <- ruch myszy (przyrosty, skalowane i przycinane)
 *   przycisk 1..3 <- lewy / prawy / srodkowy przycisk myszy
 *   przycisk 4    <- spacja
 *   przycisk 5..6 <- lewy Shift / lewy Ctrl
 *   przycisk 7..12<- E, Q, R, F, Tab, Esc
 */

#include "input_mapper.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "ble_gamepad.h"
#include "ble_hid_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "mapper";

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

/* Bitmapa modyfikatorow z bajtu 0 raportu klawiatury */
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02

/* Pelne wychylenie osi. Deskryptor deklaruje zakres -127..127, ale trzymamy sie
 * 127 tylko dla kierunkow prostych - patrz stick_from_wasd(). */
#define AXIS_MAX 127

static bool key_down(const hid_input_state_t *st, uint8_t keycode)
{
    for (int i = 0; i < HID_KEYS_MAX; i++) {
        if (st->keys[i] == keycode) {
            return true;
        }
    }
    return false;
}

/*
 * WASD to wejscie cyfrowe, a os analogowa oczekuje wektora. Przy dwoch klawiszach
 * naraz (np. W+D) proste ustawienie obu osi na maksimum daloby wektor o dlugosci
 * 1,41 - w grach objawia sie to szybszym ruchem na skos. Dlatego skos skalujemy
 * przez ~0,707.
 */
static void stick_from_wasd(const hid_input_state_t *st, int8_t *out_x, int8_t *out_y)
{
    int x = 0, y = 0;
    if (key_down(st, KEY_A)) {
        x -= 1;
    }
    if (key_down(st, KEY_D)) {
        x += 1;
    }
    if (key_down(st, KEY_W)) {
        y -= 1; /* w HID os Y rosnie w dol */
    }
    if (key_down(st, KEY_S)) {
        y += 1;
    }

    int magnitude = (x != 0 && y != 0) ? 90 : AXIS_MAX; /* 90 ~= 127 * 0,707 */
    *out_x = (int8_t)(x * magnitude);
    *out_y = (int8_t)(y * magnitude);
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
 * Mysz podaje przyrosty, a galka analogowa ma polozenie bezwzgledne.
 *
 * Pulapka widoczna w logu z plytki: AJ159 Pro raportuje ~20-25 razy na sekunde,
 * a to zadanie chodzi 100 Hz. Przy prostym przeliczeniu "przyrost z tiku -> os"
 * trzy na cztery tiki widza zero, czyli galka skacze miedzy wychyleniem a srodkiem
 * ~20 razy na sekunde. Poniewaz raport pada idzie tylko na zmianie stanu, PC
 * dostaje wtedy serie naprzemiennych R(0,x) i R(0,0) - w grze to wyglada jak
 * drganie, nie jak ruch.
 *
 * Dlatego liczymy srednia kroczaca (EMA) przyrostu ze stala czasowa ~8 tikow
 * (80 ms). Przy rownym ruchu galka trzyma stabilne wychylenie proporcjonalne do
 * predkosci myszy, a po zatrzymaniu wraca do srodka w ~80 ms.
 */
#define EMA_SHIFT 3   /* stala czasowa w tikach: 1 << 3 = 8 */
#define EMA_FRAC  256 /* arytmetyka staloprzecinkowa, zeby nie gubic wolnych ruchow */

static int32_t s_ema_x;
static int32_t s_ema_y;

static int32_t ema_step(int32_t *ema, int32_t sample)
{
    *ema += ((sample * EMA_FRAC) - *ema) / (1 << EMA_SHIFT);
    /* Dzielenie calkowitoliczbowe nigdy nie dojdzie do zera przy malej resztce,
     * a to zostawiloby galke na trwale poza srodkiem. Ponizej 1 zliczenia na tik
     * i tak nie ma co przenosic. */
    if (sample == 0 && *ema > -EMA_FRAC && *ema < EMA_FRAC) {
        *ema = 0;
    }
    return *ema / EMA_FRAC;
}

static void stick_from_mouse(const hid_input_state_t *st, int8_t *out_x, int8_t *out_y)
{
    int32_t div = CONFIG_APP_MOUSE_SCALE_DIV;
    if (div < 1) {
        div = 1;
    }
    /* Ile sredniego przyrostu na tik ma dawac pelne wychylenie. Przy div=8 to 32,
     * co wedlug pomiaru z plytki odpowiada spokojnemu ruchowi na ~1/3 zakresu. */
    int32_t full_scale = div * 4;

    int32_t avg_x = ema_step(&s_ema_x, st->mouse_dx);
    int32_t avg_y = ema_step(&s_ema_y, st->mouse_dy);

    *out_x = clamp_axis(avg_x * AXIS_MAX / full_scale);
    *out_y = clamp_axis(avg_y * AXIS_MAX / full_scale);
}

static uint16_t buttons_from_state(const hid_input_state_t *st)
{
    uint16_t b = 0;

    /* Przyciski myszy: bit 0 lewy, bit 1 prawy, bit 2 srodkowy. */
    if (st->mouse_buttons & 0x01) {
        b |= 1u << 0;
    }
    if (st->mouse_buttons & 0x02) {
        b |= 1u << 1;
    }
    if (st->mouse_buttons & 0x04) {
        b |= 1u << 2;
    }

    if (key_down(st, KEY_SPACE)) {
        b |= 1u << 3;
    }
    if (st->modifiers & MOD_LSHIFT) {
        b |= 1u << 4;
    }
    if (st->modifiers & MOD_LCTRL) {
        b |= 1u << 5;
    }
    if (key_down(st, KEY_E)) {
        b |= 1u << 6;
    }
    if (key_down(st, KEY_Q)) {
        b |= 1u << 7;
    }
    if (key_down(st, KEY_R)) {
        b |= 1u << 8;
    }
    if (key_down(st, KEY_F)) {
        b |= 1u << 9;
    }
    if (key_down(st, KEY_TAB)) {
        b |= 1u << 10;
    }
    if (key_down(st, KEY_ESC)) {
        b |= 1u << 11;
    }
    return b;
}

static void mapper_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_APP_REPORT_RATE_HZ);
    TickType_t last_wake = xTaskGetTickCount();
    gamepad_state_t prev_logged = {0};
    int64_t last_log_us = 0;

    while (true) {
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);

        hid_input_state_t in;
        /* Zdjecie stanu zeruje akumulatory myszy, wiec kazdy przyrost trafia
         * do dokladnie jednego raportu pada. */
        ble_hid_host_take_state(&in);

        gamepad_state_t out = {0};
        stick_from_wasd(&in, &out.lx, &out.ly);
        stick_from_mouse(&in, &out.rx, &out.ry);
        out.buttons = buttons_from_state(&in);

        /*
         * Otwieranie urzadzenia HID to najciezszy moment dla stacku: jeden link robi
         * dziesiatki procedur GATT (odkrywanie uslug, odczyt Report Map, subskrypcje),
         * a pozostale dwa sa aktywne. Oba crashe, ktore widzielismy, wypadly wlasnie
         * wtedy - i oba w wewnetrznych pulach NimBLE (AGENTS.md 4.21, 4.26). Nie
         * dokladamy do tego ~16 notyfikacji pada na sekunde.
         *
         * Raz, na wejsciu w ten stan, wysylamy raport zerowy, zeby PC nie zostal
         * z wychylona galka albo wcisnietym przyciskiem na czas przerwy.
         */
        static bool was_opening;
        bool opening = ble_hid_host_is_opening();
        if (opening) {
            if (!was_opening) {
                gamepad_state_t neutral = {0};
                ble_gamepad_send(&neutral);
                ESP_LOGI(TAG, "otwieranie urzadzenia - wstrzymuje raporty pada");
            }
            was_opening = true;
            continue;
        }
        if (was_opening) {
            ESP_LOGI(TAG, "otwieranie zakonczone - wracam do raportowania");
            was_opening = false;
        }

        bool sent = ble_gamepad_send(&out);

        /* Log tylko przy realnej zmianie i nie czesciej niz raz na 250 ms -
         * inaczej ruch myszy zalalby konsole. */
        if (sent) {
            int64_t now = esp_timer_get_time();
            bool changed = memcmp(&out, &prev_logged, sizeof(out)) != 0;
            if (changed && (now - last_log_us) > 250000) {
                last_log_us = now;
                prev_logged = out;
                ESP_LOGI(TAG, "pad: L(%4d,%4d) R(%4d,%4d) btn=0x%03x",
                         out.lx, out.ly, out.rx, out.ry, out.buttons);
            }
        }
    }
}

esp_err_t input_mapper_start(void)
{
    if (xTaskCreate(mapper_task, "mapper", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mapowanie wejsc na pada wlaczone (%d Hz, dzielnik myszy %d)",
             CONFIG_APP_REPORT_RATE_HZ, CONFIG_APP_MOUSE_SCALE_DIV);
    return ESP_OK;
}
