#include "metrics.h"
#include "wifi.h"
#include "sensors.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

static const char *TAG = "metrics";

// Define atomic malloc counters
atomic_uint_fast32_t malloc_count_settings = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t malloc_count_metrics = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t malloc_count_sensors = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t malloc_count_pump = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t malloc_count_main = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t malloc_count_http_server = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t malloc_count_syslog = ATOMIC_VAR_INIT(0);

// Define atomic free counters
atomic_uint_fast32_t free_count_settings = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_metrics = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_sensors = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_pump = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_main = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_http_server = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_syslog = ATOMIC_VAR_INIT(0);

static esp_err_t metrics_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    
    // Allocate larger buffer for BTHome metrics
    size_t response_size = 8192;
    char *response = malloc(response_size);
    atomic_fetch_add(&malloc_count_metrics, 1);
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for metrics response");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    int offset = 0;
    
    // Get uptime in seconds
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_seconds = uptime_us / 1000000;
    
    // Get WiFi RSSI
    int8_t rssi = wifi_get_rssi();
    
    // Get hostname for labels
    const char *hostname = (settings->hostname != NULL && settings->hostname[0] != '\0') 
                            ? settings->hostname : "weight-station";
    
    // Build Prometheus text format response
    
    // Get all sensors from sensors.c
    int sensor_count = sensors_get_count();
    for (int i = 0; i < sensor_count; i++) {
        const sensor_data_t *sensor = sensors_get_by_index(i);
        if (sensor == NULL || sensor->metric_name[0] == '\0') {
            continue;
        }
        
        // Output HELP and TYPE for this sensor
        offset += snprintf(response + offset, response_size - offset,
                          "# HELP %s %s", sensor->metric_name, sensor->display_name);
        
        if (sensor->unit[0] != '\0') {
            offset += snprintf(response + offset, response_size - offset,
                              " in %s", sensor->unit);
        }
        
        offset += snprintf(response + offset, response_size - offset, "\n");
        offset += snprintf(response + offset, response_size - offset,
                          "# TYPE %s gauge\n", sensor->metric_name);
        
        // Output value if available
        if (sensor->available && sensor->last_updated > 0) {
            // Convert timestamp to milliseconds for Prometheus
            int64_t timestamp_ms = (int64_t)sensor->last_updated * 1000;
            
            offset += snprintf(response + offset, response_size - offset,
                              "%s{hostname=\"%s\"%s%s%s%s%s%s} %.2f %lld\n", 
                              sensor->metric_name, hostname, 

                              sensor->device_name[0] != '\0' ? ",device_name=\"" : "",
                              sensor->device_name[0] != '\0' ? sensor->device_name : "",
                              sensor->device_name[0] != '\0' ? "\"" : "",

                              sensor->device_id[0] != '\0' ? ",device_id=\"" : "",
                              sensor->device_id[0] != '\0' ? sensor->device_id : "",
                              sensor->device_id[0] != '\0' ? "\"" : "",

                              sensor->value, timestamp_ms);
        }
    }
    
    // WiFi RSSI metric
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP wifi_rssi_dbm WiFi signal strength in dBm\n"
                      "# TYPE wifi_rssi_dbm gauge\n");
    
    if (rssi != 0) {
        offset += snprintf(response + offset, response_size - offset,
                          "wifi_rssi_dbm{hostname=\"%s\"} %d\n", hostname, rssi);
    }
    
    // Uptime metric
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP uptime_seconds System uptime in seconds\n"
                      "# TYPE uptime_seconds counter\n"
                      "uptime_seconds{hostname=\"%s\"} %lld\n", hostname, uptime_seconds);
    
    // Heap memory metrics
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP heap_free_bytes Current free heap memory in bytes\n"
                      "# TYPE heap_free_bytes gauge\n"
                      "heap_free_bytes{hostname=\"%s\"} %u\n", hostname, free_heap);
    
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP heap_min_free_bytes Minimum free heap memory ever reached in bytes\n"
                      "# TYPE heap_min_free_bytes gauge\n"
                      "heap_min_free_bytes{hostname=\"%s\"} %u\n", hostname, min_free_heap);
    
    uint32_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP heap_largest_free_block_bytes Largest contiguous free memory block in bytes\n"
                      "# TYPE heap_largest_free_block_bytes gauge\n"
                      "heap_largest_free_block_bytes{hostname=\"%s\"} %u\n", hostname, largest_free_block);
    
    // Malloc count metrics
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP malloc_count_total Total number of malloc calls per source file\n"
                      "# TYPE malloc_count_total counter\n");
    
    offset += snprintf(response + offset, response_size - offset,
                      "malloc_count_total{hostname=\"%s\",file=\"settings.c\"} %u\n", 
                      hostname, atomic_load(&malloc_count_settings));
    offset += snprintf(response + offset, response_size - offset,
                      "malloc_count_total{hostname=\"%s\",file=\"metrics.c\"} %u\n", 
                      hostname, atomic_load(&malloc_count_metrics));
    offset += snprintf(response + offset, response_size - offset,
                      "malloc_count_total{hostname=\"%s\",file=\"sensors.c\"} %u\n", 
                      hostname, atomic_load(&malloc_count_sensors));
    offset += snprintf(response + offset, response_size - offset,
                      "malloc_count_total{hostname=\"%s\",file=\"pump.c\"} %u\n", 
                      hostname, atomic_load(&malloc_count_pump));
    offset += snprintf(response + offset, response_size - offset,
                      "malloc_count_total{hostname=\"%s\",file=\"main.c\"} %u\n", 
                      hostname, atomic_load(&malloc_count_main));
    offset += snprintf(response + offset, response_size - offset,
                      "malloc_count_total{hostname=\"%s\",file=\"http_server.c\"} %u\n", 
                      hostname, atomic_load(&malloc_count_http_server));
    offset += snprintf(response + offset, response_size - offset,
                      "malloc_count_total{hostname=\"%s\",file=\"syslog.c\"} %u\n", 
                      hostname, atomic_load(&malloc_count_syslog));
    
    // Free count metrics
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP free_count_total Total number of free calls per source file\n"
                      "# TYPE free_count_total counter\n");
    
    offset += snprintf(response + offset, response_size - offset,
                      "free_count_total{hostname=\"%s\",file=\"settings.c\"} %u\n", 
                      hostname, atomic_load(&free_count_settings));
    offset += snprintf(response + offset, response_size - offset,
                      "free_count_total{hostname=\"%s\",file=\"metrics.c\"} %u\n", 
                      hostname, atomic_load(&free_count_metrics));
    offset += snprintf(response + offset, response_size - offset,
                      "free_count_total{hostname=\"%s\",file=\"sensors.c\"} %u\n", 
                      hostname, atomic_load(&free_count_sensors));
    offset += snprintf(response + offset, response_size - offset,
                      "free_count_total{hostname=\"%s\",file=\"pump.c\"} %u\n", 
                      hostname, atomic_load(&free_count_pump));
    offset += snprintf(response + offset, response_size - offset,
                      "free_count_total{hostname=\"%s\",file=\"main.c\"} %u\n", 
                      hostname, atomic_load(&free_count_main));
    offset += snprintf(response + offset, response_size - offset,
                      "free_count_total{hostname=\"%s\",file=\"http_server.c\"} %u\n", 
                      hostname, atomic_load(&free_count_http_server));
    offset += snprintf(response + offset, response_size - offset,
                      "free_count_total{hostname=\"%s\",file=\"syslog.c\"} %u\n", 
                      hostname, atomic_load(&free_count_syslog));
    
    // Set response headers and send
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, response, offset);
    
    free(response);
    atomic_fetch_add(&free_count_metrics, 1);
    return ESP_OK;
}

static httpd_uri_t metrics_uri = {
    .uri       = "/metrics",
    .method    = HTTP_GET,
    .handler   = metrics_handler,
    .user_ctx  = NULL
};

void metrics_init(settings_t *settings, httpd_handle_t server) {
    metrics_uri.user_ctx = settings;
    esp_err_t err = httpd_register_uri_handler(server, &metrics_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering metrics handler!", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Prometheus metrics endpoint registered at /metrics");
    }
}
