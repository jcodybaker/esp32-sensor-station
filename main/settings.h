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
    int8_t pump_scl_gpio;              // Pump I2C SCL GPIO pin (-1 = disabled)
    int8_t pump_sda_gpio;              // Pump I2C SDA GPIO pin (-1 = disabled)
    int8_t pump_i2c_addr;              // Pump I2C device address
    int16_t pump_dispense_ml;          // Amount to dispense in ml
    bool temp_use_fahrenheit;          // Display temperatures in Fahrenheit (true) or Celsius (false)
    char *syslog_server;               // Syslog server hostname or IP address
    uint16_t syslog_port;              // Syslog server port (default 514)
    char *mqtt_broker_url;             // MQTT broker URL (e.g., mqtt://broker.example.com:1883)
    char *mqtt_username;               // MQTT username (optional)
    char *mqtt_password;               // MQTT password (optional)
    char *mqtt_topic;                  // MQTT topic for sensor updates (default: station/sensor)
    char *mqtt_status_topic;           // MQTT topic for status updates (default: station/status)
} settings_t;

esp_err_t settings_init(settings_t *settings);

esp_err_t settings_register(settings_t *settings, httpd_handle_t http_server);

const char* settings_get_ds18b20_name(settings_t *settings, uint64_t address);

#endif // SETTINGS_H