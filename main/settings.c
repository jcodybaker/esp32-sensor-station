/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Non-Volatile Storage (NVS) Read and Write a Value - Example

   For other examples please check:
   https://github.com/espressif/esp-idf/tree/master/examples

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "http_server.h"

static const char *TAG = "settings";

static const char *settings_get_html = ""
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Settings</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; }"
    "h1 { color: #333; }"
    "form { background: #f4f4f4; padding: 20px; border-radius: 8px; }"
    "label { display: block; margin-top: 15px; font-weight: bold; }"
    "input, select { width: 100%; padding: 8px; margin-top: 5px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }"
    "button { background: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; margin-top: 20px; width: 100%; font-size: 16px; }"
    "button:hover { background: #45a049; }"
    ".message { padding: 10px; margin: 10px 0; border-radius: 4px; display: none; }"
    ".success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }"
    ".error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }"
    "</style>"
    "</head>"
    "<body>"
    "<h1>Weight Station Settings</h1>"
    "<div id='message' class='message'></div>"
    "<form id='settingsForm'>"
    "<label for='password'>Password:</label>"
    "<input type='password' id='password' name='password' placeholder='Leave blank to keep current'>"
    "<label for='update_url'>Update URL:</label>"
    "<input type='text' id='update_url' name='update_url' placeholder='Firmware update URL'>"
    "<label for='weight_tare'>Weight Tare:</label>"
    "<input type='number' id='weight_tare' name='weight_tare' placeholder='Tare value'>"
    "<label for='weight_scale'>Weight Scale:</label>"
    "<input type='number' id='weight_scale' name='weight_scale' placeholder='Scale value'>"
    "<label for='weight_gain'>Weight Gain:</label>"
    "<select id='weight_gain' name='weight_gain'>"
    "<option value=''>Select gain...</option>"
    "<option value='128'>128</option>"
    "<option value='64'>64</option>"
    "<option value='32'>32</option>"
    "</select>"
    "<button type='submit'>Update Settings</button>"
    "</form>"
    "<script>"
    "document.getElementById('settingsForm').addEventListener('submit', function(e) {"
    "  e.preventDefault();"
    "  var formData = new FormData(this);"
    "  var params = new URLSearchParams();"
    "  for (var pair of formData.entries()) {"
    "    if (pair[1]) params.append(pair[0], pair[1]);"
    "  }"
    "  fetch('/settings?' + params.toString(), { method: 'POST' })"
    "    .then(response => {"
    "      var msg = document.getElementById('message');"
    "      if (response.ok) {"
    "        msg.className = 'message success';"
    "        msg.textContent = 'Settings updated successfully!';"
    "        msg.style.display = 'block';"
    "        this.reset();"
    "      } else {"
    "        return response.text().then(text => {"
    "          msg.className = 'message error';"
    "          msg.textContent = 'Error: ' + text;"
    "          msg.style.display = 'block';"
    "        });"
    "      }"
    "    })"
    "    .catch(error => {"
    "      var msg = document.getElementById('message');"
    "      msg.className = 'message error';"
    "      msg.textContent = 'Network error: ' + error;"
    "      msg.style.display = 'block';"
    "    });"
    "});"
    "</script>"
    "</body>"
    "</html>";

static esp_err_t settings_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, settings_get_html, strlen(settings_get_html));
    return ESP_OK;
}


static esp_err_t settings_post_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    esp_err_t err = ESP_OK;
    bool updated = false;
    
    // Get the query string length
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query parameters provided");
        return ESP_FAIL;
    }
    
    // Allocate buffer for query string
    char *query_buf = malloc(query_len + 1);
    if (query_buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    // Get the query string
    if (httpd_req_get_url_query_str(req, query_buf, query_len + 1) != ESP_OK) {
        free(query_buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse query string");
        return ESP_FAIL;
    }
    
    // Open NVS handle
    nvs_handle_t settings_handle;
    err = nvs_open("settings", NVS_READWRITE, &settings_handle);
    if (err != ESP_OK) {
        free(query_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open NVS");
        return err;
    }
    
    // Buffer for parameter values
    char param_buf[256];
    
    // Check and update password
    if (httpd_query_key_value(query_buf, "password", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strlen(param_buf) > 0) {
            err = nvs_set_str(settings_handle, "password", param_buf);
            if (err == ESP_OK) {
                if (settings->password != NULL && (strcmp(settings->password, CONFIG_HTTPD_BASIC_AUTH_PASSWORD) != 0)) {
                    free(settings->password);
                }
                settings->password = strdup(param_buf);
                updated = true;
                ESP_LOGI(TAG, "Updated password");
            } else {
                ESP_LOGE(TAG, "Failed to write password to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update update_url
    if (httpd_query_key_value(query_buf, "update_url", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strlen(param_buf) > 0) {
            err = nvs_set_str(settings_handle, "update_url", param_buf);
            if (err == ESP_OK) {
                if (settings->update_url != NULL && (strcmp(settings->update_url, CONFIG_OTA_FIRMWARE_UPGRADE_URL) != 0)) {
                    free(settings->update_url);
                }
                settings->update_url = strdup(param_buf);
                updated = true;
                ESP_LOGI(TAG, "Updated update_url to %s", param_buf);
            } else {
                ESP_LOGE(TAG, "Failed to write update_url to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update weight_tare
    if (httpd_query_key_value(query_buf, "weight_tare", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strlen(param_buf) > 0) {
            int32_t weight_tare = atoi(param_buf);
            err = nvs_set_i32(settings_handle, "weight_tare", weight_tare);
            if (err == ESP_OK) {
                settings->weight_tare = weight_tare;
                updated = true;
                ESP_LOGI(TAG, "Updated weight_tare to %d", weight_tare);
            } else {
                ESP_LOGE(TAG, "Failed to write weight_tare to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update weight_scale
    if (httpd_query_key_value(query_buf, "weight_scale", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strlen(param_buf) > 0) {
            int32_t weight_scale = atoi(param_buf);
            err = nvs_set_i32(settings_handle, "weight_scale", weight_scale);
            if (err == ESP_OK) {
                settings->weight_scale = weight_scale;
                updated = true;
                ESP_LOGI(TAG, "Updated weight_scale to %d", weight_scale);
            } else {
                ESP_LOGE(TAG, "Failed to write weight_scale to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update weight_gain
    if (httpd_query_key_value(query_buf, "weight_gain", param_buf, sizeof(param_buf)) == ESP_OK) {
        if (strlen(param_buf) > 0) {
            int32_t weight_gain = atoi(param_buf);
            err = nvs_set_i32(settings_handle, "weight_gain", weight_gain);
            if (err == ESP_OK) {
                settings->weight_gain = (hx711_gain_t)weight_gain;
                updated = true;
                ESP_LOGI(TAG, "Updated weight_gain to %d", weight_gain);
            } else {
                ESP_LOGE(TAG, "Failed to write weight_gain to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Commit changes to NVS
    if (updated) {
        err = nvs_commit(settings_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit settings to NVS: %s", esp_err_to_name(err));
        }
    }
    
    nvs_close(settings_handle);
    free(query_buf);
    
    if (updated) {
        httpd_resp_set_status(req, HTTPD_200);
        httpd_resp_send(req, "Settings updated successfully", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No valid parameters to update");
    }
    
    return ESP_OK;
}

static httpd_uri_t settings_post_uri = {
    .uri       = "/settings",
    .method    = HTTP_POST,
    .handler   = settings_post_handler,
    .user_ctx  = NULL  // Will be set during initialization
};

static httpd_uri_t settings_get_uri = {
    .uri       = "/settings",
    .method    = HTTP_GET,
    .handler   = settings_get_handler,
};

esp_err_t settings_init(settings_t *settings, httpd_handle_t http_server)
{
    settings->update_url = NULL;
    settings->password = NULL;
    // Open NVS handle
    ESP_LOGI(TAG, "\nOpening Non-Volatile Storage (NVS) handle...");
    nvs_handle_t settings_handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &settings_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "\nReading 'update_url' from NVS...");
    size_t str_size = 0;
    err = nvs_get_str(settings_handle, "update_url", settings->update_url, &str_size);
    switch (err) {
        case ESP_OK:
            settings->update_url = malloc(str_size);
            if (settings->update_url == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for update_url");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(settings_handle, "update_url", settings->update_url, &str_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading update_url!", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "Read 'update_url' = %s", settings->update_url);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->update_url = CONFIG_OTA_FIRMWARE_UPGRADE_URL;
            ESP_LOGI(TAG, "No value for 'update_url'; using default = %s", settings->update_url);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading update_url!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\nReading 'password' from NVS...");
    err = nvs_get_str(settings_handle, "password", settings->password, &str_size);
    switch (err) {
        case ESP_OK:
            settings->password = malloc(str_size);
            if (settings->password == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for password");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(settings_handle, "password", settings->password, &str_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading password!", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "Read 'password' = %s", settings->password);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->password = CONFIG_HTTPD_BASIC_AUTH_PASSWORD;
            ESP_LOGI(TAG, "Setting 'password' on settings %p", settings);
            ESP_LOGI(TAG, "No value for 'password'; using default = %s", settings->password);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading password!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\nReading 'weight_tare' from NVS...");
    err = nvs_get_i32(settings_handle, "weight_tare", &settings->weight_tare);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG, "Read 'weight_tare' = %d", settings->weight_tare);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->weight_tare = CONFIG_WEIGHT_TARE;
            ESP_LOGI(TAG, "No value for 'weight_tare'; using default = %d", settings->weight_tare);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading weight_tare!", esp_err_to_name(err));
            return err;
    }
    
    ESP_LOGI(TAG, "\nReading 'weight_scale' from NVS...");
    err = nvs_get_i32(settings_handle, "weight_scale", &settings->weight_scale);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG, "Read 'weight_scale' = %d", settings->weight_scale);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->weight_scale = CONFIG_WEIGHT_SCALE;
            ESP_LOGI(TAG, "No value for 'weight_scale'; using default = %d", settings->weight_scale);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading weight_scale!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\nReading 'weight_gain' from NVS...");
    int32_t weight_gain_value;
    err = nvs_get_i32(settings_handle, "weight_gain", &weight_gain_value);
    switch (err) {
        case ESP_OK:
            settings->weight_gain = (hx711_gain_t)weight_gain_value;
            ESP_LOGI(TAG, "Read 'weight_gain' = %d", settings->weight_gain);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->weight_gain = CONFIG_WEIGHT_GAIN;
            ESP_LOGI(TAG, "No value for 'weight_gain'; using default = %d", settings->weight_gain);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading weight_gain!", esp_err_to_name(err));
            return err;
    }

    settings_post_uri.user_ctx = settings;
    err = httpd_register_uri_handler_with_basic_auth(settings, http_server, &settings_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering settings POST handler!", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler_with_basic_auth(settings, http_server, &settings_get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering settings GET handler!", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

