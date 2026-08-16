/*
 * Rola central: odbior raportow HID z klawiatury i myszy BLE (profil HOGP).
 *
 * Modul startuje stack NimBLE, skanuje w petli i podlacza sie do KAZDEGO
 * znalezionego urzadzenia HID, az uzbiera zalozony limit. Wbrew wzorcowi
 * z OpenLary nie ma tu jednej flagi "polaczono" - potrzebujemy klawiatury
 * i myszy naraz (AGENTS.md 4.3).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot protocol klawiatury USB HID: 6 jednoczesnie wcisnietych klawiszy. */
#define HID_KEYS_MAX 6

typedef struct {
    /* Klawiatura: bitmapa modyfikatorow (bit0 LCtrl ... bit7 RGui) i keycody USB HID. */
    uint8_t modifiers;
    uint8_t keys[HID_KEYS_MAX];

    /* Mysz: bitmapa przyciskow (bit0 lewy, bit1 prawy, bit2 srodkowy). */
    uint8_t mouse_buttons;

    /* Mysz: ruch zakumulowany od poprzedniego odczytu stanu. Mysz raportuje
     * przyrosty, a zadanie pada chodzi z inna czestotliwoscia, wiec sumujemy. */
    int32_t mouse_dx;
    int32_t mouse_dy;
    int32_t mouse_wheel;

    bool keyboard_connected;
    bool mouse_connected;
} hid_input_state_t;

/* Startuje NimBLE, esp_hidh i zadanie skanujace. Wolac raz, z app_main. */
esp_err_t ble_hid_host_start(void);

/* Snapshot stanu wejsc. Akumulatory ruchu myszy sa przy tym zerowane, wiec
 * kazdy przyrost trafia do dokladnie jednego raportu pada. */
void ble_hid_host_take_state(hid_input_state_t *out);

/* Liczba aktualnie otwartych urzadzen HID (0..HID_HOST_MAX_DEVICES). */
int ble_hid_host_device_count(void);

/*
 * Czy trwa wlasnie otwieranie urzadzenia (polaczenie + pelne odkrywanie uslug GATT).
 * To najciezszy moment dla stacku: jeden link robi dziesiatki procedur GATT, a
 * pozostale dwa sa aktywne. Zadanie mapujace wstrzymuje wtedy notyfikacje pada,
 * zeby nie dokladac obciazenia - patrz AGENTS.md 4.26.
 */
bool ble_hid_host_is_opening(void);

/* Wypisuje na konsole liste podlaczonych urzadzen - do diagnostyki. */
void ble_hid_host_log_devices(void);

#ifdef __cplusplus
}
#endif
