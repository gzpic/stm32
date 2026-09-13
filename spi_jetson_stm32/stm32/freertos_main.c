#include "SYSTEM/sys/sys.h"
#include "SYSTEM/delay/delay.h"
#include "FreeRTOS.h"
#include "task.h"
#include "dht11.h"
#include "i2c_slave.h"
#include "sensors.h"

#define I2C_TASK_PRIORITY 3U
#define HEARTBEAT_TASK_PRIORITY 1U
#define I2C_TASK_STACK_WORDS 384U
#define HEARTBEAT_TASK_STACK_WORDS 128U

volatile uint32_t freertos_heartbeat_count;
volatile UBaseType_t freertos_heartbeat_stack_remaining;
volatile UBaseType_t freertos_i2c_stack_remaining;
volatile size_t freertos_minimum_free_heap;

static TaskHandle_t heartbeat_task_handle;
static TaskHandle_t i2c_task_handle;

static void heartbeat_task(void *argument)
{
    (void)argument;
    while (1) {
        ++freertos_heartbeat_count;
        freertos_heartbeat_stack_remaining = uxTaskGetStackHighWaterMark2(NULL);
        freertos_minimum_free_heap = xPortGetMinimumEverFreeHeapSize();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void i2c_service_task(void *argument)
{
    (void)argument;
    while (1) {
        i2c_slave_poll();
        freertos_i2c_stack_remaining = uxTaskGetStackHighWaterMark2(NULL);
        /* This first RTOS stage preserves polling; STOP-to-task notification follows later. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main(void)
{
    if (HAL_Init() != HAL_OK) while (1) {}
    if (sys_stm32_clock_init(336, 8, 2, 7) != 0) while (1) {}
    delay_init(168);
    dht11_init();
    sensors_init();
    i2c_slave_init();

    if (xTaskCreate(i2c_service_task, "i2c", I2C_TASK_STACK_WORDS, NULL,
                    I2C_TASK_PRIORITY, &i2c_task_handle) != pdPASS) while (1) {}
    if (xTaskCreate(heartbeat_task, "heartbeat", HEARTBEAT_TASK_STACK_WORDS, NULL,
                    HEARTBEAT_TASK_PRIORITY, &heartbeat_task_handle) != pdPASS) while (1) {}
    vTaskStartScheduler();
    while (1) {}
}
