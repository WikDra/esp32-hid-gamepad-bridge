/*
 * Rola peripheral: pad HID widziany przez PC.
 *
 * Usluga GATT 0x1812 pochodzi z NimBLE (ble_svc_hid) - patrz AGENTS.md 4.9.
 * Tutaj jest tylko deskryptor raportu, advertising, obsluga jednego polaczenia
 * (z jawnym sprawdzeniem roli, czego brakuje w esp_hidd) i wysylka notyfikacji.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Krzyzak (hat switch). Bitmapa, bo klawisze strzalek sa niezalezne; przeciwne
 * kierunki wcisniete naraz znosza sie w ble_gamepad.c.
 *
 * UWAGA: krzyzak dziala tylko w profilu Xbox. Deskryptor pada generycznego nie ma
 * hat switcha, a dodanie go wymagaloby re-parowania tamtego profilu w Windows. */
#define GAMEPAD_DPAD_UP    0x01
#define GAMEPAD_DPAD_RIGHT 0x02
#define GAMEPAD_DPAD_DOWN  0x04
#define GAMEPAD_DPAD_LEFT  0x08

/* Osie sa 8-bitowe ze znakiem, zakres -127..127, srodek 0.
 * Przyciski: bit 0 = przycisk 1, ..., bit 11 = przycisk 12. */
typedef struct {
    int8_t lx;
    int8_t ly;
    int8_t rx;
    int8_t ry;
    uint16_t buttons;
    uint8_t dpad;
} gamepad_state_t;

/* Rejestruje usluge HID w GATT i uruchamia zadanie rozglaszajace.
 * Wolac po ble_stack_init(), a przed ble_stack_start(). */
esp_err_t ble_gamepad_start(void);

/* Czy PC jest podlaczony i zapisany na notyfikacje raportu. */
bool ble_gamepad_is_ready(void);

/* Wysyla raport, ale tylko gdy stan rozni sie od poprzednio wyslanego.
 * Zwraca true, jesli notyfikacja poszla. */
bool ble_gamepad_send(const gamepad_state_t *state);

#ifdef __cplusplus
}
#endif
