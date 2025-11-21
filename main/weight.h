#ifndef WEIGHT_H
#define WEIGHT_H

#include "settings.h"
#include <esp_http_server.h>

void weight_init(settings_t *settings, httpd_handle_t server);

#endif // WEIGHT_H
