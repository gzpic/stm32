#ifndef DHT11_H
#define DHT11_H

#include <stdint.h>

void dht11_init(void);
int platform_temperature_read(uint8_t *temperature);

extern volatile uint32_t dht11_start_count;
extern volatile uint32_t dht11_presence_low_timeouts;
extern volatile uint32_t dht11_presence_high_timeouts;
extern volatile uint32_t dht11_bit_low_timeouts;
extern volatile uint32_t dht11_bit_high_timeouts;
extern volatile uint32_t dht11_checksum_errors;
extern volatile uint8_t dht11_last_data[5];

#endif
