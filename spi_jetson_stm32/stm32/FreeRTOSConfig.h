#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
#include "freertos_hooks.h"

/* The startup vector table references these CMSIS exception names directly. */
#define vPortSVCHandler                         SVC_Handler
#define xPortPendSVHandler                      PendSV_Handler

#define configCPU_CLOCK_HZ                         ( 168000000UL )
#define configTICK_RATE_HZ                          ( 1000U )
#define configUSE_PREEMPTION                        1
#define configUSE_TIME_SLICING                      1
#define configMAX_PRIORITIES                        4
#define configMINIMAL_STACK_SIZE                    128U
#define configMAX_TASK_NAME_LEN                     16U
#define configTICK_TYPE_WIDTH_IN_BITS               TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION     1

#define configSUPPORT_DYNAMIC_ALLOCATION            1
#define configSUPPORT_STATIC_ALLOCATION             0
#define configAPPLICATION_ALLOCATED_HEAP             0
#define configTOTAL_HEAP_SIZE                       ( 16U * 1024U )

#define configUSE_TASK_NOTIFICATIONS                1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES       1
#define configUSE_MUTEXES                           0
#define configUSE_RECURSIVE_MUTEXES                 0
#define configUSE_COUNTING_SEMAPHORES               0
#define configUSE_QUEUE_SETS                        0
#define configQUEUE_REGISTRY_SIZE                   0
#define configUSE_TIMERS                            0
#define configUSE_TICKLESS_IDLE                     0
#define configUSE_IDLE_HOOK                         0
#define configUSE_TICK_HOOK                         0
#define configUSE_TRACE_FACILITY                    0
#define configGENERATE_RUN_TIME_STATS               0
#define configUSE_STATS_FORMATTING_FUNCTIONS        0

#define configPRIO_BITS                             4U
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     15U
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5U
#define configKERNEL_INTERRUPT_PRIORITY             ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << 4U )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY        ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << 4U )

#define configUSE_MALLOC_FAILED_HOOK                1
#define configCHECK_FOR_STACK_OVERFLOW               2
#define configASSERT( expression )                  do { if( !( expression ) ) freertos_assert_failed( __FILE__, __LINE__ ); } while( 0 )

#define INCLUDE_vTaskDelay                          1
#define INCLUDE_xTaskGetSchedulerState              1
#define INCLUDE_uxTaskGetStackHighWaterMark2        1

#endif
