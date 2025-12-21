#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include "settings.h"
#include <stdint.h>
#include <stdbool.h>

#define EXAMPLE_ONEWIRE_MAX_DS18B20 5

typedef struct {
    uint64_t address;
    int sensor_id;
    float last_temperature_c;
    time_t last_updated;
} ds18b20_info_t;

void init_ds18b20(settings_t *settings);
int get_ds18b20_devices(ds18b20_info_t *devices, int max_devices);

#endif // TEMPERATURE_H

