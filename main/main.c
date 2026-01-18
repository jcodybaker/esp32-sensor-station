/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "nvs_flash.h"
#include "wifi.h"
#include "sensors.h"
#include "ota.h"
#include "esp_event.h"
#include "settings.h"
#include "http_server.h"
#include "metrics.h"
#include "mqtt_publisher.h"
#include <esp_log.h>
#include "bthome_observer.h"
#include "weight.h"
#include "temperature.h"
#include "driver/i2c_master.h"
#include "pump.h"
#include "syslog.h"

bool g_ntp_initialized = false;

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    settings_t *settings = malloc(sizeof(settings_t));
    atomic_fetch_add(&malloc_count_main, 1);
    ESP_LOGI("main", "app_main settings ptr %p", settings);

    ESP_ERROR_CHECK(settings_init(settings));
    wifi_init(settings);
    syslog_init(settings);  // Initialize syslog after WiFi
    mqtt_publisher_init(settings);  // Initialize MQTT client after WiFi
    httpd_handle_t http_server = http_server_init();
    settings_register(settings, http_server);
    sensors_init(settings, http_server);
    init_ds18b20(settings);
    weight_init(settings);
    ota_init(settings, http_server);
    metrics_init(settings, http_server);
    bthome_observer_init(settings, http_server);
    pump_init(settings, http_server);
}
