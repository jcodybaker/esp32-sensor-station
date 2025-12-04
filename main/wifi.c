/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "wifi.h"


#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOSConfig.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "lwip/ip_addr.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif


/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi station";
static const char *TAG_AP = "wifi ap";

static char ap_ssid[32];

static int s_retry_num = 0;
static bool sta_configured = false;
static bool ap_active = false;
static bool ap_configured = false;
extern bool g_ntp_initialized;

void ntp_configure(void);

/* Initialize soft AP */
void wifi_configure_softap(void)
{
    if (ap_configured) {
        return;
    }
    uint8_t ap_mac[6];
    esp_err_t err;
    esp_netif_create_default_wifi_ap();
    err = esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP mac address");
        return;
    }

    snprintf(ap_ssid, sizeof(ap_ssid), "%s_%02hhX%02hhX", CONFIG_ESP_WIFI_AP_SSID_PREFIX, ap_mac[4], ap_mac[5]);

    ESP_LOGI(TAG_AP, "wifi_configure_softap configuring. SSID: %s password: '' channel:%d",
             ap_ssid, CONFIG_ESP_WIFI_AP_CHANNEL);
    wifi_config_t wifi_ap_config = {
        .ap = {{0}}
    };
    memcpy(wifi_ap_config.ap.ssid, ap_ssid, sizeof(ap_ssid));
    wifi_ap_config.ap.ssid_len = strlen(ap_ssid);
    wifi_ap_config.ap.password[0] = '\0'; // No password
    wifi_ap_config.ap.channel = CONFIG_ESP_WIFI_AP_CHANNEL;
    wifi_ap_config.ap.max_connection = CONFIG_ESP_MAX_STA_CONN_AP;
    wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_ap_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI(TAG_AP, "wifi_configure_softap finished. SSID:%s password:'' channel:%d",
             ap_ssid, CONFIG_ESP_WIFI_AP_CHANNEL);

    ap_configured = true;
    return;
}


/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    settings_t *settings = (settings_t *)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
            ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                    MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
            ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d, reason:%d",
                    MAC2STR(event->mac), event->aid, event->reason);
        } else if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "retry to connect to the AP");
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                if (!ap_active && !settings->wifi_ap_fallback_disable) {
                    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to set WiFi mode to APSTA: %s", esp_err_to_name(err));
                    }
                    ESP_ERROR_CHECK(esp_wifi_stop());
                    wifi_configure_softap();
                    ESP_ERROR_CHECK(esp_wifi_start());
                    ap_active = true;
                }
            }
            ESP_LOGI(TAG,"connect to the AP fail");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (ap_active) {
            ESP_ERROR_CHECK(esp_wifi_stop());
            esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set WiFi mode to STA: %s", esp_err_to_name(err));
            }
            ESP_ERROR_CHECK(esp_wifi_start());
            ap_active = false;
        }
    }
}

void wifi_configure_sta(settings_t *settings) {
    if (sta_configured) {
        return;
    }
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };

    memcpy(wifi_config.sta.ssid, settings->wifi_ssid, strlen(settings->wifi_ssid));
    memcpy(wifi_config.sta.password, settings->wifi_password, strlen(settings->wifi_password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    sta_configured = true;
}

int8_t wifi_get_rssi(void) {
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        return 0; // Return 0 if not connected or error
    }
    return ap_info.rssi;
}

void wifi_init(settings_t *settings)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    
    // Set hostname if available
    if (settings->hostname != NULL && settings->hostname[0] != '\0') {
        esp_err_t err = esp_netif_set_hostname(sta_netif, settings->hostname);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Set hostname to: %s", settings->hostname);
        } else {
            ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Set hostname if available
    if (settings->hostname != NULL && settings->hostname[0] != '\0') {
        esp_err_t err = esp_netif_set_hostname(sta_netif, settings->hostname);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Set hostname to: %s", settings->hostname);
        } else {
            ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        }
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        settings,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        settings,
                                                        &instance_got_ip));


    if (settings->wifi_ssid == NULL || settings->wifi_ssid[0] == '\0') {
        ESP_LOGI(TAG_AP, "No WiFi credentials set, starting in AP mode");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP) );
        wifi_configure_softap();
        ntp_configure();
        ESP_ERROR_CHECK(esp_wifi_start() );
        ap_active = true;
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    wifi_configure_sta(settings);

    ntp_configure();

    ESP_LOGI(TAG, "Attempting connection to WiFi SSID: %s", settings->wifi_ssid);
    ESP_ERROR_CHECK(esp_wifi_start() );

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(CONFIG_ESP_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to ap SSID:%s password:%s",
                 settings->wifi_ssid, settings->wifi_password);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 settings->wifi_ssid, settings->wifi_password);
    } else {
        ESP_LOGE(TAG, "Timeout waiting for WiFi connection; launching AP mode.");
        ESP_ERROR_CHECK(esp_wifi_stop());
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi mode to APSTA: %s", esp_err_to_name(err));
        }
        wifi_configure_softap();
        ESP_ERROR_CHECK(esp_wifi_start() );
        ap_active = true;
    }
}

void time_sync_notification_cb(struct timeval *tv)
{
    g_ntp_initialized = true;
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

void ntp_configure(void) {
    ESP_LOGI(TAG, "Configuring NTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = false;                      
    config.server_from_dhcp = true;
    config.renew_servers_after_new_IP = true;
    config.index_of_first_server = 1;
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    config.sync_cb = time_sync_notification_cb;
    esp_netif_sntp_init(&config);
    esp_netif_sntp_start();
}
