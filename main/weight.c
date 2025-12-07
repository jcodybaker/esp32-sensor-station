
#include <inttypes.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hx711.h>
#include <string.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include "IQmathLib.h"

#include "weight.h"
#include "sensors.h"
#include "settings.h"

static const char *TAG = "hx711";

// Global variable to store the latest weight reading
static int32_t g_latest_weight_raw = 0;
static float g_latest_weight_grams = 0;
static bool g_weight_available = false;

// Sensor IDs for registered weight sensors
static int sensor_id_grams = -1;
static int sensor_id_lbs = -1;

static void weight(void *pvParameters)
{
    settings_t *settings = (settings_t *)pvParameters;
    hx711_t dev =
    {
        .dout = CONFIG_WEIGHT_DOUT_GPIO,
        .pd_sck = CONFIG_WEIGHT_PD_SCK_GPIO,
        .gain = HX711_GAIN_A_64
    };

    // initialize device
    ESP_ERROR_CHECK(hx711_init(&dev));

    // read from device
    while (1)
    {
        esp_err_t r = hx711_wait(&dev, 500);
        if (r != ESP_OK)
        {
            ESP_LOGE(TAG, "Device not found: %d (%s)\n", r, esp_err_to_name(r));
            continue;
        }

        // Read multiple samples and calculate median
        int32_t readings[CONFIG_WEIGHT_SAMPLE_TIMES];
        bool read_success = true;
        
        for (int i = 0; i < CONFIG_WEIGHT_SAMPLE_TIMES; i++)
        {
            esp_err_t r = hx711_wait(&dev, 200);
            if (r != ESP_OK)
            {
                ESP_LOGE(TAG, "Timeout waiting for data: %d (%s)\n", r, esp_err_to_name(r));
                read_success = false;
                break;
            }
            r = hx711_read_data(&dev, &readings[i]);
            if (r != ESP_OK)
            {
                ESP_LOGE(TAG, "Could not read data: %d (%s)\n", r, esp_err_to_name(r));
                read_success = false;
                break;
            }
        }
        
        if (!read_success)
        {
            continue;
        }
        
        // Sort readings to find median
        for (int i = 0; i < CONFIG_WEIGHT_SAMPLE_TIMES - 1; i++)
        {
            for (int j = 0; j < CONFIG_WEIGHT_SAMPLE_TIMES - i - 1; j++)
            {
                if (readings[j] > readings[j + 1])
                {
                    int32_t temp = readings[j];
                    readings[j] = readings[j + 1];
                    readings[j + 1] = temp;
                }
            }
        }
        
        // Get median value
        int32_t data;
        if (CONFIG_WEIGHT_SAMPLE_TIMES % 2 == 0)
        {
            // Even number of samples - average the two middle values
            data = (readings[CONFIG_WEIGHT_SAMPLE_TIMES / 2 - 1] + readings[CONFIG_WEIGHT_SAMPLE_TIMES / 2]) / 2;
        }
        else
        {
            // Odd number of samples - take the middle value
            data = readings[CONFIG_WEIGHT_SAMPLE_TIMES / 2];
        }

        ESP_LOGI(TAG, "Raw data: %" PRIi32, data);

        // Store the latest weight reading
        g_latest_weight_raw = data;
        // Convert raw int32_t to float, multiply by scale (float), subtract tare
        float data_float = (float)(g_latest_weight_raw - settings->weight_tare);
        g_latest_weight_grams = data_float * _IQ16toF(settings->weight_scale);
        g_weight_available = true;
        
        // Update sensor values if registered
        if (sensor_id_grams >= 0) {
            // Build tare URL with current raw value
            char tare_url[64];
            snprintf(tare_url, sizeof(tare_url), "/settings?weight_tare=%d", (int)g_latest_weight_raw);
            sensors_update_with_link(sensor_id_grams, g_latest_weight_grams, true, tare_url, "Tare");
        }
        if (sensor_id_lbs >= 0) {
            float lbs = g_latest_weight_grams / 453.59237f;
            sensors_update(sensor_id_lbs, lbs, true);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

float weight_get_latest(bool *available) {
    if (available) {
        *available = g_weight_available;
    }
    return g_latest_weight_grams;
}

uint32_t weight_get_latest_raw(bool *available) {
    if (available) {
        *available = g_weight_available;
    }
    return g_latest_weight_raw;
}

void weight_init(settings_t *settings)
{
    if (settings->weight_dout_gpio < 0 || settings->weight_sck_gpio < 0) {
        ESP_LOGW(TAG, "Weight HX711 GPIOs not configured, skipping weight initialization");
        return;
    }
    // Register weight sensors
    sensor_id_grams = sensors_register("Weight", "g");
    sensor_id_lbs = sensors_register("Weight", "lbs");
    
    // Start the weight reading task
    xTaskCreate(weight, "weight", configMINIMAL_STACK_SIZE * 5, settings, 5, NULL);
}