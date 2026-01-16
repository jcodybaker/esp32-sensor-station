#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include <time.h>
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
#include "temperature.h"
#include "pump.h"
#include "metrics.h"

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
    atomic_fetch_add(&malloc_count_settings, 1);
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
    atomic_fetch_add(&malloc_count_settings, 1);
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
        "#settingsForm { background: #f4f4f4; padding: 20px; border-radius: 8px; }\n"
        "label { display: block; margin-top: 15px; font-weight: bold; }\n"
        "input, select { width: 100%; padding: 8px; margin-top: 5px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }\n"
        "input[type='checkbox'] { width: auto; }\n"
        "button { background: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; margin-top: 20px; font-size: 16px; }\n"
        "#settingsForm button { display: inline-block; background: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; margin-top: 20px; width: 100%; font-size: 16px; }\n"
        "button:hover { background: #45a049; }\n"
        "hr.minor { margin: 10px 0; border: 0; border-top: 1px solid #ccc; }\n"
        "hr.major { margin: 30px 0; border: 0; border-top: 1px solid #ccc; }\n"
        ".message { padding: 10px; margin: 10px 0; border-radius: 4px; display: none; }\n"
        ".success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }\n"
        ".error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }\n"
        "a.button { display: inline-block; background: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; text-decoration: none; font-size: 16px; }\n"
        "a.button:hover { background: #45a049; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>Sensor Station Settings</h1>\n"
        "<a href='/' class=\"button\">Home</a>\n"
        "<a href='/pump/calibrate' class=\"button\">Calibrate Pump</a>\n"
        "<form action='/ota' method='POST' style='display: inline;'>\n"
        "<button type='submit'>Start OTA Update</button>\n"
        "</form>\n"
        "<form action='/reboot' method='POST' style='display: inline;'>\n"
        "<button type='submit' style='background: #ff9800;'>Reboot Device</button>\n"
        "</form><br><br>\n"
        "<div id='message' class='message'></div>\n"
        "<form id='settingsForm'>\n"
        "<h2>General Configuration</h2>\n"
        "<label for='password'>Password:</label>\n"
        "<input type='password' id='password' name='password' placeholder='Leave blank to keep current'>\n"
        );

    // Send hostname with current value
    char *encoded_hostname = url_encode(settings->hostname);
    snprintf(buffer, 1024,
        "<label for='hostname'>Hostname:</label>\n"
        "<input type='text' id='hostname' name='hostname' value='%s'>\n",
        encoded_hostname ? encoded_hostname : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_hostname);
    atomic_fetch_add(&free_count_settings, 1);

    // Send update_url with current value
    char *encoded_update_url = url_encode(settings->update_url);
    snprintf(buffer, 1024, 
        "<hr class='minor'/>\n"
        "<label for='update_url'>Update URL:</label>\n"
        "<input type='text' id='update_url' name='update_url' value='%s'>\n",
        encoded_update_url ? encoded_update_url : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_update_url);    
    atomic_fetch_add(&free_count_settings, 1);

    // Send timezone with current value
    char *encoded_timezone = url_encode(settings->timezone);
    snprintf(buffer, 1024,
        "<label for='timezone'>Timezone (e.g., EST5EDT,M3.2.0,M11.1.0):</label>\n"
        "<input type='text' id='timezone' name='timezone' value='%s' placeholder='UTC0'>\n",
        encoded_timezone ? encoded_timezone : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_timezone);
    atomic_fetch_add(&free_count_settings, 1);

    // Send temperature unit checkbox
    snprintf(buffer, 1024,
        "<hr class='minor'/>\n"
        "<label for='temp_use_fahrenheit'>\n"
        "<input type='checkbox' id='temp_use_fahrenheit' name='temp_use_fahrenheit' value='1'%s> Display Temperatures in Fahrenheit (&deg;F)\n"
        "</label>\n",
        settings->temp_use_fahrenheit ? " checked" : "");
    httpd_resp_sendstr_chunk(req, buffer);

    // Send wifi_ssid with current value
    char *encoded_wifi_ssid = url_encode(settings->wifi_ssid);
    snprintf(buffer, 1024,
        "<hr class='minor'/>\n"
        "<h2>Wifi Configuration</h2>\n"
        "<label for='wifi_ssid'>Wifi SSID:</label>\n"
        "<input type='text' id='wifi_ssid' name='wifi_ssid' value='%s'>\n",
        encoded_wifi_ssid ? encoded_wifi_ssid : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_wifi_ssid);
    atomic_fetch_add(&free_count_settings, 1);
    
    // Send wifi_password and checkbox
    snprintf(buffer, 1024,
        "<label for='wifi_password'>Wifi Password:</label>\n"
        "<input type='password' id='wifi_password' name='wifi_password' placeholder='Leave blank to keep current'>\n"
        "<label for='wifi_ap_fallback_disable'>\n"
        "<input type='checkbox' id='wifi_ap_fallback_disable' name='wifi_ap_fallback_disable' value='1'%s> Disable WiFi AP Fallback\n"
        "</label>\n",
        settings->wifi_ap_fallback_disable ? " checked" : "");
    httpd_resp_sendstr_chunk(req, buffer);
        
    // Send syslog settings
    httpd_resp_sendstr_chunk(req,
        "<hr class='major'/>\n"
        "<h2>Syslog Configuration</h2>\n");
    
    // Send syslog_server with current value
    char *encoded_syslog_server = url_encode(settings->syslog_server);
    snprintf(buffer, 1024,
        "<label for='syslog_server'>Syslog Server (hostname or IP):</label>\n"
        "<input type='text' id='syslog_server' name='syslog_server' value='%s' placeholder='syslog.example.com'>\n",
        encoded_syslog_server ? encoded_syslog_server : "");
    httpd_resp_sendstr_chunk(req, buffer);
    free(encoded_syslog_server);
    atomic_fetch_add(&free_count_settings, 1);
    
    // Send syslog_port with current value
    snprintf(buffer, 1024,
        "<label for='syslog_port'>Syslog Port:</label>\n"
        "<input type='number' id='syslog_port' name='syslog_port' value='%u' min='1' max='65535'>\n",
        settings->syslog_port);
    httpd_resp_sendstr_chunk(req, buffer);

    // Send weight_tare with current value
    snprintf(buffer, 1024,
        "<hr class='minor'/>\n"
        "<h2>Weight Configuration</h2>\n"
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

    
    // Send weight_dt_gpio with current value
    snprintf(buffer, 1024,
        "<label for='weight_dt_gpio'>Weight (HX711) DOUT GPIO Pin (-1 = disabled, suggested: 32):</label>\n"
        "<input type='number' id='weight_dt_gpio' name='weight_dt_gpio' value='%d' min='-1' max='39'>\n",
        settings->weight_dt_gpio);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send weight_sck_gpio with current value
    snprintf(buffer, 1024,
        "<label for='weight_sck_gpio'>Weight (HX711) SCK GPIO Pin (-1 = disabled, suggested: 26):</label>\n"
        "<input type='number' id='weight_sck_gpio' name='weight_sck_gpio' value='%d' min='-1' max='39'>\n",
        settings->weight_sck_gpio);
    httpd_resp_sendstr_chunk(req, buffer);

    // Send pump_scl_gpio with current value
    snprintf(buffer, 1024,
        "<hr class='minor'/>\n"
        "<h2>Pump Configuration</h2>\n"
        "<label for='pump_scl_gpio'>Pump I2C SCL GPIO Pin (-1 = disabled):</label>\n"
        "<input type='number' id='pump_scl_gpio' name='pump_scl_gpio' value='%d' min='-1' max='39'>\n",
        settings->pump_scl_gpio);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send pump_sda_gpio with current value
    snprintf(buffer, 1024,
        "<label for='pump_sda_gpio'>Pump I2C SDA GPIO Pin (-1 = disabled):</label>\n"
        "<input type='number' id='pump_sda_gpio' name='pump_sda_gpio' value='%d' min='-1' max='39'>\n",
        settings->pump_sda_gpio);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send pump_i2c_addr with current value
    snprintf(buffer, 1024,
        "<label for='pump_i2c_addr'>Pump I2C Device Address:</label>\n"
        "<input type='number' id='pump_i2c_addr' name='pump_i2c_addr' value='%d' min='0' max='127'>\n",
        settings->pump_i2c_addr);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send pump_dispense_ml with current value
    snprintf(buffer, 1024,
        "<label for='pump_dispense_ml'>Pump Dispense Amount (ml, 1-1000):</label>\n"
        "<input type='number' id='pump_dispense_ml' name='pump_dispense_ml' value='%d' min='1' max='1000'>\n",
        settings->pump_dispense_ml);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Display pump error if set
    const char* pump_error = pump_get_last_error();
    if (pump_error != NULL) {
        httpd_resp_sendstr_chunk(req,
            "<div class='error' style='display: block; margin-top: 10px;'>\n"
            "<strong>Pump Error:</strong> ");
        httpd_resp_sendstr_chunk(req, pump_error);
        httpd_resp_sendstr_chunk(req, "</div>\n");
    }
        
    // Send ds18b20_gpio with current value
    snprintf(buffer, 1024,
        "<hr class='minor'/>\n"
        "<h2>DS18B20 Thermometer Configuration</h2>\n"
        "<label for='ds18b20_gpio'>DS18B20 Temperature Sensor GPIO Pin (-1 = disabled):</label>\n"
        "<input type='number' id='ds18b20_gpio' name='ds18b20_gpio' value='%d' min='-1' max='39'>\n",
        settings->ds18b20_gpio);
    httpd_resp_sendstr_chunk(req, buffer);
    
    // Send ds18b20_pwr_gpio with current value
    snprintf(buffer, 1024,
        "<label for='ds18b20_pwr_gpio'>DS18B20 Power GPIO Pin (-1 = disabled):</label>\n"
        "<input type='number' id='ds18b20_pwr_gpio' name='ds18b20_pwr_gpio' value='%d' min='-1' max='39'>\n",
        settings->ds18b20_pwr_gpio);
    httpd_resp_sendstr_chunk(req, buffer);

        // Send DS18B20 device names section
    httpd_resp_sendstr_chunk(req,
        "<hr class='minor'/>\n"
        "<label>DS18B20 Temperature Sensor Names:</label>\n"
        "<div id='ds18b20_names_container'>\n");
    
    // Get currently detected DS18B20 devices
    ds18b20_info_t detected_devices[EXAMPLE_ONEWIRE_MAX_DS18B20];
    int detected_count = get_ds18b20_devices(detected_devices, EXAMPLE_ONEWIRE_MAX_DS18B20);
    
    // Create a merged list: detected devices with their saved names (if any)
    size_t display_index = 0;
    
    // First, output all detected devices
    for (int i = 0; i < detected_count; i++) {
        char addr_str[17];
        snprintf(addr_str, sizeof(addr_str), "%016llX", detected_devices[i].address);
        
        // Look up the name for this device
        const char *device_name = settings_get_ds18b20_name(settings, detected_devices[i].address);
        char *encoded_name = url_encode(device_name ? device_name : "");
        
        snprintf(buffer, 1024,
            "<div class='ds18b20_name_row' style='margin: 10px 0; padding: 10px; background: #fff; border: 1px solid #ddd; border-radius: 4px;'>\n"
            "  <input type='text' name='ds18b20_name[%zu][address]' value='%s' placeholder='Device Address (hex)' style='width: 180px;' pattern='[0-9a-fA-F]{16}' title='16-character hex address' readonly>\n"
            "  <input type='text' name='ds18b20_name[%zu][name]' value='%s' placeholder='Device Name' style='width: 250px;'>\n"
            "  <button type='button' onclick='this.parentElement.remove()' style='width: auto; padding: 5px 10px; background: #dc3545; margin-left: 10px;'>Remove</button>\n"
            "</div>\n",
            display_index, addr_str, display_index, encoded_name ? encoded_name : "");
        httpd_resp_sendstr_chunk(req, buffer);
        free(encoded_name);
        atomic_fetch_add(&free_count_settings, 1);
        display_index++;
    }
    
    // Then, output any saved device names for devices that are NOT currently detected
    for (size_t i = 0; i < settings->ds18b20_names_count; i++) {
        bool is_detected = false;
        for (int j = 0; j < detected_count; j++) {
            if (settings->ds18b20_names[i].address == detected_devices[j].address) {
                is_detected = true;
                break;
            }
        }
        
        if (!is_detected) {
            char addr_str[17];
            snprintf(addr_str, sizeof(addr_str), "%016llX", settings->ds18b20_names[i].address);
            
            char *encoded_name = url_encode(settings->ds18b20_names[i].name);
            
            snprintf(buffer, 1024,
                "<div class='ds18b20_name_row' style='margin: 10px 0; padding: 10px; background: #eee; border: 1px solid #ddd; border-radius: 4px;'>\n"
                "  <input type='text' name='ds18b20_name[%zu][address]' value='%s' placeholder='Device Address (hex)' style='width: 180px;' pattern='[0-9a-fA-F]{16}' title='16-character hex address'>\n"
                "  <input type='text' name='ds18b20_name[%zu][name]' value='%s' placeholder='Device Name (not currently detected)' style='width: 250px;'>\n"
                "  <button type='button' onclick='this.parentElement.remove()' style='width: auto; padding: 5px 10px; background: #dc3545; margin-left: 10px;'>Remove</button>\n"
                "</div>\n",
                display_index, addr_str, display_index, encoded_name ? encoded_name : "");
            httpd_resp_sendstr_chunk(req, buffer);
            free(encoded_name);
            atomic_fetch_add(&free_count_settings, 1);
            display_index++;
        }
    }
    
    httpd_resp_sendstr_chunk(req,
        "</div>\n"
        "<button type='button' onclick='addDS18B20Name()' style='width: auto; background: #007bff; margin-top: 10px;'>Add DS18B20 Name</button>\n"
        "<script>\n"
        "var ds18b20NameIndex = " );
    
    // Use display_index which accounts for all devices shown
    snprintf(buffer, 1024, "%zu;\n", display_index);
    httpd_resp_sendstr_chunk(req, buffer);
    
    httpd_resp_sendstr_chunk(req,
        "function addDS18B20Name() {\n"
        "  var container = document.getElementById('ds18b20_names_container');\n"
        "  var div = document.createElement('div');\n"
        "  div.className = 'ds18b20_name_row';\n"
        "  div.style = 'margin: 10px 0; padding: 10px; background: #fff; border: 1px solid #ddd; border-radius: 4px;';\n"
        "  div.innerHTML = `\n"
        "    <input type='text' name='ds18b20_name[${ds18b20NameIndex}][address]' placeholder='Device Address (hex)' style='width: 180px;' pattern='[0-9a-fA-F]{16}' title='16-character hex address'>\n"
        "    <input type='text' name='ds18b20_name[${ds18b20NameIndex}][name]' placeholder='Device Name' style='width: 250px;'>\n"
        "    <button type='button' onclick='this.parentElement.remove()' style='width: auto; padding: 5px 10px; background: #dc3545; margin-left: 10px;'>Remove</button>\n"
        "  `;\n"
        "  container.appendChild(div);\n"
        "  ds18b20NameIndex++;\n"
        "}\n"
        "</script>\n");
    
    // Send BTHome object IDs multi-select
    httpd_resp_sendstr_chunk(req,
        "<hr class='minor'/>\n"
        "<h2>BTHome Configuration</h2>\n"
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
        atomic_fetch_add(&free_count_settings, 1);
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
        "  window.scrollTo(0, 0);\n"
        "  var formData = new FormData(this);\n"
        "  var params = new URLSearchParams();\n"
        "  // Handle BTHome objects multi-select\n"
        "  var select = document.getElementById('bthome_objects');\n"
        "  var selectedOptions = Array.from(select.selectedOptions);\n"
        "  params.append('bthome_objects_count', selectedOptions.length);\n"
        "  for (var i = 0; i < selectedOptions.length; i++) {\n"
        "    params.append('bthome_objects[' + i + ']', selectedOptions[i].value);\n"
        "  }\n"
        "  // Count MAC filters\n"
        "  var macFilterCount = 0;\n"
        "  var macInputs = document.querySelectorAll('input[name^=\"mac_filter[\"][name$=\"[mac]\"]');\n"
        "  macInputs.forEach(function(input) {\n"
        "    if (input.value) macFilterCount++;\n"
        "  });\n"
        "  params.append('mac_filter_count', macFilterCount);\n"
        "  // Count DS18B20 names\n"
        "  var ds18b20NameCount = 0;\n"
        "  var ds18b20Inputs = document.querySelectorAll('input[name^=\"ds18b20_name[\"][name$=\"[address]\"]');\n"
        "  ds18b20Inputs.forEach(function(input) {\n"
        "    if (input.value) ds18b20NameCount++;\n"
        "  });\n"
        "  params.append('ds18b20_name_count', ds18b20NameCount);\n"
        "  // Process all other form fields\n"
        "  for (var pair of formData.entries()) {\n"
        "    if (pair[1]) {\n"
        "      // Skip bthome_objects (already handled above)\n"
        "      if (pair[0] === 'bthome_objects') {\n"
        "        continue;\n"
        "      }\n"
        "      params.append(pair[0], pair[1]);\n"
        "    } else if (pair[0].startsWith('mac_filter[') && pair[0].includes('[mac]')) {\n"
        "      // Include MAC filter fields even if empty for proper indexing\n"
        "      params.append(pair[0], pair[1]);\n"
        "    }\n"
        "  }\n"
        "  fetch('/settings', {\n"
        "    method: 'POST',\n"
        "    headers: {\n"
        "      'Content-Type': 'application/x-www-form-urlencoded'\n"
        "    },\n"
        "    body: params.toString()\n"
        "  })\n"
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
    atomic_fetch_add(&free_count_settings, 1);
    return ESP_OK;
}


static esp_err_t settings_post_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    esp_err_t err = ESP_OK;
    bool updated = false;
    bool restart_needed = false;
    
    char *query_buf = NULL;
    
    // Try to get data from POST body first
    size_t content_len = req->content_len;
    if (content_len > 0) {
        // Allocate buffer for POST data
        query_buf = malloc(content_len + 1);
        atomic_fetch_add(&malloc_count_settings, 1);
        if (query_buf == NULL) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
        
        // Read the POST data from request body
        int ret = httpd_req_recv(req, query_buf, content_len);
        if (ret <= 0) {
            free(query_buf);
            atomic_fetch_add(&free_count_settings, 1);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request timeout");
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read POST data");
            }
            return ESP_FAIL;
        }
        query_buf[ret] = '\0';
    } else {
        // Fall back to query parameters (for backward compatibility with tare button)
        size_t query_len = httpd_req_get_url_query_len(req);
        if (query_len == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No POST data or query parameters provided");
            return ESP_FAIL;
        }
        
        query_buf = malloc(query_len + 1);
        atomic_fetch_add(&malloc_count_settings, 1);
        if (query_buf == NULL) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
        
        if (httpd_req_get_url_query_str(req, query_buf, query_len + 1) != ESP_OK) {
            free(query_buf);
            atomic_fetch_add(&free_count_settings, 1);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse query string");
            return ESP_FAIL;
        }
    }
    
    // Open NVS handle
    nvs_handle_t settings_handle;
    err = nvs_open("settings", NVS_READWRITE, &settings_handle);
    if (err != ESP_OK) {
        free(query_buf);
        atomic_fetch_add(&free_count_settings, 1);
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
                    atomic_fetch_add(&free_count_settings, 1);
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
                    atomic_fetch_add(&free_count_settings, 1);
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
    
    // Check and update ds18b20_gpio
    if (httpd_query_key_value(query_buf, "ds18b20_gpio", param_buf, sizeof(param_buf)) == ESP_OK) {
        int8_t ds18b20_gpio = (int8_t)atoi(param_buf);
        if (ds18b20_gpio == settings->ds18b20_gpio) {
            ESP_LOGI(TAG, "DS18B20 GPIO unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i8(settings_handle, "ds18b20_gpio", ds18b20_gpio);
            if (err == ESP_OK) {
                settings->ds18b20_gpio = ds18b20_gpio;
                updated = true;
                ESP_LOGI(TAG, "Updated ds18b20_gpio to %d", ds18b20_gpio);
            } else {
                ESP_LOGE(TAG, "Failed to write ds18b20_gpio to NVS: %s", esp_err_to_name(err));
            }
            restart_needed = true;
        }
    }
    
    // Check and update ds18b20_pwr_gpio
    if (httpd_query_key_value(query_buf, "ds18b20_pwr_gpio", param_buf, sizeof(param_buf)) == ESP_OK) {
        int8_t ds18b20_pwr_gpio = (int8_t)atoi(param_buf);
        if (ds18b20_pwr_gpio == settings->ds18b20_pwr_gpio) {
            ESP_LOGI(TAG, "DS18B20 Power GPIO unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i8(settings_handle, "ds18b20_pwr", ds18b20_pwr_gpio);
            if (err == ESP_OK) {
                settings->ds18b20_pwr_gpio = ds18b20_pwr_gpio;
                updated = true;
                ESP_LOGI(TAG, "Updated ds18b20_pwr_gpio to %d", ds18b20_pwr_gpio);
            } else {
                ESP_LOGE(TAG, "Failed to write ds18b20_pwr_gpio to NVS: %s", esp_err_to_name(err));
            }
            restart_needed = true;
        }
    }
    
    // Check and update weight_dt_gpio
    if (httpd_query_key_value(query_buf, "weight_dt_gpio", param_buf, sizeof(param_buf)) == ESP_OK) {
        int8_t weight_dt_gpio = (int8_t)atoi(param_buf);
        if (weight_dt_gpio == settings->weight_dt_gpio) {
            ESP_LOGI(TAG, "Weight DOUT GPIO unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i8(settings_handle, "weight_dt_gpio", weight_dt_gpio);
            if (err == ESP_OK) {
                settings->weight_dt_gpio = weight_dt_gpio;
                updated = true;
                ESP_LOGI(TAG, "Updated weight_dt_gpio to %d", weight_dt_gpio);
            } else {
                ESP_LOGE(TAG, "Failed to write weight_dt_gpio to NVS: %s", esp_err_to_name(err));
            }
            restart_needed = true;
        }
    }
    
    // Check and update weight_sck_gpio
    if (httpd_query_key_value(query_buf, "weight_sck_gpio", param_buf, sizeof(param_buf)) == ESP_OK) {
        int8_t weight_sck_gpio = (int8_t)atoi(param_buf);
        if (weight_sck_gpio == settings->weight_sck_gpio) {
            ESP_LOGI(TAG, "Weight SCK GPIO unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i8(settings_handle, "weight_sck_gpio", weight_sck_gpio);
            if (err == ESP_OK) {
                settings->weight_sck_gpio = weight_sck_gpio;
                updated = true;
                ESP_LOGI(TAG, "Updated weight_sck_gpio to %d", weight_sck_gpio);
            } else {
                ESP_LOGE(TAG, "Failed to write weight_sck_gpio to NVS: %s", esp_err_to_name(err));
            }
            restart_needed = true;
        }
    }

    
    // Check and update pump_scl_gpio
    if (httpd_query_key_value(query_buf, "pump_scl_gpio", param_buf, sizeof(param_buf)) == ESP_OK) {
        int8_t pump_scl_gpio = (int8_t)atoi(param_buf);
        if (pump_scl_gpio == settings->pump_scl_gpio) {
            ESP_LOGI(TAG, "Pump SCL GPIO unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i8(settings_handle, "pump_scl_gpio", pump_scl_gpio);
            if (err == ESP_OK) {
                settings->pump_scl_gpio = pump_scl_gpio;
                updated = true;
                ESP_LOGI(TAG, "Updated pump_scl_gpio to %d", pump_scl_gpio);
            } else {
                ESP_LOGE(TAG, "Failed to write pump_scl_gpio to NVS: %s", esp_err_to_name(err));
            }
            restart_needed = true;
        }
    }
    
    // Check and update pump_sda_gpio
    if (httpd_query_key_value(query_buf, "pump_sda_gpio", param_buf, sizeof(param_buf)) == ESP_OK) {
        int8_t pump_sda_gpio = (int8_t)atoi(param_buf);
        if (pump_sda_gpio == settings->pump_sda_gpio) {
            ESP_LOGI(TAG, "Pump SDA GPIO unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i8(settings_handle, "pump_sda_gpio", pump_sda_gpio);
            if (err == ESP_OK) {
                settings->pump_sda_gpio = pump_sda_gpio;
                updated = true;
                ESP_LOGI(TAG, "Updated pump_sda_gpio to %d", pump_sda_gpio);
            } else {
                ESP_LOGE(TAG, "Failed to write pump_sda_gpio to NVS: %s", esp_err_to_name(err));
            }
            restart_needed = true;
        }
    }

        // Check and update pump_i2c_addr
    if (httpd_query_key_value(query_buf, "pump_i2c_addr", param_buf, sizeof(param_buf)) == ESP_OK) {
        int8_t pump_i2c_addr = (int8_t)atoi(param_buf);
        if (pump_i2c_addr == settings->pump_i2c_addr) {
            ESP_LOGI(TAG, "Pump I2C address unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0) {
            err = nvs_set_i8(settings_handle, "pump_i2c_addr", pump_i2c_addr);
            if (err == ESP_OK) {
                settings->pump_i2c_addr = pump_i2c_addr;
                updated = true;
                ESP_LOGI(TAG, "Updated pump_i2c_addr to %d", pump_i2c_addr);
            } else {
                ESP_LOGE(TAG, "Failed to write pump_i2c_addr to NVS: %s", esp_err_to_name(err));
            }
            restart_needed = true;
        }
    }

    // Check and update pump_dispense_ml
    if (httpd_query_key_value(query_buf, "pump_dispense_ml", param_buf, sizeof(param_buf)) == ESP_OK) {
        int16_t pump_dispense_ml = (int16_t)atoi(param_buf);
        if (pump_dispense_ml < 1 || pump_dispense_ml > 1000) {
            ESP_LOGW(TAG, "Invalid pump_dispense_ml value: %d, must be 1-1000", pump_dispense_ml);
        } else if (pump_dispense_ml == settings->pump_dispense_ml) {
            ESP_LOGI(TAG, "Pump dispense amount unchanged");
            param_buf[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(param_buf) > 0 && pump_dispense_ml >= 1 && pump_dispense_ml <= 1000) {
            err = nvs_set_i16(settings_handle, "pump_disp_ml", pump_dispense_ml);
            if (err == ESP_OK) {
                settings->pump_dispense_ml = pump_dispense_ml;
                updated = true;
                ESP_LOGI(TAG, "Updated pump_dispense_ml to %d", pump_dispense_ml);
            } else {
                ESP_LOGE(TAG, "Failed to write pump_dispense_ml to NVS: %s", esp_err_to_name(err));
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
                    atomic_fetch_add(&free_count_settings, 1);
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
                    atomic_fetch_add(&free_count_settings, 1);
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

    // Check and update temp_use_fahrenheit
    bool temp_use_fahrenheit = false;
    if (httpd_query_key_value(query_buf, "temp_use_fahrenheit", param_buf, sizeof(param_buf)) == ESP_OK) {
        temp_use_fahrenheit = true;
    }
    if (temp_use_fahrenheit != settings->temp_use_fahrenheit) {
        err = nvs_set_u8(settings_handle, "temp_use_f", temp_use_fahrenheit ? 1 : 0);
        if (err == ESP_OK) {
            settings->temp_use_fahrenheit = temp_use_fahrenheit;
            updated = true;
            restart_needed = true;
            ESP_LOGI(TAG, "Updated temp_use_fahrenheit to %d", temp_use_fahrenheit);
        } else {
            ESP_LOGE(TAG, "Failed to write temp_use_fahrenheit to NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "Temperature unit setting unchanged");
    }

    // Check and update syslog_server
    if (httpd_query_key_value(query_buf, "syslog_server", param_buf, sizeof(param_buf)) == ESP_OK) {
        url_decode(decoded_param, param_buf);  // Decode URL encoding
        if (settings->syslog_server != NULL && strcmp(decoded_param, settings->syslog_server) == 0) {
            ESP_LOGI(TAG, "Syslog server unchanged");
            decoded_param[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(decoded_param) > 0 || (settings->syslog_server != NULL && strlen(settings->syslog_server) > 0)) {
            err = nvs_set_str(settings_handle, "syslog_server", decoded_param);
            if (err == ESP_OK) {
                if (settings->syslog_server != NULL) {
                    free(settings->syslog_server);
                    atomic_fetch_add(&free_count_settings, 1);
                }
                settings->syslog_server = strdup(decoded_param);
                updated = true;
                restart_needed = true;
                ESP_LOGI(TAG, "Updated syslog_server to %s", decoded_param);
            } else {
                ESP_LOGE(TAG, "Failed to write syslog_server to NVS: %s", esp_err_to_name(err));
            }
        }
    }

    // Check and update syslog_port
    if (httpd_query_key_value(query_buf, "syslog_port", param_buf, sizeof(param_buf)) == ESP_OK) {
        uint16_t syslog_port = (uint16_t)atoi(param_buf);
        if (syslog_port > 0 && syslog_port != settings->syslog_port) {
            err = nvs_set_u16(settings_handle, "syslog_port", syslog_port);
            if (err == ESP_OK) {
                settings->syslog_port = syslog_port;
                updated = true;
                restart_needed = true;
                ESP_LOGI(TAG, "Updated syslog_port to %u", syslog_port);
            } else {
                ESP_LOGE(TAG, "Failed to write syslog_port to NVS: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "Syslog port unchanged or invalid");
        }
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
                    atomic_fetch_add(&free_count_settings, 1);
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

    // Check and update timezone
    if (httpd_query_key_value(query_buf, "timezone", param_buf, sizeof(param_buf)) == ESP_OK) {
        url_decode(decoded_param, param_buf);  // Decode URL encoding
        if (strcmp(decoded_param, settings->timezone) == 0) {
            ESP_LOGI(TAG, "Timezone unchanged");
            decoded_param[0] = '\0'; // Clear to avoid updating
        }
        if (strlen(decoded_param) > 0) {
            err = nvs_set_str(settings_handle, "timezone", decoded_param);
            if (err == ESP_OK) {
                if (settings->timezone != NULL) {
                    free(settings->timezone);
                    atomic_fetch_add(&free_count_settings, 1);
                }
                settings->timezone = strdup(decoded_param);
                // Apply timezone using setenv and tzset
                setenv("TZ", settings->timezone, 1);
                tzset();
                updated = true;
                ESP_LOGI(TAG, "Updated timezone to %s", decoded_param);
            } else {
                ESP_LOGE(TAG, "Failed to write timezone to NVS: %s", esp_err_to_name(err));
            }
        }
    }
    
    // Check and update BTHome object IDs
    // Only process if bthome_objects_count field is present in the query
    if (httpd_query_key_value(query_buf, "bthome_objects_count", param_buf, sizeof(param_buf)) == ESP_OK) {
        size_t expected_count = (size_t)atoi(param_buf);
        ESP_LOGI(TAG, "BTHome objects count field present: %zu", expected_count);
        
        // The multi-select will send multiple parameters with the same name
        // We need to parse them all and create a blob
        uint8_t *selected_ids = malloc(256);  // Maximum 256 object IDs
        atomic_fetch_add(&malloc_count_settings, 1);
        if (!selected_ids) {
            nvs_close(settings_handle);
            free(query_buf);
            atomic_fetch_add(&free_count_settings, 1);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
        size_t selected_count = 0;
        
        // Parse all bthome_objects[] parameters
        char *query_ptr = query_buf;
        ESP_LOGI(TAG, "Parsing BTHome object IDs from query string");
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
                atomic_fetch_add(&free_count_settings, 1);
            }
            
            if (selected_count > 0) {
                settings->selected_bthome_object_ids = malloc(selected_count);
                atomic_fetch_add(&malloc_count_settings, 1);
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
        atomic_fetch_add(&free_count_settings, 1);
    } else {
        ESP_LOGI(TAG, "BTHome object IDs field not present in request, skipping");
    }
    
    // Check and update MAC address filters
    // Only process if mac_filter_count field is present in the query
    if (httpd_query_key_value(query_buf, "mac_filter_count", param_buf, sizeof(param_buf)) == ESP_OK) {
        size_t expected_count = (size_t)atoi(param_buf);
        ESP_LOGI(TAG, "MAC filter count field present: %zu", expected_count);
        
        // Format: mac_filter[N][field]=value where field is: mac, name, enabled
        mac_filter_t *filters = malloc(64 * sizeof(mac_filter_t));  // Maximum 64 filters
        atomic_fetch_add(&malloc_count_settings, 1);
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
                atomic_fetch_add(&free_count_settings, 1);
            }
            
            if (filter_count > 0) {
                settings->mac_filters = malloc(filter_count * sizeof(mac_filter_t));
                atomic_fetch_add(&malloc_count_settings, 1);
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
        free(filters);
        atomic_fetch_add(&free_count_settings, 1);
    } else {
        ESP_LOGI(TAG, "MAC filter field not present in request, skipping");
    }
    
    // Check and update DS18B20 device names
    // Only process if ds18b20_name_count field is present in the query
    if (httpd_query_key_value(query_buf, "ds18b20_name_count", param_buf, sizeof(param_buf)) == ESP_OK) {
        size_t expected_count = (size_t)atoi(param_buf);
        ESP_LOGI(TAG, "DS18B20 name count field present: %zu", expected_count);
        
        // Format: ds18b20_name[N][field]=value where field is: address, name
        ds18b20_name_t *names = malloc(64 * sizeof(ds18b20_name_t));  // Maximum 64 devices
        atomic_fetch_add(&malloc_count_settings, 1);
        if (!names) {
            nvs_close(settings_handle);
            free(query_buf);
            atomic_fetch_add(&free_count_settings, 1);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
        size_t name_count = 0;
        
        // Parse all ds18b20_name parameters
        ESP_LOGI(TAG, "Parsing DS18B20 device names from query string");
        for (size_t i = 0; i < 64; i++) {
            char key_buf[64];
            bool name_found = false;
            
            // Try to get address
            snprintf(key_buf, sizeof(key_buf), "ds18b20_name%%5B%zu%%5D%%5Baddress%%5D", i);
            if (httpd_query_key_value(query_buf, key_buf, param_buf, sizeof(param_buf)) == ESP_OK) {
                url_decode(decoded_param, param_buf);
                
                // Parse address string (format: 16 hex characters)
                uint64_t address = 0;
                if (strlen(decoded_param) == 16 && sscanf(decoded_param, "%016llX", &address) == 1) {
                    names[name_count].address = address;
                    name_found = true;
                } else if (strlen(decoded_param) == 16 && sscanf(decoded_param, "%016llx", &address) == 1) {
                    names[name_count].address = address;
                    name_found = true;
                } else {
                    ESP_LOGW(TAG, "Invalid DS18B20 address format: %s", decoded_param);
                    continue;
                }
            } else {
                break;  // No more names
            }
            
            if (name_found) {
                // Get name
                snprintf(key_buf, sizeof(key_buf), "ds18b20_name%%5B%zu%%5D%%5Bname%%5D", i);
                if (httpd_query_key_value(query_buf, key_buf, param_buf, sizeof(param_buf)) == ESP_OK) {
                    url_decode(decoded_param, param_buf);
                    strncpy(names[name_count].name, decoded_param, sizeof(names[name_count].name) - 1);
                    names[name_count].name[sizeof(names[name_count].name) - 1] = '\0';
                } else {
                    names[name_count].name[0] = '\0';
                }
                
                ESP_LOGI(TAG, "Found DS18B20 name[%zu]: address=%016llX, name='%s'",
                         name_count,
                         names[name_count].address,
                         names[name_count].name);
                
                name_count++;
            }
        }
        
        // Check if DS18B20 names have changed
        bool ds18b20_names_changed = false;
        if (name_count != settings->ds18b20_names_count) {
            ds18b20_names_changed = true;
        } else {
            for (size_t i = 0; i < name_count; i++) {
                if (names[i].address != settings->ds18b20_names[i].address ||
                    strcmp(names[i].name, settings->ds18b20_names[i].name) != 0) {
                    ds18b20_names_changed = true;
                    break;
                }
            }
        }
        
        if (ds18b20_names_changed) {
            if (name_count > 0) {
                err = nvs_set_blob(settings_handle, "ds18b20_names", names, name_count * sizeof(ds18b20_name_t));
            } else {
                // If no names, erase the key
                err = nvs_erase_key(settings_handle, "ds18b20_names");
                if (err == ESP_ERR_NVS_NOT_FOUND) {
                    err = ESP_OK;  // Already doesn't exist, that's fine
                }
            }
            
            if (err == ESP_OK) {
                if (settings->ds18b20_names != NULL) {
                    free(settings->ds18b20_names);
                    atomic_fetch_add(&free_count_settings, 1);
                }
                
                if (name_count > 0) {
                    settings->ds18b20_names = malloc(name_count * sizeof(ds18b20_name_t));
                    atomic_fetch_add(&malloc_count_settings, 1);
                    if (settings->ds18b20_names != NULL) {
                        memcpy(settings->ds18b20_names, names, name_count * sizeof(ds18b20_name_t));
                        settings->ds18b20_names_count = name_count;
                    } else {
                        ESP_LOGE(TAG, "Failed to allocate memory for DS18B20 names");
                        settings->ds18b20_names_count = 0;
                    }
                } else {
                    settings->ds18b20_names = NULL;
                    settings->ds18b20_names_count = 0;
                }
                
                updated = true;
                ESP_LOGI(TAG, "Updated DS18B20 names - count: %zu", name_count);
            } else {
                ESP_LOGE(TAG, "Failed to write ds18b20_names to NVS: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "DS18B20 names unchanged");
        }
        free(names);
        atomic_fetch_add(&free_count_settings, 1);
    } else {
        ESP_LOGI(TAG, "DS18B20 name field not present in request, skipping");
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
    atomic_fetch_add(&free_count_settings, 1);
    
    if (updated) {
        httpd_resp_set_status(req, HTTPD_200);
        if (restart_needed) {
            httpd_resp_send(req, "Settings updated successfully. Restarting...", HTTPD_RESP_USE_STRLEN);
        } else {
            httpd_resp_send(req, "Settings updated successfully", HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_send(req, "Settings updated successfully", HTTPD_RESP_USE_STRLEN);
        if (restart_needed) {
            ESP_LOGI(TAG, "Restarting system to apply changes...");
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_restart();
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No valid parameters to update");
    }
    
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Reboot requested");
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_send(req, "<html><head><meta http-equiv=\"refresh\" content=\"5; url=/\"></head><body>Rebooting...</body></html>", HTTPD_RESP_USE_STRLEN);
    
    // Schedule restart after a short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    
    return ESP_OK;
}

static httpd_uri_t reboot_post_uri = {
    .uri       = "/reboot",
    .method    = HTTP_POST,
    .handler   = reboot_post_handler,
    .user_ctx  = NULL
};

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
    settings->timezone = NULL;
    settings->selected_bthome_object_ids = NULL;
    settings->selected_bthome_object_ids_count = 0;
    settings->mac_filters = NULL;
    settings->mac_filters_count = 0;
    settings->ds18b20_gpio = -1;
    settings->ds18b20_pwr_gpio = -1;
    settings->weight_dt_gpio = -1;
    settings->weight_sck_gpio = -1;
    settings->pump_scl_gpio = -1;
    settings->pump_sda_gpio = -1;
    settings->pump_i2c_addr = 0x37;
    settings->pump_dispense_ml = 100;  // Default 100ml
    settings->syslog_server = NULL;
    settings->syslog_port = 514;  // Default syslog port
    // Open NVS handle
    ESP_LOGI(TAG, "Opening Non-Volatile Storage (NVS) handle...");
    nvs_handle_t settings_handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &settings_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Reading 'update_url' from NVS...");
    size_t str_size = 0;
    err = nvs_get_str(settings_handle, "update_url", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->update_url = malloc(str_size);
            atomic_fetch_add(&malloc_count_settings, 1);
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

    ESP_LOGI(TAG, "Reading 'password' from NVS...");
    err = nvs_get_str(settings_handle, "password", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->password = malloc(str_size);
            atomic_fetch_add(&malloc_count_settings, 1);
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

    ESP_LOGI(TAG, "Reading 'weight_tare' from NVS...");
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
    
    ESP_LOGI(TAG, "Reading 'weight_scale' from NVS...");
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

    ESP_LOGI(TAG, "Reading 'weight_gain' from NVS...");
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

    ESP_LOGI(TAG, "Reading 'wifi_ssid' from NVS...");
    err = nvs_get_str(settings_handle, "wifi_ssid", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->wifi_ssid = malloc(str_size);
            atomic_fetch_add(&malloc_count_settings, 1);
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

    ESP_LOGI(TAG, "Reading 'wifi_password' from NVS...");
    err = nvs_get_str(settings_handle, "wifi_password", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->wifi_password = malloc(str_size);
            atomic_fetch_add(&malloc_count_settings, 1);
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

    ESP_LOGI(TAG, "Reading 'wifi_ap_fallback_disable' from NVS...");
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

    ESP_LOGI(TAG, "Reading 'temp_use_fahrenheit' from NVS...");
    uint8_t temp_use_f_value;
    err = nvs_get_u8(settings_handle, "temp_use_f", &temp_use_f_value);
    switch (err) {
        case ESP_OK:
            settings->temp_use_fahrenheit = temp_use_f_value != 0;
            ESP_LOGI(TAG, "Read 'temp_use_fahrenheit' = %d", settings->temp_use_fahrenheit);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->temp_use_fahrenheit = false;  // Default to Celsius
            ESP_LOGI(TAG, "No value for 'temp_use_fahrenheit'; using default = %d (Celsius)", settings->temp_use_fahrenheit);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading temp_use_fahrenheit!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'hostname' from NVS...");
    err = nvs_get_str(settings_handle, "hostname", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->hostname = malloc(str_size);
            atomic_fetch_add(&malloc_count_settings, 1);
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

    ESP_LOGI(TAG, "Reading 'timezone' from NVS...");
    err = nvs_get_str(settings_handle, "timezone", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->timezone = malloc(str_size);
            atomic_fetch_add(&malloc_count_settings, 1);
            if (settings->timezone == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for timezone");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(settings_handle, "timezone", settings->timezone, &str_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading timezone!", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "Read 'timezone' = '%s'", settings->timezone);
            // Apply timezone using setenv and tzset
            setenv("TZ", settings->timezone, 1);
            tzset();
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->timezone = strdup("UTC0");
            ESP_LOGI(TAG, "No value for 'timezone'; using default = '%s'", settings->timezone);
            // Apply default timezone
            setenv("TZ", settings->timezone, 1);
            tzset();
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading timezone!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'bthome_obj_ids' from NVS...");
    size_t blob_size = 0;
    err = nvs_get_blob(settings_handle, "bthome_obj_ids", NULL, &blob_size);
    switch (err) {
        case ESP_OK:
            settings->selected_bthome_object_ids = malloc(blob_size);
            atomic_fetch_add(&malloc_count_settings, 1);
            if (settings->selected_bthome_object_ids == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for selected_bthome_object_ids");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_blob(settings_handle, "bthome_obj_ids", settings->selected_bthome_object_ids, &blob_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading bthome_obj_ids!", esp_err_to_name(err));
                free(settings->selected_bthome_object_ids);
                atomic_fetch_add(&free_count_settings, 1);
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

    ESP_LOGI(TAG, "Reading 'ds18b20_gpio' from NVS...");
    int8_t ds18b20_gpio_value;
    err = nvs_get_i8(settings_handle, "ds18b20_gpio", &ds18b20_gpio_value);
    switch (err) {
        case ESP_OK:
            settings->ds18b20_gpio = ds18b20_gpio_value;
            ESP_LOGI(TAG, "Read 'ds18b20_gpio' = %d", settings->ds18b20_gpio);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->ds18b20_gpio = -1;  // Disabled by default
            ESP_LOGI(TAG, "No value for 'ds18b20_gpio'; using default = %d (disabled)", settings->ds18b20_gpio);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading ds18b20_gpio!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'ds18b20_pwr_gpio' from NVS...");
    int8_t ds18b20_power_gpio_value;
    err = nvs_get_i8(settings_handle, "ds18b20_pwr", &ds18b20_power_gpio_value);
    switch (err) {
        case ESP_OK:
            settings->ds18b20_pwr_gpio = ds18b20_power_gpio_value;
            ESP_LOGI(TAG, "Read 'ds18b20_pwr_gpio' = %d", settings->ds18b20_pwr_gpio);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->ds18b20_pwr_gpio = -1;  // Disabled by default
            ESP_LOGI(TAG, "No value for 'ds18b20_pwr_gpio'; using default = %d (disabled)", settings->ds18b20_pwr_gpio);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading ds18b20_pwr_gpio!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'weight_dt_gpio' from NVS...");
    int8_t weight_dt_gpio_value;
    err = nvs_get_i8(settings_handle, "weight_dt_gpio", &weight_dt_gpio_value);
    switch (err) {
        case ESP_OK:
            settings->weight_dt_gpio = weight_dt_gpio_value;
            ESP_LOGI(TAG, "Read 'weight_dt_gpio' = %d", settings->weight_dt_gpio);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->weight_dt_gpio = -1;  // Disabled by default
            ESP_LOGI(TAG, "No value for 'weight_dt_gpio'; using default = %d (disabled)", settings->weight_dt_gpio);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading weight_dt_gpio!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'weight_sck_gpio' from NVS...");
    int8_t weight_sck_gpio_value;
    err = nvs_get_i8(settings_handle, "weight_sck_gpio", &weight_sck_gpio_value);
    switch (err) {
        case ESP_OK:
            settings->weight_sck_gpio = weight_sck_gpio_value;
            ESP_LOGI(TAG, "Read 'weight_sck_gpio' = %d", settings->weight_sck_gpio);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->weight_sck_gpio = -1;  // Disabled by default
            ESP_LOGI(TAG, "No value for 'weight_sck_gpio'; using default = %d (disabled)", settings->weight_sck_gpio);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading weight_sck_gpio!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'pump_scl_gpio' from NVS...");
    int8_t pump_scl_gpio_value;
    err = nvs_get_i8(settings_handle, "pump_scl_gpio", &pump_scl_gpio_value);
    switch (err) {
        case ESP_OK:
            settings->pump_scl_gpio = pump_scl_gpio_value;
            ESP_LOGI(TAG, "Read 'pump_scl_gpio' = %d", settings->pump_scl_gpio);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->pump_scl_gpio = -1;  // Disabled by default
            ESP_LOGI(TAG, "No value for 'pump_scl_gpio'; using default = %d (disabled)", settings->pump_scl_gpio);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading pump_scl_gpio!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'pump_sda_gpio' from NVS...");
    int8_t pump_sda_gpio_value;
    err = nvs_get_i8(settings_handle, "pump_sda_gpio", &pump_sda_gpio_value);
    switch (err) {
        case ESP_OK:
            settings->pump_sda_gpio = pump_sda_gpio_value;
            ESP_LOGI(TAG, "Read 'pump_sda_gpio' = %d", settings->pump_sda_gpio);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->pump_sda_gpio = -1;  // Disabled by default
            ESP_LOGI(TAG, "No value for 'pump_sda_gpio'; using default = %d (disabled)", settings->pump_sda_gpio);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading pump_sda_gpio!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'pump_i2c_addr' from NVS...");
    int8_t pump_i2c_addr_value;
    err = nvs_get_i8(settings_handle, "pump_i2c_addr", &pump_i2c_addr_value);
    switch (err) {
        case ESP_OK:
            settings->pump_i2c_addr = pump_i2c_addr_value;
            ESP_LOGI(TAG, "Read 'pump_i2c_addr' = 0x%02X", settings->pump_i2c_addr);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->pump_i2c_addr = 0x37;  // Default I2C address
            ESP_LOGI(TAG, "No value for 'pump_i2c_addr'; using default = 0x%02X", settings->pump_i2c_addr);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading pump_i2c_addr!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'pump_dispense_ml' from NVS...");
    int16_t pump_dispense_ml_value;
    err = nvs_get_i16(settings_handle, "pump_disp_ml", &pump_dispense_ml_value);
    switch (err) {
        case ESP_OK:
            settings->pump_dispense_ml = pump_dispense_ml_value;
            ESP_LOGI(TAG, "Read 'pump_dispense_ml' = %d", settings->pump_dispense_ml);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->pump_dispense_ml = CONFIG_PUMP_DEFAULT_DISPENSE_ML;
            ESP_LOGI(TAG, "No value for 'pump_dispense_ml'; using default = %d", settings->pump_dispense_ml);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading pump_dispense_ml!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'mac_filters' from NVS...");
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
            atomic_fetch_add(&malloc_count_settings, 1);
            if (settings->mac_filters == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for mac_filters");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_blob(settings_handle, "mac_filters", settings->mac_filters, &blob_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading mac_filters!", esp_err_to_name(err));
                free(settings->mac_filters);
                atomic_fetch_add(&free_count_settings, 1);
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

    ESP_LOGI(TAG, "Reading 'ds18b20_names' from NVS...");
    blob_size = 0;
    err = nvs_get_blob(settings_handle, "ds18b20_names", NULL, &blob_size);
    switch (err) {
        case ESP_OK:
            if (blob_size % sizeof(ds18b20_name_t) != 0) {
                ESP_LOGE(TAG, "Invalid ds18b20_names blob size: %zu", blob_size);
                break;
            }
            settings->ds18b20_names_count = blob_size / sizeof(ds18b20_name_t);
            settings->ds18b20_names = malloc(blob_size);
            atomic_fetch_add(&malloc_count_settings, 1);
            if (settings->ds18b20_names == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for ds18b20_names");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_blob(settings_handle, "ds18b20_names", settings->ds18b20_names, &blob_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading ds18b20_names!", esp_err_to_name(err));
                free(settings->ds18b20_names);
                atomic_fetch_add(&free_count_settings, 1);
                settings->ds18b20_names = NULL;
                settings->ds18b20_names_count = 0;
                return err;
            }
            ESP_LOGI(TAG, "Read 'ds18b20_names' - %zu names", settings->ds18b20_names_count);
            for (size_t i = 0; i < settings->ds18b20_names_count; i++) {
                ESP_LOGI(TAG, "  Name[%zu]: address=%016llX, name='%s'",
                         i,
                         settings->ds18b20_names[i].address,
                         settings->ds18b20_names[i].name);
            }
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->ds18b20_names = NULL;
            settings->ds18b20_names_count = 0;
            ESP_LOGI(TAG, "No value for 'ds18b20_names'; using empty list");
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading ds18b20_names!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'syslog_server' from NVS...");
    err = nvs_get_str(settings_handle, "syslog_server", NULL, &str_size);
    switch (err) {
        case ESP_OK:
            settings->syslog_server = malloc(str_size);
            atomic_fetch_add(&malloc_count_settings, 1);
            if (settings->syslog_server == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for syslog_server");
                return ESP_ERR_NO_MEM;
            }
            err = nvs_get_str(settings_handle, "syslog_server", settings->syslog_server, &str_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) reading syslog_server!", esp_err_to_name(err));
                return err;
            }
            ESP_LOGI(TAG, "Read 'syslog_server' = '%s'", settings->syslog_server);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->syslog_server = strdup("");
            ESP_LOGI(TAG, "No value for 'syslog_server'; using default = ''");
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading syslog_server!", esp_err_to_name(err));
            return err;
    }

    ESP_LOGI(TAG, "Reading 'syslog_port' from NVS...");
    uint16_t syslog_port_value;
    err = nvs_get_u16(settings_handle, "syslog_port", &syslog_port_value);
    switch (err) {
        case ESP_OK:
            settings->syslog_port = syslog_port_value;
            ESP_LOGI(TAG, "Read 'syslog_port' = %u", settings->syslog_port);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            settings->syslog_port = 514;  // Default syslog port
            ESP_LOGI(TAG, "No value for 'syslog_port'; using default = %u", settings->syslog_port);
            break;
        default:
            ESP_LOGE(TAG, "Error (%s) reading syslog_port!", esp_err_to_name(err));
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
    err = httpd_register_uri_handler_with_basic_auth(settings, http_server, &reboot_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering reboot POST handler!", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

const char* settings_get_ds18b20_name(settings_t *settings, uint64_t address) {
    if (settings == NULL || settings->ds18b20_names == NULL) {
        return NULL;
    }
    
    for (size_t i = 0; i < settings->ds18b20_names_count; i++) {
        if (settings->ds18b20_names[i].address == address) {
            return settings->ds18b20_names[i].name;
        }
    }
    
    return NULL;
}

