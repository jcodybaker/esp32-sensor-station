#ifndef SETTINGS_H
#define SETTINGS_H

#include <esp_err.h>
#include <hx711.h>
#include <esp_http_server.h>
#include "IQmathLib.h"

typedef struct {
    char *update_url;
    char *password;
    int32_t weight_tare;
    _iq8 weight_scale;
    hx711_gain_t weight_gain;
    char * wifi_ssid;
    char * wifi_password;
    bool wifi_ap_fallback_disable;
} settings_t;

esp_err_t settings_init(settings_t *settings);

esp_err_t settings_register(settings_t *settings, httpd_handle_t http_server);

#endif // SETTINGS_H