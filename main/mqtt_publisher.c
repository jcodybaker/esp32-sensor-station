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
#include <esp_crt_bundle.h>

static const char *TAG = "mqtt_publisher";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static settings_t *mqtt_settings = NULL;
static bool mqtt_connected = false;
static char *json_buffer = NULL;
static size_t json_buffer_size = 1024;
static SemaphoreHandle_t json_mutex = NULL;
static char last_error[256] = "";
static SemaphoreHandle_t error_mutex = NULL;
static TaskHandle_t mqtt_status_task_handle = NULL;

static void mqtt_status_task(void *pvParameters)
{
    const TickType_t delay = pdMS_TO_TICKS(30000); // 30 seconds
    
    ESP_LOGI(TAG, "MQTT status task started");
    
    while (1) {
        vTaskDelay(delay);
        
        if (mqtt_is_enabled()) {
            esp_err_t err = mqtt_publish_status();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to publish status: %s", esp_err_to_name(err));
            }
        }
    }
}

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
            if (error_mutex != NULL && xSemaphoreTake(error_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                    snprintf(last_error, sizeof(last_error), "TCP Transport error - errno: %d (%s)",
                            event->error_handle->esp_transport_sock_errno,
                            strerror(event->error_handle->esp_transport_sock_errno));
                    ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", 
                            event->error_handle->esp_tls_last_esp_err);
                    ESP_LOGE(TAG, "Last tls stack error number: 0x%x", 
                            event->error_handle->esp_tls_stack_err);
                } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    snprintf(last_error, sizeof(last_error), "Connection refused - code: 0x%x",
                            event->error_handle->connect_return_code);
                    ESP_LOGE(TAG, "Connection refused error: 0x%x", 
                            event->error_handle->connect_return_code);
                } else {
                    snprintf(last_error, sizeof(last_error), "MQTT error type: %d",
                            event->error_handle->error_type);
                }
                xSemaphoreGive(error_mutex);
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
    
    // Create mutex for error string
    if (error_mutex == NULL) {
        error_mutex = xSemaphoreCreateMutex();
        if (error_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create error mutex");
            return ESP_FAIL;
        }
    }
    
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
    
    // Check if using mqtts:// and enable TLS verification
    if (strncmp(settings->mqtt_broker_url, "mqtts://", 8) == 0) {
        mqtt_cfg.broker.verification.skip_cert_common_name_check = false;
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(TAG, "MQTTS detected - TLS verification enabled with certificate bundle");
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
    
    // Start periodic status publishing task
    if (mqtt_status_task_handle == NULL) {
        BaseType_t task_created = xTaskCreate(
            mqtt_status_task,
            "mqtt_status",
            4096,
            NULL,
            5,
            &mqtt_status_task_handle
        );
        
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create MQTT status task");
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "MQTT client initialized successfully");
    return ESP_OK;
}

bool mqtt_is_enabled(void)
{
    return mqtt_client != NULL && mqtt_connected;
}

const char* mqtt_get_last_error(void)
{
    static char error_copy[256];
    if (error_mutex != NULL && xSemaphoreTake(error_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(error_copy, last_error, sizeof(error_copy) - 1);
        error_copy[sizeof(error_copy) - 1] = '\0';
        xSemaphoreGive(error_mutex);
        return error_copy;
    }
    return "";
}

esp_err_t mqtt_publish_status(void)
{
    if (!mqtt_is_enabled()) {
        return ESP_FAIL;
    }
    
    // Get default topic if not configured
    const char *topic = mqtt_settings->mqtt_status_topic;
    if (!topic || strlen(topic) == 0) {
        topic = "station/status";
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
                      "\"heap_largest_free_block_bytes\":%lu}", largest_free_block);
        
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

esp_err_t mqtt_publish_single_sensor(int sensor_id)
{
    if (!mqtt_is_enabled()) {
        return ESP_FAIL;
    }
    
    // Get default topic if not configured
    const char *topic = mqtt_settings->mqtt_topic;
    if (!topic || strlen(topic) == 0) {
        topic = "station/sensor";
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
    
    // Get the sensor data
    const sensor_data_t *sensor = sensors_get_by_index(sensor_id);
    if (sensor == NULL || sensor->metric_name[0] == '\0') {
        ESP_LOGW(TAG, "Sensor %d not found or has no metric name", sensor_id);
        xSemaphoreGive(json_mutex);
        return ESP_FAIL;
    }
    
    // Only publish if sensor is available
    if (!sensor->available || sensor->last_updated == 0) {
        ESP_LOGD(TAG, "Sensor %d is not available, skipping publish", sensor_id);
        xSemaphoreGive(json_mutex);
        return ESP_OK;
    }
    
    // Use pre-allocated JSON buffer
    char *json = json_buffer;
    size_t json_size = json_buffer_size;
    
    int offset = 0;
    offset += snprintf(json + offset, json_size - offset, "{");
    
    // Add timestamp from sensor's last_updated
    offset += snprintf(json + offset, json_size - offset, "\"timestamp\":%lld,", (long long)sensor->last_updated);
    
    // Add hostname
    const char *hostname = (mqtt_settings->hostname != NULL && mqtt_settings->hostname[0] != '\0') 
                            ? mqtt_settings->hostname : "station";
    offset += snprintf(json + offset, json_size - offset, "\"hostname\":\"%s\",", hostname);
    
    // Add sensor array with single sensor
    offset += snprintf(json + offset, json_size - offset, "\"sensor\":");
    
    offset += snprintf(json + offset, json_size - offset, "{");
    offset += snprintf(json + offset, json_size - offset, 
                      "\"metric_name\":\"%s\",", sensor->metric_name);
    offset += snprintf(json + offset, json_size - offset, 
                      "\"display_name\":\"%s\",", sensor->display_name);
    offset += snprintf(json + offset, json_size - offset, 
                      "\"unit\":\"%s\",", sensor->unit);
    offset += snprintf(json + offset, json_size - offset, 
                      "\"value\":%.2f", sensor->value);
    
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
    
    offset += snprintf(json + offset, json_size - offset, "}");
    
    // Publish to MQTT
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, json, offset, 0, 0);
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish MQTT message");
        xSemaphoreGive(json_mutex);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Published sensor %d (%s) to MQTT topic '%s' (msg_id=%d, size=%d)", 
             sensor_id, sensor->metric_name, topic, msg_id, offset);
    
    xSemaphoreGive(json_mutex);
    return ESP_OK;
}

void mqtt_publisher_cleanup(void)
{
    // Stop periodic status task
    if (mqtt_status_task_handle != NULL) {
        vTaskDelete(mqtt_status_task_handle);
        mqtt_status_task_handle = NULL;
        ESP_LOGI(TAG, "MQTT status task stopped");
    }
    
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
    
    if (error_mutex != NULL) {
        vSemaphoreDelete(error_mutex);
        error_mutex = NULL;
    }
}
