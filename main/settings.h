#ifndef SETTINGS_H
#define SETTINGS_H

#include <esp_err.h>
#include <hx711.h>
#include <esp_http_server.h>
#include <esp_gap_ble_api.h>
#include "IQmathLib.h"

// Structure to hold MAC address filter configuration
typedef struct {
    esp_bd_addr_t mac_addr;  // 6-byte MAC address
    char name[32];           // Human-readable name for the device
    bool enabled;            // Whether this filter is active
} mac_filter_t;

// Structure to hold DS18B20 device name configuration
typedef struct {
    uint64_t address;        // DS18B20 64-bit address
    char name[32];           // Human-readable name for the device
} ds18b20_name_t;

// Number of supported hall-effect flow sensor inputs
#define FLOW_SENSOR_COUNT 5
// Max length (excluding null terminator) of a user-assigned flow sensor name
#define FLOW_SENSOR_NAME_MAX_LEN 16

typedef struct {
    char *update_url;
    char *password;
    int32_t weight_tare;
    _iq16 weight_scale;
    hx711_gain_t weight_gain;
    char * wifi_ssid;
    char * wifi_password;
    bool wifi_ap_fallback_disable;
    char * hostname;
    char * timezone;
    uint8_t *selected_bthome_object_ids;
    size_t selected_bthome_object_ids_count;
    mac_filter_t *mac_filters;         // Array of MAC address filters
    size_t mac_filters_count;          // Number of MAC address filters
    ds18b20_name_t *ds18b20_names;     // Array of DS18B20 device names
    size_t ds18b20_names_count;        // Number of DS18B20 device names
    int8_t ds18b20_gpio;               // DS18B20 temperature sensor GPIO pin (-1 = disabled)
    int8_t ds18b20_pwr_gpio;           // DS18B20 power GPIO pin (-1 = disabled)
    int8_t weight_dt_gpio;           // HX711 DOUT GPIO pin (-1 = disabled)
    int8_t weight_sck_gpio;            // HX711 SCK GPIO pin (-1 = disabled)
    int8_t sensor_i2c_scl_gpio;        // Shared I2C bus SCL GPIO pin, used by pump/EZO devices (-1 = disabled)
    int8_t sensor_i2c_sda_gpio;        // Shared I2C bus SDA GPIO pin, used by pump/EZO devices (-1 = disabled)
    int8_t pump_i2c_addr;              // Pump I2C device address
    int16_t pump_dispense_ml;          // Amount to dispense in ml
    int8_t ezo_ph_i2c_addr;            // Atlas Scientific EZO pH I2C device address (0 = disabled), shares the sensor I2C bus
    int8_t rcwl9620_trigger_gpio;       // RCWL-9620 ultrasonic sensor trigger GPIO pin (-1 = disabled)
    int8_t rcwl9620_echo_gpio;          // RCWL-9620 ultrasonic sensor echo GPIO pin (-1 = disabled)
    double rcwl9620_bias_cm;            // Distance (cm) reported as "normal"; subtracted from raw readings
    int8_t a02yyuw_rx_gpio;             // A02YYUW ultrasonic sensor UART RX GPIO pin (-1 = disabled)
    double a02yyuw_bias_cm;             // Distance (cm) reported as "normal"; subtracted from raw readings
    int8_t flow_sensor_gpio[FLOW_SENSOR_COUNT];            // Hall-effect flow sensor data GPIO pins (-1 = disabled)
    double flow_sensor_liters_per_pulse[FLOW_SENSOR_COUNT]; // Liters added to the running total per pulse
    double flow_sensor_total_liters[FLOW_SENSOR_COUNT];    // Running total, in liters (persisted only if CONFIG_FLOW_SENSOR_PERSIST_TOTALS)
    char flow_sensor_name[FLOW_SENSOR_COUNT][FLOW_SENSOR_NAME_MAX_LEN + 1]; // User-assigned names (empty = "Flow N")
    bool flow_use_gallons;             // Display flow totals in gallons (true) or liters (false)
    bool bthome_enabled;               // Enable BTHome BLE observer at runtime (requires restart to take effect)
    bool temp_use_fahrenheit;          // Display temperatures in Fahrenheit (true) or Celsius (false)
    char *syslog_server;               // Syslog server hostname or IP address
    uint16_t syslog_port;              // Syslog server port (default 514)
    char *mqtt_broker_url;             // MQTT broker URL (e.g., mqtt://broker.example.com:1883)
    char *mqtt_username;               // MQTT username (optional)
    char *mqtt_password;               // MQTT password (optional)
    char *mqtt_topic;                  // MQTT topic for sensor updates (default: station/sensor)
    char *mqtt_status_topic;           // MQTT topic for status updates (default: station/status)
    uint16_t mqtt_debounce_seconds;    // Minimum seconds between per-sensor MQTT publishes (0 = disabled, default 10)
    bool ha_discovery_enabled;         // Publish Home Assistant MQTT Discovery config messages (requires restart)
    char *ha_discovery_prefix;         // Home Assistant discovery topic prefix (default: homeassistant)
    uint8_t lcd_brightness;            // M5StickC Plus LCD backlight brightness (0-255)
} settings_t;

esp_err_t settings_init(settings_t *settings);

esp_err_t settings_register(settings_t *settings, httpd_handle_t http_server);

const char* settings_get_ds18b20_name(settings_t *settings, uint64_t address);

/**
 * @brief Persist a flow sensor's running total (liters) to NVS and update settings in place
 *
 * @param settings Pointer to settings structure
 * @param index Flow sensor index (0 to FLOW_SENSOR_COUNT-1)
 * @param total_liters New running total, in liters
 * @return esp_err_t ESP_OK on success
 */
esp_err_t settings_save_flow_total(settings_t *settings, int index, double total_liters);

#endif // SETTINGS_H