idf_component_register(SRCS "ota.c" "wifi.c" "weight.c" "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_http_client app_update esp_https_ota
                                  esp_netif mbedtls nvs_flash esp_wifi esp_psram
                                  )
