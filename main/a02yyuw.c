#include "a02yyuw.h"

#if CONFIG_ENABLE_A02YYUW

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/uart.h>

#include "sensors.h"

// A02YYUW communicates over UART at 9600bps, sending free-running 4-byte frames:
// [0xFF header][distance high byte][distance low byte][checksum = (header+high+low) & 0xFF]
// Distance is reported in millimeters; the sensor's minimum range is ~30mm.
#define A02YYUW_UART_PORT UART_NUM_1
#define A02YYUW_UART_BAUD_RATE 9600
#define A02YYUW_UART_BUF_SIZE 256
#define A02YYUW_FRAME_HEADER 0xFF
#define A02YYUW_FRAME_SIZE 4
#define A02YYUW_MIN_DISTANCE_MM 30

static const char *TAG = "a02yyuw";
static int sensor_id_cm = -1;

static void a02yyuw_task(void *pvParameters) {
    uint8_t frame[A02YYUW_FRAME_SIZE];
    int frame_pos = 0;

    while (1) {
        uint8_t byte;
        int len = uart_read_bytes(A02YYUW_UART_PORT, &byte, 1, pdMS_TO_TICKS(1000));
        if (len <= 0) {
            ESP_LOGW(TAG, "No data received from A02YYUW within timeout");
            sensors_update(sensor_id_cm, 0.0f, false);
            frame_pos = 0;
            continue;
        }

        if (frame_pos == 0 && byte != A02YYUW_FRAME_HEADER) {
            continue; // Resync: wait for the frame header
        }

        frame[frame_pos++] = byte;
        if (frame_pos < A02YYUW_FRAME_SIZE) {
            continue;
        }
        frame_pos = 0;

        uint8_t checksum = (uint8_t)(frame[0] + frame[1] + frame[2]);
        if (checksum != frame[3]) {
            ESP_LOGW(TAG, "Checksum mismatch: got 0x%02X, expected 0x%02X", frame[3], checksum);
            continue;
        }

        uint16_t distance_mm = ((uint16_t)frame[1] << 8) | frame[2];
        if (distance_mm < A02YYUW_MIN_DISTANCE_MM) {
            ESP_LOGW(TAG, "Distance reading %u mm below sensor's minimum range", distance_mm);
            sensors_update(sensor_id_cm, 0.0f, false);
            continue;
        }

        float distance_cm = distance_mm / 10.0f;
        ESP_LOGI(TAG, "Distance: %.1f cm", distance_cm);
        sensors_update(sensor_id_cm, distance_cm, true);
    }
}

void a02yyuw_init(settings_t *settings) {
    if (settings->a02yyuw_rx_gpio < 0) {
        ESP_LOGW(TAG, "A02YYUW RX GPIO not configured, skipping A02YYUW initialization");
        return;
    }

    esp_err_t err = uart_driver_install(A02YYUW_UART_PORT, A02YYUW_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return;
    }

    uart_config_t uart_config = {
        .baud_rate = A02YYUW_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(A02YYUW_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(err));
        uart_driver_delete(A02YYUW_UART_PORT);
        return;
    }

    err = uart_set_pin(A02YYUW_UART_PORT, UART_PIN_NO_CHANGE, settings->a02yyuw_rx_gpio,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART RX pin %d: %s", settings->a02yyuw_rx_gpio, esp_err_to_name(err));
        uart_driver_delete(A02YYUW_UART_PORT);
        return;
    }

    sensor_id_cm = sensors_register("Distance", "cm", "distance_cm", "a02yyuw", NULL);

    xTaskCreate(a02yyuw_task, "a02yyuw", configMINIMAL_STACK_SIZE * 5, settings, 5, NULL);
    ESP_LOGI(TAG, "A02YYUW initialized on RX GPIO %d", settings->a02yyuw_rx_gpio);
}

#else // !CONFIG_ENABLE_A02YYUW

void a02yyuw_init(settings_t *settings) {
    (void)settings;
}

#endif // CONFIG_ENABLE_A02YYUW
