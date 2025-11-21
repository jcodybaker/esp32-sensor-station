/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "nvs_flash.h"
#include "wifi.h"
#include "weight.h"
#include "ota.h"
#include "esp_event.h"
#include "settings.h"
#include "http_server.h"
#include <esp_log.h>

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

    wifi_init_sta();
    settings_t *settings = malloc(sizeof(settings_t));
    ESP_LOGI("main", "app_main settings ptr %p", settings);

    httpd_handle_t http_server = http_server_init();
    ESP_ERROR_CHECK(settings_init(settings, http_server));
    weight_init(settings, http_server);
    // ota_init();
}
