#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bthome.h"
#include "bthome_ble.h"
#include "settings.h"
#include "http_server.h"
#include "bthome_observer.h"
#include "sensors.h"

static const char *TAG = "bthome_observer";
extern bool g_ntp_initialized;

#define CACHE_SIZE 10
#define MAX_BTHOME_SENSORS 50

#define BTHOME_SENSOR_TEMPERATURE_F 0xF1  // Custom ID for Fahrenheit temperature

// BTHome sensor mapping for integration with sensor system
typedef struct {
    esp_bd_addr_t addr;
    uint8_t object_id;
    int sensor_id;
    bool registered;
} bthome_sensor_mapping_t;

static bthome_sensor_mapping_t bthome_sensor_map[MAX_BTHOME_SENSORS];
static int bthome_sensor_count = 0;
static SemaphoreHandle_t sensor_map_mutex = NULL;
static settings_t *g_settings = NULL;

// Compare two MAC addresses
static bool mac_equal(const esp_bd_addr_t a, const esp_bd_addr_t b) {
    return memcmp(a, b, 6) == 0;
}

// Check if a MAC address is in the enabled filters
static bool is_mac_enabled(const esp_bd_addr_t addr, char *name_out, size_t name_size) {
    if (g_settings == NULL || g_settings->mac_filters == NULL) {
        return false;
    }
    
    for (size_t i = 0; i < g_settings->mac_filters_count; i++) {
        if (g_settings->mac_filters[i].enabled &&
            mac_equal(g_settings->mac_filters[i].mac_addr, addr)) {
            if (name_out && name_size > 0) {
                strncpy(name_out, g_settings->mac_filters[i].name, name_size - 1);
                name_out[name_size - 1] = '\0';
            }
            return true;
        }
    }
    return false;
}

// Check if an object ID is selected
static bool is_object_id_selected(uint8_t object_id) {
    if (g_settings == NULL || g_settings->selected_bthome_object_ids == NULL) {
        return false;
    }
    
    for (size_t i = 0; i < g_settings->selected_bthome_object_ids_count; i++) {
        if (g_settings->selected_bthome_object_ids[i] == object_id) {
            return true;
        }
    }
    return false;
}

// LFU Cache Entry
typedef struct {
    esp_bd_addr_t addr;
    int rssi;
    bthome_packet_t packet;
    uint32_t frequency;     // Access frequency counter
    struct timeval last_seen;   // Timestamp of last access
    bool occupied;          // Whether this slot is in use
} cache_entry_t;

// LFU Cache
static cache_entry_t packet_cache[CACHE_SIZE];
static SemaphoreHandle_t cache_mutex = NULL;

// Find the LFU entry to evict (lowest frequency, oldest if tie)
static int find_lfu_entry(void) {
    int lfu_idx = -1;
    uint32_t min_freq = UINT32_MAX;
    struct timeval oldest_time = { .tv_sec = UINT32_MAX, .tv_usec = UINT32_MAX };
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!packet_cache[i].occupied) {
            return i;  // Use empty slot first
        }
        
        if (packet_cache[i].frequency < min_freq || 
            (packet_cache[i].frequency == min_freq && timercmp(&packet_cache[i].last_seen, &oldest_time, <))) {
            min_freq = packet_cache[i].frequency;
            oldest_time = packet_cache[i].last_seen;
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
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to get current time");
        xSemaphoreGive(cache_mutex);
        return;
    }
    
    // Check if MAC already exists
    int idx = find_entry_by_mac(addr);
    
    if (idx >= 0) {
        // Update existing entry
        bthome_packet_free(&packet_cache[idx].packet);
        bthome_packet_init(&packet_cache[idx].packet);
        if (bthome_packet_copy(&packet_cache[idx].packet, packet) == 0) {
            packet_cache[idx].rssi = rssi;
            packet_cache[idx].frequency++;
            packet_cache[idx].last_seen = now;
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
                packet_cache[idx].last_seen = now;
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
    httpd_resp_sendstr_chunk(req, "<h1>BTHome Packets</h1>\n");
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

        struct tm *nowtm;
        nowtm = localtime(&entry->last_seen.tv_sec);
        
        int len = snprintf(buffer, sizeof(buffer), 
                "<div class='rssi'>RSSI: %d dBm | Frequency: %lu | Last: ",
                entry->rssi, (unsigned long)entry->frequency);
        len += strftime(buffer + len, sizeof(buffer) - len, "%Y-%m-%d %H:%M:%S", nowtm);
        len += snprintf(buffer + len, sizeof(buffer) - len, ".%06ld</div>", entry->last_seen.tv_usec);
        strncat(buffer + len, "</div>", sizeof(buffer) - len);
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
    
    if (!g_ntp_initialized) {
        httpd_resp_sendstr_chunk(req, "<div class='info'>Warning: NTP time not synchronized. BTHome capture will start once synchronized.</div>");
    }
    if (count == 0) {
        httpd_resp_sendstr_chunk(req, "<div class='no-data'>No packets cached yet.</div>");
    }
    
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);  // End chunked response
    
    xSemaphoreGive(cache_mutex);
    return ESP_OK;
}

// Find or register a BTHome sensor in the sensor system
static int find_or_register_bthome_sensor(esp_bd_addr_t addr, uint8_t object_id) {
    if (sensor_map_mutex == NULL) {
        return -1;
    }
    
    // Check if this MAC is enabled in settings
    char device_name[32];
    if (!is_mac_enabled(addr, device_name, sizeof(device_name))) {
        return -1;  // MAC not in enabled filters
    }
    
    xSemaphoreTake(sensor_map_mutex, portMAX_DELAY);
    
    // Check if already registered
    for (int i = 0; i < bthome_sensor_count; i++) {
        if (mac_equal(bthome_sensor_map[i].addr, addr) && 
            bthome_sensor_map[i].object_id == object_id) {
            int sensor_id = bthome_sensor_map[i].sensor_id;
            xSemaphoreGive(sensor_map_mutex);
            return sensor_id;
        }
    }
    
    // Not found, register new sensor
    if (bthome_sensor_count >= MAX_BTHOME_SENSORS) {
        ESP_LOGW(TAG, "Maximum BTHome sensors reached (%d)", MAX_BTHOME_SENSORS);
        xSemaphoreGive(sensor_map_mutex);
        return -1;
    }
    
    const char *type_name = bthome_get_object_name(object_id);
    const char *unit = bthome_get_object_unit(object_id);

    if (object_id == BTHOME_SENSOR_TEMPERATURE_F) {
        type_name = "Temperature";
        unit = "°F";
    }

    // Create sensor name using configured device name from settings
    char sensor_name[SENSOR_DISPLAY_NAME_MAX_LEN];
    sensor_name[0] = '\0';
    
    if (device_name[0] != '\0' && type_name != NULL) {
        // Use configured name + measurement type
        // Use strncpy and strncat to avoid truncation warnings
        strncpy(sensor_name, device_name, sizeof(sensor_name) - 1);
        sensor_name[sizeof(sensor_name) - 1] = '\0';
        
        size_t len = strlen(sensor_name);
        if (len < sizeof(sensor_name) - 2) {
            sensor_name[len] = ' ';
            sensor_name[len + 1] = '\0';
            strncat(sensor_name, type_name, sizeof(sensor_name) - strlen(sensor_name) - 1);
        }
    } else if (type_name != NULL) {
        // Fallback to just measurement type
        strncpy(sensor_name, type_name, sizeof(sensor_name) - 1);
        sensor_name[sizeof(sensor_name) - 1] = '\0';
    } else {
        // Last resort fallback - use hex format
        const char prefix[] = "Sensor 0x";
        strncpy(sensor_name, prefix, sizeof(sensor_name) - 1);
        size_t prefix_len = strlen(sensor_name);
        if (prefix_len < sizeof(sensor_name) - 3) {
            // Manually format the hex value to avoid snprintf warning
            char hex[3];
            hex[0] = "0123456789ABCDEF"[(object_id >> 4) & 0xF];
            hex[1] = "0123456789ABCDEF"[object_id & 0xF];
            hex[2] = '\0';
            strncat(sensor_name, hex, sizeof(sensor_name) - prefix_len - 1);
        }
    }
    
    // Generate prometheus metric name
    char metric_name[128];
    snprintf(metric_name, sizeof(metric_name), "bthome_%s", type_name ? type_name : "sensor");
    // Replace spaces and hyphens with underscores, convert to lowercase
    for (char *p = metric_name; *p; p++) {
        if (*p == ' ' || *p == '-' || *p == ':') {
            *p = '_';
        } else if (*p >= 'A' && *p <= 'Z') {
            *p = *p + ('a' - 'A');
        }
    }

    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    // Register with sensor system
    int sensor_id = sensors_register(
        sensor_name, // Don't set a display name for the C value if temp_use_fahrenheit is true
        unit ? unit : "",
        metric_name,
        device_name[0] != '\0' ? device_name : addr_str,
        addr_str);
    if (sensor_id < 0) {
        ESP_LOGE(TAG, "Failed to register BTHome sensor: %s", sensor_name);
        xSemaphoreGive(sensor_map_mutex);
        return -1;
    }
    
    // Store mapping
    memcpy(bthome_sensor_map[bthome_sensor_count].addr, addr, 6);
    bthome_sensor_map[bthome_sensor_count].object_id = object_id;
    bthome_sensor_map[bthome_sensor_count].sensor_id = sensor_id;
    bthome_sensor_map[bthome_sensor_count].registered = true;
    bthome_sensor_count++;
    ESP_LOGI(TAG, "Registered BTHome sensor: %s (ID %d)", sensor_name, sensor_id);
   
    xSemaphoreGive(sensor_map_mutex);
    return sensor_id;
}

static void bthome_packet_callback(esp_bd_addr_t addr, int rssi, 
                                    const bthome_packet_t *packet, void *user_data) {
    if (g_ntp_initialized == false) {
        ESP_LOGW(TAG, "NTP time not synchronized yet, ignoring BTHome packet");
        return;
    }
    
    // Cache the packet first
    cache_packet(addr, rssi, packet);
    
    // Format MAC address
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    
    ESP_LOGI(TAG, "BTHome packet from %s (RSSI: %d dBm)", mac_str, rssi);
    
    // Register and update sensors for all measurements (filtered by settings)
    for (size_t i = 0; i < packet->measurement_count; i++) {
        if(!is_object_id_selected(packet->measurements[i].object_id)) {
            continue;
        }
        const bthome_measurement_t *m = &packet->measurements[i];
        float factor = bthome_get_scaling_factor(m->object_id);
        float value = bthome_get_scaled_value(m, factor);
        
        // Convert temperature to Fahrenheit if configured
        bool is_temperature = (m->object_id == BTHOME_SENSOR_TEMPERATURE ||
                              m->object_id == BTHOME_SENSOR_TEMPERATURE_SINT16_1 ||
                              m->object_id == BTHOME_SENSOR_TEMPERATURE_SINT8 ||
                              m->object_id == BTHOME_SENSOR_TEMPERATURE_SINT8_035 ||
                              m->object_id == BTHOME_SENSOR_DEWPOINT);
        if (is_temperature && g_settings && g_settings->temp_use_fahrenheit) {
            float f_value = value * 9.0f / 5.0f + 32.0f;
            // Find or register this sensor (only if MAC and object_id are enabled in settings)
            int sensor_id = find_or_register_bthome_sensor(addr, BTHOME_SENSOR_TEMPERATURE_F);
            if (sensor_id >= 0) {
                // Update sensor value
                sensors_update(sensor_id, f_value, true);
            }
        }
        
        // Find or register this sensor (only if MAC and object_id are enabled in settings)
        int sensor_id = find_or_register_bthome_sensor(addr, m->object_id);
        if (sensor_id >= 0) {
            // Update sensor value
            sensors_update(sensor_id, value, true);
        }
    }
    
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
                if (g_settings && g_settings->temp_use_fahrenheit) {
                    float temp_f = value * 9.0f / 5.0f + 32.0f;
                    ESP_LOGI(TAG, "    Temperature: %.2f °F", temp_f);
                } else {
                    ESP_LOGI(TAG, "    Temperature: %.2f °C", value);
                }
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
    // Store settings pointer for filtering
    g_settings = settings;
    
    // Initialize cache
    memset(packet_cache, 0, sizeof(packet_cache));
    cache_mutex = xSemaphoreCreateMutex();
    if (cache_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create cache mutex");
        return;
    }
    
    // Initialize BTHome sensor mapping
    memset(bthome_sensor_map, 0, sizeof(bthome_sensor_map));
    bthome_sensor_count = 0;
    sensor_map_mutex = xSemaphoreCreateMutex();
    if (sensor_map_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor map mutex");
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
                &packet_cache[i].last_seen,
                user_data
            );
            
            if (!continue_iteration) {
                break;
            }
        }
    }
    
    xSemaphoreGive(cache_mutex);
}
