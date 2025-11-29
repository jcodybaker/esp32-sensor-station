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
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "http_server.h"
#include <stdbool.h>
#include <math.h>
#include "IQmathLib.h"

static const char *TAG = "settings";

// URL encode function - encodes special characters for HTML attribute values
// Returns allocated string that must be freed by caller
static char *url_encode(const char *src) {
    if (!src) return strdup("");
    
    // Calculate required buffer size
    size_t len = 0;
    for (const char *p = src; *p; p++) {
        // Characters that need encoding in HTML attributes
        if (*p == '"' || *p == '<' || *p == '>' || *p == '&' || *p == '\'' || 
            *p == '%' || *p == '+' || *p == '=' || *p == '?' || *p == '#' ||
            (*p < 32) || (*p > 126)) {
            len += 3; // %XX
        } else {
            len += 1;
        }
    }
    
    char *encoded = malloc(len + 1);
    if (!encoded) return NULL;
    
    char *dst = encoded;
    for (const char *p = src; *p; p++) {
        if (*p == '"' || *p == '<' || *p == '>' || *p == '&' || *p == '\'' || 
            *p == '%' || *p == '+' || *p == '=' || *p == '?' || *p == '#' ||
            (*p < 32) || (*p > 126)) {
            sprintf(dst, "%%%02X", (unsigned char)*p);
            dst += 3;
        } else {
            *dst++ = *p;
        }
    }
    *dst = '\0';
    return encoded;
}

// URL decode function - decodes %XX sequences in place
static void url_decode(char *dst, const char *src) {
    char a, b;
    const char *read_ptr = src;
    char *write_ptr = dst;
    
    while (*read_ptr) {
        if (*read_ptr == '%' && read_ptr[1] != '\0' && read_ptr[2] != '\0') {
            a = read_ptr[1];
            b = read_ptr[2];
            if (isxdigit(a) && isxdigit(b)) {
                if (a >= 'a')
                    a -= 'a'-'A';
                if (a >= 'A')
                    a -= ('A' - 10);
                else
                    a -= '0';
                if (b >= 'a')
                    b -= 'a'-'A';
                if (b >= 'A')
                    b -= ('A' - 10);
                else
                    b -= '0';
                *write_ptr++ = 16*a+b;
                read_ptr += 3;
            } else {
                // Invalid hex sequence, just copy the %
                *write_ptr++ = *read_ptr++;
            }
        } else if (*read_ptr == '+') {
            *write_ptr++ = ' ';
            read_ptr++;
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }
    *write_ptr = '\0';
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    
    // Send HTML header and styles
    httpd_resp_sendstr_chunk(req, 
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
        "input[type='checkbox'] { width: auto; }"
        "button { background: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; margin-top: 20px; width: 100%; font-size: 16px; }"
        "button:hover { background: #45a049; }"
        "hr.minor { margin: 10px 0; border: 0; border-top: 1px solid #ccc; }"
        "hr.major { margin: 30px 0; border: 0; border-top: 1px solid #ccc; }"
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
        );
    
    // Send update_url with current value
    char buffer[512];
    char *encoded_update_url = url_encode(settings->update_url);
    snprintf(buffer, sizeof(buffer), 
        "<hr class='minor'/>"
        "<label for='update_url'>Update URL:</label>"
        "<input type='text' id='update_url' name='update_url' value='%s'>",
        encoded_update_url ? encoded_update_url : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_update_url);
    
    // Send weight_tare with current value
    snprintf(buffer, sizeof(buffer),
        "<hr class='minor'/>"
        "<label for='weight_tare'>Weight Tare:</label>"
        "<input type='number' id='weight_tare' name='weight_tare' value='%" PRId32 "'>",
        settings->weight_tare);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send weight_scale with current value
    snprintf(buffer, sizeof(buffer),
        "<label for='weight_scale'>Weight Scale:</label>"
        "<input type='number' step='0.00390625' id='weight_scale' name='weight_scale' value='%.8f'>",
        _IQ8toF(settings->weight_scale));
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send weight_gain with current value selected
    snprintf(buffer, sizeof(buffer),
        "<label for='weight_gain'>Weight Gain:</label>"
        "<select id='weight_gain' name='weight_gain'>"
        "<option value='128'%s>128</option>"
        "<option value='64'%s>64</option>"
        "<option value='32'%s>32</option>"
        "</select>",
        settings->weight_gain == HX711_GAIN_A_128 ? " selected" : "",
        settings->weight_gain == HX711_GAIN_A_64 ? " selected" : "",
        settings->weight_gain == HX711_GAIN_B_32 ? " selected" : "");
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send wifi_ssid with current value
    char *encoded_wifi_ssid = url_encode(settings->wifi_ssid);
    snprintf(buffer, sizeof(buffer),
        "<hr class='minor'/>"
        "<label for='wifi_ssid'>Wifi SSID:</label>"
        "<input type='text' id='wifi_ssid' name='wifi_ssid' value='%s'>",
        encoded_wifi_ssid ? encoded_wifi_ssid : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_wifi_ssid);
    
    // Send wifi_password and checkbox
    snprintf(buffer, sizeof(buffer),
        "<label for='wifi_password'>Wifi Password:</label>"
        "<input type='password' id='wifi_password' name='wifi_password' placeholder='Leave blank to keep current'>"
        "<label for='wifi_ap_fallback_disable'>"
        "<input type='checkbox' id='wifi_ap_fallback_disable' name='wifi_ap_fallback_disable' value='1'%s> Disable WiFi AP Fallback"
        "</label>",
        settings->wifi_ap_fallback_disable ? " checked" : "");
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send form footer and JavaScript
    httpd_resp_sendstr_chunk(req,
        "<button type='submit'>Update Settings</button>"
        "</form>"
        "<hr class='major'/>"
        "<form action='/ota' method='POST'>"
        "<button type='submit'>Start OTA Update</button>"
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
        "</html>");
    
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}


static esp_err_t settings_post_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    esp_err_t err = ESP_OK;
    bool updated = false;
    bool restart_needed = false;
    
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
    char decoded_param[256];
    
    // Check and update password
    if (httpd_query_key_value(query_buf, "password", param_buf, sizeof(param_buf)) == ESP_OK) {
        url_decode(decoded_param, param_buf);  // Decode URL encoding
        if (strcmp(decoded_param, settings->password) == 0) {
            ESP_LOGI(TAG, "Password unchanged");
            decoded_param[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(decoded_param) > 0) {
            err = nvs_set_str(settings_handle, "password", decoded_param);
            if (err == ESP_OK) {
                if (settings->password != NULL) {
                    free(settings->password);
                }
                settings->password = strdup(decoded_param);
                updated = true;
                ESP_LOGI(TAG, "Updated password");
            } else {
                ESP_LOGE(TAG, "Failed to write password to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update update_url
    if (httpd_query_key_value(query_buf, "update_url", param_buf, sizeof(param_buf)) == ESP_OK) {
        url_decode(decoded_param, param_buf);  // Decode URL encoding
        if (strcmp(decoded_param, settings->update_url) == 0) {
            ESP_LOGI(TAG, "Update URL unchanged");
            decoded_param[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(decoded_param) > 0) {
            err = nvs_set_str(settings_handle, "update_url", decoded_param);
            if (err == ESP_OK) {
                if (settings->update_url != NULL) {
                    free(settings->update_url);
                }
                settings->update_url = strdup(decoded_param);
                updated = true;
                ESP_LOGI(TAG, "Updated update_url to %s", decoded_param);
            } else {
                ESP_LOGE(TAG, "Failed to write update_url to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update weight_tare
    if (httpd_query_key_value(query_buf, "weight_tare", param_buf, sizeof(param_buf)) == ESP_OK) {
        int32_t weight_tare = atoi(param_buf);
        if (weight_tare == settings->weight_tare) {
            ESP_LOGI(TAG, "Weight tare unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
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
        float weight_scale_f = atof(param_buf);
        _iq8 weight_scale = _IQ8(weight_scale_f);
        if (weight_scale == settings->weight_scale) {
            ESP_LOGI(TAG, "Weight scale unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i32(settings_handle, "weight_scale", weight_scale);
            if (err == ESP_OK) {
                settings->weight_scale = weight_scale;
                updated = true;
                ESP_LOGI(TAG, "Updated weight_scale to %.8f (0x%08" PRIX32 ")", _IQ8toF(weight_scale), weight_scale);
            } else {
                ESP_LOGE(TAG, "Failed to write weight_scale to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update weight_gain
    if (httpd_query_key_value(query_buf, "weight_gain", param_buf, sizeof(param_buf)) == ESP_OK) {
        int32_t weight_gain = atoi(param_buf);
        if (weight_gain == settings->weight_gain) {
            ESP_LOGI(TAG, "Weight gain unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
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

    if (httpd_query_key_value(query_buf, "wifi_ssid", param_buf, sizeof(param_buf)) == ESP_OK) {
        url_decode(decoded_param, param_buf);  // Decode URL encoding
        if (strcmp(decoded_param, settings->wifi_ssid) == 0) {
            ESP_LOGI(TAG, "WiFi ssid unchanged");
            decoded_param[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(decoded_param) > 0) {
            err = nvs_set_str(settings_handle, "wifi_ssid", decoded_param);
            if (err == ESP_OK) {
                if (settings->wifi_ssid != NULL) {
                    free(settings->wifi_ssid);
                }
                settings->wifi_ssid = strdup(decoded_param);
                updated = true;
                ESP_LOGI(TAG, "Updated ssid");  
                restart_needed = true;
            } else {
                ESP_LOGE(TAG, "Failed to write wifi_ssid to NVS: %s", esp_err_to_name(err));
            }
        }
    }

    if (httpd_query_key_value(query_buf, "wifi_password", param_buf, sizeof(param_buf)) == ESP_OK) {
        url_decode(decoded_param, param_buf);  // Decode URL encoding
        if (strcmp(decoded_param, settings->wifi_password) == 0) {
            ESP_LOGI(TAG, "WiFi password unchanged");
            decoded_param[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(decoded_param) > 0) {
            err = nvs_set_str(settings_handle, "wifi_password", decoded_param);
            if (err == ESP_OK) {
                if (settings->wifi_password != NULL) {
                    free(settings->wifi_password);
                }
                settings->wifi_password = strdup(decoded_param);
                updated = true;
                ESP_LOGI(TAG, "Updated wifi_password");
                restart_needed = true;
            } else {
                ESP_LOGE(TAG, "Failed to write wifi_password to NVS: %s", esp_err_to_name(err));
            }
        }
    }

    // Check and update wifi_ap_fallback_disable
    bool wifi_ap_fallback_disable = false;
    if (httpd_query_key_value(query_buf, "wifi_ap_fallback_disable", param_buf, sizeof(param_buf)) == ESP_OK) {
        wifi_ap_fallback_disable = true;
    }
    if (wifi_ap_fallback_disable != settings->wifi_ap_fallback_disable) {
        err = nvs_set_u8(settings_handle, "wifi_ap_fb_dis", wifi_ap_fallback_disable ? 1 : 0);
        if (err == ESP_OK) {
            settings->wifi_ap_fallback_disable = wifi_ap_fallback_disable;
            updated = true;
            ESP_LOGI(TAG, "Updated wifi_ap_fallback_disable to %d", wifi_ap_fallback_disable);
        } else {
            ESP_LOGE(TAG, "Failed to write wifi_ap_fallback_disable to NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "WiFi AP fallback disable unchanged");
    }
    
    
    // Commit changes to NVS
    if (updated) {
        err = nvs_commit(settings_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit settings to NVS: %s", esp_err_to_name(err));
        }
        if (restart_needed) {
            ESP_LOGI(TAG, "Restarting to apply new WiFi settings...");
            esp_restart();
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
    .user_ctx  = NULL  // Will be set during initialization
};

esp_err_t settings_init(settings_t *settings)
{
    settings->update_url = NULL;
    settings->password = NULL;
    settings->wifi_ssid = NULL;
    settings->wifi_password = NULL;
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
    err = nvs_get_str(settings_handle, "update_url", NULL, &str_size);
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
            ESP_LOGI(TAG, "Read 'update_url' = '%s'", settings->update_url);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->update_url = strdup(CONFIG_OTA_FIRMWARE_UPGRADE_URL);
            ESP_LOGI(TAG, "No value for 'update_url'; using default = '%s'", settings->update_url);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading update_url!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\nReading 'password' from NVS...");
    err = nvs_get_str(settings_handle, "password", NULL, &str_size);
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
            ESP_LOGI(TAG, "Read 'password' = '%s'", settings->password);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->password = CONFIG_HTTPD_BASIC_AUTH_PASSWORD;
            ESP_LOGI(TAG, "No value for 'password'; using default = '%s'", settings->password);
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
    int32_t weight_scale_raw;
    err = nvs_get_i32(settings_handle, "weight_scale", &weight_scale_raw);
    switch (err) {
        case ESP_OK:
            settings->weight_scale = (_iq8)weight_scale_raw;
            ESP_LOGI(TAG, "Read 'weight_scale' = %.8f (0x%08" PRIX32 ")", _IQ8toF(settings->weight_scale), weight_scale_raw);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->weight_scale = (_iq8)CONFIG_WEIGHT_SCALE;
            ESP_LOGI(TAG, "No value for 'weight_scale'; using default = %.8f (0x%08" PRIX32 ")", _IQ8toF(settings->weight_scale), (int32_t)settings->weight_scale);
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

    ESP_LOGI(TAG, "\nReading 'wifi_ssid' from NVS...");
    err = nvs_get_str(settings_handle, "wifi_ssid", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->wifi_ssid = malloc(str_size);
            if (settings->wifi_ssid == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for wifi_ssid");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(settings_handle, "wifi_ssid", settings->wifi_ssid, &str_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading wifi_ssid!", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "Read 'wifi_ssid' = '%s'", settings->wifi_ssid);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->wifi_ssid = strdup(CONFIG_ESP_WIFI_SSID);
            ESP_LOGI(TAG, "No value for 'wifi_ssid'; using default = '%s'", settings->wifi_ssid);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading wifi_ssid!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\nReading 'wifi_password' from NVS...");
    err = nvs_get_str(settings_handle, "wifi_password", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->wifi_password = malloc(str_size);
            if (settings->wifi_password == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for password");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(settings_handle, "wifi_password", settings->wifi_password, &str_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading password!", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "Read 'wifi_password' = '%s'", settings->wifi_password);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->wifi_password = strdup(CONFIG_ESP_WIFI_PASSWORD);
            ESP_LOGI(TAG, "No value for 'wifi_password'; using default = '%s'", settings->wifi_password);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading wifi_password!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\nReading 'wifi_ap_fallback_disable' from NVS...");
    uint8_t wifi_ap_fb_dis_value;
    err = nvs_get_u8(settings_handle, "wifi_ap_fb_dis", &wifi_ap_fb_dis_value);
    switch (err) {
        case ESP_OK:
            settings->wifi_ap_fallback_disable = wifi_ap_fb_dis_value != 0;
            ESP_LOGI(TAG, "Read 'wifi_ap_fallback_disable' = %d", settings->wifi_ap_fallback_disable);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
#ifdef CONFIG_ESP_WIFI_AP_FALLBACK_DISABLE
            settings->wifi_ap_fallback_disable = true;
#else
            settings->wifi_ap_fallback_disable = false;
#endif
            ESP_LOGI(TAG, "No value for 'wifi_ap_fallback_disable'; using default = %d", settings->wifi_ap_fallback_disable);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading wifi_ap_fallback_disable!", esp_err_to_name(err));
            return err;
    }

    nvs_close(settings_handle);
    return ESP_OK;
}

esp_err_t settings_register(settings_t *settings, httpd_handle_t http_server) {
    settings_post_uri.user_ctx = settings;
    settings_get_uri.user_ctx = settings;
    esp_err_t err = httpd_register_uri_handler_with_basic_auth(settings, http_server, &settings_post_uri);
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

