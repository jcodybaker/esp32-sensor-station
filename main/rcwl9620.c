#include "rcwl9620.h"

#if CONFIG_ENABLE_RCWL9620

#include <inttypes.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ultrasonic.h>

#include "sensors.h"

static const char *TAG = "rcwl9620";

static ultrasonic_sensor_t dev;
static int sensor_id_cm = -1;

static void rcwl9620_task(void *pvParameters) {
    while (1) {
        uint32_t distance_cm;
        esp_err_t err = ultrasonic_measure_cm(&dev, CONFIG_RCWL9620_MAX_DISTANCE_CM, &distance_cm);
        if (err != ESP_OK) {
            switch (err) {
                case ESP_ERR_ULTRASONIC_PING:
                    ESP_LOGE(TAG, "Sensor not ready (previous ping still in progress)");
                    break;
                case ESP_ERR_ULTRASONIC_PING_TIMEOUT:
                    ESP_LOGE(TAG, "Sensor ping timeout (device not responding)");
                    break;
                case ESP_ERR_ULTRASONIC_ECHO_TIMEOUT:
                    ESP_LOGW(TAG, "Echo timeout (out of range)");
                    break;
                default:
                    ESP_LOGE(TAG, "Failed to measure distance: %s", esp_err_to_name(err));
                    break;
            }
            sensors_update(sensor_id_cm, 0.0f, false);
        } else {
            ESP_LOGD(TAG, "Distance: %" PRIu32 " cm", distance_cm);
            sensors_update(sensor_id_cm, (float)distance_cm, true);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void rcwl9620_init(settings_t *settings) {
    if (settings->rcwl9620_trigger_gpio < 0 || settings->rcwl9620_echo_gpio < 0) {
        ESP_LOGW(TAG, "RCWL-9620 GPIOs not configured, skipping RCWL-9620 initialization");
        return;
    }

    dev.trigger_pin = settings->rcwl9620_trigger_gpio;
    dev.echo_pin = settings->rcwl9620_echo_gpio;

    esp_err_t err = ultrasonic_init(&dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize RCWL-9620 on trigger GPIO %d, echo GPIO %d: %s",
                 settings->rcwl9620_trigger_gpio, settings->rcwl9620_echo_gpio, esp_err_to_name(err));
        return;
    }

    sensor_id_cm = sensors_register("Distance", "cm", "distance_cm", "rcwl9620", NULL);

    xTaskCreate(rcwl9620_task, "rcwl9620", configMINIMAL_STACK_SIZE * 5, settings, 5, NULL);
    ESP_LOGI(TAG, "RCWL-9620 initialized on trigger GPIO %d, echo GPIO %d",
             settings->rcwl9620_trigger_gpio, settings->rcwl9620_echo_gpio);
}

#else // !CONFIG_ENABLE_RCWL9620

void rcwl9620_init(settings_t *settings) {
    (void)settings;
}

#endif // CONFIG_ENABLE_RCWL9620
