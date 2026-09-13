#include "FreeRTOS.h"
#include "task.h"
#include "freertos_hooks.h"

volatile uint32_t freertos_assert_line;
volatile const char *freertos_assert_file;
volatile uint32_t freertos_malloc_failed_count;
volatile uint32_t freertos_stack_overflow_count;
volatile const char *freertos_stack_overflow_task;

static void freertos_stop(void)
{
    taskDISABLE_INTERRUPTS();
    while (1) {}
}

void freertos_assert_failed(const char *file, uint32_t line)
{
    freertos_assert_file = file;
    freertos_assert_line = line;
    freertos_stop();
}

void vApplicationMallocFailedHook(void)
{
    ++freertos_malloc_failed_count;
    freertos_stop();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    freertos_stack_overflow_task = task_name;
    ++freertos_stack_overflow_count;
    freertos_stop();
}
