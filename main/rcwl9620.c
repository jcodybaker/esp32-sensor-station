#include "rcwl9620.h"

#if CONFIG_ENABLE_RCWL9620

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_rom_sys.h>

#include "sensors.h"

static const char *TAG = "rcwl9620";

// This sensor unit's echo pin doesn't go high until ~20-30ms after the
// trigger pulse (measured via logic trace) -- well outside the ~150us-6ms
// range typical HC-SR04-style sensors use, and outside what off-the-shelf
// ultrasonic drivers (e.g. esp-idf-lib/ultrasonic, PING_TIMEOUT=6ms) assume.
// Implemented locally (rather than depending on such a driver) so this
// timeout lives in git-tracked source instead of a gitignored
// managed_components/ vendor copy that would silently revert on a clean
// dependency fetch.
#define RCWL9620_PING_TIMEOUT_US 50000
#define RCWL9620_TRIGGER_LOW_DELAY_US 4
#define RCWL9620_TRIGGER_HIGH_DELAY_US 10
// Same speed-of-sound constant the working Arduino reference (M5Unit-Sonic's
// SONIC_IO::getDistance()) uses: mm = duration_us * 0.34 / 2. In cm, that's
// duration_us * 0.017.
#define RCWL9620_CM_PER_US 0.017

static gpio_num_t trigger_pin;
static gpio_num_t echo_pin;
static int sensor_id_cm = -1;

typedef enum {
    RCWL9620_OK,
    RCWL9620_ERR_PING_TIMEOUT,
    RCWL9620_ERR_ECHO_TIMEOUT,
} rcwl9620_err_t;

static rcwl9620_err_t rcwl9620_measure_cm(uint32_t max_distance_cm, double *distance_cm) {
    gpio_set_level(trigger_pin, 0);
    esp_rom_delay_us(RCWL9620_TRIGGER_LOW_DELAY_US);
    gpio_set_level(trigger_pin, 1);
    esp_rom_delay_us(RCWL9620_TRIGGER_HIGH_DELAY_US);
    gpio_set_level(trigger_pin, 0);

    int64_t start = esp_timer_get_time();
    while (!gpio_get_level(echo_pin)) {
        if (esp_timer_get_time() - start > RCWL9620_PING_TIMEOUT_US) {
            return RCWL9620_ERR_PING_TIMEOUT;
        }
    }

    int64_t echo_start = esp_timer_get_time();
    int64_t max_echo_us = (int64_t)(max_distance_cm / RCWL9620_CM_PER_US);
    int64_t echo_end = echo_start;
    while (gpio_get_level(echo_pin)) {
        echo_end = esp_timer_get_time();
        if (echo_end - echo_start > max_echo_us) {
            return RCWL9620_ERR_ECHO_TIMEOUT;
        }
    }

    *distance_cm = (double)(echo_end - echo_start) * RCWL9620_CM_PER_US;
    return RCWL9620_OK;
}

static void rcwl9620_task(void *pvParameters) {
    settings_t *settings = (settings_t *)pvParameters;

    while (1) {
        double distance_cm;
        rcwl9620_err_t err = rcwl9620_measure_cm(CONFIG_RCWL9620_MAX_DISTANCE_CM, &distance_cm);
        if (err != RCWL9620_OK) {
            switch (err) {
                case RCWL9620_ERR_PING_TIMEOUT:
                    ESP_LOGE(TAG, "Sensor ping timeout (device not responding)");
                    break;
                case RCWL9620_ERR_ECHO_TIMEOUT:
                    ESP_LOGW(TAG, "Echo timeout (out of range)");
                    break;
                default:
                    break;
            }
            sensors_update(sensor_id_cm, 0.0f, false);
        } else {
            ESP_LOGI(TAG, "Distance: %.2f cm", distance_cm);
            // Build bias URL with current raw value, so clicking "Bias" defines
            // this reading as the "normal" position (mirrors weight.c's tare link).
            char bias_url[64];
            snprintf(bias_url, sizeof(bias_url), "/settings?rcwl9620_bias_cm=%.2f", distance_cm);
            sensors_update_with_link(sensor_id_cm, distance_cm - settings->rcwl9620_bias_cm, true, bias_url, "Bias");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void rcwl9620_init(settings_t *settings) {
    if (settings->rcwl9620_trigger_gpio < 0 || settings->rcwl9620_echo_gpio < 0) {
        ESP_LOGW(TAG, "RCWL-9620 GPIOs not configured, skipping RCWL-9620 initialization");
        return;
    }

    trigger_pin = (gpio_num_t)settings->rcwl9620_trigger_gpio;
    echo_pin = (gpio_num_t)settings->rcwl9620_echo_gpio;

    gpio_set_direction(trigger_pin, GPIO_MODE_OUTPUT);
    gpio_set_direction(echo_pin, GPIO_MODE_INPUT);
    gpio_set_level(trigger_pin, 0);

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
