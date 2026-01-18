#include "sensors.h"
#include "settings.h"
#include "metrics.h"
#include "mqtt_publisher.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

static const char *TAG = "sensors";

// Sensor registry
static sensor_data_t sensors[MAX_SENSORS];
static int sensor_count = 0;
static SemaphoreHandle_t sensors_mutex = NULL;

#define SENSOR_STALE_TIMEOUT_SECONDS 600  // 10 minutes

static const char *sensors_display_html = ""
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<title>Sensor Station</title>\n"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
    "<style>\n"
    "body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; text-align: center; }\n"
    "h1 { color: #333; }\n"
    ".sensors-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; margin: 20px 0; }\n"
    ".sensor-card { background: #f4f4f4; padding: 20px; border-radius: 8px; }\n"
    ".sensor-name { font-size: 18px; color: #666; margin-bottom: 10px; }\n"
    ".sensor-value { font-size: 48px; font-weight: bold; color: #4CAF50; margin: 10px 0; word-wrap: break-word; }\n"
    ".sensor-unit { font-size: 20px; color: #666; }\n"
    ".sensor-updated { font-size: 12px; color: #999; margin-top: 10px; }\n"
    ".status { padding: 10px; margin: 10px 0; border-radius: 4px; }\n"
    ".status.active { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }\n"
    ".status.inactive { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }\n"
    ".unavailable { opacity: 0.5; }\n"
    ".unavailable .sensor-value { color: #999; }\n"
    "a { display: inline-block; margin: 10px 10px; color: #4CAF50; text-decoration: none; font-size: 18px; }\n"
    "a:hover { text-decoration: underline; }\n"
    ".sensor-action { margin-top: 10px; }\n"
    ".sensor-action button { background: #4CAF50; color: white; border: none; padding: 8px 16px; border-radius: 4px; cursor: pointer; font-size: 14px; }\n"
    ".sensor-action button:hover { background: #45a049; }\n"
    ".sensor-action button:disabled { background: #ccc; cursor: not-allowed; }\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>Sensor Station</h1>\n"
    "<div id='sensors-container' class='sensors-grid'></div>\n"
    "<div id='status' class='status inactive'>Loading...</div>\n"
    "<a href='/settings'>Settings</a> | <a href='/bthome/packets'>BTHome Packets</a>\n"
    "<script>\n"
    "function formatTimeAgo(timestamp) {\n"
    "  if (!timestamp || timestamp === 0) return 'Never';\n"
    "  const now = Math.floor(Date.now() / 1000);\n"
    "  const diff = now - timestamp;\n"
    "  if (diff < 60) return diff + 's ago';\n"
    "  if (diff < 3600) return Math.floor(diff / 60) + 'm ago';\n"
    "  if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';\n"
    "  return Math.floor(diff / 86400) + 'd ago';\n"
    "}\n"
    "function updateSensors() {\n"
    "  fetch('/sensors/data')\n"
    "    .then(response => response.json())\n"
    "    .then(data => {\n"
    "      const container = document.getElementById('sensors-container');\n"
    "      if (data.sensors && data.sensors.length > 0) {\n"
    "        container.innerHTML = data.sensors.map(sensor => {\n"
    "          const availClass = sensor.available ? '' : 'unavailable';\n"
    "          const value = sensor.available ? sensor.value.toLocaleString(undefined, {maximumFractionDigits: 2}) : '--';\n"
    "          const updated = formatTimeAgo(sensor.last_updated);\n"
    "          const actionBtn = (sensor.link_url && sensor.link_text) ? \n"
    "            `<div class='sensor-action'><button onclick='sensorAction(\"${sensor.link_url}\")' ${sensor.available ? '' : 'disabled'}>${sensor.link_text}</button></div>` : '';\n"
    "          return `\n"
    "            <div class='sensor-card ${availClass}'>\n"
    "              <div class='sensor-name'>${sensor.name}</div>\n"
    "              <div class='sensor-value'>${value}</div>\n"
    "              <div class='sensor-unit'>${sensor.unit}</div>\n"
    "              <div class='sensor-updated'>${updated}</div>\n"
    "              ${actionBtn}\n"
    "            </div>\n"
    "          `;\n"
    "        }).join('');\n"
    "        document.getElementById('status').textContent = 'Active';\n"
    "        document.getElementById('status').className = 'status active';\n"
    "      } else {\n"
    "        container.innerHTML = '<p style=\"grid-column: 1/-1; color: #999;\">No sensors registered</p>';\n"
    "        document.getElementById('status').textContent = 'No sensors available';\n"
    "        document.getElementById('status').className = 'status inactive';\n"
    "      }\n"
    "    })\n"
    "    .catch(error => {\n"
    "      document.getElementById('status').textContent = 'Error: ' + error;\n"
    "      document.getElementById('status').className = 'status inactive';\n"
    "    });\n"
    "}\n"
    "function sensorAction(url) {\n"
    "  fetch(url, {method: 'POST'})\n"
    "    .then(response => {\n"
    "      if (response.ok) updateSensors();\n"
    "      else alert('Action failed');\n"
    "    })\n"
    "    .catch(error => alert('Action error: ' + error));\n"
    "}\n"
    "updateSensors();\n"
    "setInterval(updateSensors, 1000);\n"
    "</script>\n"
    "<footer style='margin-top: 40px; padding-top: 20px; border-top: 1px solid #ddd; text-align: center; color: #999; font-size: 12px;'>\n"
    "<div id='version'>Loading version...</div>\n"
    "</footer>\n"
    "<script>\n"
    "fetch('/version')\n"
    "  .then(response => response.json())\n"
    "  .then(data => {\n"
    "    document.getElementById('version').innerHTML = \n"
    "      'Firmware: ' + data.version + '<br>Hash: ' + data.hash;\n"
    "  })\n"
    "  .catch(() => {\n"
    "    document.getElementById('version').textContent = 'Version info unavailable';\n"
    "  });\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t sensors_display_handler(httpd_req_t *req) {
    settings_t *settings = (settings_t *)req->user_ctx;
    const char *hostname = (settings->hostname != NULL && settings->hostname[0] != '\0') 
                            ? settings->hostname : "unknown";
    
    // Create a buffer for the complete HTML with hostname
    char *html_with_hostname = malloc(strlen(sensors_display_html) + strlen(hostname) + 32);
    atomic_fetch_add(&malloc_count_sensors, 1);
    if (html_with_hostname == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // Replace "Sensor Station</h1>" with "Sensor Station: hostname</h1>"
    const char *marker = "Sensor Station</h1>";
    const char *pos = strstr(sensors_display_html, marker);
    if (pos != NULL) {
        size_t before_len = pos - sensors_display_html;
        strcpy(html_with_hostname, sensors_display_html);
        html_with_hostname[before_len] = '\0';
        strcat(html_with_hostname, "Sensor Station: ");
        strcat(html_with_hostname, hostname);
        strcat(html_with_hostname, "</h1>");
        strcat(html_with_hostname, pos + strlen(marker));
    } else {
        strcpy(html_with_hostname, sensors_display_html);
    }
    
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, html_with_hostname, strlen(html_with_hostname));
    free(html_with_hostname);
    atomic_fetch_add(&free_count_sensors, 1);
    return ESP_OK;
}

static esp_err_t sensors_data_handler(httpd_req_t *req) {
    // Build JSON response with all sensors
    char *json_buf = malloc(2048); // Allocate buffer for JSON
    atomic_fetch_add(&malloc_count_sensors, 1);
    if (json_buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    if (sensors_mutex != NULL) {
        xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    }
    
    int pos = snprintf(json_buf, 2048, "{\"sensors\":[");
    
    for (int i = 0; i < sensor_count && pos < 2000; i++) {
        if (sensors[i].display_name[0] == '\0' || sensors[i].unit[0] == '\0') {
            continue;
        }
        if (i > 0) {
            pos += snprintf(json_buf + pos, 2048 - pos, ",");
        }
        
        // Build JSON object for this sensor
        char sensor_json[512];
        int spos = snprintf(sensor_json, sizeof(sensor_json),
                       "{\"name\":\"%s\",\"unit\":\"%s\",\"value\":%.2f,\"last_updated\":%" PRId64 ",\"available\":%s",
                       sensors[i].display_name,
                       sensors[i].unit,
                       sensors[i].value,
                       (int64_t)sensors[i].last_updated,
                       sensors[i].available ? "true" : "false");
        
        // Add optional link fields if present
        if (sensors[i].link_url[0] != '\0' && sensors[i].link_text[0] != '\0') {
            spos += snprintf(sensor_json + spos, sizeof(sensor_json) - spos,
                           ",\"link_url\":\"%s\",\"link_text\":\"%s\"",
                           sensors[i].link_url,
                           sensors[i].link_text);
        }
        
        spos += snprintf(sensor_json + spos, sizeof(sensor_json) - spos, "}");
        
        // Append to main buffer
        pos += snprintf(json_buf + pos, 2048 - pos, "%s", sensor_json);
    }
    
    pos += snprintf(json_buf + pos, 2048 - pos, "]}");
    
    if (sensors_mutex != NULL) {
        xSemaphoreGive(sensors_mutex);
    }
    
    httpd_resp_set_status(req, HTTPD_200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, json_buf, strlen(json_buf));
    free(json_buf);
    atomic_fetch_add(&free_count_sensors, 1);
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

static httpd_uri_t sensors_display_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = sensors_display_handler,
    .user_ctx  = NULL
};

static httpd_uri_t sensors_data_uri = {
    .uri       = "/sensors/data",
    .method    = HTTP_GET,
    .handler   = sensors_data_handler,
    .user_ctx  = NULL
};

static httpd_uri_t version_uri = {
    .uri       = "/version",
    .method    = HTTP_GET,
    .handler   = version_handler,
    .user_ctx  = NULL
};

int sensors_register(
    const char *display_name,
    const char *unit,
    const char *metric_name,
    const char *device_name, 
    const char *device_id) {
    if (sensors_mutex != NULL) {
        xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    }
    
    if (sensor_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "Cannot register sensor '%s': maximum number of sensors (%d) reached", 
                 display_name, MAX_SENSORS);
        if (sensors_mutex != NULL) {
            xSemaphoreGive(sensors_mutex);
        }
        return -1;
    }
    
    int id = sensor_count++;
    
    // Copy name, unit, and metric_name, ensuring null termination
    strncpy(sensors[id].display_name, display_name, SENSOR_DISPLAY_NAME_MAX_LEN - 1);
    sensors[id].display_name[SENSOR_DISPLAY_NAME_MAX_LEN - 1] = '\0';

    if (device_name && strlen(device_name) > 0) {
        strncpy(sensors[id].device_name, device_name, SENSOR_DEVICE_NAME_MAX_LEN - 1);
        sensors[id].device_name[SENSOR_DEVICE_NAME_MAX_LEN - 1] = '\0';
    } else {
        sensors[id].device_name[0] = '\0';
    }
    
    if (device_id && strlen(device_id) > 0) {
        strncpy(sensors[id].device_id, device_id, SENSOR_DEVICE_ID_MAX_LEN - 1);
        sensors[id].device_id[SENSOR_DEVICE_ID_MAX_LEN - 1] = '\0';
    } else {
        sensors[id].device_id[0] = '\0';
    }
    
    strncpy(sensors[id].unit, unit, SENSOR_UNIT_MAX_LEN - 1);
    sensors[id].unit[SENSOR_UNIT_MAX_LEN - 1] = '\0';
    
    if (metric_name && strlen(metric_name) > 0) {
        strncpy(sensors[id].metric_name, metric_name, SENSOR_DISPLAY_NAME_MAX_LEN - 1);
        sensors[id].metric_name[SENSOR_DISPLAY_NAME_MAX_LEN - 1] = '\0';
    } else {
        sensors[id].metric_name[0] = '\0';
    }
    
    sensors[id].value = 0.0f;
    sensors[id].last_updated = 0;
    sensors[id].available = false;
    sensors[id].link_url[0] = '\0';
    sensors[id].link_text[0] = '\0';
    
    ESP_LOGI(TAG, "Registered sensor %d: '%s' (%s) [metric: %s]", id, sensors[id].display_name, sensors[id].unit, sensors[id].metric_name);
    
    if (sensors_mutex != NULL) {
        xSemaphoreGive(sensors_mutex);
    }
    return id;
}

bool sensors_update(int sensor_id, float value, bool available) {
    return sensors_update_with_link(sensor_id, value, available, NULL, NULL);
}

bool sensors_update_with_link(int sensor_id, float value, bool available, const char *link_url, const char *link_text) {
    if (sensors_mutex != NULL) {
        xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    }
    
    if (sensor_id < 0 || sensor_id >= sensor_count) {
        ESP_LOGE(TAG, "Invalid sensor_id %d (valid range: 0-%d)", sensor_id, sensor_count - 1);
        if (sensors_mutex != NULL) {
            xSemaphoreGive(sensors_mutex);
        }
        return false;
    }
    
    sensors[sensor_id].value = value;
    sensors[sensor_id].available = available;
    sensors[sensor_id].last_updated = time(NULL);
    
    // Update link fields if provided
    if (link_url != NULL && link_text != NULL) {
        strncpy(sensors[sensor_id].link_url, link_url, sizeof(sensors[sensor_id].link_url) - 1);
        sensors[sensor_id].link_url[sizeof(sensors[sensor_id].link_url) - 1] = '\0';
        
        strncpy(sensors[sensor_id].link_text, link_text, sizeof(sensors[sensor_id].link_text) - 1);
        sensors[sensor_id].link_text[sizeof(sensors[sensor_id].link_text) - 1] = '\0';
    } else {
        // Clear link fields if not provided
        sensors[sensor_id].link_url[0] = '\0';
        sensors[sensor_id].link_text[0] = '\0';
    }
    
    if (sensors_mutex != NULL) {
        xSemaphoreGive(sensors_mutex);
    }
    
    // Publish to MQTT if enabled (don't hold mutex during MQTT publish)
    if (mqtt_is_enabled()) {
        mqtt_publish_sensors();
    }
    
    return true;
}

float sensors_get_value(int sensor_id, bool *available) {
    if (sensors_mutex != NULL) {
        xSemaphoreTake(sensors_mutex, portMAX_DELAY);
    }
    
    if (sensor_id < 0 || sensor_id >= sensor_count) {
        ESP_LOGE(TAG, "Invalid sensor_id %d (valid range: 0-%d)", sensor_id, sensor_count - 1);
        if (available) {
            *available = false;
        }
        if (sensors_mutex != NULL) {
            xSemaphoreGive(sensors_mutex);
        }
        return 0.0f;
    }
    
    if (available) {
        *available = sensors[sensor_id].available;
    }
    
    float value = sensors[sensor_id].value;
    
    if (sensors_mutex != NULL) {
        xSemaphoreGive(sensors_mutex);
    }
    return value;
}

int sensors_get_count(void) {
    return sensor_count;
}

const sensor_data_t* sensors_get_by_index(int index) {
    if (index < 0 || index >= sensor_count) {
        return NULL;
    }
    return &sensors[index];
}


// Cleanup task to mark stale sensors as unavailable
static void sensor_cleanup_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  // Check every minute
        
        if (sensors_mutex != NULL) {
            xSemaphoreTake(sensors_mutex, portMAX_DELAY);
        }
        
        time_t now = time(NULL);
        for (int i = 0; i < sensor_count; i++) {
            if (sensors[i].available && sensors[i].last_updated > 0) {
                time_t age = now - sensors[i].last_updated;
                if (age > SENSOR_STALE_TIMEOUT_SECONDS) {
                    ESP_LOGW(TAG, "Sensor %d (%s) is stale (%ld seconds old), marking unavailable",
                             i, sensors[i].display_name, (long)age);
                    sensors[i].available = false;
                }
            }
        }
        
        if (sensors_mutex != NULL) {
            xSemaphoreGive(sensors_mutex);
        }
    }
}

void sensors_init(settings_t *settings, httpd_handle_t server)
{
    // Initialize sensor array
    memset(sensors, 0, sizeof(sensors));
    sensor_count = 0;
    
    // Create mutex for thread safety
    sensors_mutex = xSemaphoreCreateMutex();
    if (sensors_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensors mutex");
    }
    
    // Start cleanup task
    xTaskCreate(sensor_cleanup_task, "sensor_cleanup", 2048, NULL, 5, NULL);
    
    // Set user_ctx to settings so handlers can access hostname
    sensors_display_uri.user_ctx = settings;
    
    // Register HTTP handlers
    esp_err_t err = httpd_register_uri_handler(server, &sensors_display_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering sensor display handler!", esp_err_to_name(err));
    }
    
    err = httpd_register_uri_handler(server, &sensors_data_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering sensor data handler!", esp_err_to_name(err));
    }
    
    err = httpd_register_uri_handler(server, &version_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) registering version handler!", esp_err_to_name(err));
    }
    
}