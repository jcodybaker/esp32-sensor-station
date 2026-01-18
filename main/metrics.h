#ifndef METRICS_H
#define METRICS_H

#include "settings.h"
#include <esp_http_server.h>
#include <stdatomic.h>

// Atomic malloc counters per source file
extern atomic_uint_fast32_t malloc_count_settings;
extern atomic_uint_fast32_t malloc_count_metrics;
extern atomic_uint_fast32_t malloc_count_sensors;
extern atomic_uint_fast32_t malloc_count_pump;
extern atomic_uint_fast32_t malloc_count_main;
extern atomic_uint_fast32_t malloc_count_http_server;
extern atomic_uint_fast32_t malloc_count_syslog;
extern atomic_uint_fast32_t malloc_count_mqtt_publisher;

// Atomic free counters per source file
extern atomic_uint_fast32_t free_count_settings;
extern atomic_uint_fast32_t free_count_metrics;
extern atomic_uint_fast32_t free_count_sensors;
extern atomic_uint_fast32_t free_count_pump;
extern atomic_uint_fast32_t free_count_main;
extern atomic_uint_fast32_t free_count_http_server;
extern atomic_uint_fast32_t free_count_syslog;
extern atomic_uint_fast32_t free_count_mqtt_publisher;

void metrics_init(settings_t *settings, httpd_handle_t server);

#endif // METRICS_H
