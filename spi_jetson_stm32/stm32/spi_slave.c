#include "stm32f4xx_hal.h"
#include "spi_slave.h"
#include "service.h"

/* Ordinary SRAM only: F407 DMA cannot access CCM. */
static uint8_t rx[PROTO_MAX_FRAME + 1];
static spi_service service;
/* Single-slot ISR -> background mailbox. DMA stays stopped until consumed. */
static struct {
    uint8_t data[PROTO_MAX_FRAME + 1];
    size_t size;
    int error;
    volatile unsigned ready;
} command_buffer;
static volatile unsigned armed;
static unsigned processed;
/* Debugger-visible diagnostics; overflow/early transactions never overwrite data. */
volatile uint32_t spi_slave_dropped_transactions;
volatile uint32_t spi_slave_dma_stop_faults;
#define DMA_ERRORS (DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0 | \
                    DMA_LISR_TEIF3 | DMA_LISR_DMEIF3 | DMA_LISR_FEIF3)
#define DMA_CLEAR (DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | \
                   DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0 | DMA_LIFCR_CFEIF3 | \
                   DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CTEIF3 | DMA_LIFCR_CHTIF3 | \
                   DMA_LIFCR_CTCIF3)

static int stop_dma(void)
{
    unsigned budget = 4096;
    DMA2_Stream0->CR &= ~DMA_SxCR_EN;
    DMA2_Stream3->CR &= ~DMA_SxCR_EN;
    while ((DMA2_Stream0->CR | DMA2_Stream3->CR) & DMA_SxCR_EN) {
        if (--budget == 0) {
            SPI1->CR2 = 0;
            ++spi_slave_dma_stop_faults;
            return 0; /* Fail closed; do not copy memory still owned by DMA. */
        }
    }
    SPI1->CR2 = 0;
    __DSB();
    return 1;
}

static void arm(void)
{
    /* Reset discards the prefetched transmit byte and any residual RX/OVR.
       NSS must be high; protocol requires a 10 ms inter-transaction gap. */
    __HAL_RCC_SPI1_FORCE_RESET();
    __HAL_RCC_SPI1_RELEASE_RESET();
    DMA2->LIFCR = DMA_CLEAR;
    DMA2_Stream0->PAR = (uint32_t)&SPI1->DR;
    DMA2_Stream0->M0AR = (uint32_t)rx;
    DMA2_Stream0->NDTR = sizeof rx;
    DMA2_Stream0->FCR = 0;
    DMA2_Stream0->CR = (3u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_PL_1;
    DMA2_Stream3->PAR = (uint32_t)&SPI1->DR;
    DMA2_Stream3->M0AR = (uint32_t)service.tx;
    DMA2_Stream3->NDTR = sizeof service.tx;
    DMA2_Stream3->FCR = 0;
    DMA2_Stream3->CR = (3u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC |
                       DMA_SxCR_PL_1 | DMA_SxCR_DIR_0;
    /* MSTR=SSM=CPOL=CPHA=DFF=LSBFIRST=CRCEN=0: slave, hardware NSS, mode 0. */
    SPI1->CR1 = 0;
    __DMB();
    DMA2_Stream0->CR |= DMA_SxCR_EN;
    DMA2_Stream3->CR |= DMA_SxCR_EN;
    SPI1->CR2 = SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    SPI1->CR1 = SPI_CR1_SPE;
}

void spi_slave_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_4;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gpio);
    /* Route PA4 to EXTI4 without changing its SPI alternate-function mode. */
    SYSCFG->EXTICR[1] &= ~SYSCFG_EXTICR2_EXTI4;
    EXTI->IMR &= ~EXTI_IMR_MR4;
    EXTI->FTSR &= ~EXTI_FTSR_TR4;
    EXTI->RTSR |= EXTI_RTSR_TR4;
    service_init(&service);
    command_buffer.ready = 0;
    processed = 0;
    armed = 0;
    spi_slave_dropped_transactions = 0;
    spi_slave_dma_stop_faults = 0;
    if (!stop_dma()) return;
    /* If the host already holds CS low, discard that partial transaction. */
    while (!(GPIOA->IDR & GPIO_PIN_4)) {}
    arm();
    EXTI->PR = EXTI_PR_PR4;
    armed = 1;
    HAL_NVIC_SetPriority(EXTI4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);
    EXTI->IMR |= EXTI_IMR_MR4;
}

void EXTI4_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR4) {
        size_t size, i;
        int error;
        EXTI->PR = EXTI_PR_PR4;
        if (!armed || command_buffer.ready) {
            ++spi_slave_dropped_transactions;
            return;
        }
        armed = 0;
        if (!stop_dma()) return;
        size = sizeof rx - DMA2_Stream0->NDTR;
        error = (DMA2->LISR & DMA_ERRORS) != 0 ||
                (SPI1->SR & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_RXNE)) != 0 ||
                size > PROTO_MAX_FRAME;
        /* Bounded snapshot only: no frame parsing, CRC or callbacks in ISR. */
        for (i = 0; i < size; ++i) command_buffer.data[i] = rx[i];
        command_buffer.size = size;
        command_buffer.error = error;
        __DMB();
        command_buffer.ready = 1; /* Publish only after the snapshot is complete. */
    }
}

void spi_slave_poll(void)
{
    uint32_t primask;
    if (!command_buffer.ready) return;
    __DMB();
    if (!processed) {
        service_transaction(&service, command_buffer.data, command_buffer.size,
                            command_buffer.error);
        processed = 1; /* Never execute again while waiting for NSS to go high. */
    }
    /* Only the short DMA rearm is protected; callbacks run with interrupts enabled. */
    primask = __get_PRIMASK();
    __disable_irq();
    if (GPIOA->IDR & GPIO_PIN_4) {
        if (EXTI->PR & EXTI_PR_PR4) {
            EXTI->PR = EXTI_PR_PR4;
            ++spi_slave_dropped_transactions;
        }
        arm();
        command_buffer.ready = 0;
        processed = 0;
        __DMB();
        armed = 1;
    }
    __set_PRIMASK(primask);
}
