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
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include "IQmathLib.h"
#include "bthome.h"

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
    
    // Allocate buffers on heap to avoid stack overflow
    char *buffer = malloc(1024);
    if (!buffer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    
    // Send HTML header and styles
    httpd_resp_sendstr_chunk(req, 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<title>Settings</title>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; }\n"
        "h1 { color: #333; }\n"
        "form { background: #f4f4f4; padding: 20px; border-radius: 8px; }\n"
        "label { display: block; margin-top: 15px; font-weight: bold; }\n"
        "input, select { width: 100%; padding: 8px; margin-top: 5px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }\n"
        "input[type='checkbox'] { width: auto; }\n"
        "button { background: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; margin-top: 20px; width: 100%; font-size: 16px; }\n"
        "button:hover { background: #45a049; }\n"
        "hr.minor { margin: 10px 0; border: 0; border-top: 1px solid #ccc; }\n"
        "hr.major { margin: 30px 0; border: 0; border-top: 1px solid #ccc; }\n"
        ".message { padding: 10px; margin: 10px 0; border-radius: 4px; display: none; }\n"
        ".success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }\n"
        ".error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>Weight Station Settings</h1>\n"
        "<a href='/'>Home</a><br>\n"
        "<div id='message' class='message'></div>\n"
        "<form id='settingsForm'>\n"
        "<label for='password'>Password:</label>\n"
        "<input type='password' id='password' name='password' placeholder='Leave blank to keep current'>\n"
        );
    
    // Send update_url with current value
    char *encoded_update_url = url_encode(settings->update_url);
    snprintf(buffer, 1024, 
        "<hr class='minor'/>\n"
        "<label for='update_url'>Update URL:</label>\n"
        "<input type='text' id='update_url' name='update_url' value='%s'>\n",
        encoded_update_url ? encoded_update_url : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_update_url);
    
    // Send weight_tare with current value
    snprintf(buffer, 1024,
        "<hr class='minor'/>\n"
        "<label for='weight_tare'>Weight Tare:</label>\n"
        "<input type='number' id='weight_tare' name='weight_tare' value='%" PRId32 "'>\n",
        settings->weight_tare);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send weight_scale with current value
    snprintf(buffer, 1024,
        "<label for='weight_scale'>Weight Scale:</label>\n"
        "<input type='text' id='weight_scale' name='weight_scale' value='%.8f'>\n",
        _IQ16toF(settings->weight_scale));
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send weight_gain with current value selected
    snprintf(buffer, 1024,
        "<label for='weight_gain'>Weight Gain:</label>\n"
        "<select id='weight_gain' name='weight_gain'>\n"
        "<option value='128'%s>128</option>\n"
        "<option value='64'%s>64</option>\n"
        "<option value='32'%s>32</option>\n"
        "</select>\n",
        settings->weight_gain == HX711_GAIN_A_128 ? " selected" : "",
        settings->weight_gain == HX711_GAIN_A_64 ? " selected" : "",
        settings->weight_gain == HX711_GAIN_B_32 ? " selected" : "");
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send wifi_ssid with current value
    char *encoded_wifi_ssid = url_encode(settings->wifi_ssid);
    snprintf(buffer, 1024,
        "<hr class='minor'/>\n"
        "<label for='wifi_ssid'>Wifi SSID:</label>\n"
        "<input type='text' id='wifi_ssid' name='wifi_ssid' value='%s'>\n",
        encoded_wifi_ssid ? encoded_wifi_ssid : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_wifi_ssid);
    
    // Send wifi_password and checkbox
    snprintf(buffer, 1024,
        "<label for='wifi_password'>Wifi Password:</label>\n"
        "<input type='password' id='wifi_password' name='wifi_password' placeholder='Leave blank to keep current'>\n"
        "<label for='wifi_ap_fallback_disable'>\n"
        "<input type='checkbox' id='wifi_ap_fallback_disable' name='wifi_ap_fallback_disable' value='1'%s> Disable WiFi AP Fallback\n"
        "</label>\n",
        settings->wifi_ap_fallback_disable ? " checked" : "");
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send hostname with current value
    char *encoded_hostname = url_encode(settings->hostname);
    snprintf(buffer, 1024,
        "<label for='hostname'>Hostname:</label>\n"
        "<input type='text' id='hostname' name='hostname' value='%s'>\n",
        encoded_hostname ? encoded_hostname : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_hostname);
    
    // Send BTHome object IDs multi-select
    httpd_resp_sendstr_chunk(req,
        "<hr class='minor'/>\n"
        "<label for='bthome_objects'>BTHome Objects to Monitor:</label>\n"
        "<select id='bthome_objects' name='bthome_objects' multiple size='10' style='height: 200px;'>\n");
    
    // Generate options for all BTHome object IDs
    // We'll include sensor and binary sensor IDs from the bthome.h header

    for (uint8_t i = 0; i < 0xFF; i++) {
        const char *name = bthome_get_object_name(i);
        if (name == NULL) {
            continue; // Skip unknown IDs
        }
        const char *unit = bthome_get_object_unit(i);
        
        if (name != NULL) {
            // Check if this ID is selected
            bool is_selected = false;
            for (size_t j = 0; j < settings->selected_bthome_object_ids_count; j++) {
                if (settings->selected_bthome_object_ids[j] == i) {
                    is_selected = true;
                    break;
                }
            }
            
            // Build the label (name + unit)
            char label[128];
            if (unit != NULL && strlen(unit) > 0) {
                snprintf(label, sizeof(label), "%s (%s)", name, unit);
            } else {
                snprintf(label, sizeof(label), "%s", name);
            }
            
            snprintf(buffer, 1024,
                "<option value='%hhd'%s>0x%02hhX - %s</option>\n",
                i, is_selected ? " selected" : "", i, label);
            httpd_resp_sendstr_chunk(req, buffer);
        }
    }
    
    httpd_resp_sendstr_chunk(req, "</select>\n");
    
    // Send MAC address filters section
    httpd_resp_sendstr_chunk(req,
        "<hr class='minor'/>\n"
        "<label>BTHome MAC Address Filters:</label>\n"
        "<div id='mac_filters_container'>\n");
    
    // Output existing MAC filters
    for (size_t i = 0; i < settings->mac_filters_count; i++) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 settings->mac_filters[i].mac_addr[0],
                 settings->mac_filters[i].mac_addr[1],
                 settings->mac_filters[i].mac_addr[2],
                 settings->mac_filters[i].mac_addr[3],
                 settings->mac_filters[i].mac_addr[4],
                 settings->mac_filters[i].mac_addr[5]);
        
        char *encoded_name = url_encode(settings->mac_filters[i].name);
        
        snprintf(buffer, 1024,
            "<div class='mac_filter_row' style='margin: 10px 0; padding: 10px; background: #fff; border: 1px solid #ddd; border-radius: 4px;'>\n"
            "  <input type='text' name='mac_filter[%zu][mac]' value='%s' placeholder='xx:xx:xx:xx:xx:xx' style='width: 180px;' pattern='[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}' title='MAC address format: xx:xx:xx:xx:xx:xx'>\n"
            "  <input type='text' name='mac_filter[%zu][name]' value='%s' placeholder='Device Name' style='width: 200px;'>\n"
            "  <label style='display: inline;'><input type='checkbox' name='mac_filter[%zu][enabled]' value='1'%s> Enabled</label>\n"
            "  <button type='button' onclick='this.parentElement.remove()' style='width: auto; padding: 5px 10px; background: #dc3545; margin-left: 10px;'>Remove</button>\n"
            "</div>\n",
            i, mac_str, i, encoded_name ? encoded_name : "", i, settings->mac_filters[i].enabled ? " checked" : "");
        httpd_resp_sendstr_chunk(req, buffer);
        free(encoded_name);
    }
    
    httpd_resp_sendstr_chunk(req,
        "</div>\n"
        "<button type='button' onclick='addMacFilter()' style='width: auto; background: #007bff; margin-top: 10px;'>Add MAC Filter</button>\n"
        "<script>\n"
        "var macFilterIndex = " );
    
    snprintf(buffer, 1024, "%zu;\n", settings->mac_filters_count);
    httpd_resp_sendstr_chunk(req, buffer);
    
    httpd_resp_sendstr_chunk(req,
        "function addMacFilter() {\n"
        "  var container = document.getElementById('mac_filters_container');\n"
        "  var div = document.createElement('div');\n"
        "  div.className = 'mac_filter_row';\n"
        "  div.style = 'margin: 10px 0; padding: 10px; background: #fff; border: 1px solid #ddd; border-radius: 4px;';\n"
        "  div.innerHTML = `\n"
        "    <input type='text' name='mac_filter[${macFilterIndex}][mac]' placeholder='xx:xx:xx:xx:xx:xx' style='width: 180px;' pattern='[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}' title='MAC address format: xx:xx:xx:xx:xx:xx'>\n"
        "    <input type='text' name='mac_filter[${macFilterIndex}][name]' placeholder='Device Name' style='width: 200px;'>\n"
        "    <label style='display: inline;'><input type='checkbox' name='mac_filter[${macFilterIndex}][enabled]' value='1' checked> Enabled</label>\n"
        "    <button type='button' onclick='this.parentElement.remove()' style='width: auto; padding: 5px 10px; background: #dc3545; margin-left: 10px;'>Remove</button>\n"
        "  `;\n"
        "  container.appendChild(div);\n"
        "  macFilterIndex++;\n"
        "}\n"
        "</script>\n");
    
    // Get firmware version info
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char hash_str[17];
    for (int i = 0; i < 8; i++) {
        sprintf(&hash_str[i * 2], "%02x", app_desc->app_elf_sha256[i]);
    }
    hash_str[16] = '\0';
    
    // Send form footer and JavaScript
    httpd_resp_sendstr_chunk(req,
        "<button type='submit'>Update Settings</button>\n"
        "</form>\n"
        "<hr class='major'/>\n"
        "<form action='/ota' method='POST'>\n"
        "<button type='submit'>Start OTA Update</button>\n"
        "</form>\n"
        "<footer style='margin-top: 40px; padding-top: 20px; border-top: 1px solid #ddd; text-align: center; color: #999; font-size: 12px;'>\n");
    
    snprintf(buffer, 1024,
        "Firmware: %s<br>Hash: %s\n",
        app_desc->version, hash_str);
    httpd_resp_sendstr_chunk(req, buffer);
    
    httpd_resp_sendstr_chunk(req,
        "</footer>\n"
        "<script>\n"
        "document.getElementById('settingsForm').addEventListener('submit', function(e) {\n"
        "  e.preventDefault();\n"
        "  var formData = new FormData(this);\n"
        "  var params = new URLSearchParams();\n"
        "  for (var pair of formData.entries()) {\n"
        "    if (pair[1]) {\n"
        "      // Handle multi-select specially\n"
        "      if (pair[0] === 'bthome_objects') {\n"
        "        var select = document.getElementById('bthome_objects');\n"
        "        var selectedOptions = Array.from(select.selectedOptions);\n"
        "        for (var i = 0; i < selectedOptions.length; i++) {\n"
        "          params.append('bthome_objects[' + i + ']', selectedOptions[i].value);\n"
        "        }\n"
        "      } else {\n"
        "        params.append(pair[0], pair[1]);\n"
        "      }\n"
        "    } else if (pair[0].startsWith('mac_filter[') && pair[0].includes('[mac]')) {\n"
        "      // Include MAC filter fields even if empty for proper indexing\n"
        "      params.append(pair[0], pair[1]);\n"
        "    }\n"
        "  }\n"
        "  fetch('/settings?' + params.toString(), { method: 'POST' })\n"
        "    .then(response => {\n"
        "      var msg = document.getElementById('message');\n"
        "      if (response.ok) {\n"
        "        msg.className = 'message success';\n"
        "        msg.textContent = 'Settings updated successfully!';\n"
        "        msg.style.display = 'block';\n"
        "      } else {\n"
        "        return response.text().then(text => {\n"
        "          msg.className = 'message error';\n"
        "          msg.textContent = 'Error: ' + text;\n"
        "          msg.style.display = 'block';\n"
        "        });\n"
        "      }\n"
        "    })\n"
        "    .catch(error => {\n"
        "      var msg = document.getElementById('message');\n"
        "      msg.className = 'message error';\n"
        "      msg.textContent = 'Network error: ' + error;\n"
        "      msg.style.display = 'block';\n"
        "    });\n"
        "});\n"
        "</script>\n"
        "</body>\n"
        "</html>\n");
    
    httpd_resp_sendstr_chunk(req, NULL);
    free(buffer);
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
            ESP_LOGI(TAG, "Weight tare unchanged; v='%s', parsed: %d, old: %d", param_buf, weight_tare, settings->weight_tare);
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
        _iq8 weight_scale = _IQ16(weight_scale_f);
        if (weight_scale == settings->weight_scale) {
            ESP_LOGI(TAG, "Weight scale unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i32(settings_handle, "weight_scale", weight_scale);
            if (err == ESP_OK) {
                settings->weight_scale = weight_scale;
                updated = true;
                ESP_LOGI(TAG, "Updated weight_scale to %.8f (0x%08" PRIX32 ")", _IQ16toF(weight_scale), weight_scale);
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

    // Check and update hostname
    if (httpd_query_key_value(query_buf, "hostname", param_buf, sizeof(param_buf)) == ESP_OK) {
        url_decode(decoded_param, param_buf);  // Decode URL encoding
        if (strcmp(decoded_param, settings->hostname) == 0) {
            ESP_LOGI(TAG, "Hostname unchanged");
            decoded_param[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(decoded_param) > 0) {
            err = nvs_set_str(settings_handle, "hostname", decoded_param);
            if (err == ESP_OK) {
                if (settings->hostname != NULL) {
                    free(settings->hostname);
                }
                settings->hostname = strdup(decoded_param);
                updated = true;
                ESP_LOGI(TAG, "Updated hostname to %s", decoded_param);
                restart_needed = true;
            } else {
                ESP_LOGE(TAG, "Failed to write hostname to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update BTHome object IDs
    // The multi-select will send multiple parameters with the same name
    // We need to parse them all and create a blob
    uint8_t *selected_ids = malloc(256);  // Maximum 256 object IDs
    if (!selected_ids) {
        nvs_close(settings_handle);
        free(query_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    size_t selected_count = 0;
    
    // Parse all bthome_objects[] parameters
    char *query_ptr = query_buf;
    ESP_LOGI(TAG, "Parsing BTHome object IDs from query string %s", query_buf);
    while (selected_count < 256) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "bthome_objects%%5B%zu%%5D", selected_count);
        if (httpd_query_key_value(query_ptr, key_buf, param_buf, sizeof(param_buf)) == ESP_OK) {
            int id_value = atoi(param_buf);
            if (id_value >= 0 && id_value <= 255) {
                selected_ids[selected_count] = (uint8_t)id_value;
                ESP_LOGI(TAG, "Found BTHome object ID: 0x%02X", selected_ids[selected_count]);
                selected_count++;
            }
            // Move to next occurrence
            query_ptr = strstr(query_ptr, key_buf);
            if (query_ptr) {
                query_ptr += strlen(key_buf);
            } else {
                break;
            }
        } else {
            break;
        }
    }
    
    // Check if the list has changed
    bool bthome_ids_changed = false;
    if (selected_count != settings->selected_bthome_object_ids_count) {
        bthome_ids_changed = true;
    } else {
        for (size_t i = 0; i < selected_count; i++) {
            if (selected_ids[i] != settings->selected_bthome_object_ids[i]) {
                bthome_ids_changed = true;
                break;
            }
        }
    }
    
    if (bthome_ids_changed) {
        if (selected_count > 0) {
            err = nvs_set_blob(settings_handle, "bthome_obj_ids", selected_ids, selected_count);
        } else {
            // If no IDs selected, erase the key
            err = nvs_erase_key(settings_handle, "bthome_obj_ids");
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;  // Already doesn't exist, that's fine
            }
        }
        
        if (err == ESP_OK) {
            if (settings->selected_bthome_object_ids != NULL) {
                free(settings->selected_bthome_object_ids);
            }
            
            if (selected_count > 0) {
                settings->selected_bthome_object_ids = malloc(selected_count);
                if (settings->selected_bthome_object_ids != NULL) {
                    memcpy(settings->selected_bthome_object_ids, selected_ids, selected_count);
                    settings->selected_bthome_object_ids_count = selected_count;
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for selected BTHome object IDs");
                    settings->selected_bthome_object_ids_count = 0;
                }
            } else {
                settings->selected_bthome_object_ids = NULL;
                settings->selected_bthome_object_ids_count = 0;
            }
            
            updated = true;
            ESP_LOGI(TAG, "Updated BTHome object IDs - count: %zu", selected_count);
        } else {
            ESP_LOGE(TAG, "Failed to write bthome_obj_ids to NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "BTHome object IDs unchanged");
    }
    free(selected_ids);
    
    // Check and update MAC address filters
    // Format: mac_filter[N][field]=value where field is: mac, name, enabled
    mac_filter_t *filters = malloc(64 * sizeof(mac_filter_t));  // Maximum 64 filters
    if (!filters) {
        nvs_close(settings_handle);
        free(query_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    size_t filter_count = 0;
    
    // Parse all mac_filter parameters
    ESP_LOGI(TAG, "Parsing MAC address filters from query string");
    for (size_t i = 0; i < 64; i++) {
        char key_buf[64];
        bool filter_found = false;
        
        // Try to get MAC address
        snprintf(key_buf, sizeof(key_buf), "mac_filter%%5B%zu%%5D%%5Bmac%%5D", i);
        if (httpd_query_key_value(query_buf, key_buf, param_buf, sizeof(param_buf)) == ESP_OK) {
            url_decode(decoded_param, param_buf);
            
            // Parse MAC address string (format: xx:xx:xx:xx:xx:xx)
            int mac_parts[6];
            if (sscanf(decoded_param, "%02x:%02x:%02x:%02x:%02x:%02x",
                      &mac_parts[0], &mac_parts[1], &mac_parts[2],
                      &mac_parts[3], &mac_parts[4], &mac_parts[5]) == 6) {
                for (int j = 0; j < 6; j++) {
                    filters[filter_count].mac_addr[j] = (uint8_t)mac_parts[j];
                }
                filter_found = true;
            } else {
                ESP_LOGW(TAG, "Invalid MAC address format: %s", decoded_param);
                continue;
            }
        } else {
            break;  // No more filters
        }
        
        if (filter_found) {
            // Get name
            snprintf(key_buf, sizeof(key_buf), "mac_filter%%5B%zu%%5D%%5Bname%%5D", i);
            if (httpd_query_key_value(query_buf, key_buf, param_buf, sizeof(param_buf)) == ESP_OK) {
                url_decode(decoded_param, param_buf);
                strncpy(filters[filter_count].name, decoded_param, sizeof(filters[filter_count].name) - 1);
                filters[filter_count].name[sizeof(filters[filter_count].name) - 1] = '\0';
            } else {
                filters[filter_count].name[0] = '\0';
            }
            
            // Get enabled flag
            snprintf(key_buf, sizeof(key_buf), "mac_filter%%5B%zu%%5D%%5Benabled%%5D", i);
            filters[filter_count].enabled = (httpd_query_key_value(query_buf, key_buf, param_buf, sizeof(param_buf)) == ESP_OK);
            
            ESP_LOGI(TAG, "Found MAC filter[%zu]: %02x:%02x:%02x:%02x:%02x:%02x, name='%s', enabled=%d",
                     filter_count,
                     filters[filter_count].mac_addr[0],
                     filters[filter_count].mac_addr[1],
                     filters[filter_count].mac_addr[2],
                     filters[filter_count].mac_addr[3],
                     filters[filter_count].mac_addr[4],
                     filters[filter_count].mac_addr[5],
                     filters[filter_count].name,
                     filters[filter_count].enabled);
            
            filter_count++;
        }
    }
    
    // Check if MAC filters have changed
    bool mac_filters_changed = false;
    if (filter_count != settings->mac_filters_count) {
        mac_filters_changed = true;
    } else {
        for (size_t i = 0; i < filter_count; i++) {
            if (memcmp(filters[i].mac_addr, settings->mac_filters[i].mac_addr, 6) != 0 ||
                strcmp(filters[i].name, settings->mac_filters[i].name) != 0 ||
                filters[i].enabled != settings->mac_filters[i].enabled) {
                mac_filters_changed = true;
                break;
            }
        }
    }
    
    if (mac_filters_changed) {
        if (filter_count > 0) {
            err = nvs_set_blob(settings_handle, "mac_filters", filters, filter_count * sizeof(mac_filter_t));
        } else {
            // If no filters, erase the key
            err = nvs_erase_key(settings_handle, "mac_filters");
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;  // Already doesn't exist, that's fine
            }
        }
        
        if (err == ESP_OK) {
            if (settings->mac_filters != NULL) {
                free(settings->mac_filters);
            }
            
            if (filter_count > 0) {
                settings->mac_filters = malloc(filter_count * sizeof(mac_filter_t));
                if (settings->mac_filters != NULL) {
                    memcpy(settings->mac_filters, filters, filter_count * sizeof(mac_filter_t));
                    settings->mac_filters_count = filter_count;
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for MAC filters");
                    settings->mac_filters_count = 0;
                }
            } else {
                settings->mac_filters = NULL;
                settings->mac_filters_count = 0;
            }
            
            updated = true;
            ESP_LOGI(TAG, "Updated MAC filters - count: %zu", filter_count);
        } else {
            ESP_LOGE(TAG, "Failed to write mac_filters to NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "MAC filters unchanged");
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
    free(filters);
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
    settings->hostname = NULL;
    settings->selected_bthome_object_ids = NULL;
    settings->selected_bthome_object_ids_count = 0;
    settings->mac_filters = NULL;
    settings->mac_filters_count = 0;
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
            settings->password = strdup(CONFIG_HTTPD_BASIC_AUTH_PASSWORD);
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
            ESP_LOGI(TAG, "Read 'weight_scale' = %.8f (0x%08" PRIX32 ")", _IQ16toF(settings->weight_scale), weight_scale_raw);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->weight_scale = (_iq8)CONFIG_WEIGHT_SCALE;
            ESP_LOGI(TAG, "No value for 'weight_scale'; using default = %.8f (0x%08" PRIX32 ")", _IQ16toF(settings->weight_scale), (int32_t)settings->weight_scale);
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

    ESP_LOGI(TAG, "\nReading 'hostname' from NVS...");
    err = nvs_get_str(settings_handle, "hostname", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->hostname = malloc(str_size);
            if (settings->hostname == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for hostname");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(settings_handle, "hostname", settings->hostname, &str_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading hostname!", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "Read 'hostname' = '%s'", settings->hostname);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->hostname = strdup(CONFIG_ESP_WIFI_HOSTNAME);
            ESP_LOGI(TAG, "No value for 'hostname'; using default = '%s'", settings->hostname);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading hostname!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\nReading 'bthome_obj_ids' from NVS...");
    size_t blob_size = 0;
    err = nvs_get_blob(settings_handle, "bthome_obj_ids", NULL, &blob_size);
    switch (err) {
        case ESP_OK:
            settings->selected_bthome_object_ids = malloc(blob_size);
            if (settings->selected_bthome_object_ids == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for selected_bthome_object_ids");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_blob(settings_handle, "bthome_obj_ids", settings->selected_bthome_object_ids, &blob_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading bthome_obj_ids!", esp_err_to_name(err));
                free(settings->selected_bthome_object_ids);
                settings->selected_bthome_object_ids = NULL;
                return err;
            }
            settings->selected_bthome_object_ids_count = blob_size;
            ESP_LOGI(TAG, "Read 'bthome_obj_ids' - %zu IDs", settings->selected_bthome_object_ids_count);
            for (size_t i = 0; i < settings->selected_bthome_object_ids_count; i++) {
                ESP_LOGI(TAG, "  ID[%zu] = 0x%02X", i, settings->selected_bthome_object_ids[i]);
            }
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->selected_bthome_object_ids = NULL;
            settings->selected_bthome_object_ids_count = 0;
            ESP_LOGI(TAG, "No value for 'bthome_obj_ids'; using empty list");
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading bthome_obj_ids!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "\\nReading 'mac_filters' from NVS...");
    blob_size = 0;
    err = nvs_get_blob(settings_handle, "mac_filters", NULL, &blob_size);
    switch (err) {
        case ESP_OK:
            if (blob_size % sizeof(mac_filter_t) != 0) {
                ESP_LOGE(TAG, "Invalid mac_filters blob size: %zu", blob_size);
                break;
            }
            settings->mac_filters_count = blob_size / sizeof(mac_filter_t);
            settings->mac_filters = malloc(blob_size);
            if (settings->mac_filters == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for mac_filters");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_blob(settings_handle, "mac_filters", settings->mac_filters, &blob_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading mac_filters!", esp_err_to_name(err));
                free(settings->mac_filters);
                settings->mac_filters = NULL;
                settings->mac_filters_count = 0;
                return err;
            }
            ESP_LOGI(TAG, "Read 'mac_filters' - %zu filters", settings->mac_filters_count);
            for (size_t i = 0; i < settings->mac_filters_count; i++) {
                ESP_LOGI(TAG, "  Filter[%zu]: %02x:%02x:%02x:%02x:%02x:%02x, name='%s', enabled=%d",
                         i,
                         settings->mac_filters[i].mac_addr[0],
                         settings->mac_filters[i].mac_addr[1],
                         settings->mac_filters[i].mac_addr[2],
                         settings->mac_filters[i].mac_addr[3],
                         settings->mac_filters[i].mac_addr[4],
                         settings->mac_filters[i].mac_addr[5],
                         settings->mac_filters[i].name,
                         settings->mac_filters[i].enabled);
            }
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->mac_filters = NULL;
            settings->mac_filters_count = 0;
            ESP_LOGI(TAG, "No value for 'mac_filters'; using empty list");
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading mac_filters!", esp_err_to_name(err));
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

