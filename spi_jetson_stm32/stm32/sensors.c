#include "stm32f4xx_hal.h"
#include "SYSTEM/delay/delay.h"
#include "sensors.h"

#define ADC_SAMPLE_TIME_480 7u
#define ADC_POLL_LIMIT 100000u

static int adc_read(ADC_TypeDef *adc, uint8_t channel, uint16_t *value)
{
    uint32_t sample_register;
    uint32_t sample_shift;
    uint32_t count = 0;

    if (channel < 10u) {
        sample_register = adc->SMPR2;
        sample_shift = (uint32_t)channel * 3u;
        sample_register &= ~(7u << sample_shift);
        adc->SMPR2 = sample_register | (ADC_SAMPLE_TIME_480 << sample_shift);
    } else {
        sample_register = adc->SMPR1;
        sample_shift = ((uint32_t)channel - 10u) * 3u;
        sample_register &= ~(7u << sample_shift);
        adc->SMPR1 = sample_register | (ADC_SAMPLE_TIME_480 << sample_shift);
    }

    adc->SQR1 = 0;
    adc->SQR2 = 0;
    adc->SQR3 = channel;
    adc->SR = 0;
    adc->CR2 |= ADC_CR2_ADON;
    adc->CR2 |= ADC_CR2_SWSTART;
    while (!(adc->SR & ADC_SR_EOC)) {
        if (++count == ADC_POLL_LIMIT) return 0;
    }
    *value = (uint16_t)adc->DR;
    return 1;
}

void sensors_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_ADC3_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOF, &gpio);

    ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CLOCKPRESCALER_PCLK_DIV4 |
               ADC_CCR_TSVREFE;
    ADC1->CR1 = 0;
    ADC1->CR2 = 0;
    ADC3->CR1 = 0;
    ADC3->CR2 = 0;
}

int platform_internal_temperature_read(int16_t *temperature_centi_c)
{
    uint32_t total = 0;
    uint16_t raw;
    uint8_t index;

    if (!temperature_centi_c) return 0;
    for (index = 0; index < 10u; ++index) {
        if (!adc_read(ADC1, ADC_CHANNEL_TEMPSENSOR, &raw)) return 0;
        total += raw;
        delay_ms(5);
    }
    /* STM32F407 typical values from the bundled internal-temperature example. */
    *temperature_centi_c = (int16_t)((((float)(total / 10u) * 3.3f / 4096.0f -
                                      0.76f) / 0.0025f + 25.0f) * 100.0f);
    return 1;
}

int platform_light_read(uint8_t *light_percent)
{
    uint32_t total = 0;
    uint32_t scaled;
    uint16_t raw;
    uint8_t index;

    if (!light_percent) return 0;
    for (index = 0; index < 10u; ++index) {
        if (!adc_read(ADC3, ADC_CHANNEL_5, &raw)) return 0;
        total += raw;
        delay_ms(5);
    }
    scaled = (total / 10u) / 40u;
    if (scaled > 100u) scaled = 100u;
    *light_percent = (uint8_t)(100u - scaled);
    return 1;
}
