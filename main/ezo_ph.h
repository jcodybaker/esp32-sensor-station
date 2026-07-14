#ifndef EZO_PH_H
#define EZO_PH_H

#include "settings.h"
#include <esp_http_server.h>

void ezo_ph_init(settings_t *settings, httpd_handle_t server);

const char* ezo_ph_get_last_error(void);

#endif // EZO_PH_H
