#ifndef SETTINGS_H
#define SETTINGS_H

#include <esp_err.h>
#include <hx711.h>
#include <esp_http_server.h>
#include <esp_gap_ble_api.h>
#include "IQmathLib.h"

// Structure to hold MAC address filter configuration
typedef struct {
    esp_bd_addr_t mac_addr;  // 6-byte MAC address
    char name[32];           // Human-readable name for the device
    bool enabled;            // Whether this filter is active
} mac_filter_t;

typedef struct {
    char *update_url;
    char *password;
    int32_t weight_tare;
    _iq16 weight_scale;
    hx711_gain_t weight_gain;
    char * wifi_ssid;
    char * wifi_password;
    bool wifi_ap_fallback_disable;
    char * hostname;
    uint8_t *selected_bthome_object_ids;
    size_t selected_bthome_object_ids_count;
    mac_filter_t *mac_filters;         // Array of MAC address filters
    size_t mac_filters_count;          // Number of MAC address filters
} settings_t;

esp_err_t settings_init(settings_t *settings);

esp_err_t settings_register(settings_t *settings, httpd_handle_t http_server);

#endif // SETTINGS_H