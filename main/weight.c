
#include <inttypes.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hx711.h>
#include <string.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include "IQmathLib.h"

#include "weight.h"
#include "settings.h"
#include "http_server.h"

static const char *TAG = "hx711";

// Global variable to store the latest weight reading
static int32_t g_latest_weight_raw = 0;
static float g_latest_weight_grams = 0;
static bool g_weight_available = false;

static void weight(void *pvParameters)
{
    settings_t *settings = (settings_t *)pvParameters;
    hx711_t dev =
    {
        .dout = CONFIG_WEIGHT_DOUT_GPIO,
        .pd_sck = CONFIG_WEIGHT_PD_SCK_GPIO,
        .gain = HX711_GAIN_A_64
    };

    // initialize device
    ESP_ERROR_CHECK(hx711_init(&dev));

    // read from device
    while (1)
    {
        esp_err_t r = hx711_wait(&dev, 500);
        if (r != ESP_OK)
        {
            ESP_LOGE(TAG, "Device not found: %d (%s)\n", r, esp_err_to_name(r));
            continue;
        }

        // Read multiple samples and calculate median
        int32_t readings[CONFIG_WEIGHT_SAMPLE_TIMES];
        bool read_success = true;
        
        for (int i = 0; i < CONFIG_WEIGHT_SAMPLE_TIMES; i++)
        {
            ESP_ERROR_CHECK(hx711_wait(&dev, 200));
            r = hx711_read_data(&dev, &readings[i]);
            if (r != ESP_OK)
            {
                ESP_LOGE(TAG, "Could not read data: %d (%s)\n", r, esp_err_to_name(r));
                read_success = false;
                break;
            }
        }
        
        if (!read_success)
        {
            continue;
        }
        
        // Sort readings to find median
        for (int i = 0; i < CONFIG_WEIGHT_SAMPLE_TIMES - 1; i++)
        {
            for (int j = 0; j < CONFIG_WEIGHT_SAMPLE_TIMES - i - 1; j++)
            {
                if (readings[j] > readings[j + 1])
                {
                    int32_t temp = readings[j];
                    readings[j] = readings[j + 1];
                    readings[j + 1] = temp;
                }
            }
        }
        
        // Get median value
        int32_t data;
        if (CONFIG_WEIGHT_SAMPLE_TIMES % 2 == 0)
        {
            // Even number of samples - average the two middle values
            data = (readings[CONFIG_WEIGHT_SAMPLE_TIMES / 2 - 1] + readings[CONFIG_WEIGHT_SAMPLE_TIMES / 2]) / 2;
        }
        else
        {
            // Odd number of samples - take the middle value
            data = readings[CONFIG_WEIGHT_SAMPLE_TIMES / 2];
        }

        ESP_LOGI(TAG, "Raw data: %" PRIi32, data);

        // Store the latest weight reading
        g_latest_weight_raw = data;
        // Convert raw int32_t to float, multiply by scale (float), subtract tare
        float data_float = (float)(g_latest_weight_raw - settings->weight_tare);
        g_latest_weight_grams = data_float * _IQ16toF(settings->weight_scale);
        g_weight_available = true;

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static const char *weight_display_html = ""
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Weight Station</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; text-align: center; }"
    "h1 { color: #333; }"
    ".weight-display { background: #f4f4f4; padding: 40px 20px; border-radius: 8px; margin: 20px 0; }"
    ".weight-value { font-size: 72px; font-weight: bold; color: #4CAF50; margin: 20px 0; }"
    ".weight-label { font-size: 24px; color: #666; }"
    ".raw-value { font-size: 16px; font-weight: 300; color: #999; margin-top: 10px; }"
    ".status { padding: 10px; margin: 10px 0; border-radius: 4px; }"
    ".status.active { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }"
    ".status.inactive { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }"
    ".button { display: inline-block; margin: 10px 5px; padding: 12px 24px; background: #4CAF50; color: white; text-decoration: none; font-size: 18px; border: none; border-radius: 4px; cursor: pointer; }"
    ".button:hover { background: #45a049; }"
    ".button:disabled { background: #ccc; cursor: not-allowed; }"
    "a { display: inline-block; margin-top: 20px; color: #4CAF50; text-decoration: none; font-size: 18px; }"
    "a:hover { text-decoration: underline; }"
    "</style>"
    "</head>"
    "<body>"
    "<h1>Weight Station</h1>"
    "<div class='weight-display'>"
    "<div class='weight-label'>Current Weight</div>"
    "<div class='weight-value' id='weight'>--</div>"
    "<div class='weight-label'>grams (<span id='weight-lbs'>--</span> lbs)</div>"
    "<div class='raw-value' id='raw-value'>Raw: --</div>"
    "</div>"
    "<div id='status' class='status inactive'>No data available</div>"
    "<button id='tare-btn' class='button' onclick='tare()' disabled>Tare</button><br>"
    "<a href='/settings'>Settings</a>"
    "<script>"
    "let currentRaw = 0;"
    "function updateWeight() {"
    "  fetch('/weight/data')"
    "    .then(response => response.json())"
    "    .then(data => {"
    "      if (data.available) {"
    "        currentRaw = data.raw;"
    "        document.getElementById('weight').textContent = data.weight.toLocaleString();"
    "        const lbs = (data.weight / 453.59237).toFixed(2);"
    "        document.getElementById('weight-lbs').textContent = lbs;"
    "        document.getElementById('raw-value').textContent = 'Raw: ' + data.raw.toLocaleString();"
    "        document.getElementById('status').textContent = 'Active';"
    "        document.getElementById('status').className = 'status active';"
    "        document.getElementById('tare-btn').disabled = false;"
    "      } else {"
    "        document.getElementById('weight').textContent = '--';"
    "        document.getElementById('weight-lbs').textContent = '--';"
    "        document.getElementById('raw-value').textContent = 'Raw: --';"
    "        document.getElementById('status').textContent = 'No data available';"
    "        document.getElementById('status').className = 'status inactive';"
    "        document.getElementById('tare-btn').disabled = true;"
    "      }"
    "    })"
    "    .catch(error => {"
    "      document.getElementById('status').textContent = 'Error: ' + error;"
    "      document.getElementById('status').className = 'status inactive';"
    "      document.getElementById('tare-btn').disabled = true;"
    "    });"
    "}"
    "function tare() {"
    "  const btn = document.getElementById('tare-btn');"
    "  btn.disabled = true;"
    "  btn.textContent = 'Taring...';"
    "  fetch('/settings?weight_tare=' + currentRaw, {"
    "    method: 'POST'"
    "  })"
    "  .then(response => {"
    "    if (response.ok) {"
    "      document.getElementById('status').textContent = 'Tare set successfully';"
    "      document.getElementById('status').className = 'status active';"
    "      setTimeout(() => updateWeight(), 500);"
    "    } else {"
    "      throw new Error('Failed to set tare');"
    "    }"
    "    btn.textContent = 'Tare';"
    "    btn.disabled = false;"
    "  })"
    "  .catch(error => {"
    "    document.getElementById('status').textContent = 'Error setting tare: ' + error;"
    "    document.getElementById('status').className = 'status inactive';"
    "    btn.textContent = 'Tare';"
    "    btn.disabled = false;"
    "  });"
    "}"
    "updateWeight();"
    "setInterval(updateWeight, 500);"
    "</script>"
    "<footer style='margin-top: 40px; padding-top: 20px; border-top: 1px solid #ddd; text-align: center; color: #999; font-size: 12px;'>"
    "<div id='version'>Loading version...</div>"
    "</footer>"
    "<script>"
    "fetch('/version')"
    "  .then(response => response.json())"
    "  .then(data => {"
    "    document.getElementById('version').innerHTML = "
    "      'Firmware: ' + data.version + '<br>Hash: ' + data.hash;"
    "  })"
    "  .catch(() => {"
    "    document.getElementById('version').textContent = 'Version info unavailable';"
    "  });"
    "</script>"
    "</body>"
    "</html>";

static esp_err_t weight_display_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    const char *hostname = (settings->hostname != NULL && settings->hostname[0] != '\0') 
                            ? settings->hostname : "unknown";
    
    // Create a buffer for the complete HTML with hostname
    char *html_with_hostname = malloc(strlen(weight_display_html) + strlen(hostname) + 32);
    if (html_with_hostname == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Replace "Weight Station</h1>" with "Weight Station: hostname</h1>"
    const char *marker = "Weight Station</h1>";
    const char *pos = strstr(weight_display_html, marker);
    if (pos != NULL) {
        size_t before_len = pos - weight_display_html;
        strcpy(html_with_hostname, weight_display_html);
        html_with_hostname[before_len] = '\0';
        strcat(html_with_hostname, "Weight Station: ");
        strcat(html_with_hostname, hostname);
        strcat(html_with_hostname, "</h1>");
        strcat(html_with_hostname, pos + strlen(marker));
    } else {
        strcpy(html_with_hostname, weight_display_html);
    }
    
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, html_with_hostname, strlen(html_with_hostname));
    free(html_with_hostname);
    return ESP_OK;
}

static esp_err_t weight_data_handler(httpd_req_t *req) {
    char json_buf[128];
    
    if (g_weight_available) {
        snprintf(json_buf, sizeof(json_buf), 
                "{\"available\":true,\"weight\":%.2f,\"raw\":%" PRIi32 "}", 
                g_latest_weight_grams, g_latest_weight_raw);
    } else {
        snprintf(json_buf, sizeof(json_buf), "{\"available\":false}");
    }
    
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, json_buf, strlen(json_buf));
    return ESP_OK;
}

static esp_err_t version_handler(httpd_req_t *req) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char json_buf[256];
    
    // Format the hash as a hex string (first 8 bytes for brevity)
    char hash_str[17];
    for (int i = 0; i < 8; i++) {
        sprintf(&hash_str[i * 2], "%02x", app_desc->app_elf_sha256[i]);
    }
    hash_str[16] = '\0';
    
    snprintf(json_buf, sizeof(json_buf), 
            "{\"version\":\"%s\",\"hash\":\"%s\",\"date\":\"%s\",\"time\":\"%s\"}",
            app_desc->version, hash_str, app_desc->date, app_desc->time);
    
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, json_buf, strlen(json_buf));
    return ESP_OK;
}

static httpd_uri_t weight_display_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = weight_display_handler,
    .user_ctx  = NULL
};

static httpd_uri_t weight_data_uri = {
    .uri       = "/weight/data",
    .method    = HTTP_GET,
    .handler   = weight_data_handler,
    .user_ctx  = NULL
};

static httpd_uri_t version_uri = {
    .uri       = "/version",
    .method    = HTTP_GET,
    .handler   = version_handler,
    .user_ctx  = NULL
};

float weight_get_latest(bool *available) {
    if (available) {
        *available = g_weight_available;
    }
    return g_latest_weight_grams;
}

uint32_t weight_get_latest_raw(bool *available) {
    if (available) {
        *available = g_weight_available;
    }
    return g_latest_weight_raw;
}

void weight_init(settings_t *settings, httpd_handle_t server)
{
    // Set user_ctx to settings so handlers can access hostname
    weight_display_uri.user_ctx = settings;
    
    // Register HTTP handlers
    esp_err_t err = httpd_register_uri_handler(server, &weight_display_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering weight display handler!", esp_err_to_name(err));
    }
    
    err = httpd_register_uri_handler(server, &weight_data_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering weight data handler!", esp_err_to_name(err));
    }
    
    err = httpd_register_uri_handler(server, &version_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering version handler!", esp_err_to_name(err));
    }
    
    // Start the weight reading task
    xTaskCreate(weight, "weight", configMINIMAL_STACK_SIZE * 5, settings, 5, NULL);
}