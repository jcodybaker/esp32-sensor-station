#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bthome.h"
#include "bthome_ble.h"
#include "settings.h"
#include "http_server.h"
#include "bthome_observer.h"

static const char *TAG = "bthome_observer";

#define CACHE_SIZE 10

// LFU Cache Entry
typedef struct {
    esp_bd_addr_t addr;
    int rssi;
    bthome_packet_t packet;
    uint32_t frequency;     // Access frequency counter
    uint32_t last_access;   // Timestamp of last access
    bool occupied;          // Whether this slot is in use
} cache_entry_t;

// LFU Cache
static cache_entry_t packet_cache[CACHE_SIZE];
static uint32_t global_timestamp = 0;
static SemaphoreHandle_t cache_mutex = NULL;

// Compare two MAC addresses
static bool mac_equal(const esp_bd_addr_t a, const esp_bd_addr_t b) {
    return memcmp(a, b, 6) == 0;
}

// Find the LFU entry to evict (lowest frequency, oldest if tie)
static int find_lfu_entry(void) {
    int lfu_idx = -1;
    uint32_t min_freq = UINT32_MAX;
    uint32_t oldest_time = UINT32_MAX;
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!packet_cache[i].occupied) {
            return i;  // Use empty slot first
        }
        
        if (packet_cache[i].frequency < min_freq || 
            (packet_cache[i].frequency == min_freq && packet_cache[i].last_access < oldest_time)) {
            min_freq = packet_cache[i].frequency;
            oldest_time = packet_cache[i].last_access;
            lfu_idx = i;
        }
    }
    
    return lfu_idx;
}

// Find entry by MAC address
static int find_entry_by_mac(const esp_bd_addr_t addr) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (packet_cache[i].occupied && mac_equal(packet_cache[i].addr, addr)) {
            return i;
        }
    }
    return -1;
}

// Add or update packet in cache
static void cache_packet(esp_bd_addr_t addr, int rssi, const bthome_packet_t *packet) {
    if (cache_mutex == NULL) {
        return;
    }
    
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    global_timestamp++;
    
    // Check if MAC already exists
    int idx = find_entry_by_mac(addr);
    
    if (idx >= 0) {
        // Update existing entry
        bthome_packet_free(&packet_cache[idx].packet);
        bthome_packet_init(&packet_cache[idx].packet);
        if (bthome_packet_copy(&packet_cache[idx].packet, packet) == 0) {
            packet_cache[idx].rssi = rssi;
            packet_cache[idx].frequency++;
            packet_cache[idx].last_access = global_timestamp;
        } else {
            ESP_LOGE(TAG, "Failed to copy packet for cache update");
        }
    } else {
        // Find slot to use (LFU eviction)
        idx = find_lfu_entry();
        
        if (idx >= 0) {
            if (packet_cache[idx].occupied) {
                bthome_packet_free(&packet_cache[idx].packet);
            }
            
            bthome_packet_init(&packet_cache[idx].packet);
            if (bthome_packet_copy(&packet_cache[idx].packet, packet) == 0) {
                memcpy(packet_cache[idx].addr, addr, 6);
                packet_cache[idx].rssi = rssi;
                packet_cache[idx].frequency = 1;
                packet_cache[idx].last_access = global_timestamp;
                packet_cache[idx].occupied = true;
            } else {
                ESP_LOGE(TAG, "Failed to copy packet for new cache entry");
            }
        }
    }
    
    xSemaphoreGive(cache_mutex);
}

// HTTP handler for displaying cached packets
static esp_err_t bthome_packets_handler(httpd_req_t *req) {
    if (cache_mutex == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cache not initialized");
        return ESP_FAIL;
    }
    
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    
    // Start HTML response
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html>\n<html>\n<head>\n<title>BTHome Packets</title>\n");
    httpd_resp_sendstr_chunk(req, "<meta name='viewport' content='width=device-width, initial-scale=1'>\n");
    httpd_resp_sendstr_chunk(req, "<style>\n");
    httpd_resp_sendstr_chunk(req, "body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; }\n");
    httpd_resp_sendstr_chunk(req, "h1 { color: #333; }\n");
    httpd_resp_sendstr_chunk(req, "a { color: #4CAF50; text-decoration: none; font-size: 18px; }\n");
    httpd_resp_sendstr_chunk(req, "a:hover { text-decoration: underline; }\n");
    httpd_resp_sendstr_chunk(req, ".packet { border: 1px solid #ddd; margin: 20px 0; padding: 20px; border-radius: 8px; background: #f4f4f4; }\n");
    httpd_resp_sendstr_chunk(req, ".mac { font-weight: bold; color: #0066cc; font-size: 1.2em; margin-bottom: 10px; }\n");
    httpd_resp_sendstr_chunk(req, ".rssi { color: #666; margin-bottom: 10px; font-size: 0.95em; }\n");
    httpd_resp_sendstr_chunk(req, ".measurement { margin: 8px 0 8px 20px; padding: 8px; background: #fff; border-left: 3px solid #4CAF50; border-radius: 4px; }\n");
    httpd_resp_sendstr_chunk(req, ".event { margin: 8px 0 8px 20px; padding: 8px; background: #fff; border-left: 3px solid #FF9800; border-radius: 4px; }\n");
    httpd_resp_sendstr_chunk(req, ".info { margin: 8px 0 8px 20px; color: #666; font-size: 0.9em; background: #fff; padding: 6px; border-radius: 4px; }\n");
    httpd_resp_sendstr_chunk(req, ".no-data { text-align: center; color: #666; padding: 40px 20px; background: #f4f4f4; border-radius: 8px; margin: 20px 0; }\n");
    httpd_resp_sendstr_chunk(req, "</style>\n</head>\n<body>\n");
    httpd_resp_sendstr_chunk(req, "<h1>BTHome Packets (LFU Cache)</h1>\n");
    httpd_resp_sendstr_chunk(req, "<a href='/'>Home</a> | <a href='/settings'>Settings</a><br><br>\n");
    
    int count = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!packet_cache[i].occupied) {
            continue;
        }
        count++;
        
        cache_entry_t *entry = &packet_cache[i];
        char buffer[512];
        
        // MAC address and RSSI
        snprintf(buffer, sizeof(buffer), 
                "<div class='packet'><div class='mac'>%02X:%02X:%02X:%02X:%02X:%02X</div>",
                entry->addr[0], entry->addr[1], entry->addr[2], 
                entry->addr[3], entry->addr[4], entry->addr[5]);
        httpd_resp_sendstr_chunk(req, buffer);
        
        snprintf(buffer, sizeof(buffer), 
                "<div class='rssi'>RSSI: %d dBm | Frequency: %lu | Last: %lu</div>",
                entry->rssi, (unsigned long)entry->frequency, (unsigned long)entry->last_access);
        httpd_resp_sendstr_chunk(req, buffer);
        
        // Device name
        if (entry->packet.device_name != NULL && entry->packet.device_name_len > 0) {
            size_t copy_len = entry->packet.device_name_len < 255 ? entry->packet.device_name_len : 255;
            char name_buffer[256];
            memcpy(name_buffer, entry->packet.device_name, copy_len);
            name_buffer[copy_len] = '\0';
            
            snprintf(buffer, sizeof(buffer), 
                    "<div class='info'>Device Name: \"%s\" (%s)</div>",
                    name_buffer, entry->packet.use_complete_name ? "Complete" : "Shortened");
            httpd_resp_sendstr_chunk(req, buffer);
        }
        
        // Device info
        snprintf(buffer, sizeof(buffer),
                "<div class='info'>Version: %d | Encrypted: %s | Trigger-based: %s</div>",
                entry->packet.device_info.version,
                entry->packet.device_info.encrypted ? "Yes" : "No",
                entry->packet.device_info.trigger_based ? "Yes" : "No");
        httpd_resp_sendstr_chunk(req, buffer);
        
        if (entry->packet.has_packet_id) {
            snprintf(buffer, sizeof(buffer), "<div class='info'>Packet ID: %d</div>", entry->packet.packet_id);
            httpd_resp_sendstr_chunk(req, buffer);
        }
        
        // Measurements
        for (size_t j = 0; j < entry->packet.measurement_count; j++) {
            const bthome_measurement_t *m = &entry->packet.measurements[j];
            float factor = bthome_get_scaling_factor(m->object_id);
            float value = bthome_get_scaled_value(m, factor);
            const char *name = bthome_get_object_name(m->object_id);
            const char *unit = bthome_get_object_unit(m->object_id);
            
            if (name != NULL) {
                if (unit != NULL && strlen(unit) > 0) {
                    snprintf(buffer, sizeof(buffer),
                            "<div class='measurement'>%s: %.2f %s (0x%02X)</div>",
                            name, value, unit, m->object_id);
                } else {
                    snprintf(buffer, sizeof(buffer),
                            "<div class='measurement'>%s: %.2f (0x%02X)</div>",
                            name, value, m->object_id);
                }
            } else {
                snprintf(buffer, sizeof(buffer),
                        "<div class='measurement'>Object 0x%02X: %.2f</div>",
                        m->object_id, value);
            }
            httpd_resp_sendstr_chunk(req, buffer);
        }
        
        // Events
        for (size_t j = 0; j < entry->packet.event_count; j++) {
            const bthome_event_t *e = &entry->packet.events[j];
            
            if (e->event_type == BTHOME_EVENT_BUTTON) {
                const char *event_str = "Unknown";
                switch (e->event_value) {
                    case BTHOME_BUTTON_PRESS: event_str = "Press"; break;
                    case BTHOME_BUTTON_DOUBLE_PRESS: event_str = "Double Press"; break;
                    case BTHOME_BUTTON_TRIPLE_PRESS: event_str = "Triple Press"; break;
                    case BTHOME_BUTTON_LONG_PRESS: event_str = "Long Press"; break;
                    case BTHOME_BUTTON_LONG_DOUBLE_PRESS: event_str = "Long Double Press"; break;
                    case BTHOME_BUTTON_LONG_TRIPLE_PRESS: event_str = "Long Triple Press"; break;
                    case BTHOME_BUTTON_HOLD_PRESS: event_str = "Hold Press"; break;
                }
                snprintf(buffer, sizeof(buffer),
                        "<div class='event'>Button Event: %s (0x%02X, value=%d)</div>",
                        event_str, e->event_type, e->event_value);
            } else if (e->event_type == BTHOME_EVENT_DIMMER) {
                const char *event_str = "Unknown";
                switch (e->event_value) {
                    case BTHOME_DIMMER_ROTATE_LEFT: event_str = "Rotate Left"; break;
                    case BTHOME_DIMMER_ROTATE_RIGHT: event_str = "Rotate Right"; break;
                }
                snprintf(buffer, sizeof(buffer),
                        "<div class='event'>Dimmer Event: %s, Steps: %d (0x%02X)</div>",
                        event_str, e->steps, e->event_type);
            } else {
                snprintf(buffer, sizeof(buffer),
                        "<div class='event'>Event 0x%02X: value=%d, steps=%d</div>",
                        e->event_type, e->event_value, e->steps);
            }
            httpd_resp_sendstr_chunk(req, buffer);
        }
        
        httpd_resp_sendstr_chunk(req, "</div>");
    }
    
    if (count == 0) {
        httpd_resp_sendstr_chunk(req, "<div class='no-data'>No packets cached yet.</div>");
    }
    
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);  // End chunked response
    
    xSemaphoreGive(cache_mutex);
    return ESP_OK;
}

static void bthome_packet_callback(esp_bd_addr_t addr, int rssi, 
                                    const bthome_packet_t *packet, void *user_data) {
    // Cache the packet first
    cache_packet(addr, rssi, packet);
    
    // Format MAC address
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    
    ESP_LOGI(TAG, "BTHome packet from %s (RSSI: %d dBm)", mac_str, rssi);
    
    // Print device name if present
    if (packet->device_name != NULL && packet->device_name_len > 0) {
        // Create a temporary buffer to null-terminate the name for printing
        char name_buffer[256];
        size_t copy_len = packet->device_name_len < sizeof(name_buffer) - 1 ? 
                         packet->device_name_len : sizeof(name_buffer) - 1;
        memcpy(name_buffer, packet->device_name, copy_len);
        name_buffer[copy_len] = '\0';
        
        ESP_LOGI(TAG, "  Device Name: \"%s\" (%s)", name_buffer, 
                 packet->use_complete_name ? "Complete" : "Shortened");
    }
    
    ESP_LOGI(TAG, "  Version: %d, Encrypted: %d, Trigger-based: %d",
             packet->device_info.version,
             packet->device_info.encrypted,
             packet->device_info.trigger_based);
    
    if (packet->has_packet_id) {
        ESP_LOGI(TAG, "  Packet ID: %d", packet->packet_id);
    }
    
    // Print all measurements
    for (size_t i = 0; i < packet->measurement_count; i++) {
        const bthome_measurement_t *m = &packet->measurements[i];
        float factor = bthome_get_scaling_factor(m->object_id);
        float value = bthome_get_scaled_value(m, factor);
        
        ESP_LOGI(TAG, "  Measurement 0x%02X: %.2f", m->object_id, value);
        
        // Specific sensor type examples
        switch (m->object_id) {
            case BTHOME_SENSOR_TEMPERATURE:
                ESP_LOGI(TAG, "    Temperature: %.2f °C", value);
                break;
            case BTHOME_SENSOR_HUMIDITY:
                ESP_LOGI(TAG, "    Humidity: %.2f %%", value);
                break;
            case BTHOME_SENSOR_BATTERY:
                ESP_LOGI(TAG, "    Battery: %d %%", (int)value);
                break;
            case BTHOME_SENSOR_PRESSURE:
                ESP_LOGI(TAG, "    Pressure: %.2f hPa", value);
                break;
            case BTHOME_SENSOR_ILLUMINANCE:
                ESP_LOGI(TAG, "    Illuminance: %.2f lux", value);
                break;
            case BTHOME_SENSOR_DISTANCE_MM:
                ESP_LOGI(TAG, "    Distance: %.2f mm", value);
                break;
            case BTHOME_BINARY_VIBRATION:
                ESP_LOGI(TAG, "    Vibration: %s", value ? "Detected" : "Not Detected");
                break;
            default:
                break;
        }
    }
    
    // Print all events
    for (size_t i = 0; i < packet->event_count; i++) {
        const bthome_event_t *e = &packet->events[i];
        ESP_LOGI(TAG, "  Event 0x%02X: value=%d, steps=%d", 
                 e->event_type, e->event_value, e->steps);
        
        if (e->event_type == BTHOME_EVENT_BUTTON) {
            const char *event_str = "Unknown";
            switch (e->event_value) {
                case BTHOME_BUTTON_PRESS: event_str = "Press"; break;
                case BTHOME_BUTTON_DOUBLE_PRESS: event_str = "Double Press"; break;
                case BTHOME_BUTTON_TRIPLE_PRESS: event_str = "Triple Press"; break;
                case BTHOME_BUTTON_LONG_PRESS: event_str = "Long Press"; break;
                case BTHOME_BUTTON_HOLD_PRESS: event_str = "Hold Press"; break;
            }
            ESP_LOGI(TAG, "    Button Event: %s", event_str);
        }
    }
}

void bthome_observer_init(settings_t *settings, httpd_handle_t server) {
    // Initialize cache
    memset(packet_cache, 0, sizeof(packet_cache));
    cache_mutex = xSemaphoreCreateMutex();
    if (cache_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create cache mutex");
        return;
    }
    
    // Initialize NVS (required for BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI(TAG, "Starting BTHome BLE Scanner");
    
    // Initialize the BLE scanner
    ret = bthome_ble_scanner_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE scanner: %s", esp_err_to_name(ret));
        return;
    }
    
    // Configure scanner
    bthome_ble_scanner_config_t config;
    bthome_ble_scanner_get_default_config(&config);
    
    // Set callback function
    config.callback = bthome_packet_callback;
    config.user_data = NULL;
    
    // Use passive scanning (lower power)
    config.scan_type = BLE_SCAN_TYPE_PASSIVE;
    
    // Scan interval and window (in units of 0.625ms)
    config.scan_interval = 0x50;  // 50ms
    config.scan_window = 0x30;    // 30ms
    
    // Start continuous scanning (scan_duration = 0)
    config.scan_duration = 0;
    
    // Start scanning
    ret = bthome_ble_scanner_start(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE scanner: %s", esp_err_to_name(ret));
        bthome_ble_scanner_deinit();
        return;
    }
    
    ESP_LOGI(TAG, "BLE scanner started, listening for BTHome advertisements...");
    
    // Register HTTP handler for cached packets
    httpd_uri_t packets_uri = {
        .uri       = "/bthome/packets",
        .method    = HTTP_GET,
        .handler   = bthome_packets_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler_with_basic_auth(settings, server, &packets_uri);
    ESP_LOGI(TAG, "Registered HTTP handler at /bthome/packets");
}

// Iterate through all cached BTHome packets
void bthome_cache_iterate(bthome_cache_iterator_t callback, void *user_data) {
    if (cache_mutex == NULL || callback == NULL) {
        return;
    }
    
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (packet_cache[i].occupied) {
            bool continue_iteration = callback(
                packet_cache[i].addr,
                packet_cache[i].rssi,
                &packet_cache[i].packet,
                user_data
            );
            
            if (!continue_iteration) {
                break;
            }
        }
    }
    
    xSemaphoreGive(cache_mutex);
}
