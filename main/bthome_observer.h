
#ifndef BTHOME_OBSERVER_H
#define BTHOME_OBSERVER_H

#include <stdbool.h>
#include <esp_http_server.h>
#include "settings.h"
#include "bthome.h"
#include "esp_gap_ble_api.h"

void bthome_observer_init(settings_t *settings, httpd_handle_t server);

// Callback function type for iterating cached packets
// Returns true to continue iteration, false to stop
typedef bool (*bthome_cache_iterator_t)(const esp_bd_addr_t addr, int rssi, 
                                         const bthome_packet_t *packet, 
                                         const struct timeval *last_seen, void *user_data);

// Iterate through all cached BTHome packets
// The callback is called for each occupied cache entry
void bthome_cache_iterate(bthome_cache_iterator_t callback, void *user_data);

#endif // BTHOME_OBSERVER_H