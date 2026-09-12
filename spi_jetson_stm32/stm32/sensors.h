#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>

void sensors_init(void);
int platform_internal_temperature_read(int16_t *temperature_centi_c);
int platform_light_read(uint8_t *light_percent);

#endif
