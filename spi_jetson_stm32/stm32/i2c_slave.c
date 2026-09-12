#include "stm32f4xx_hal.h"
#include "i2c_slave.h"
#include "service.h"

#define I2C_SLAVE_ADDRESS 0x42u
#define I2C_CLOCK_MHZ 42u

static protocol_service service;
static uint8_t busy_tx[PROTO_MAX_FRAME + 1];
static size_t busy_tx_size;
static uint8_t command_rx[PROTO_MAX_FRAME + 1];
static struct {
    size_t size;
    int error;
    volatile unsigned ready;
} command_buffer;

static volatile size_t rx_size;
static volatile unsigned rx_overflow;
static volatile unsigned receiving_write;
static volatile unsigned read_active;
static volatile unsigned read_is_response;
static volatile size_t read_count;
static volatile unsigned read_finished;
static const uint8_t *read_tx;
static size_t read_tx_size;

/* Debugger-visible diagnostics. AF is normal at the end of an I2C read. */
volatile uint32_t i2c_slave_dropped_writes;
volatile uint32_t i2c_slave_rx_overflows;
volatile uint32_t i2c_slave_bus_errors;
volatile uint32_t i2c_slave_reads;
volatile uint32_t i2c_slave_busy_reads;

static uint8_t read_byte(size_t index)
{
    return index < read_tx_size ? read_tx[index] : 0xffu;
}

static void begin_read(void)
{
    read_is_response = service.pending;
    if (read_is_response) {
        read_tx = service.tx;
        read_tx_size = service.tx_size;
    } else {
        read_tx = busy_tx;
        read_tx_size = busy_tx_size;
        ++i2c_slave_busy_reads;
    }
    read_count = 0;
    read_finished = 0;
    read_active = 1;
    I2C1->DR = read_byte(read_count++);
    ++i2c_slave_reads;
}

void i2c_slave_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t busy_data[1] = {COMMAND_BUSY};

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
    I2C1->CR2 = I2C_CLOCK_MHZ;
    I2C1->OAR1 = (1u << 14) | (I2C_SLAVE_ADDRESS << 1);
    I2C1->OAR2 = 0;
    I2C1->CCR = 210u;
    I2C1->TRISE = 43u;
    I2C1->CR1 = I2C_CR1_ACK | I2C_CR1_PE;
    I2C1->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;

    service_init(&service);
    busy_tx_size = proto_reply(busy_tx, sizeof busy_tx, busy_data, sizeof busy_data);
    command_buffer.ready = 0;
    rx_size = 0;
    rx_overflow = 0;
    receiving_write = 0;
    read_active = 0;
    read_finished = 0;
    i2c_slave_dropped_writes = 0;
    i2c_slave_rx_overflows = 0;
    i2c_slave_bus_errors = 0;
    i2c_slave_reads = 0;
    i2c_slave_busy_reads = 0;

    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, 1, 1);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
}

void I2C1_EV_IRQHandler(void)
{
    uint32_t sr1 = I2C1->SR1;

    if (sr1 & I2C_SR1_ADDR) {
        uint32_t sr2 = I2C1->SR2; /* Reading both clears ADDR. */
        if (sr2 & I2C_SR2_TRA) {
            begin_read();
        } else if (command_buffer.ready || read_active) {
            receiving_write = 0;
            ++i2c_slave_dropped_writes;
        } else {
            rx_size = 0;
            rx_overflow = 0;
            receiving_write = 1;
        }
        return;
    }

    if (sr1 & I2C_SR1_STOPF) {
        (void)I2C1->SR1;
        I2C1->CR1 |= I2C_CR1_PE;
        if (receiving_write) {
            receiving_write = 0;
            command_buffer.size = rx_size;
            command_buffer.error = rx_overflow;
            __DMB();
            command_buffer.ready = 1;
        }
        if (read_active) {
            read_active = 0;
            read_finished = 1;
        }
        return;
    }

    if (sr1 & I2C_SR1_RXNE) {
        uint8_t value = (uint8_t)I2C1->DR;
        if (receiving_write) {
            if (rx_size < sizeof command_rx) command_rx[rx_size++] = value;
            else rx_overflow = 1;
        }
    }

    if ((sr1 & I2C_SR1_TXE) && read_active) I2C1->DR = read_byte(read_count++);
}

void I2C1_ER_IRQHandler(void)
{
    uint32_t errors = I2C1->SR1 &
        (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF | I2C_SR1_OVR | I2C_SR1_TIMEOUT);
    if (errors & ~I2C_SR1_AF) {
        ++i2c_slave_bus_errors;
        if (receiving_write) rx_overflow = 1;
    }
    if (errors & I2C_SR1_AF) {
        read_active = 0;
        read_finished = 1;
    }
    I2C1->SR1 &= ~errors;
}

void i2c_slave_poll(void)
{
    uint32_t primask;
    if (read_finished) {
        primask = __get_PRIMASK();
        __disable_irq();
        if (read_finished) {
            if (read_is_response && read_count >= read_tx_size)
                service_consume_response(&service);
            read_finished = 0;
        }
        __set_PRIMASK(primask);
    }
    if (!command_buffer.ready || read_active) return;
    __DMB();
    service_process_write(&service, command_rx, command_buffer.size,
                          command_buffer.error);
    primask = __get_PRIMASK();
    __disable_irq();
    command_buffer.ready = 0;
    __DMB();
    __set_PRIMASK(primask);
}
