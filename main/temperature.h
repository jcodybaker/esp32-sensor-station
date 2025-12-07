#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include "settings.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint64_t address;
    int sensor_id;
} ds18b20_info_t;

void init_ds18b20(settings_t *settings);
int get_ds18b20_devices(ds18b20_info_t *devices, int max_devices);

#endif // TEMPERATURE_H

