#include "metrics.h"
#include "weight.h"
#include "wifi.h"
#include "bthome_observer.h"
#include "bthome.h"
#include "IQmathLib.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "metrics";

// Context for BTHome metrics iteration
typedef struct {
    char *buffer;
    size_t buffer_size;
    int *offset;
    settings_t *settings;
    const char *hostname;
    uint8_t *seen_metrics;      // Array to track which metrics we've seen
    size_t seen_metrics_count;  // Number of unique metrics seen
} bthome_metrics_ctx_t;

// Context for first pass - collecting unique metrics
typedef struct {
    uint8_t *metric_ids;
    size_t *metric_count;
    size_t max_count;
    settings_t *settings;
} metric_collection_ctx_t;

// Helper function to check if an object ID is selected
static bool is_object_id_selected(uint8_t object_id, settings_t *settings) {
    if (settings->selected_bthome_object_ids == NULL || 
        settings->selected_bthome_object_ids_count == 0) {
        return false;
    }
    
    for (size_t i = 0; i < settings->selected_bthome_object_ids_count; i++) {
        if (settings->selected_bthome_object_ids[i] == object_id) {
            return true;
        }
    }
    
    return false;
}

// Helper function to check if a MAC address is filtered and get its name
// Returns true if the MAC should be included (no filters or explicitly enabled)
// Sets device_name to the configured name if available, otherwise to MAC string
static bool is_mac_allowed(const esp_bd_addr_t addr, settings_t *settings, 
                           char *device_name, size_t device_name_size) {
    // If no filters configured, allow all devices and use MAC as name
    if (settings->mac_filters == NULL || settings->mac_filters_count == 0) {
        snprintf(device_name, device_name_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        return true;
    }
    
    // Check if this MAC is in the filter list
    for (size_t i = 0; i < settings->mac_filters_count; i++) {
        if (memcmp(settings->mac_filters[i].mac_addr, addr, 6) == 0) {
            // Found matching filter
            if (!settings->mac_filters[i].enabled) {
                return false;  // Filter exists but is disabled
            }
            
            // Use configured name if available, otherwise use MAC
            if (settings->mac_filters[i].name[0] != '\0') {
                strncpy(device_name, settings->mac_filters[i].name, device_name_size - 1);
                device_name[device_name_size - 1] = '\0';
            } else {
                snprintf(device_name, device_name_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                         addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            }
            return true;
        }
    }
    
    // MAC not in filter list - reject when filters are configured
    return false;
}

// Helper to convert BTHome object name to Prometheus metric name
static void make_prometheus_metric_name(const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, "bthome_%s", name);
    
    // Replace spaces and hyphens with underscores, convert to lowercase
    for (char *p = out; *p; p++) {
        if (*p == ' ' || *p == '-') {
            *p = '_';
        } else if (*p >= 'A' && *p <= 'Z') {
            *p = *p + ('a' - 'A');
        }
    }
}

// First pass: collect unique metric IDs
static bool collect_metrics_iterator(const esp_bd_addr_t addr, int rssi,
                                      const bthome_packet_t *packet, void *user_data) {
    metric_collection_ctx_t *ctx = (metric_collection_ctx_t *)user_data;
    
    for (size_t i = 0; i < packet->measurement_count; i++) {
        const bthome_measurement_t *m = &packet->measurements[i];
        
        if (!is_object_id_selected(m->object_id, ctx->settings)) {
            continue;
        }
        
        // Check if we've already recorded this metric
        bool found = false;
        for (size_t j = 0; j < *ctx->metric_count; j++) {
            if (ctx->metric_ids[j] == m->object_id) {
                found = true;
                break;
            }
        }
        
        if (!found && *ctx->metric_count < ctx->max_count) {
            ctx->metric_ids[*ctx->metric_count] = m->object_id;
            (*ctx->metric_count)++;
        }
    }
    
    return true;
}

// Second pass: output metrics for a specific object ID
static bool output_metric_for_id_iterator(const esp_bd_addr_t addr, int rssi,
                                           const bthome_packet_t *packet, void *user_data) {
    bthome_metrics_ctx_t *ctx = (bthome_metrics_ctx_t *)user_data;
    char device_name[64];
    
    // Check if this MAC address is allowed by filters
    if (!is_mac_allowed(addr, ctx->settings, device_name, sizeof(device_name))) {
        return true;  // Skip this device
    }
    
    char mac_str[18];
    // Format MAC address for mac label
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    
    // Find measurements matching the current metric we're outputting
    uint8_t current_metric_id = ctx->seen_metrics[ctx->seen_metrics_count - 1];
    
    for (size_t i = 0; i < packet->measurement_count; i++) {
        const bthome_measurement_t *m = &packet->measurements[i];
        
        if (m->object_id != current_metric_id) {
            continue;
        }
        
        float factor = bthome_get_scaling_factor(m->object_id);
        float value = bthome_get_scaled_value(m, factor);
        const char *name = bthome_get_object_name(m->object_id);
        
        if (name == NULL) {
            continue;
        }
        
        char metric_name[128];
        make_prometheus_metric_name(name, metric_name, sizeof(metric_name));
        
        // Add the metric value with device name and MAC
        *ctx->offset += snprintf(ctx->buffer + *ctx->offset, 
                                 ctx->buffer_size - *ctx->offset,
                                 "%s{hostname=\"%s\",device=\"%s\",mac=\"%s\"} %.2f\n",
                                 metric_name, ctx->hostname, device_name, mac_str, value);
    }
    
    return true;
}

// Output RSSI for all devices
static bool output_rssi_iterator(const esp_bd_addr_t addr, int rssi,
                                  const bthome_packet_t *packet, void *user_data) {
    bthome_metrics_ctx_t *ctx = (bthome_metrics_ctx_t *)user_data;
    char device_name[64];
    
    // Check if this MAC address is allowed by filters
    if (!is_mac_allowed(addr, ctx->settings, device_name, sizeof(device_name))) {
        return true;  // Skip this device
    }
    
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    
    *ctx->offset += snprintf(ctx->buffer + *ctx->offset, 
                             ctx->buffer_size - *ctx->offset,
                             "bthome_rssi_dbm{hostname=\"%s\",device=\"%s\",mac=\"%s\"} %d\n",
                             ctx->hostname, device_name, mac_str, rssi);
    
    return true;
}


static esp_err_t metrics_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    
    // Allocate larger buffer for BTHome metrics
    size_t response_size = 8192;
    char *response = malloc(response_size);
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for metrics response");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    int offset = 0;
    
    // Get uptime in seconds
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_seconds = uptime_us / 1000000;
    
    // Get weight
    bool weight_available = false;
    float weight = weight_get_latest(&weight_available);
    int32_t weight_raw = weight_get_latest_raw(&weight_available);
    
    // Get WiFi RSSI
    int8_t rssi = wifi_get_rssi();
    
    // Get hostname for labels
    const char *hostname = (settings->hostname != NULL && settings->hostname[0] != '\0') 
                            ? settings->hostname : "weight-station";
    
    // Build Prometheus text format response
    // Weight metric
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP weight_grams Current weight reading in grams\n"
                      "# TYPE weight_grams gauge\n");
    
    if (weight_available) {
        offset += snprintf(response + offset, response_size - offset,
                          "weight_grams{hostname=\"%s\"} %.2f\n", hostname, weight);
    }

    // Raw weight metric
    offset += snprintf(response + offset, response_size - offset,
                      "# HELP weight_raw Current weight reading in raw units\n"
                      "# TYPE weight_raw gauge\n");
    
    if (weight_available) {
        offset += snprintf(response + offset, response_size - offset,
                          "weight_raw{hostname=\"%s\"} %" PRIi32 "\n", hostname, weight_raw);
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
    
    // Add BTHome metrics from cache (properly grouped by metric family)
    if (settings->selected_bthome_object_ids_count > 0) {
        // First pass: collect all unique metric IDs
        uint8_t unique_metrics[256];
        size_t unique_count = 0;
        
        metric_collection_ctx_t collect_ctx = {
            .metric_ids = unique_metrics,
            .metric_count = &unique_count,
            .max_count = 256,
            .settings = settings
        };
        
        bthome_cache_iterate(collect_metrics_iterator, &collect_ctx);
        
        // Output BTHome RSSI metric family first
        offset += snprintf(response + offset, response_size - offset,
                          "# HELP bthome_rssi_dbm BTHome device signal strength in dBm\n"
                          "# TYPE bthome_rssi_dbm gauge\n");
        
        bthome_metrics_ctx_t rssi_ctx = {
            .buffer = response,
            .buffer_size = response_size,
            .offset = &offset,
            .settings = settings,
            .hostname = hostname,
            .seen_metrics = NULL,
            .seen_metrics_count = 0
        };
        
        bthome_cache_iterate(output_rssi_iterator, &rssi_ctx);
        
        // Now output each unique metric family with its HELP and TYPE
        for (size_t i = 0; i < unique_count; i++) {
            uint8_t metric_id = unique_metrics[i];
            const char *name = bthome_get_object_name(metric_id);
            const char *unit_desc = bthome_get_object_unit_description(metric_id);
            
            if (name == NULL) {
                continue;
            }
            
            char metric_name[128];
            make_prometheus_metric_name(name, metric_name, sizeof(metric_name));
            
            // Output HELP and TYPE for this metric family
            offset += snprintf(response + offset, response_size - offset,
                              "# HELP %s BTHome %s", metric_name, name);
            
            if (unit_desc != NULL && strlen(unit_desc) > 0) {
                offset += snprintf(response + offset, response_size - offset,
                                  " in %s", unit_desc);
            }
            
            offset += snprintf(response + offset, response_size - offset, "\n");
            offset += snprintf(response + offset, response_size - offset,
                              "# TYPE %s gauge\n", metric_name);
            
            // Output all values for this metric family
            bthome_metrics_ctx_t metric_ctx = {
                .buffer = response,
                .buffer_size = response_size,
                .offset = &offset,
                .settings = settings,
                .hostname = hostname,
                .seen_metrics = &metric_id,
                .seen_metrics_count = 1
            };
            
            bthome_cache_iterate(output_metric_for_id_iterator, &metric_ctx);
        }
    }
    
    // Set response headers and send
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, response, offset);
    
    free(response);
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
