#include "stm32f4xx_hal.h"
#include "SYSTEM/sys/sys.h"

#define BASIC_SPI_CAPTURE_SIZE 64u
#define BASIC_SPI_REPLY_BYTE 0xA5u
#define BASIC_CS_MEASUREMENT_ONLY 0
#define BASIC_SCK_COUNTER_ONLY 1

volatile uint32_t basic_spi_transaction_count;
volatile uint32_t basic_spi_nonempty_transaction_count;
volatile uint32_t basic_spi_empty_transaction_count;
volatile uint32_t basic_spi_last_size;
volatile uint32_t basic_spi_overrun_count;
volatile uint8_t basic_spi_last_rx[BASIC_SPI_CAPTURE_SIZE];
volatile uint32_t basic_cs_falling_count;
volatile uint32_t basic_cs_rising_count;
volatile uint32_t basic_cs_low_cycles;
static volatile uint32_t basic_cs_fall_cycle;

static uint8_t spi_read_byte(void)
{
    return (uint8_t)SPI1->DR;
}

static void spi_write_byte(uint8_t value)
{
    SPI1->DR = value;
}

static void spi_clear_overrun(void)
{
    volatile uint32_t unused;
    unused = SPI1->DR;
    unused = SPI1->SR;
    (void)unused;
}

static void basic_spi_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* SPI1 slave, hardware NSS, 8-bit, MSB first, mode 2. */
    SPI1->CR1 = SPI_CR1_CPOL;
#if BASIC_CS_MEASUREMENT_ONLY
    SPI1->CR2 = 0;
#else
    SPI1->CR2 = SPI_CR2_RXNEIE | SPI_CR2_ERRIE;
#endif
    SPI1->CR1 |= SPI_CR1_SPE;
    while (!(SPI1->SR & SPI_SR_TXE)) {}
    spi_write_byte(BASIC_SPI_REPLY_BYTE);
#if !BASIC_CS_MEASUREMENT_ONLY
    HAL_NVIC_SetPriority(SPI1_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(SPI1_IRQn);
#endif
}

static void basic_cs_measure_init(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    SYSCFG->EXTICR[1] &= ~SYSCFG_EXTICR2_EXTI4;
    EXTI->IMR &= ~EXTI_IMR_MR4;
    EXTI->RTSR |= EXTI_RTSR_TR4;
    EXTI->FTSR |= EXTI_FTSR_TR4;
    EXTI->PR = EXTI_PR_PR4;
    HAL_NVIC_SetPriority(EXTI4_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);
    EXTI->IMR |= EXTI_IMR_MR4;
}

static void basic_sck_counter_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_5;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(GPIOA, &gpio);

    TIM2->CR1 = 0;
    TIM2->PSC = 0;
    TIM2->ARR = 0xffffffffu;
    TIM2->CCMR1 = TIM_CCMR1_CC1S_0;
    TIM2->CCER = 0;
    TIM2->SMCR = (5u << TIM_SMCR_TS_Pos) |
                 (7u << TIM_SMCR_SMS_Pos);
    TIM2->CNT = 0;
    TIM2->CR1 = TIM_CR1_CEN;
}

void EXTI4_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR4) {
        uint32_t now = DWT->CYCCNT;
        EXTI->PR = EXTI_PR_PR4;
        if (GPIOA->IDR & GPIO_PIN_4) {
            ++basic_cs_rising_count;
            basic_cs_low_cycles = now - basic_cs_fall_cycle;
        } else {
            ++basic_cs_falling_count;
            basic_cs_fall_cycle = now;
        }
        __DSB();
    }
}

void SPI1_IRQHandler(void)
{
    uint32_t sr = SPI1->SR;

    if (sr & SPI_SR_RXNE) {
        uint8_t value = spi_read_byte();
        uint32_t index = basic_spi_last_size;
        if (index < BASIC_SPI_CAPTURE_SIZE)
            basic_spi_last_rx[index] = value;
        basic_spi_last_size = index + 1;
        ++basic_spi_nonempty_transaction_count;
        ++basic_spi_transaction_count;
        if (SPI1->SR & SPI_SR_TXE)
            spi_write_byte(BASIC_SPI_REPLY_BYTE);
    }
    if (SPI1->SR & SPI_SR_OVR) {
        spi_clear_overrun();
        ++basic_spi_overrun_count;
    }
}

int main(void)
{
    uint32_t i;

    HAL_Init();
    if (sys_stm32_clock_init(336, 8, 2, 7) != 0) {
        while (1) {}
    }

    basic_spi_transaction_count = 0;
    basic_spi_nonempty_transaction_count = 0;
    basic_spi_empty_transaction_count = 0;
    basic_spi_last_size = 0;
    basic_spi_overrun_count = 0;
    basic_cs_falling_count = 0;
    basic_cs_rising_count = 0;
    basic_cs_low_cycles = 0;
    basic_cs_fall_cycle = 0;
    for (i = 0; i < BASIC_SPI_CAPTURE_SIZE; ++i) basic_spi_last_rx[i] = 0;
#if BASIC_SCK_COUNTER_ONLY
    basic_sck_counter_init();
#else
    basic_spi_init();
    /* CS is gated by SPI1 hardware NSS; EXTI edge counting is disabled here. */
#endif

    while (1) {}
}
