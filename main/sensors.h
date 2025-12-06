#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "settings.h"
#include <esp_http_server.h>

// Maximum number of sensors that can be registered
#define MAX_SENSORS 60

// Maximum length for sensor name and unit strings
#define SENSOR_NAME_MAX_LEN 40
#define SENSOR_UNIT_MAX_LEN 16

typedef struct {
    char name[SENSOR_NAME_MAX_LEN];
    char unit[SENSOR_UNIT_MAX_LEN];
    float value;
    time_t last_updated;
    bool available;
    char link_url[64];      // Optional action link URL
    char link_text[32];     // Optional action link text
} sensor_data_t;

/**
 * @brief Initialize the sensors subsystem and register HTTP handlers
 * 
 * @param settings Pointer to settings structure
 * @param server HTTP server handle
 */
void sensors_init(settings_t *settings, httpd_handle_t server);

/**
 * @brief Register a new sensor
 * 
 * @param name Name of the sensor (will be truncated if too long)
 * @param unit Unit string for the sensor value (e.g., "g", "°C", "lbs")
 * @return int Sensor ID (index) if successful, -1 if registration failed
 */
int sensors_register(const char *name, const char *unit);

/**
 * @brief Update a sensor's value
 * 
 * @param sensor_id Sensor ID returned from sensors_register
 * @param value New sensor value
 * @param available Whether the sensor data is available/valid
 * @return true if update was successful, false otherwise
 */
bool sensors_update(int sensor_id, float value, bool available);

/**
 * @brief Update a sensor's value with optional link
 * 
 * @param sensor_id Sensor ID returned from sensors_register
 * @param value New sensor value
 * @param available Whether the sensor data is available/valid
 * @param link_url Optional action link URL (can be NULL)
 * @param link_text Optional action link text (can be NULL)
 * @return true if update was successful, false otherwise
 */
bool sensors_update_with_link(int sensor_id, float value, bool available, 
                               const char *link_url, const char *link_text);

/**
 * @brief Get the current value of a sensor
 * 
 * @param sensor_id Sensor ID returned from sensors_register
 * @param available Pointer to bool that will be set to sensor availability (can be NULL)
 * @return float Current sensor value
 */
float sensors_get_value(int sensor_id, bool *available);

#endif // SENSORS_H
