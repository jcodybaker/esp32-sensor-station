#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <esp_http_server.h>
#include "settings.h"

esp_err_t ota_init(settings_t *settings, httpd_handle_t http_server, bool ota_mode);
esp_err_t ota_check_pending_update(settings_t *settings);
void ota_trigger_update_on_wifi_connect(void);
const char* ota_get_last_status(void);

#endif // OTA_H
