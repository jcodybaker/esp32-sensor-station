#include "mqtt_publisher.h"
#include "sensors.h"
#include "wifi.h"
#include "metrics.h"
#include <esp_log.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mqtt_client.h>

static const char *TAG = "mqtt_publisher";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static settings_t *mqtt_settings = NULL;
static bool mqtt_connected = false;
static char *json_buffer = NULL;
static size_t json_buffer_size = 4096;
static SemaphoreHandle_t json_mutex = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            mqtt_connected = true;
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected from broker");
            mqtt_connected = false;
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", 
                        event->error_handle->esp_tls_last_esp_err);
                ESP_LOGE(TAG, "Last tls stack error number: 0x%x", 
                        event->error_handle->esp_tls_stack_err);
                ESP_LOGE(TAG, "Last captured errno : %d (%s)", 
                        event->error_handle->esp_transport_sock_errno,
                        strerror(event->error_handle->esp_transport_sock_errno));
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "Connection refused error: 0x%x", 
                        event->error_handle->connect_return_code);
            }
            mqtt_connected = false;
            break;
            
        default:
            break;
    }
}

esp_err_t mqtt_publisher_init(settings_t *settings)
{
    mqtt_settings = settings;
    
    // Check if MQTT is configured
    if (!settings->mqtt_broker_url || strlen(settings->mqtt_broker_url) == 0) {
        ESP_LOGI(TAG, "MQTT not configured, skipping initialization");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing MQTT client");
    ESP_LOGI(TAG, "MQTT Broker: %s", settings->mqtt_broker_url);
    
    // Create mutex for JSON buffer
    if (json_mutex == NULL) {
        json_mutex = xSemaphoreCreateMutex();
        if (json_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON mutex");
            return ESP_FAIL;
        }
    }
    
    // Allocate JSON buffer
    if (json_buffer == NULL) {
        json_buffer = malloc(json_buffer_size);
        atomic_fetch_add(&malloc_count_mqtt_publisher, 1);
        if (json_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate JSON buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = settings->mqtt_broker_url,
    };
    
    // Set port if specified
    if (settings->mqtt_port > 0) {
        mqtt_cfg.broker.address.port = settings->mqtt_port;
    }
    
    // Set credentials if provided
    if (settings->mqtt_username && strlen(settings->mqtt_username) > 0) {
        mqtt_cfg.credentials.username = settings->mqtt_username;
    }
    if (settings->mqtt_password && strlen(settings->mqtt_password) > 0) {
        mqtt_cfg.credentials.authentication.password = settings->mqtt_password;
    }
    
    // Set client ID to hostname if available
    if (settings->hostname && strlen(settings->hostname) > 0) {
        mqtt_cfg.credentials.client_id = settings->hostname;
    }
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    // Register event handler
    esp_err_t err = esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, 
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(err));
        return err;
    }
    
    // Start MQTT client
    err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "MQTT client initialized successfully");
    return ESP_OK;
}

bool mqtt_is_enabled(void)
{
    return mqtt_client != NULL && mqtt_connected;
}

esp_err_t mqtt_publish_sensors(void)
{
    if (!mqtt_is_enabled()) {
        return ESP_FAIL;
    }
    
    // Get default topic if not configured
    const char *topic = mqtt_settings->mqtt_topic;
    if (!topic || strlen(topic) == 0) {
        topic = "station/sensors";
    }
    
    // Take mutex to protect JSON buffer
    if (json_mutex == NULL || json_buffer == NULL) {
        ESP_LOGE(TAG, "MQTT client not properly initialized");
        return ESP_FAIL;
    }
    
    if (xSemaphoreTake(json_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire JSON mutex");
        return ESP_FAIL;
    }
    
    // Use pre-allocated JSON buffer
    char *json = json_buffer;
    size_t json_size = json_buffer_size;
    
    int offset = 0;
    offset += snprintf(json + offset, json_size - offset, "{");
    
    // Add timestamp
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    int64_t timestamp_ms = (int64_t)tv_now.tv_sec * 1000LL + (int64_t)tv_now.tv_usec / 1000LL;
    offset += snprintf(json + offset, json_size - offset, "\"timestamp\":%lld,", timestamp_ms);
    
    // Add hostname
    const char *hostname = (mqtt_settings->hostname != NULL && mqtt_settings->hostname[0] != '\0') 
                            ? mqtt_settings->hostname : "weight-station";
    offset += snprintf(json + offset, json_size - offset, "\"hostname\":\"%s\",", hostname);
    
    // Add uptime
    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_seconds = uptime_us / 1000000;
    offset += snprintf(json + offset, json_size - offset, "\"uptime_seconds\":%lld,", uptime_seconds);
    
    // Add WiFi RSSI
    int8_t rssi = wifi_get_rssi();
    offset += snprintf(json + offset, json_size - offset, "\"wifi_rssi_dbm\":%d,", rssi);
    
    // Add heap metrics
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    
    offset += snprintf(json + offset, json_size - offset, 
                      "\"heap_free_bytes\":%lu,", free_heap);
    offset += snprintf(json + offset, json_size - offset, 
                      "\"heap_min_free_bytes\":%lu,", min_free_heap);
    offset += snprintf(json + offset, json_size - offset, 
                      "\"heap_largest_free_block_bytes\":%lu,", largest_free_block);
    
    // Add sensors array
    offset += snprintf(json + offset, json_size - offset, "\"sensors\":[");
    
    int sensor_count = sensors_get_count();
    bool first_sensor = true;
    
    for (int i = 0; i < sensor_count; i++) {
        const sensor_data_t *sensor = sensors_get_by_index(i);
        if (sensor == NULL || sensor->metric_name[0] == '\0') {
            continue;
        }
        
        // Only include available sensors
        if (!sensor->available || sensor->last_updated == 0) {
            continue;
        }
        
        if (!first_sensor) {
            offset += snprintf(json + offset, json_size - offset, ",");
        }
        first_sensor = false;
        
        offset += snprintf(json + offset, json_size - offset, "{");
        offset += snprintf(json + offset, json_size - offset, 
                          "\"metric_name\":\"%s\",", sensor->metric_name);
        offset += snprintf(json + offset, json_size - offset, 
                          "\"display_name\":\"%s\",", sensor->display_name);
        offset += snprintf(json + offset, json_size - offset, 
                          "\"unit\":\"%s\",", sensor->unit);
        offset += snprintf(json + offset, json_size - offset, 
                          "\"value\":%.2f,", sensor->value);
        offset += snprintf(json + offset, json_size - offset, 
                          "\"last_updated\":%lld", (long long)sensor->last_updated);
        
        // Add optional device name and ID
        if (sensor->device_name[0] != '\0') {
            offset += snprintf(json + offset, json_size - offset, 
                              ",\"device_name\":\"%s\"", sensor->device_name);
        }
        if (sensor->device_id[0] != '\0') {
            offset += snprintf(json + offset, json_size - offset, 
                              ",\"device_id\":\"%s\"", sensor->device_id);
        }
        
        offset += snprintf(json + offset, json_size - offset, "}");
    }
    
    offset += snprintf(json + offset, json_size - offset, "]");
    offset += snprintf(json + offset, json_size - offset, "}");
    
    // Publish to MQTT
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, json, offset, 0, 0);
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish MQTT message");
        xSemaphoreGive(json_mutex);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Published sensors to MQTT topic '%s' (msg_id=%d, size=%d)", 
             topic, msg_id, offset);
    
    xSemaphoreGive(json_mutex);
    return ESP_OK;
}

void mqtt_publisher_cleanup(void)
{
    if (mqtt_client != NULL) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
    }
    
    if (json_buffer != NULL) {
        free(json_buffer);
        atomic_fetch_add(&free_count_mqtt_publisher, 1);
        json_buffer = NULL;
    }
    
    if (json_mutex != NULL) {
        vSemaphoreDelete(json_mutex);
        json_mutex = NULL;
    }
}
