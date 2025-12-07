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

#define EXAMPLE_ONEWIRE_BUS_GPIO    18
#define EXAMPLE_ONEWIRE_MAX_DS18B20 5

static const char *TAG = "ds18b20";
static int ds18b20_device_num = 0;

typedef struct {
    ds18b20_device_handle_t dev;
    int sensor_id;
    uint64_t address;
} ds18b20_device_t;

static ds18b20_device_t ds18b20s[EXAMPLE_ONEWIRE_MAX_DS18B20];
static onewire_bus_handle_t bus = NULL;

void run_ds18b20(void *pvParameters) {
    // settings_t *settings = (settings_t *) pvParameters;
    float temperature;
    while (1) {
        esp_err_t trigger_err = ds18b20_trigger_temperature_conversion_for_all(bus);
        for (int i = 0; i < ds18b20_device_num; i ++) {
            if (trigger_err || ds18b20_get_temperature(ds18b20s[i].dev, &temperature) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read temperature from DS18B20[%d]", i);
                sensors_update(ds18b20s[i].sensor_id, 0.0f, false);
                continue;
            }
            ESP_LOGI(TAG, "temperature read from DS18B20[%d]: %.2fC", i, temperature);
            sensors_update(ds18b20s[i].sensor_id, temperature, true);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void init_ds18b20(settings_t *settings) {
    if (settings->ds18b20_gpio < 0) {
        ESP_LOGW(TAG, "DS18B20 GPIO not configured, skipping DS18B20 initialization");
        return;
    }
    // install 1-wire bus
    onewire_bus_handle_t bus = NULL;
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = settings->ds18b20_gpio,
        .flags = {
            .en_pull_up = true, // enable the internal pull-up resistor in case the external device didn't have one
        }
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10, // 1byte ROM command + 8byte ROM number + 1byte device command
    };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_config, &rmt_config, &bus));
    ESP_LOGI(TAG, "1-Wire bus installed on GPIO%d", EXAMPLE_ONEWIRE_BUS_GPIO);

    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_onewire_device;
    esp_err_t search_result = ESP_OK;

    // create 1-wire device iterator, which is used for device search
    ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
    ESP_LOGI(TAG, "Device iterator created, start searching...");
    do {
        search_result = onewire_device_iter_get_next(iter, &next_onewire_device);
        if (search_result == ESP_OK) { // found a new device, let's check if we can upgrade it to a DS18B20
            ds18b20_config_t ds_cfg = {};
            onewire_device_address_t address;
            // check if the device is a DS18B20, if so, return the ds18b20 handle
            if (ds18b20_new_device_from_enumeration(&next_onewire_device, &ds_cfg, &ds18b20s[ds18b20_device_num].dev) == ESP_OK) {
                ds18b20_get_device_address(ds18b20s[ds18b20_device_num].dev, &address);
                ds18b20s[ds18b20_device_num].address = address;
                ds18b20s[ds18b20_device_num].sensor_id = sensors_register("Temperature", "C");
                ESP_LOGI(TAG, "Found a DS18B20[%d], address: %016llX", ds18b20_device_num, address);
                ds18b20_device_num++;
                if (ds18b20_device_num >= EXAMPLE_ONEWIRE_MAX_DS18B20) {
                    ESP_LOGI(TAG, "Max DS18B20 number reached, stop searching...");
                    break;
                }
                
            } else {
                ESP_LOGI(TAG, "Found an unknown device, address: %016llX", next_onewire_device.address);
            }
        }
    } while (search_result != ESP_ERR_NOT_FOUND);
    ESP_ERROR_CHECK(onewire_del_device_iter(iter));
    ESP_LOGI(TAG, "Searching done, %d DS18B20 device(s) found", ds18b20_device_num);
    
    // Start the weight reading task
    xTaskCreate(run_ds18b20, "run_ds18b20", configMINIMAL_STACK_SIZE * 5, settings, 5, NULL);
}

int get_ds18b20_devices(ds18b20_info_t *devices, int max_devices) {
    int count = ds18b20_device_num < max_devices ? ds18b20_device_num : max_devices;
    for (int i = 0; i < count; i++) {
        devices[i].address = ds18b20s[i].address;
        devices[i].sensor_id = ds18b20s[i].sensor_id;
    }
    return count;
}
