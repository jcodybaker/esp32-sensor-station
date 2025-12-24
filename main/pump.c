#include "pump.h"
#include "http_server.h"
#include "sensors.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define PUMP_BUFFER_SIZE 41
#define PUMP_PROCESSING_DELAY 300 // milliseconds
#define PUMP_MAX_ATTEMPTS 2
#define PUMP_ERROR_BUFFER_SIZE 128
#define PUMP_MAX_LOCK_WAIT_MS 10000 // milliseconds

static const char *TAG = "pump";
static char error_buffer[PUMP_ERROR_BUFFER_SIZE];

#define PUMP_ERROR_RETURN(fmt, ...) do { \
    snprintf(error_buffer, PUMP_ERROR_BUFFER_SIZE, fmt, ##__VA_ARGS__); \
    ESP_LOGE(TAG, "%s", error_buffer); \
} while(0)

const char* pump_get_last_error() {
    if (error_buffer[0] == '\0') {
        return NULL;
    }
    return error_buffer;
}

typedef struct {
    settings_t *settings;
    char buf[PUMP_BUFFER_SIZE];
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    SemaphoreHandle_t xSemaphore;
    int voltage_sensor_id;
    int total_volume_sensor_id;
} pump_context_t;


char* pump_send_cmd(pump_context_t *pump_ctx, const char *cmd);

static esp_err_t pump_dispense_ml_param_parser(httpd_req_t *req, int *out_amount) {
    // Get the query string
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len <= 1) {
        return ESP_OK;
    }

    char *buf = malloc(buf_len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    if (httpd_req_get_url_query_str(req, buf, buf_len) != ESP_OK) {
        free(buf);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Failed to get query string", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Parse the 'ml' parameter
    char param[16];
    if (httpd_query_key_value(buf, "ml", param, sizeof(param)) != ESP_OK) {
        free(buf);
        return ESP_OK;
    }
    free(buf);

    // Convert to integer and validate range
    *out_amount = atoi(param);
    if (*out_amount < 1 || *out_amount > 1000) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Amount must be between 1 and 1000", HTTPD_RESP_USE_STRLEN);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t pump_dispense_handler(httpd_req_t *req) {
    pump_context_t *pump_ctx = (pump_context_t*)(req->user_ctx);

    int ml = pump_ctx->settings->pump_dispense_ml;
    switch (pump_dispense_ml_param_parser(req, &ml)) {
        case ESP_OK:
            break;
        case ESP_ERR_INVALID_ARG:
            return ESP_OK; // Error response already sent
        default:
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "Internal error parsing parameters", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
    }
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "D,%d", ml);
    ESP_LOGI(TAG, "Sending pump command: %s", cmd);
    
    const char *response = pump_send_cmd(pump_ctx, cmd);
    if (response == NULL) {
        const char *error = pump_get_last_error();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, error ? error : "Pump command failed", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Send success response
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void pump_monitor_task(void *arg) {
    pump_context_t *pump_ctx = (pump_context_t *)arg;
    
    while (1) {
        // Query voltage
        const char *voltage_response = pump_send_cmd(pump_ctx, "PV,?");
        if (voltage_response != NULL) {
            // Parse response format: "?PV,12.3"
            float voltage = 0.0f;
            if (sscanf(voltage_response, "?PV,%f", &voltage) == 1) {
                sensors_update(pump_ctx->voltage_sensor_id, voltage, true);
                ESP_LOGD(TAG, "Pump voltage: %.2f V", voltage);
            } else {
                ESP_LOGW(TAG, "Failed to parse voltage response: %s", voltage_response);
                sensors_update(pump_ctx->voltage_sensor_id, 0.0f, false);
            }
        } else {
            ESP_LOGW(TAG, "Failed to query pump voltage");
            sensors_update(pump_ctx->voltage_sensor_id, 0.0f, false);
        }
        
        // Query total volume
        const char *volume_response = pump_send_cmd(pump_ctx, "TV,?");
        if (volume_response != NULL) {
            // Parse response format: "?TV,623.00"
            float total_volume = 0.0f;
            if (sscanf(volume_response, "?TV,%f", &total_volume) == 1) {
                sensors_update(pump_ctx->total_volume_sensor_id, total_volume, true);
                ESP_LOGD(TAG, "Pump total volume: %.2f ml", total_volume);
            } else {
                ESP_LOGW(TAG, "Failed to parse total volume response: %s", volume_response);
                sensors_update(pump_ctx->total_volume_sensor_id, 0.0f, false);
            }
        } else {
            ESP_LOGW(TAG, "Failed to query pump total volume");
            sensors_update(pump_ctx->total_volume_sensor_id, 0.0f, false);
        }
        
        // Wait 10 seconds before next update
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static httpd_uri_t pump_dispense_uri = {
    .uri       = "/pump/dispense",
    .method    = HTTP_POST,
    .handler   = pump_dispense_handler,
    .user_ctx  = NULL
};

void pump_init(settings_t *settings, httpd_handle_t server) {
    if (settings->pump_scl_gpio < 0 || settings->pump_sda_gpio < 0) {
        PUMP_ERROR_RETURN("Pump initialization skipped because weight sensor GPIOs are not configured");
        return;
    }

    ESP_LOGI(TAG, "Initializing pump on SCL GPIO %d, SDA GPIO %d", settings->pump_scl_gpio, settings->pump_sda_gpio);
    pump_context_t *pump_ctx = malloc(sizeof(pump_context_t));
    if (!pump_ctx) {
        PUMP_ERROR_RETURN("Failed to allocate memory for pump");
        return;
    }
    pump_ctx->settings = settings;
    memset(pump_ctx->buf, 0, PUMP_BUFFER_SIZE);

    // Quick and dirty I2C setup to send "FIND" to address 0x67
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = settings->pump_sda_gpio,
        .scl_io_num = settings->pump_scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    esp_err_t err = i2c_new_master_bus(&i2c_bus_config, &pump_ctx->bus_handle);
    if (err != ESP_OK) {
        PUMP_ERROR_RETURN("Failed to create new I2C master bus");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = settings->pump_i2c_addr,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(pump_ctx->bus_handle, &dev_cfg, &pump_ctx->dev_handle);
    if (err != ESP_OK) {
        PUMP_ERROR_RETURN("Failed to add I2C device to bus");
        return;
    }
    const char *response = pump_send_cmd(pump_ctx, "I");
    if (response == NULL) {
        PUMP_ERROR_RETURN("Failed to communicate with pump during initialization");
        return;
    }
    ESP_LOGI(TAG, "Pump initialized successfully, firmware version: %s", response);
    
    pump_ctx->xSemaphore = xSemaphoreCreateMutex();
    if (pump_ctx->xSemaphore == NULL) {
        PUMP_ERROR_RETURN("Failed to create semaphore for pump");
        return;
    }

    // Register sensors
    pump_ctx->voltage_sensor_id = sensors_register("Pump Voltage", "V");
    if (pump_ctx->voltage_sensor_id < 0) {
        ESP_LOGW(TAG, "Failed to register pump voltage sensor");
    }
    
    pump_ctx->total_volume_sensor_id = sensors_register("Pump Total Volume", "ml");
    if (pump_ctx->total_volume_sensor_id < 0) {
        ESP_LOGW(TAG, "Failed to register pump total volume sensor");
    }
    
    // Create monitoring task
    BaseType_t task_created = xTaskCreate(
        pump_monitor_task,
        "pump_monitor",
        4096,
        pump_ctx,
        5,
        NULL
    );
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pump monitor task");
    } else {
        ESP_LOGI(TAG, "Pump monitor task started");
    }

    pump_dispense_uri.user_ctx = pump_ctx;
    // Register HTTP handler
    esp_err_t err_http = httpd_register_uri_handler_with_basic_auth(settings, server, &pump_dispense_uri);
    if (err_http != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HTTP handler: %s", esp_err_to_name(err_http));
    } else {
        ESP_LOGI(TAG, "Registered pump dispense HTTP handler at /pump/dispense");
    }
    return;
}

char* pump_send_cmd(pump_context_t *pump_ctx, const char *cmd) {
    if(!xSemaphoreTake(pump_ctx->xSemaphore, pdMS_TO_TICKS(PUMP_MAX_LOCK_WAIT_MS))) {
        return NULL;
    }
    esp_err_t err = i2c_master_transmit(pump_ctx->dev_handle, (uint8_t*)cmd, strlen(cmd), -1);
    if (err != ESP_OK) {
        PUMP_ERROR_RETURN("Failed to send `%s` command to pump: %s", cmd, esp_err_to_name(err));
        xSemaphoreGive(pump_ctx->xSemaphore);
        return NULL;
    }

    for (int attempt = 0; attempt < PUMP_MAX_ATTEMPTS; attempt++) {
        memset(pump_ctx->buf, 0, PUMP_BUFFER_SIZE);
        vTaskDelay(pdMS_TO_TICKS(PUMP_PROCESSING_DELAY)); // Wait for pump to process command
        err = i2c_master_receive(pump_ctx->dev_handle, (uint8_t*)pump_ctx->buf, PUMP_BUFFER_SIZE - 1, PUMP_PROCESSING_DELAY);
        switch (err) {
            case ESP_ERR_TIMEOUT:
                ESP_LOGW(TAG, "Timeout while waiting for pump response, attempt %d", attempt + 1);
                continue;
            case ESP_OK:
                break;
            default:
                PUMP_ERROR_RETURN("Error receiving pump response: %s", esp_err_to_name(err));
                xSemaphoreGive(pump_ctx->xSemaphore);
                return NULL;
        }
        switch (pump_ctx->buf[0]) {
            case 1:
                xSemaphoreGive(pump_ctx->xSemaphore);
                return pump_ctx->buf+1;
            case 2:
                xSemaphoreGive(pump_ctx->xSemaphore);
                return NULL; // syntax error
            case 254:
                continue; // still processing; try again
            case 255:
                xSemaphoreGive(pump_ctx->xSemaphore);
                return ""; // no data
            default:
                PUMP_ERROR_RETURN("Pump returned unknown response code: %d", pump_ctx->buf[0]);
                xSemaphoreGive(pump_ctx->xSemaphore);
                return NULL;
        }
    }
    // No response after max attempts
    PUMP_ERROR_RETURN("No response from pump after %d attempts", PUMP_MAX_ATTEMPTS);
    xSemaphoreGive(pump_ctx->xSemaphore);
    return NULL;
}