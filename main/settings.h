#ifndef SETTINGS_H
#define SETTINGS_H

#include <esp_err.h>
#include <hx711.h>
#include <esp_http_server.h>

typedef struct {
    char *update_url;
    char *password;
    int32_t weight_tare;
    int32_t weight_scale;
    hx711_gain_t weight_gain;
} settings_t;

esp_err_t settings_init(settings_t *settings, httpd_handle_t http_server);

#endif // SETTINGS_H