#include "metrics.h"
#include "wifi.h"
#include "sensors.h"
#include "http_server.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
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
atomic_uint_fast32_t malloc_count_mqtt_publisher = ATOMIC_VAR_INIT(0);

// Define atomic free counters
atomic_uint_fast32_t free_count_settings = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_metrics = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_sensors = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_pump = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_main = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_http_server = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_syslog = ATOMIC_VAR_INIT(0);
atomic_uint_fast32_t free_count_mqtt_publisher = ATOMIC_VAR_INIT(0);

// Worst-case single chunk is a per-sensor metric line: metric/display name
// (40 bytes each, x3 occurrences across the HELP/TYPE/value lines) + unit
// (16) + hostname (settings form caps this at 255 bytes, see param_buf in
// settings.c) + device_name (32) + device_id (20) + a generously-bounded
// float/timestamp rendering (48 + 20) + surrounding literal text (~70).
// That totals ~620 bytes; round up with headroom.
#define METRICS_CHUNK_BUF_SIZE 1024

// Formats into buf (capped at METRICS_CHUNK_BUF_SIZE) and sends it as one
// HTTP chunk, so the overall response can grow with the sensor count without
// needing a buffer sized for the whole response.
static esp_err_t metrics_emit(httpd_req_t *req, char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, METRICS_CHUNK_BUF_SIZE, fmt, args);
    va_end(args);
    if (len < 0) {
        return ESP_FAIL;
    }
    if (len >= METRICS_CHUNK_BUF_SIZE) {
        len = METRICS_CHUNK_BUF_SIZE - 1;
    }
    return httpd_resp_send_chunk(req, buf, len);
}

static esp_err_t metrics_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;

    char *buf = malloc(METRICS_CHUNK_BUF_SIZE);
    atomic_fetch_add(&malloc_count_metrics, 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for metrics chunk buffer");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get uptime in seconds
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_seconds = uptime_us / 1000000;

    // Get WiFi RSSI
    int8_t rssi = wifi_get_rssi();

    // Get hostname for labels
    const char *hostname = (settings->hostname != NULL && settings->hostname[0] != '\0')
                            ? settings->hostname : "weight-station";

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    esp_err_t err = ESP_OK;

    // Build Prometheus text format response, one sensor per chunk
    int sensor_count = sensors_get_count();
    for (int i = 0; i < sensor_count && err == ESP_OK; i++) {
        const sensor_data_t *sensor = sensors_get_by_index(i);
        if (sensor == NULL || sensor->metric_name[0] == '\0') {
            continue;
        }

        err = metrics_emit(req, buf, "# HELP %s %s%s%s\n# TYPE %s gauge\n",
                          sensor->metric_name, sensor->display_name,
                          sensor->unit[0] != '\0' ? " in " : "",
                          sensor->unit[0] != '\0' ? sensor->unit : "",
                          sensor->metric_name);
        if (err != ESP_OK) {
            break;
        }

        // Output value if available
        if (sensor->available && sensor->last_updated > 0) {
            // Convert timestamp to milliseconds for Prometheus
            int64_t timestamp_ms = (int64_t)sensor->last_updated * 1000;

            err = metrics_emit(req, buf,
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
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "# HELP wifi_rssi_dbm WiFi signal strength in dBm\n"
                          "# TYPE wifi_rssi_dbm gauge\n");
    }
    if (err == ESP_OK && rssi != 0) {
        err = metrics_emit(req, buf, "wifi_rssi_dbm{hostname=\"%s\"} %d\n", hostname, rssi);
    }

    // Uptime metric
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "# HELP uptime_seconds System uptime in seconds\n"
                          "# TYPE uptime_seconds counter\n"
                          "uptime_seconds{hostname=\"%s\"} %lld\n", hostname, uptime_seconds);
    }

    // Heap memory metrics
    if (err == ESP_OK) {
        uint32_t free_heap = esp_get_free_heap_size();
        err = metrics_emit(req, buf,
                          "# HELP heap_free_bytes Current free heap memory in bytes\n"
                          "# TYPE heap_free_bytes gauge\n"
                          "heap_free_bytes{hostname=\"%s\"} %lu\n", hostname, free_heap);
    }

    if (err == ESP_OK) {
        uint32_t min_free_heap = esp_get_minimum_free_heap_size();
        err = metrics_emit(req, buf,
                          "# HELP heap_min_free_bytes Minimum free heap memory ever reached in bytes\n"
                          "# TYPE heap_min_free_bytes gauge\n"
                          "heap_min_free_bytes{hostname=\"%s\"} %lu\n", hostname, min_free_heap);
    }

    if (err == ESP_OK) {
        uint32_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        err = metrics_emit(req, buf,
                          "# HELP heap_largest_free_block_bytes Largest contiguous free memory block in bytes\n"
                          "# TYPE heap_largest_free_block_bytes gauge\n"
                          "heap_largest_free_block_bytes{hostname=\"%s\"} %lu\n", hostname, largest_free_block);
    }

    // Malloc count metrics
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "# HELP malloc_count_total Total number of malloc calls per source file\n"
                          "# TYPE malloc_count_total counter\n");
    }

    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"settings.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_settings));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"metrics.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_metrics));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"sensors.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_sensors));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"pump.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_pump));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"main.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_main));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"http_server.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_http_server));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"syslog.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_syslog));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "malloc_count_total{hostname=\"%s\",file=\"mqtt_publisher.c\"} %u\n",
                          hostname, atomic_load(&malloc_count_mqtt_publisher));
    }

    // Free count metrics
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "# HELP free_count_total Total number of free calls per source file\n"
                          "# TYPE free_count_total counter\n");
    }

    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"settings.c\"} %u\n",
                          hostname, atomic_load(&free_count_settings));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"metrics.c\"} %u\n",
                          hostname, atomic_load(&free_count_metrics));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"sensors.c\"} %u\n",
                          hostname, atomic_load(&free_count_sensors));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"pump.c\"} %u\n",
                          hostname, atomic_load(&free_count_pump));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"main.c\"} %u\n",
                          hostname, atomic_load(&free_count_main));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"http_server.c\"} %u\n",
                          hostname, atomic_load(&free_count_http_server));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"syslog.c\"} %u\n",
                          hostname, atomic_load(&free_count_syslog));
    }
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "free_count_total{hostname=\"%s\",file=\"mqtt_publisher.c\"} %u\n",
                          hostname, atomic_load(&free_count_mqtt_publisher));
    }

    // HTTP request/outcome metrics, one series per (method, uri) route
    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "# HELP http_requests_total Total HTTP requests received, per route\n"
                          "# TYPE http_requests_total counter\n");
    }
    int http_route_count = http_metrics_route_count();
    for (int i = 0; i < http_route_count && err == ESP_OK; i++) {
        http_route_metric_snapshot_t route;
        http_metrics_route_get(i, &route);
        err = metrics_emit(req, buf,
                          "http_requests_total{hostname=\"%s\",method=\"%s\",uri=\"%s\"} %lu\n",
                          hostname, route.method, route.uri, (unsigned long)route.total_count);
    }

    if (err == ESP_OK) {
        err = metrics_emit(req, buf,
                          "# HELP http_requests_failed_total Total HTTP requests that resulted in an error response, per route\n"
                          "# TYPE http_requests_failed_total counter\n");
    }
    for (int i = 0; i < http_route_count && err == ESP_OK; i++) {
        http_route_metric_snapshot_t route;
        http_metrics_route_get(i, &route);
        err = metrics_emit(req, buf,
                          "http_requests_failed_total{hostname=\"%s\",method=\"%s\",uri=\"%s\"} %lu\n",
                          hostname, route.method, route.uri, (unsigned long)route.failed_count);
    }

    // Terminate the chunked response
    httpd_resp_send_chunk(req, NULL, 0);

    free(buf);
    atomic_fetch_add(&free_count_metrics, 1);
    return err;
}

static httpd_uri_t metrics_uri = {
    .uri       = "/metrics",
    .method    = HTTP_GET,
    .handler   = metrics_handler,
    .user_ctx  = NULL
};

void metrics_init(settings_t *settings, httpd_handle_t server) {
    metrics_uri.user_ctx = settings;
    esp_err_t err = httpd_register_uri_handler_instrumented(server, &metrics_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering metrics handler!", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Prometheus metrics endpoint registered at /metrics");
    }
}
