idf_component_register(SRCS "mqtt_publisher.c" "pump.c" "temperature.c" "sensors.c" "bthome_observer.c" "settings.c" "http_server.c" "ota.c" "wifi.c" "weight.c" "main.c" "metrics.c" "pump.c" "syslog.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES bt esp_http_client app_update esp_https_ota
                                  esp_netif mbedtls nvs_flash esp_wifi esp_psram
                                  esp-tls esp_http_server bthome mqtt
                                  )
