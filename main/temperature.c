/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "settings.h"
#include "sensors.h"
#include "temperature.h"
#include "driver/gpio.h"

#define EXAMPLE_ONEWIRE_MAX_DS18B20 5

static const char *TAG = "ds18b20";
static int ds18b20_device_num = 0;

typedef struct {
    ds18b20_device_handle_t dev;
    int sensor_id_c;
    int sensor_id_f;
    uint64_t address;
    float last_temperature_c;
    time_t last_updated;
} ds18b20_device_t;

static ds18b20_device_t ds18b20s[EXAMPLE_ONEWIRE_MAX_DS18B20];
static onewire_bus_handle_t bus = NULL;
static settings_t *device_settings = NULL;

void run_ds18b20(void *pvParameters) {
    // settings_t *settings = (settings_t *) pvParameters;
    float temperature;
    while (1) {
        esp_err_t trigger_err = ds18b20_trigger_temperature_conversion_for_all(bus);
        for (int i = 0; i < ds18b20_device_num; i ++) {
            if (trigger_err || ds18b20_get_temperature(ds18b20s[i].dev, &temperature) != ESP_OK) {
                const char *name = device_settings ? settings_get_ds18b20_name(device_settings, ds18b20s[i].address) : NULL;
                if (name && strlen(name) > 0) {
                    ESP_LOGE(TAG, "Failed to read temperature from DS18B20 '%s' [%016llX]", name, ds18b20s[i].address);
                } else {
                    ESP_LOGE(TAG, "Failed to read temperature from DS18B20[%d] [%016llX]", i, ds18b20s[i].address);
                }
                sensors_update(ds18b20s[i].sensor_id_c, 0.0f, false);
                if (ds18b20s[i].sensor_id_f >= 0) {
                    sensors_update(ds18b20s[i].sensor_id_f, 0.0f, false);
                }
                continue;
            }
            
            // Store the raw Celsius temperature and timestamp
            ds18b20s[i].last_temperature_c = temperature;
            ds18b20s[i].last_updated = time(NULL);
            
            // Convert to Fahrenheit if configured
            float display_temp = temperature;
            const char *unit = "C";
            if (ds18b20s[i].sensor_id_f >= 0) {
                display_temp = temperature * 9.0f / 5.0f + 32.0f;
                unit = "F";
                sensors_update(ds18b20s[i].sensor_id_f, display_temp, true);
            }
            sensors_update(ds18b20s[i].sensor_id_c, temperature, true);
            
            const char *name = device_settings ? settings_get_ds18b20_name(device_settings, ds18b20s[i].address) : NULL;
            if (name && strlen(name) > 0) {
                ESP_LOGI(TAG, "temperature read from DS18B20 '%s' [%016llX]: %.2f%s", name, ds18b20s[i].address, display_temp, unit);
            } else {
                ESP_LOGI(TAG, "temperature read from DS18B20[%d] [%016llX]: %.2f%s", i, ds18b20s[i].address, display_temp, unit);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void init_ds18b20(settings_t *settings) {
    if (settings->ds18b20_gpio < 0) {
        ESP_LOGW(TAG, "DS18B20 GPIO not configured, skipping DS18B20 initialization");
        return;
    }
    if (settings->ds18b20_pwr_gpio >= 0) {
        // Configure DS18B20 power GPIO and set it high
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << settings->ds18b20_pwr_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        gpio_set_level(settings->ds18b20_pwr_gpio, 1);
        ESP_LOGI(TAG, "DS18B20 power GPIO %d set to HIGH", settings->ds18b20_pwr_gpio);
        // Wait a moment for the sensors to power up
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // hard reset the bus
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << settings->ds18b20_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(settings->ds18b20_gpio, 0);
    ESP_LOGI(TAG, "DS18B20 reset GPIO %d set to 0", settings->ds18b20_gpio);
    // Wait a moment for the sensors to power up
    vTaskDelay(pdMS_TO_TICKS(100));

    // Store settings pointer for later use
    device_settings = settings;
    
    // install 1-wire bus
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = settings->ds18b20_gpio,
        .flags = {
            .en_pull_up = true, // enable the internal pull-up resistor in case the external device didn't have one
        }
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10, // 1byte ROM command + 8byte ROM number + 1byte device command
    };
    ESP_LOGI(TAG, "Initializing 1-Wire bus on GPIO%d", settings->ds18b20_gpio);
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_config, &rmt_config, &bus));
    ESP_LOGI(TAG, "1-Wire bus installed on GPIO%d", settings->ds18b20_gpio);

    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_onewire_device;
    esp_err_t search_result = ESP_OK;

    // create 1-wire device iterator, which is used for device search
    ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
    ESP_LOGI(TAG, "Device iterator created, start searching...");
    do {
        search_result = onewire_device_iter_get_next(iter, &next_onewire_device);
        if (search_result == ESP_OK) { // found a new device, let's check if we can upgrade it to a DS18B20
            ESP_LOGI(TAG, "Found a device, address: %016llX", next_onewire_device.address);
            ds18b20_config_t ds_cfg = {};
            onewire_device_address_t address;
            // check if the device is a DS18B20, if so, return the ds18b20 handle
            if (ds18b20_new_device_from_enumeration(&next_onewire_device, &ds_cfg, &ds18b20s[ds18b20_device_num].dev) == ESP_OK) {
                ds18b20_get_device_address(ds18b20s[ds18b20_device_num].dev, &address);
                ds18b20s[ds18b20_device_num].address = address;
                const char *unit = settings->temp_use_fahrenheit ? "F" : "C";                
                const char *device_name = settings_get_ds18b20_name(settings, address);
                char addr_str[13];
                snprintf(addr_str, sizeof(addr_str), "%02X%02X%02X%02X%02X%02X",
                         (uint8_t)(address >> 40), (uint8_t)(address >> 32), (uint8_t)(address >> 24),
                         (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)(address >> 0));
                
                if (settings->temp_use_fahrenheit) {
                    ds18b20s[ds18b20_device_num].sensor_id_f = sensors_register(
                        "Temperature", unit, NULL, NULL, NULL);
                    ds18b20s[ds18b20_device_num].sensor_id_c = sensors_register(
                        NULL, NULL, "temperature", device_name ? device_name : addr_str, addr_str);
                } else {
                    ds18b20s[ds18b20_device_num].sensor_id_c = sensors_register(
                        "Temperature", unit, "temperature", device_name ? device_name : addr_str, addr_str);
                    ds18b20s[ds18b20_device_num].sensor_id_f = -1;
                }
                
                if (device_name && strlen(device_name) > 0) {
                    ESP_LOGI(TAG, "Found a DS18B20[%d] '%s', address: %016llX", ds18b20_device_num, device_name, address);
                } else {
                    ESP_LOGI(TAG, "Found a DS18B20[%d], address: %016llX", ds18b20_device_num, address);
                }
                
                ds18b20_device_num++;
                if (ds18b20_device_num >= EXAMPLE_ONEWIRE_MAX_DS18B20) {
                    ESP_LOGI(TAG, "Max DS18B20 number reached, stop searching...");
                    break;
                }
            } else {
                ESP_LOGI(TAG, "Found an unknown device, address: %016llX", next_onewire_device.address);
            }
        }
    } while (search_result == ESP_OK);
    ESP_ERROR_CHECK(onewire_del_device_iter(iter));
    if (ds18b20_device_num == 0) {
        ESP_LOGW(TAG, "No DS18B20 device found on the bus");
        return;
    }
    ESP_LOGI(TAG, "Searching done, %d DS18B20 device(s) found", ds18b20_device_num);
    
    // Start the weight reading task
    xTaskCreate(run_ds18b20, "run_ds18b20", configMINIMAL_STACK_SIZE * 5, settings, 5, NULL);
}

int get_ds18b20_devices(ds18b20_info_t *devices, int max_devices) {
    int count = ds18b20_device_num < max_devices ? ds18b20_device_num : max_devices;
    for (int i = 0; i < count; i++) {
        devices[i].address = ds18b20s[i].address;
        devices[i].last_temperature_c = ds18b20s[i].last_temperature_c;
        devices[i].last_updated = ds18b20s[i].last_updated;
    }
    return count;
}
