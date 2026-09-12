#include "stm32f4xx_hal.h"
#include "SYSTEM/sys/sys.h"

#define I2C_SLAVE_ADDRESS 0x42u
#define I2C_REGISTER_COUNT 16u

volatile uint32_t i2c_address_count;
volatile uint32_t i2c_rx_count;
volatile uint32_t i2c_tx_count;
volatile uint32_t i2c_stop_count;
volatile uint32_t i2c_error_count;
volatile uint32_t i2c_last_error;
volatile uint8_t i2c_last_rx;
volatile uint8_t i2c_register_pointer;
volatile uint8_t i2c_registers[I2C_REGISTER_COUNT];
static volatile uint8_t i2c_expect_register_pointer;

static void i2c1_slave_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &gpio);

    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();

    I2C1->CR1 = 0;
    I2C1->CR2 = 42u;
    I2C1->OAR1 = (1u << 14) | (I2C_SLAVE_ADDRESS << 1);
    I2C1->OAR2 = 0;
    I2C1->CCR = 210u;
    I2C1->TRISE = 43u;
    I2C1->CR1 = I2C_CR1_ACK | I2C_CR1_PE;
    I2C1->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;

    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, 1, 1);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
}

void I2C1_EV_IRQHandler(void)
{
    uint32_t sr1 = I2C1->SR1;

    if (sr1 & I2C_SR1_ADDR) {
        uint32_t sr2 = I2C1->SR2;
        ++i2c_address_count;
        if (sr2 & I2C_SR2_TRA) {
            I2C1->DR = i2c_registers[i2c_register_pointer];
            i2c_register_pointer =
                (uint8_t)((i2c_register_pointer + 1u) % I2C_REGISTER_COUNT);
            ++i2c_tx_count;
        } else {
            i2c_expect_register_pointer = 1;
        }
        return;
    }

    if (sr1 & I2C_SR1_STOPF) {
        (void)I2C1->SR1;
        I2C1->CR1 |= I2C_CR1_PE;
        ++i2c_stop_count;
        return;
    }

    if (sr1 & I2C_SR1_RXNE) {
        uint8_t value = (uint8_t)I2C1->DR;
        i2c_last_rx = value;
        ++i2c_rx_count;
        if (i2c_expect_register_pointer) {
            i2c_register_pointer = value % I2C_REGISTER_COUNT;
            i2c_expect_register_pointer = 0;
        } else {
            i2c_registers[i2c_register_pointer] = value;
            i2c_register_pointer =
                (uint8_t)((i2c_register_pointer + 1u) % I2C_REGISTER_COUNT);
        }
    }

    if (sr1 & I2C_SR1_TXE) {
        I2C1->DR = i2c_registers[i2c_register_pointer];
        i2c_register_pointer =
            (uint8_t)((i2c_register_pointer + 1u) % I2C_REGISTER_COUNT);
        ++i2c_tx_count;
    }
}

void I2C1_ER_IRQHandler(void)
{
    const uint32_t error_mask = I2C_SR1_BERR | I2C_SR1_ARLO |
                                I2C_SR1_AF | I2C_SR1_OVR |
                                I2C_SR1_TIMEOUT;
    uint32_t errors = I2C1->SR1 & error_mask;

    i2c_last_error = errors;
    if (errors != 0) ++i2c_error_count;
    I2C1->SR1 &= ~error_mask;
}

int main(void)
{
    uint32_t i;

    HAL_Init();
    if (sys_stm32_clock_init(336, 8, 2, 7) != 0) {
        while (1) {}
    }

    i2c_address_count = 0;
    i2c_rx_count = 0;
    i2c_tx_count = 0;
    i2c_stop_count = 0;
    i2c_error_count = 0;
    i2c_last_error = 0;
    i2c_last_rx = 0;
    i2c_register_pointer = 0;
    i2c_expect_register_pointer = 1;
    for (i = 0; i < I2C_REGISTER_COUNT; ++i)
        i2c_registers[i] = 0;
    i2c_registers[0] = 0xa5;
    i2c_registers[1] = 0x01;

    i2c1_slave_init();
    while (1) {}
}
