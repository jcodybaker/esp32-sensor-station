#ifndef WEIGHT_H
#define WEIGHT_H

#include "settings.h"
#include <esp_http_server.h>
#include <stdint.h>
#include <stdbool.h>
#include "IQmathLib.h"

void weight_init(settings_t *settings, httpd_handle_t server);
_iq8 weight_get_latest(bool *available);
uint32_t weight_get_latest_raw(bool *available);

#endif // WEIGHT_H
