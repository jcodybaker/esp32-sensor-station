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
#include "ha_discovery.h"
#include <esp_log.h>
#include "bthome_observer.h"
#include "weight.h"
#include "temperature.h"
#include "driver/i2c_master.h"
#include "pump.h"
#include "ezo_ph.h"
#include "syslog.h"
#include "rcwl9620.h"
#include "a02yyuw.h"
#include "flow_sensor.h"
#include "m5stick_display.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif

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
    
    // Check if OTA update is pending
    bool ota_mode = (ota_check_pending_update(settings) == ESP_OK);

#if CONFIG_BT_ENABLED
    // Reclaim the BT controller + Bluedroid BSS (~40KB, much of it a single
    // contiguous block) whenever Bluetooth won't be used this boot:
    //   - an OTA-mode boot never touches BT (bthome_observer_init is skipped), and
    //   - in normal mode BTHome is the only BT consumer, so a disabled BTHome
    //     means nothing will init the controller.
    // This gives mbedtls/esp-tls room for its large handshake allocations (the
    // MQTT and syslog TLS sessions). Enabling BTHome sets restart_needed, so the
    // next boot skips this branch and BT initialises normally.
    if (ota_mode || !settings->bthome_enabled) {
        esp_bt_mem_release(ESP_BT_MODE_BTDM);
    }
#endif

    wifi_init(settings);
    syslog_init(settings);  // Initialize syslog after WiFi
    
    // Only initialize MQTT and sensors if NOT in OTA mode
    if (!ota_mode) {
        ha_discovery_init(settings);    // Compute HA node id / availability topic before MQTT LWT setup
        mqtt_publisher_init(settings);  // Initialize MQTT client after WiFi
    }
    
    httpd_handle_t http_server = http_server_init();
    settings_register(settings, http_server);
    
    // Only initialize sensors if NOT in OTA mode
    if (!ota_mode) {
        // Power on the display / Grove-port power rail before any sensor
        // driver that might depend on it (e.g. a Grove-wired RCWL-9620).
        m5stick_display_power_init();

        sensors_init(settings, http_server);

        // Create the display task now, before the heavier drivers below
        // (BLE for BTHome in particular) claim most of free heap. On the
        // goldfish-reservoir unit, free heap bottomed out at 404 bytes
        // during boot and xTaskCreatePinnedToCore() for the display task
        // intermittently failed outright, leaving the backlight on with
        // nothing ever drawn to the screen.
        m5stick_display_init(settings);

        init_ds18b20(settings);
        weight_init(settings);
        bthome_observer_init(settings, http_server);
        pump_init(settings, http_server);
        ezo_ph_init(settings, http_server);
        rcwl9620_init(settings);
        a02yyuw_init(settings);
        flow_sensor_init(settings);
    }
    
    ota_init(settings, http_server, ota_mode);

    // Prometheus scrapes would otherwise land mid-download, allocating a
    // response buffer in competition with the OTA TLS session.
    if (!ota_mode) {
        metrics_init(settings, http_server);
    }
}
