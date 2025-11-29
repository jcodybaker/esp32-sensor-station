#include "metrics.h"
#include "weight.h"
#include "wifi.h"
#include "IQmathLib.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "metrics";

static esp_err_t metrics_handler(httpd_req_t *req) {
    char response[1024];
    int offset = 0;
    
    // Get uptime in seconds
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_seconds = uptime_us / 1000000;
    
    // Get weight
    bool weight_available = false;
    _iq8 weight = weight_get_latest(&weight_available);
    int32_t weight_raw = weight_get_latest_raw(&weight_available);
    
    // Get WiFi RSSI
    int8_t rssi = wifi_get_rssi();
    
    // Build Prometheus text format response
    // Weight metric
    offset += snprintf(response + offset, sizeof(response) - offset,
                      "# HELP weight_grams Current weight reading in grams\n"
                      "# TYPE weight_grams gauge\n");
    
    if (weight_available) {
        offset += snprintf(response + offset, sizeof(response) - offset,
                          "weight_grams %.2f\n", _IQ8toF(weight));
    }

    // Build Prometheus text format response
    // Raw weight metric
    offset += snprintf(response + offset, sizeof(response) - offset,
                      "# HELP weight_raw Current weight reading in raw units\n"
                      "# TYPE weight_raw gauge\n");
    
    if (weight_available) {
        offset += snprintf(response + offset, sizeof(response) - offset,
                          "weight_raw %" PRIi32 "\n", weight_raw);
    }
    
    // WiFi RSSI metric
    offset += snprintf(response + offset, sizeof(response) - offset,
                      "# HELP wifi_rssi_dbm WiFi signal strength in dBm\n"
                      "# TYPE wifi_rssi_dbm gauge\n");
    
    if (rssi != 0) {
        offset += snprintf(response + offset, sizeof(response) - offset,
                          "wifi_rssi_dbm %d\n", rssi);
    }
    
    // Uptime metric
    offset += snprintf(response + offset, sizeof(response) - offset,
                      "# HELP uptime_seconds System uptime in seconds\n"
                      "# TYPE uptime_seconds counter\n"
                      "uptime_seconds %lld\n", uptime_seconds);
    
    // Set response headers and send
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, response, offset);
    
    return ESP_OK;
}

static httpd_uri_t metrics_uri = {
    .uri       = "/metrics",
    .method    = HTTP_GET,
    .handler   = metrics_handler,
    .user_ctx  = NULL
};

void metrics_init(httpd_handle_t server) {
    esp_err_t err = httpd_register_uri_handler(server, &metrics_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering metrics handler!", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Prometheus metrics endpoint registered at /metrics");
    }
}
