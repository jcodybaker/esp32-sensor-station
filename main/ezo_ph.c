#include "ezo_ph.h"

#if CONFIG_ENABLE_EZO_PH

#include "pump.h"
#include "http_server.h"
#include "sensors.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "driver/i2c_master.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define EZO_PH_BUFFER_SIZE 41
// Atlas Scientific EZO pH datasheet worst-case processing time for both
// readings ("R") and calibration ("Cal,...") commands.
#define EZO_PH_PROCESSING_DELAY 900 // milliseconds
#define EZO_PH_MAX_ATTEMPTS 2
#define EZO_PH_ERROR_BUFFER_SIZE 128
#define EZO_PH_MAX_LOCK_WAIT_MS 10000 // milliseconds

static const char *TAG = "ezo_ph";
static char error_buffer[EZO_PH_ERROR_BUFFER_SIZE];

#define EZO_PH_ERROR_RETURN(fmt, ...) do { \
    snprintf(error_buffer, EZO_PH_ERROR_BUFFER_SIZE, fmt, ##__VA_ARGS__); \
    ESP_LOGE(TAG, "%s", error_buffer); \
} while(0)

const char* ezo_ph_get_last_error(void) {
    if (error_buffer[0] == '\0') {
        return NULL;
    }
    return error_buffer;
}

typedef struct {
    settings_t *settings;
    char buf[EZO_PH_BUFFER_SIZE];
    i2c_master_dev_handle_t dev_handle;
    SemaphoreHandle_t xSemaphore;
    int ph_sensor_id;
} ezo_ph_context_t;

// Single instance: there is only ever one EZO pH probe on the shared I2C bus.
static ezo_ph_context_t s_ph_ctx;

static char* ezo_ph_send_cmd(ezo_ph_context_t *ph_ctx, const char *cmd) {
    if (!xSemaphoreTake(ph_ctx->xSemaphore, pdMS_TO_TICKS(EZO_PH_MAX_LOCK_WAIT_MS))) {
        return NULL;
    }
    esp_err_t err = i2c_master_transmit(ph_ctx->dev_handle, (uint8_t*)cmd, strlen(cmd), -1);
    if (err != ESP_OK) {
        EZO_PH_ERROR_RETURN("Failed to send `%s` command to EZO pH: %s", cmd, esp_err_to_name(err));
        xSemaphoreGive(ph_ctx->xSemaphore);
        return NULL;
    }

    for (int attempt = 0; attempt < EZO_PH_MAX_ATTEMPTS; attempt++) {
        memset(ph_ctx->buf, 0, EZO_PH_BUFFER_SIZE);
        vTaskDelay(pdMS_TO_TICKS(EZO_PH_PROCESSING_DELAY)); // Wait for probe to process command
        err = i2c_master_receive(ph_ctx->dev_handle, (uint8_t*)ph_ctx->buf, EZO_PH_BUFFER_SIZE - 1, EZO_PH_PROCESSING_DELAY);
        switch (err) {
            case ESP_ERR_TIMEOUT:
                ESP_LOGW(TAG, "Timeout while waiting for EZO pH response, attempt %d", attempt + 1);
                continue;
            case ESP_OK:
                break;
            default:
                EZO_PH_ERROR_RETURN("Error receiving EZO pH response: %s", esp_err_to_name(err));
                xSemaphoreGive(ph_ctx->xSemaphore);
                return NULL;
        }
        switch (ph_ctx->buf[0]) {
            case 1:
                xSemaphoreGive(ph_ctx->xSemaphore);
                return ph_ctx->buf+1;
            case 2:
                xSemaphoreGive(ph_ctx->xSemaphore);
                return NULL; // syntax error
            case 254:
                continue; // still processing; try again
            case 255:
                xSemaphoreGive(ph_ctx->xSemaphore);
                return ""; // no data
            default:
                EZO_PH_ERROR_RETURN("EZO pH returned unknown response code: %d", ph_ctx->buf[0]);
                xSemaphoreGive(ph_ctx->xSemaphore);
                return NULL;
        }
    }
    // No response after max attempts
    EZO_PH_ERROR_RETURN("No response from EZO pH after %d attempts", EZO_PH_MAX_ATTEMPTS);
    xSemaphoreGive(ph_ctx->xSemaphore);
    return NULL;
}

static void ezo_ph_monitor_task(void *arg) {
    ezo_ph_context_t *ph_ctx = (ezo_ph_context_t *)arg;

    while (1) {
        const char *response = ezo_ph_send_cmd(ph_ctx, "R");
        if (response != NULL && response[0] != '\0') {
            float ph = 0.0f;
            if (sscanf(response, "%f", &ph) == 1) {
                sensors_update_with_link(ph_ctx->ph_sensor_id, ph, true, "/ezo_ph/calibrate", "Calibrate");
                ESP_LOGD(TAG, "pH: %.2f", ph);
            } else {
                ESP_LOGW(TAG, "Failed to parse pH response: %s", response);
                sensors_update(ph_ctx->ph_sensor_id, 0.0f, false);
            }
        } else {
            ESP_LOGW(TAG, "Failed to query pH reading");
            sensors_update(ph_ctx->ph_sensor_id, 0.0f, false);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// Parses the calibration status from a "Cal,?" response of the form "?CAL,n".
// Returns -1 if the response could not be parsed.
static int ezo_ph_parse_cal_status(const char *response) {
    int status = -1;
    if (response != NULL && sscanf(response, "?CAL,%d", &status) == 1) {
        return status;
    }
    return -1;
}

static const char *ezo_ph_cal_status_text(int status) {
    switch (status) {
        case 0: return "Not calibrated";
        case 1: return "Single point (mid) calibrated";
        case 2: return "Two point calibrated";
        case 3: return "Three point calibrated";
        default: return "Unknown";
    }
}

static esp_err_t ezo_ph_calibrate_start_handler(httpd_req_t *req) {
    ezo_ph_context_t *ph_ctx = (ezo_ph_context_t*)(req->user_ctx);

    const char *cal_response = ezo_ph_send_cmd(ph_ctx, "Cal,?");
    int cal_status = ezo_ph_parse_cal_status(cal_response);

    const char *reading_response = ezo_ph_send_cmd(ph_ctx, "R");
    float reading = 0.0f;
    bool have_reading = (reading_response != NULL && reading_response[0] != '\0' &&
                          sscanf(reading_response, "%f", &reading) == 1);

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/html");

    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<title>EZO pH Calibration</title>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; text-align: center; }\n"
        "h1 { color: #333; }\n"
        ".info-box { background: #e3f2fd; padding: 20px; border-radius: 8px; margin: 20px 0; border: 2px solid #2196F3; }\n"
        "button { background: #4CAF50; color: white; padding: 12px 30px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; margin: 10px; }\n"
        "button:hover { background: #45a049; }\n"
        "button.clear { background: #dc3545; }\n"
        "button.clear:hover { background: #c82333; }\n"
        "a { display: inline-block; margin: 10px; color: #666; text-decoration: none; }\n"
        "a:hover { text-decoration: underline; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>EZO pH Calibration</h1>\n");

    char reading_str[16];
    if (have_reading) {
        snprintf(reading_str, sizeof(reading_str), "%.2f pH", reading);
    } else {
        snprintf(reading_str, sizeof(reading_str), "unavailable");
    }

    char info_html[512];
    snprintf(info_html, sizeof(info_html),
        "<div class='info-box'>\n"
        "<p><strong>Current reading:</strong> %s</p>\n"
        "<p><strong>Calibration status:</strong> %s</p>\n"
        "</div>\n",
        reading_str, ezo_ph_cal_status_text(cal_status));
    httpd_resp_sendstr_chunk(req, info_html);

    httpd_resp_sendstr_chunk(req,
        "<p>Place the probe in a buffer solution, let the reading stabilize, then calibrate that point.</p>\n"
        "<form method='GET' action='/ezo_ph/calibrate/input' style='display: inline;'>\n"
        "<input type='hidden' name='point' value='mid'>\n"
        "<button type='submit'>Calibrate Mid Point (pH 7)</button>\n"
        "</form>\n"
        "<form method='GET' action='/ezo_ph/calibrate/input' style='display: inline;'>\n"
        "<input type='hidden' name='point' value='low'>\n"
        "<button type='submit'>Calibrate Low Point (pH 4)</button>\n"
        "</form>\n"
        "<form method='GET' action='/ezo_ph/calibrate/input' style='display: inline;'>\n"
        "<input type='hidden' name='point' value='high'>\n"
        "<button type='submit'>Calibrate High Point (pH 10)</button>\n"
        "</form>\n"
        "<br>\n"
        "<form method='POST' action='/ezo_ph/calibrate/clear' style='display: inline;' onsubmit='return confirm(\"Clear all calibration data?\");'>\n"
        "<button type='submit' class='clear'>Clear Calibration</button>\n"
        "</form>\n"
        "<br>\n"
        "<a href='/settings'>Back to Settings</a>\n"
        "</body>\n"
        "</html>\n");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Returns the default buffer value string for a calibration point, or NULL if
// the point name is not recognized.
static const char *ezo_ph_point_default_value(const char *point) {
    if (strcmp(point, "mid") == 0) return "7.00";
    if (strcmp(point, "low") == 0) return "4.00";
    if (strcmp(point, "high") == 0) return "10.00";
    return NULL;
}

static const char *ezo_ph_point_label(const char *point) {
    if (strcmp(point, "mid") == 0) return "Mid Point";
    if (strcmp(point, "low") == 0) return "Low Point";
    if (strcmp(point, "high") == 0) return "High Point";
    return "Unknown";
}

static esp_err_t ezo_ph_calibrate_input_handler(httpd_req_t *req) {
    char point[8] = {0};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char *query_buf = malloc(query_len);
        if (query_buf != NULL) {
            if (httpd_req_get_url_query_str(req, query_buf, query_len) == ESP_OK) {
                httpd_query_key_value(query_buf, "point", point, sizeof(point));
            }
            free(query_buf);
        }
    }

    const char *default_value = ezo_ph_point_default_value(point);
    if (default_value == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid or missing 'point' parameter (expected mid, low, or high)", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/html");

    char html[1536];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<title>EZO pH Calibration - %s</title>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; text-align: center; }\n"
        "h1 { color: #333; }\n"
        ".info-box { background: #fff3cd; padding: 20px; border-radius: 8px; margin: 20px 0; border: 2px solid #ffc107; }\n"
        "form { background: #f4f4f4; padding: 20px; border-radius: 8px; margin: 20px 0; }\n"
        "label { display: block; margin: 15px 0 5px 0; font-weight: bold; }\n"
        "input[type='number'] { width: 100%%; padding: 10px; font-size: 18px; border: 2px solid #ddd; border-radius: 4px; box-sizing: border-box; }\n"
        "button { background: #4CAF50; color: white; padding: 12px 30px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; margin: 10px; }\n"
        "button:hover { background: #45a049; }\n"
        "a { display: inline-block; margin: 10px; color: #666; text-decoration: none; }\n"
        "a:hover { text-decoration: underline; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>EZO pH Calibration - %s</h1>\n"
        "<div class='info-box'>\n"
        "<p>Make sure the probe is in the buffer solution and the reading has stabilized.</p>\n"
        "</div>\n"
        "<form method='POST' action='/ezo_ph/calibrate/submit?point=%s'>\n"
        "<label for='actual_ph'>Actual Buffer pH Value:</label>\n"
        "<input type='number' id='actual_ph' name='actual_ph' step='0.01' min='0' max='14' value='%s' required autofocus>\n"
        "<button type='submit'>Submit Calibration</button>\n"
        "</form>\n"
        "<a href='/ezo_ph/calibrate'>Cancel</a>\n"
        "</body>\n"
        "</html>\n",
        ezo_ph_point_label(point), ezo_ph_point_label(point), point, default_value);

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ezo_ph_calibrate_submit_handler(httpd_req_t *req) {
    ezo_ph_context_t *ph_ctx = (ezo_ph_context_t*)(req->user_ctx);

    char point[8] = {0};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char *query_buf = malloc(query_len);
        if (query_buf != NULL) {
            if (httpd_req_get_url_query_str(req, query_buf, query_len) == ESP_OK) {
                httpd_query_key_value(query_buf, "point", point, sizeof(point));
            }
            free(query_buf);
        }
    }
    if (ezo_ph_point_default_value(point) == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid or missing 'point' parameter (expected mid, low, or high)", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Read POST body
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Failed to read request body", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[ret] = '\0';

    char value_str[16] = {0};
    if (httpd_query_key_value(buf, "actual_ph", value_str, sizeof(value_str)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Missing actual_ph parameter", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    float actual_ph = atof(value_str);
    if (actual_ph < 0.0f || actual_ph > 14.0f) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid pH value (must be between 0 and 14)", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char cal_cmd[32];
    snprintf(cal_cmd, sizeof(cal_cmd), "Cal,%s,%.2f", point, actual_ph);
    ESP_LOGI(TAG, "Sending calibration command: %s", cal_cmd);

    const char *response = ezo_ph_send_cmd(ph_ctx, cal_cmd);
    if (response == NULL) {
        const char *error = ezo_ph_get_last_error();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/html");

        char error_html[512];
        snprintf(error_html, sizeof(error_html),
            "<!DOCTYPE html><html><head><title>Error</title></head><body>"
            "<h1>Calibration Error</h1><p>Failed to calibrate %s point: %s</p>"
            "<a href='/ezo_ph/calibrate'>Try Again</a> | <a href='/settings'>Back to Settings</a>"
            "</body></html>",
            ezo_ph_point_label(point), error ? error : "Unknown error");
        httpd_resp_send(req, error_html, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/html");

    char success_html[1024];
    snprintf(success_html, sizeof(success_html),
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<title>Calibration Complete</title>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; text-align: center; }\n"
        "h1 { color: #333; }\n"
        ".success-box { background: #d4edda; padding: 20px; border-radius: 8px; margin: 20px 0; border: 2px solid #28a745; }\n"
        "a { display: inline-block; margin: 10px; padding: 12px 30px; background: #4CAF50; color: white; text-decoration: none; border-radius: 4px; }\n"
        "a:hover { background: #45a049; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>Calibration Complete!</h1>\n"
        "<div class='success-box'>\n"
        "<p>%s calibrated to pH: <strong>%.2f</strong></p>\n"
        "</div>\n"
        "<a href='/ezo_ph/calibrate'>Calibrate Another Point</a>\n"
        "<a href='/settings'>Settings</a>\n"
        "</body>\n"
        "</html>\n",
        ezo_ph_point_label(point), actual_ph);

    httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ezo_ph_calibrate_clear_handler(httpd_req_t *req) {
    ezo_ph_context_t *ph_ctx = (ezo_ph_context_t*)(req->user_ctx);

    ESP_LOGI(TAG, "Clearing EZO pH calibration");
    const char *response = ezo_ph_send_cmd(ph_ctx, "Cal,clear");
    if (response == NULL) {
        const char *error = ezo_ph_get_last_error();
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_html[512];
        snprintf(error_html, sizeof(error_html),
            "<!DOCTYPE html><html><head><title>Error</title></head><body>"
            "<h1>Calibration Error</h1><p>Failed to clear calibration: %s</p>"
            "<a href='/ezo_ph/calibrate'>Back</a>"
            "</body></html>",
            error ? error : "Unknown error");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, error_html, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/ezo_ph/calibrate");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_uri_t ezo_ph_calibrate_start_uri = {
    .uri       = "/ezo_ph/calibrate",
    .method    = HTTP_GET,
    .handler   = ezo_ph_calibrate_start_handler,
    .user_ctx  = NULL
};

static httpd_uri_t ezo_ph_calibrate_input_uri = {
    .uri       = "/ezo_ph/calibrate/input",
    .method    = HTTP_GET,
    .handler   = ezo_ph_calibrate_input_handler,
    .user_ctx  = NULL
};

static httpd_uri_t ezo_ph_calibrate_submit_uri = {
    .uri       = "/ezo_ph/calibrate/submit",
    .method    = HTTP_POST,
    .handler   = ezo_ph_calibrate_submit_handler,
    .user_ctx  = NULL
};

static httpd_uri_t ezo_ph_calibrate_clear_uri = {
    .uri       = "/ezo_ph/calibrate/clear",
    .method    = HTTP_POST,
    .handler   = ezo_ph_calibrate_clear_handler,
    .user_ctx  = NULL
};

void ezo_ph_init(settings_t *settings, httpd_handle_t server) {
    if (settings->ezo_ph_i2c_addr <= 0) {
        EZO_PH_ERROR_RETURN("EZO pH initialization skipped because the I2C address is not configured");
        return;
    }

    i2c_master_bus_handle_t bus_handle = pump_get_i2c_bus();
    if (bus_handle == NULL) {
        EZO_PH_ERROR_RETURN("EZO pH initialization skipped because the shared I2C bus is not available "
                             "(configure the Sensor I2C SCL/SDA GPIO settings)");
        return;
    }

    ESP_LOGI(TAG, "Initializing EZO pH at I2C address 0x%02X", settings->ezo_ph_i2c_addr);
    ezo_ph_context_t *ph_ctx = &s_ph_ctx;
    memset(ph_ctx, 0, sizeof(ezo_ph_context_t));
    ph_ctx->settings = settings;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = settings->ezo_ph_i2c_addr,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &ph_ctx->dev_handle);
    if (err != ESP_OK) {
        EZO_PH_ERROR_RETURN("Failed to add EZO pH I2C device to bus");
        return;
    }

    ph_ctx->xSemaphore = xSemaphoreCreateMutex();
    if (ph_ctx->xSemaphore == NULL) {
        EZO_PH_ERROR_RETURN("Failed to create semaphore for EZO pH");
        i2c_master_bus_rm_device(ph_ctx->dev_handle);
        return;
    }

    const char *response = ezo_ph_send_cmd(ph_ctx, "I");
    if (response == NULL) {
        EZO_PH_ERROR_RETURN("Failed to communicate with EZO pH during initialization");
        vSemaphoreDelete(ph_ctx->xSemaphore);
        i2c_master_bus_rm_device(ph_ctx->dev_handle);
        return;
    }
    ESP_LOGI(TAG, "EZO pH initialized successfully, device info: %s", response);

    ph_ctx->ph_sensor_id = sensors_register("pH", "pH", "ph_value", "ezo_ph", "");
    if (ph_ctx->ph_sensor_id < 0) {
        ESP_LOGW(TAG, "Failed to register pH sensor");
    }

    BaseType_t task_created = xTaskCreate(
        ezo_ph_monitor_task,
        "ezo_ph_monitor",
        4096,
        ph_ctx,
        5,
        NULL
    );
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create EZO pH monitor task");
    } else {
        ESP_LOGI(TAG, "EZO pH monitor task started");
    }

    ezo_ph_calibrate_start_uri.user_ctx = ph_ctx;
    ezo_ph_calibrate_input_uri.user_ctx = ph_ctx;
    ezo_ph_calibrate_submit_uri.user_ctx = ph_ctx;
    ezo_ph_calibrate_clear_uri.user_ctx = ph_ctx;

    esp_err_t err_http = httpd_register_uri_handler_with_basic_auth(settings, server, &ezo_ph_calibrate_start_uri);
    if (err_http != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register EZO pH calibration start handler: %s", esp_err_to_name(err_http));
    }

    err_http = httpd_register_uri_handler_with_basic_auth(settings, server, &ezo_ph_calibrate_input_uri);
    if (err_http != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register EZO pH calibration input handler: %s", esp_err_to_name(err_http));
    }

    err_http = httpd_register_uri_handler_with_basic_auth(settings, server, &ezo_ph_calibrate_submit_uri);
    if (err_http != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register EZO pH calibration submit handler: %s", esp_err_to_name(err_http));
    }

    err_http = httpd_register_uri_handler_with_basic_auth(settings, server, &ezo_ph_calibrate_clear_uri);
    if (err_http != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register EZO pH calibration clear handler: %s", esp_err_to_name(err_http));
    } else {
        ESP_LOGI(TAG, "Registered EZO pH calibration handlers");
    }
}

#else // !CONFIG_ENABLE_EZO_PH

void ezo_ph_init(settings_t *settings, httpd_handle_t server) {
    (void)settings;
    (void)server;
}

const char* ezo_ph_get_last_error(void) {
    return NULL;
}

#endif // CONFIG_ENABLE_EZO_PH
