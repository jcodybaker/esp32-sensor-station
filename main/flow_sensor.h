#ifndef FLOW_SENSOR_H
#define FLOW_SENSOR_H

#include "settings.h"

// US liquid gallon, used to convert the canonical liters total for display
// when settings->flow_use_gallons is set.
#define FLOW_LITERS_TO_US_GALLONS 0.264172052

void flow_sensor_init(settings_t *settings);

#endif // FLOW_SENSOR_H
