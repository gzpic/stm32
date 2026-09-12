#include "stm32f4xx_hal.h"
#include "SYSTEM/delay/delay.h"
#include "dht11.h"
#include <string.h>

#define DHT11_PORT GPIOG
#define DHT11_PIN GPIO_PIN_9

volatile uint32_t dht11_start_count;
volatile uint32_t dht11_presence_low_timeouts;
volatile uint32_t dht11_presence_high_timeouts;
volatile uint32_t dht11_bit_low_timeouts;
volatile uint32_t dht11_bit_high_timeouts;
volatile uint32_t dht11_checksum_errors;
volatile uint8_t dht11_last_data[5];

static void dht11_write(GPIO_PinState value)
{
    HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, value);
}

static int wait_level(GPIO_PinState level, uint8_t limit)
{
    uint8_t retry = 0;
    while (HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) != level && retry < limit) {
        ++retry;
        delay_us(1);
    }
    return retry < limit;
}

static void dht11_start(void)
{
    dht11_write(GPIO_PIN_RESET);
    delay_ms(20);
    dht11_write(GPIO_PIN_SET);
    delay_us(30);
}

static int dht11_present(void)
{
    if (!wait_level(GPIO_PIN_RESET, 100)) {
        ++dht11_presence_low_timeouts;
        return 0;
    }
    if (!wait_level(GPIO_PIN_SET, 100)) {
        ++dht11_presence_high_timeouts;
        return 0;
    }
    return 1;
}

static int dht11_bit(uint8_t *value)
{
    if (!wait_level(GPIO_PIN_RESET, 100)) {
        ++dht11_bit_low_timeouts;
        return 0;
    }
    if (!wait_level(GPIO_PIN_SET, 100)) {
        ++dht11_bit_high_timeouts;
        return 0;
    }
    delay_us(40);
    *value = HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) == GPIO_PIN_SET;
    return 1;
}

static int dht11_byte(uint8_t *value)
{
    uint8_t bit;
    uint8_t data = 0;
    for (bit = 0; bit < 8; ++bit) {
        uint8_t value_bit;
        if (!dht11_bit(&value_bit)) return 0;
        data = (uint8_t)((data << 1) | value_bit);
    }
    *value = data;
    return 1;
}

void dht11_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOG_CLK_ENABLE();
    gpio.Pin = DHT11_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT11_PORT, &gpio);
    dht11_write(GPIO_PIN_SET);
    dht11_start_count = 0;
    dht11_presence_low_timeouts = 0;
    dht11_presence_high_timeouts = 0;
    dht11_bit_low_timeouts = 0;
    dht11_bit_high_timeouts = 0;
    dht11_checksum_errors = 0;
    memset((void *)dht11_last_data, 0, sizeof dht11_last_data);
}

int platform_temperature_read(uint8_t *temperature)
{
    uint8_t data[5];
    uint8_t index;
    if (!temperature) return 0;
    ++dht11_start_count;
    dht11_start();
    if (!dht11_present()) return 0;
    for (index = 0; index < sizeof data; ++index) {
        if (!dht11_byte(&data[index])) return 0;
        dht11_last_data[index] = data[index];
    }
    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        ++dht11_checksum_errors;
        return 0;
    }
    *temperature = data[2];
    return 1;
}
