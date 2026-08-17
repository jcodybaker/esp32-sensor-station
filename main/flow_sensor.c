#include "flow_sensor.h"

#if CONFIG_ENABLE_FLOW_SENSOR

#include <stdatomic.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>

#include "sensors.h"

static const char *TAG = "flow_sensor";

// Hall-effect flow sensors (e.g. YF-S201) can chatter briefly around each
// transition; ignore edges closer together than this to avoid over-counting
// a single tick as several.
#define FLOW_SENSOR_DEBOUNCE_US 1000

// How often the poll task drains pulse counts and republishes the running
// totals.
#define FLOW_SENSOR_POLL_INTERVAL_MS 500

// Minimum time between NVS writes of the accumulated total for a given
// sensor, so a fast-flowing sensor doesn't wear the flash by committing on
// every poll. Only relevant when CONFIG_FLOW_SENSOR_PERSIST_TOTALS is set.
#define FLOW_SENSOR_PERSIST_INTERVAL_MS 30000

// The "Increment" shadow sensor resets to zero once a sensor has gone this
// long without a pulse, so it reads "how much flowed during the most recent
// use" rather than an ever-growing total.
#define FLOW_SENSOR_IDLE_RESET_US (20LL * 60 * 1000000)

static _Atomic uint32_t s_pulse_counts[FLOW_SENSOR_COUNT];
static volatile int64_t s_last_pulse_us[FLOW_SENSOR_COUNT];

// Sensor registry ids:
// - *_liters is always registered (canonical unit, used for Prometheus/MQTT)
// - *_display is only used when flow_use_gallons is set
// (mirrors the DS18B20 Celsius/Fahrenheit shadow-sensor pattern in temperature.c)
// - *_increment is a display-only "since idle" shadow with no metric_name,
//   so it never gets exported to Prometheus/MQTT.
static int s_sensor_id_liters[FLOW_SENSOR_COUNT];
static int s_sensor_id_display[FLOW_SENSOR_COUNT];
static int s_sensor_id_increment[FLOW_SENSOR_COUNT];

static void IRAM_ATTR flow_sensor_isr_handler(void *arg) {
    int index = (int)(intptr_t)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_last_pulse_us[index] < FLOW_SENSOR_DEBOUNCE_US) {
        return;
    }
    s_last_pulse_us[index] = now;
    atomic_fetch_add(&s_pulse_counts[index], 1);
}

static void flow_sensor_task(void *pvParameters) {
    settings_t *settings = (settings_t *)pvParameters;
    double totals[FLOW_SENSOR_COUNT];
    double increments[FLOW_SENSOR_COUNT] = {0};
#if CONFIG_FLOW_SENSOR_PERSIST_TOTALS
    bool dirty[FLOW_SENSOR_COUNT] = {0};
    TickType_t last_persist = xTaskGetTickCount();
#endif

    for (int i = 0; i < FLOW_SENSOR_COUNT; i++) {
        totals[i] = settings->flow_sensor_total_liters[i];
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(FLOW_SENSOR_POLL_INTERVAL_MS));
        int64_t now = esp_timer_get_time();

        for (int i = 0; i < FLOW_SENSOR_COUNT; i++) {
            if (s_sensor_id_liters[i] < 0) {
                continue;
            }

            uint32_t count = atomic_exchange(&s_pulse_counts[i], 0);
            if (count > 0) {
                double added = (double)count * settings->flow_sensor_liters_per_pulse[i];
                totals[i] += added;
                increments[i] += added;
#if CONFIG_FLOW_SENSOR_PERSIST_TOTALS
                dirty[i] = true;
#endif
            } else if (now - s_last_pulse_us[i] >= FLOW_SENSOR_IDLE_RESET_US) {
                increments[i] = 0.0;
            }

            // Keep settings in sync so the Settings page shows a live total
            // even when NVS persistence (CONFIG_FLOW_SENSOR_PERSIST_TOTALS)
            // is disabled.
            settings->flow_sensor_total_liters[i] = totals[i];

            double display_multiplier = settings->flow_use_gallons ? FLOW_LITERS_TO_US_GALLONS : 1.0;
            sensors_update(s_sensor_id_liters[i], totals[i], true);
            if (s_sensor_id_display[i] >= 0) {
                sensors_update(s_sensor_id_display[i], totals[i] * display_multiplier, true);
            }
            if (s_sensor_id_increment[i] >= 0) {
                sensors_update(s_sensor_id_increment[i], increments[i] * display_multiplier, true);
            }
        }

#if CONFIG_FLOW_SENSOR_PERSIST_TOTALS
        if ((xTaskGetTickCount() - last_persist) >= pdMS_TO_TICKS(FLOW_SENSOR_PERSIST_INTERVAL_MS)) {
            for (int i = 0; i < FLOW_SENSOR_COUNT; i++) {
                if (!dirty[i]) {
                    continue;
                }
                if (settings_save_flow_total(settings, i, totals[i]) == ESP_OK) {
                    dirty[i] = false;
                } else {
                    ESP_LOGW(TAG, "Failed to persist flow sensor %d total", i);
                }
            }
            last_persist = xTaskGetTickCount();
        }
#endif
    }
}

void flow_sensor_init(settings_t *settings) {
    bool any_configured = false;
    bool isr_service_installed = false;

    for (int i = 0; i < FLOW_SENSOR_COUNT; i++) {
        s_sensor_id_liters[i] = -1;
        s_sensor_id_display[i] = -1;
        s_sensor_id_increment[i] = -1;
        s_pulse_counts[i] = 0;
        s_last_pulse_us[i] = 0;

        if (settings->flow_sensor_gpio[i] < 0) {
            continue;
        }

        gpio_num_t pin = (gpio_num_t)settings->flow_sensor_gpio[i];
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure flow sensor %d GPIO %d: %s", i, pin, esp_err_to_name(err));
            continue;
        }

        if (!isr_service_installed) {
            err = gpio_install_isr_service(0);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
                continue;
            }
            isr_service_installed = true;
        }

        err = gpio_isr_handler_add(pin, flow_sensor_isr_handler, (void *)(intptr_t)i);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add ISR handler for flow sensor %d GPIO %d: %s", i, pin, esp_err_to_name(err));
            continue;
        }

        char display_name[24];
        char increment_name[40];
        char device_id[8];
        if (settings->flow_sensor_name[i][0] != '\0') {
            snprintf(display_name, sizeof(display_name), "%s", settings->flow_sensor_name[i]);
        } else {
            snprintf(display_name, sizeof(display_name), "Flow %d", i + 1);
        }
        snprintf(increment_name, sizeof(increment_name), "%s Increment", display_name);
        snprintf(device_id, sizeof(device_id), "%d", i);

        const char *unit = settings->flow_use_gallons ? "gal" : "L";
        if (settings->flow_use_gallons) {
            s_sensor_id_display[i] = sensors_register(display_name, unit, NULL, NULL, NULL);
            s_sensor_id_liters[i] = sensors_register(NULL, NULL, "flow_total_liters", "flow_sensor", device_id);
        } else {
            s_sensor_id_liters[i] = sensors_register(display_name, unit, "flow_total_liters", "flow_sensor", device_id);
            s_sensor_id_display[i] = -1;
        }
        // Display-only: no metric_name, so it is never exported to Prometheus/MQTT.
        s_sensor_id_increment[i] = sensors_register(increment_name, unit, NULL, NULL, NULL);

        ESP_LOGI(TAG, "Flow sensor %d '%s' initialized on GPIO %d (%.6f L/pulse, running total %.3f L)",
                 i, display_name, pin, settings->flow_sensor_liters_per_pulse[i], settings->flow_sensor_total_liters[i]);
        any_configured = true;
    }

    if (!any_configured) {
        ESP_LOGW(TAG, "No flow sensor GPIOs configured, skipping flow sensor initialization");
        return;
    }

    xTaskCreate(flow_sensor_task, "flow_sensor", configMINIMAL_STACK_SIZE * 5, settings, 5, NULL);
}

#else // !CONFIG_ENABLE_FLOW_SENSOR

void flow_sensor_init(settings_t *settings) {
    (void)settings;
}

#endif // CONFIG_ENABLE_FLOW_SENSOR
