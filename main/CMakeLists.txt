set(m5stick_display_srcs "m5stick_display_stub.c")
set(m5stick_display_requires "")
if(CONFIG_ENABLE_M5STICKC_DISPLAY)
    set(m5stick_display_srcs "m5stick_display.cpp")
    set(m5stick_display_requires "m5gfx" "m5unified")
endif()

idf_component_register(SRCS "mqtt_publisher.c" "ha_discovery.c" "pump.c" "temperature.c" "sensors.c" "bthome_observer.c" "settings.c" "http_server.c" "ota.c" "wifi.c" "weight.c" "main.c" "metrics.c" "pump.c" "syslog.c" "rcwl9620.c" "a02yyuw.c" "ezo_ph.c" "flow_sensor.c" "${m5stick_display_srcs}"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES bt esp_http_client app_update esp_https_ota
                                  esp_netif mbedtls nvs_flash esp_wifi esp_psram
                                  esp-tls esp_http_server bthome mqtt
                                  ${m5stick_display_requires}
                    )

if(CONFIG_ENABLE_M5STICKC_DISPLAY)
    # M5GFX/M5Unified require C++17, exceptions, and RTTI.
    set_source_files_properties(m5stick_display.cpp PROPERTIES COMPILE_OPTIONS "-std=gnu++17")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M5GFX_BOARD=board_M5StickCPlus)
endif()
