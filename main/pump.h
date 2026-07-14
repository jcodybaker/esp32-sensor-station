#ifndef PUMP_H
#define PUMP_H

#include "settings.h"
#include <esp_http_server.h>
#include "driver/i2c_master.h"

void pump_init(settings_t *settings, httpd_handle_t server);

const char* pump_get_last_error();

// Returns the shared I2C master bus used for Atlas Scientific EZO circuits
// (pump, pH probe, etc. chained on the same bus with different addresses),
// or NULL if it hasn't been created yet because the pump SCL/SDA GPIOs are
// not configured in settings.
i2c_master_bus_handle_t pump_get_i2c_bus(void);

#endif // PUMP_H

