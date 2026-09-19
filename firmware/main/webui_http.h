/*
 * HTTP side of the configuration panel. Started and stopped by webui.c as the network comes and
 * goes, so the server never outlives the interface it was reachable on.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the server. The password is the one the panel will ask for; webui.c owns it, because it
 * is the same value used as the WPA2 key. */
esp_err_t webui_http_start(const char *password);

void webui_http_stop(void);

bool webui_http_running(void);

#ifdef __cplusplus
}
#endif
