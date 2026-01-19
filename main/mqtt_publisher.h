#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

#include "settings.h"
#include "sensors.h"
#include <esp_err.h>

/**
 * @brief Initialize MQTT client with settings
 * 
 * @param settings Pointer to settings structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mqtt_publisher_init(settings_t *settings);

/**
 * @brief Publish all sensor data to MQTT
 * 
 * Publishes status of the device (but not sensors). Only publishes if MQTT is enabled (broker URL is set).
 * 
 * @return esp_err_t ESP_OK on success, ESP_FAIL if MQTT not configured
 */
esp_err_t mqtt_publish_status(void);

/**
 * @brief Publish a single sensor to MQTT
 * 
 * Publishes only the specified sensor as JSON to the configured MQTT topic.
 * Only publishes if MQTT is enabled (broker URL is set).
 * 
 * @param sensor_id Sensor ID to publish
 * @return esp_err_t ESP_OK on success, ESP_FAIL if MQTT not configured or sensor not found
 */
esp_err_t mqtt_publish_single_sensor(int sensor_id);

/**
 * @brief Check if MQTT is enabled and connected
 * 
 * @return true if MQTT is configured and connected
 */
bool mqtt_is_enabled(void);

/**
 * @brief Get the last MQTT error message
 * 
 * @return const char* Last error message or empty string
 */
const char* mqtt_get_last_error(void);

/**
 * @brief Disconnect and cleanup MQTT client
 */
void mqtt_publisher_cleanup(void);

#endif // MQTT_PUBLISHER_H
