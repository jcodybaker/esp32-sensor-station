#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include "settings.h"
#include "esp_wifi.h"

void wifi_init(settings_t *settings);
int8_t wifi_get_rssi(void);
// Writes the current STA IP address (dotted-decimal) into buf. Returns false
// (buf left untouched) if not connected.
bool wifi_get_ip_str(char *buf, size_t buf_len);
// Writes the device's own configuration-AP SSID into buf. Returns false
// (buf left untouched) if the AP isn't currently up (e.g. STA is connected).
bool wifi_get_ap_ssid(char *buf, size_t buf_len);

#endif // WIFI_H
