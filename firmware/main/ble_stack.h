/*
 * Wspolna inicjalizacja stacku NimBLE dla obu rol (central + peripheral).
 *
 * Po co osobny modul: ble_hs_cfg jest globalne i pojedyncze. W IDF wlasnie na tym
 * wykladaja sie esp_hidd i esp_hidh, gdy sa uzyte razem - kazde nadpisuje sync_cb
 * i reset_cb drugiego (AGENTS.md 4.2). Zeby nie powtorzyc tego bledu u siebie,
 * caly ble_hs_cfg ustawiamy w JEDNYM miejscu, a kolejnosc wzgledem esp_hidh_init()
 * jest wymuszona przez API: init -> (role sie rejestruja) -> start.
 *
 * Kolejnosc uzycia:
 *   ble_stack_init();          // nimble_port_init + bezpieczenstwo + magazyn kluczy
 *   ble_hid_host_start();      // wola esp_hidh_init(), ktore psuje sync_cb
 *   ble_gamepad_start();       // rejestruje usluge HID w GATT
 *   ble_stack_start();         // przywraca nasze callbacki i odpala watek hosta
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nimble_port_init() + konfiguracja ble_hs_cfg. Nie startuje jeszcze radia. */
esp_err_t ble_stack_init(void);

/* Przywraca nasze sync_cb/reset_cb (bo esp_hidh_init je nadpisuje) i uruchamia
 * watek hosta NimBLE. Wolac jako ostatnie, po zarejestrowaniu wszystkich rol. */
esp_err_t ble_stack_start(void);

/* Blokuje do momentu, w ktorym stack jest gotowy do pracy (BLE_GAP synced). */
bool ble_stack_wait_synced(uint32_t timeout_ms);

/* Typ wlasnego adresu wyliczony przez ble_hs_id_infer_auto(). */
uint8_t ble_stack_own_addr_type(void);

#ifdef __cplusplus
}
#endif
