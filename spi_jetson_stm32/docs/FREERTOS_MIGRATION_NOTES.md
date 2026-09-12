# FreeRTOS 迁移配置笔记

本笔记记录 `spi_jetson_stm32` 从裸机 I2C 从机迁移到 FreeRTOS 的每一步。每个阶段必须先构建、下载和验收，再进入下一阶段；未验证的内容只能记录在“待执行”中，不能当作已完成。

## 目标与边界

- 保持 Jetson -> I2C1 -> STM32 的现有帧格式、地址 `0x42`、BUSY 读重试和命令语义不变。
- 保持 I2C1 使用 PB6/SCL、PB7/SDA；不在第一阶段改变硬件接线。
- 初期不启用 tickless idle、软件定时器、队列或动态创建任务以外的额外功能。
- 首次目标是“FreeRTOS 能调度且原 I2C 命令仍可用”，不是立即重构所有模块。

## 已确认基线

| 项目 | 当前状态 |
|---|---|
| MCU | STM32F407ZGTx，Cortex-M4F，168 MHz |
| 编译器 | Keil MDK 5.43a，Arm Compiler 6.24 |
| 现有入口 | `stm32/main.c` 的无限循环调用 `i2c_slave_poll()` |
| I2C 中断 | `I2C1_EV_IRQn` 优先级 1，`I2C1_ER_IRQn` 优先级 1/1 |
| HAL SysTick | `SysTick_Handler()` 仅调用 `HAL_IncTick()`，HAL tick 优先级 15 |
| I2C 协议验证 | Jetson 实测通过写帧、读帧、CRC、BUSY、传感器 type 命令 |
| FreeRTOS 源码 | `E:\Coding\FreeRTOS-Kernel-main\FreeRTOS-Kernel-main`，README 标识 V11.1.0 |

## 端口选择

当前工程使用 Arm Compiler 6。内核目录的 `portable/RVDS/ARM_CM4F` 使用 Arm Compiler 5 的 `__asm void` 语法，不能直接加入 AC6 工程。

第一候选是 `portable/GCC/ARM_CM4F`，因为其面向 Cortex-M4F 且使用 Clang/GCC 风格的内联汇编。加入业务代码前，必须先用 Keil AC6 单独编译此端口；若编译不通过，不修改内核实现，而是改用经验证的 AC6 兼容端口。

第一阶段只需要以下内核文件：

```text
FreeRTOS-Kernel-main/list.c
FreeRTOS-Kernel-main/queue.c
FreeRTOS-Kernel-main/tasks.c
FreeRTOS-Kernel-main/portable/GCC/ARM_CM4F/port.c
FreeRTOS-Kernel-main/portable/MemMang/heap_4.c
```

以及以下 include 路径：

```text
FreeRTOS-Kernel-main/include
FreeRTOS-Kernel-main/portable/GCC/ARM_CM4F
stm32
```

`FreeRTOSConfig.h` 由本项目创建，不能直接使用模板文件的时钟、堆和功能开关。

## 最小配置草案

以下是第一阶段的目标值，尚未写入工程：

| 宏 | 目标值 | 原因 |
|---|---:|---|
| `configCPU_CLOCK_HZ` | `168000000` | 与现有时钟配置一致 |
| `configTICK_RATE_HZ` | `1000` | 与 HAL 1 ms tick 一致 |
| `configUSE_PREEMPTION` | `1` | 使用抢占式调度 |
| `configMAX_PRIORITIES` | `4` | 初期任务数量少，减少复杂度 |
| `configMINIMAL_STACK_SIZE` | `128` words | Idle 任务起点，后续按水位调整 |
| `configTOTAL_HEAP_SIZE` | `16384` bytes | 初期使用 `heap_4.c` 的保守起点，需结合 AXF 验证 SRAM 余量 |
| `configUSE_TIMERS` | `0` | 第一阶段不引入定时器服务任务 |
| `configUSE_TICKLESS_IDLE` | `0` | 先稳定普通 SysTick 调度 |
| `configPRIO_BITS` | `4` | STM32F407 NVIC 实现 4 个优先级位 |
| `configLIBRARY_LOWEST_INTERRUPT_PRIORITY` | `15` | Cortex-M 最低库优先级 |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | `5` | 可调用 `FromISR` API 的最高库优先级 |
| `configKERNEL_INTERRUPT_PRIORITY` | `0xF0` | PendSV/SysTick 使用最低优先级 |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | `0x50` | 对应库优先级 5 |

`configASSERT` 在调试阶段必须启用，用于捕获非法中断优先级和 API 调用上下文。

## 异常入口配置

FreeRTOS 启动调度器后需要接管 SVC、PendSV 和 SysTick。现有 `vendor/hal_example/User/stm32f4xx_it.c` 必须改为：

```c
void SVC_Handler(void)
{
    vPortSVCHandler();
}

void PendSV_Handler(void)
{
    xPortPendSVHandler();
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    xPortSysTickHandler();
}
```

这一步只有在端口编译成功、`FreeRTOSConfig.h` 完整后才能执行。若 SysTick 同时服务 HAL 和 FreeRTOS，`configTICK_RATE_HZ` 必须保持 1000 Hz；不能在同一 SysTick 上配置两套不同频率。

## 中断优先级规则

现有 I2C1 中断优先级为 1，数值比 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY=5` 更高。因此：

- 阶段 1、2 中，I2C ISR 不调用任何 FreeRTOS API，可保持优先级 1。
- 阶段 4 改用 `xTaskNotifyFromISR()` 后，I2C1 事件和错误中断必须改为库优先级 5--15，例如 6。
- 优先级 0--4 的 ISR 不得调用 `xQueueSendFromISR()`、`xTaskNotifyFromISR()` 等 FreeRTOS API。
- 所有 `FromISR` 调用必须检查 `xHigherPriorityTaskWoken`，并以 `portYIELD_FROM_ISR()` 结束。

## 阶段记录

### 阶段 0：调研与基线

状态：完成。

- 已确认 FreeRTOS V11.1.0、本机源码位置、当前 AC6 编译器和 Cortex-M4F 目标。
- 已确认 RVDS 端口不能直接用于 AC6。
- 已确认当前 SysTick、SVC、PendSV 占位实现和 I2C 中断优先级。

### 阶段 1：内核编译，不启动调度器

状态：待执行。

1. 将内核文件和候选端口加入新的 Keil 分组。
2. 新增 `FreeRTOSConfig.h`，采用本笔记的最小配置。
3. 加入 `heap_4.c`，但暂不创建任务、不调用 `vTaskStartScheduler()`。
4. 运行 Keil 完整构建，确认端口与 Arm Compiler 6 的兼容性。

验收：0 errors；现有 I2C 固件仍可下载并响应 `F0/03 00`、`F0/03 01`。

### 阶段 2：最小调度器

状态：待执行。

1. 修改 SVC、PendSV、SysTick 异常入口。
2. 创建一个低优先级心跳任务，仅递增调试计数。
3. 启动调度器；Idle task 必须正常运行。
4. 通过调试器检查任务状态、堆余量和任务栈高水位。

验收：复位后不进 HardFault；心跳计数增长；HAL tick 增长；尚不要求 I2C 服务任务化。

### 阶段 3：I2C 轮询迁入任务

状态：待执行。

1. 创建 I2C 服务任务，替代 `main()` 中的 `while (1)`。
2. 初期任务持续调用 `i2c_slave_poll()`，不从 ISR 调用 FreeRTOS API。
3. 保持 I2C IRQ 优先级为 1。

验收：Jetson 执行回显、身份、`F0/03 00`、`F0/03 01`、非法 type 全部通过；BUSY 重试逻辑不变。

### 阶段 4：I2C ISR 通知任务

状态：待执行。

1. 在 I2C STOP 收到完整写帧后调用 `xTaskNotifyFromISR()`。
2. I2C 服务任务使用 `ulTaskNotifyTake()` 阻塞等待，不再忙轮询。
3. I2C1 EV/ER 优先级改为 6，并启用 `configASSERT` 检查。

验收：连续请求无漏帧；BUSY 响应正确；I2C ISR 不再因为轮询任务占用 CPU 而延迟处理。

### 阶段 5：传感器和诊断任务化

状态：待执行。

1. 将耗时的 DHT11/ADC 读取放入受控任务或命令处理路径。
2. 保证 I2C ISR 只收发字节和唤醒任务，不执行传感器读取。
3. 按实测高水位调整每个任务栈和 `configTOTAL_HEAP_SIZE`。

验收：传感器命令、256 字节帧、连续请求、STM32 复位恢复和长时间运行测试通过。

## 每阶段必须记录

- 修改的文件和 Keil 分组。
- `FreeRTOSConfig.h` 的实际宏值。
- Keil 输出中的 Code/RO/RW/ZI 与 error/warning 数量。
- 每个任务的优先级、栈深度、栈高水位和堆剩余量。
- I2C EV/ER 的 NVIC 优先级及是否调用 `FromISR` API。
- Jetson 实测命令、原始响应帧和失败现象。
