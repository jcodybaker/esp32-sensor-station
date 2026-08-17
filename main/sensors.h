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
#define SENSOR_DISPLAY_NAME_MAX_LEN 40
#define SENSOR_DEVICE_NAME_MAX_LEN 32
#define SENSOR_DEVICE_ID_MAX_LEN 20
#define SENSOR_UNIT_MAX_LEN 16

typedef struct {
    char display_name[SENSOR_DISPLAY_NAME_MAX_LEN];
    char unit[SENSOR_UNIT_MAX_LEN];
    char metric_name[SENSOR_DISPLAY_NAME_MAX_LEN];  // Prometheus metric name
    char device_name[SENSOR_DEVICE_NAME_MAX_LEN]; // Device label for Prometheus
    char device_id[SENSOR_DEVICE_ID_MAX_LEN];    // Device ID for Prometheus
    double value;
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
 * @param display_name Display name of the sensor (will be truncated if too long)
 * @param unit Unit string for the sensor value (e.g., "g", "°C", "lbs")
 * @param metric_name Metric name for the sensor
 * @param device_name Optional device name label for Prometheus (can be NULL)
 * @param device_id Optional device ID label for Prometheus (can be NULL)s
 * @return int Sensor ID (index) if successful, -1 if registration failed
 */
int sensors_register(
    const char *display_name,
    const char *unit,
    const char *metric_name,
    const char *device_name, 
    const char *device_id);

/**
 * @brief Update a sensor's value
 * 
 * @param sensor_id Sensor ID returned from sensors_register
 * @param value New sensor value
 * @param available Whether the sensor data is available/valid
 * @return true if update was successful, false otherwise
 */
bool sensors_update(int sensor_id, double value, bool available);

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
bool sensors_update_with_link(int sensor_id, double value, bool available,
                               const char *link_url, const char *link_text);

/**
 * @brief Get the current value of a sensor
 * 
 * @param sensor_id Sensor ID returned from sensors_register
 * @param available Pointer to bool that will be set to sensor availability (can be NULL)
 * @return double Current sensor value
 */
double sensors_get_value(int sensor_id, bool *available);

/**
 * @brief Get the number of registered sensors
 * 
 * @return int Number of registered sensors
 */
int sensors_get_count(void);

/**
 * @brief Get sensor data by index
 * 
 * @param index Sensor index (0 to sensor_count-1)
 * @return const sensor_data_t* Pointer to sensor data, or NULL if index is invalid
 */
const sensor_data_t* sensors_get_by_index(int index);

/**
 * @brief Get the number of sensors meant for user-facing display
 *
 * Excludes sensors registered with no display_name or unit (e.g. an
 * internal Celsius shadow reading kept only for its Prometheus metric),
 * the same filter /sensors/data uses.
 *
 * @return int Number of visible sensors
 */
int sensors_get_visible_count(void);

/**
 * @brief Get visible sensor data by index
 *
 * @param index Visible sensor index (0 to sensors_get_visible_count()-1)
 * @return const sensor_data_t* Pointer to sensor data, or NULL if index is invalid
 */
const sensor_data_t* sensors_get_visible_by_index(int index);

#endif // SENSORS_H
