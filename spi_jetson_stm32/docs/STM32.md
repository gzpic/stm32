# STM32F407 从机说明

本目录实现 STM32F407 的 SPI1 从机通信层。它负责通过 DMA 接收 Jetson 写命令，在 CS 上升沿保存一份稳定的事务快照，并在主循环中交给共享协议层解析和执行。协议格式和业务命令本身位于 `../common`。

面向初学者的 GPIO、SPI、DMA 和中断配置顺序见 [SPI 主从入门笔记](BEGINNER_SPI_NOTES.md)。

主从之间的完整交互先看 [主从总体说明](MASTER_SLAVE.md)；Jetson 侧操作见 [Jetson 主机说明](JETSON.md)。

## 文件职责

| 文件 | 作用 |
|---|---|
| `main.c` | 初始化 HAL、168 MHz 系统时钟和 SPI 从机，然后持续调用非阻塞后台轮询 |
| `spi_slave.h` | 对固件入口公开 `spi_slave_init()` 和 `spi_slave_poll()` |
| `spi_slave.c` | 配置 SPI1、DMA2 和 EXTI4，处理 CS 上升沿、命令 buffer、DMA 收发和异常计数 |
| `spi_slave.uvprojx` | STM32F407ZGTx 的 Keil MDK 工程，当前使用 Arm Compiler 6 |

Keil 工程还会编译 `../common` 中的协议、服务和命令代码，并引用 `../vendor/hal_example` 中的 HAL、CMSIS、启动文件及系统时钟代码。

## 硬件资源

| 功能 | STM32F407 引脚/资源 |
|---|---|
| NSS / CS | PA4，低有效，EXTI4 检测上升沿 |
| SCK | PA5，SPI1 AF5 |
| MISO | PA6，SPI1 AF5 |
| MOSI | PA7，SPI1 AF5 |
| DONE | PB0，普通 GPIO 推挽输出，高表示完成/可继续，低表示未完成 |
| IRQ（计划） | PB1，普通 GPIO 输入下拉 + EXTI1 上升沿，Jetson 高脉冲至少 1 ms |
| SPI 接收 DMA | DMA2 Stream0 Channel3 |
| SPI 发送 DMA | DMA2 Stream3 Channel3 |

通信使用 Mode 0、8 bit、MSB first。两块板必须共地并使用 3.3 V 电平；建议给 NSS 外接 10 kΩ 上拉，DONE 外接 10 kΩ 下拉。上述引脚、DMA 流和 EXTI4 不能再被其他模块占用。当前代码尚未启用 DONE，启用时按 [DONE 握手管脚设计](READY_BUSY_DESIGN.md) 实现。

## 管脚选择方法

当前代码使用 PA4/PA5/PA6/PA7，而不是本地寄存器版 `实验25 SPI实验` 中的 PB3/PB4/PB5/PB14。选择方法如下：

| 方案 | 管脚 | 适用性 | 说明 |
|---|---|---|---|
| 当前默认 | PA4=NSS, PA5=SCK, PA6=MISO, PA7=MOSI | 首选 | SPI1 AF5 完整硬件 NSS 组合；PA4 可同时路由到 EXTI4，用于 CS 上升沿收帧 |
| 原 SPI Flash 例程 | PB3=SCK, PB4=MISO, PB5=MOSI, PB14=CS | 备选 | `实验25 SPI实验` 用这组管脚访问 W25Q Flash；PB14 是软件片选，不是 SPI1 硬件 NSS |

本地寄存器例程的依据在 `E:\Coding\stm32\4，程序源码\1，标准例程-寄存器版本\实验25 SPI实验`：

- `readme.txt` 写明 SPI1_SCK=PB3、SPI1_MISO=PB4、SPI1_MOSI=PB5、SPI1_CS=PB14。
- `Drivers\BSP\SPI\spi.h` 定义 SPI1 的 SCK/MISO/MOSI 为 PB3/PB4/PB5，AF5。
- `Drivers\BSP\NORFLASH\norflash.h` 定义 NORFLASH_CS 为 PB14。
- `Drivers\SYSTEM\sys\sys.c` 中的 `sys_gpio_set()` 用于配置 GPIO 模式、速度、输出类型和上下拉；`sys_gpio_af_set()` 用于写 AFR 选择 AF0～AF15，其中注释列出 AF5 对应 SPI1/SPI2/I2S2；`sys_nvic_ex_config()` 用于把 GPIOx 的某一位映射到 EXTI 线。

该例程是 STM32 主机访问板载 Flash，`spi.c` 中启用了主机模式、软件 NSS：MSTR=1、SSM=1、SSI=1。当前项目要求 STM32 做从机，并在 CS 上升沿结束 DMA 事务，因此优先使用 PA4 硬件 NSS + EXTI4 的组合。

### STM32 当前配置步骤

当前项目不复用寄存器版 `spi.c` 的主机初始化，而是在 `stm32/spi_slave.c` 中重新配置 SPI1 从机：

1. 打开 GPIOA、SYSCFG、DMA2、SPI1 时钟。
2. 将 PA4/PA5/PA6/PA7 配置为 `GPIO_MODE_AF_PP`、`GPIO_SPEED_FREQ_VERY_HIGH`、`GPIO_AF5_SPI1`。
3. 对 PA4 再配置上拉，降低 Jetson 未驱动 CS 时误触发的风险；板级上仍建议外接 10 kΩ 上拉。
4. 将 PA4 路由到 EXTI4，只开上升沿中断；CS 从低到高时认为本次 SPI 事务结束。
5. 配置 DMA2 Stream0/Channel3 接收 SPI1->DR，DMA2 Stream3/Channel3 发送到 SPI1->DR。
6. 配置 SPI1 为从机、硬件 NSS、Mode 0、8 bit、MSB first、关闭硬件 CRC。

若按原例程 PB 组重做配置，GPIO 复用仍是 AF5，但 PB14 不能按硬件 NSS 使用。此时配置方法会变成：PB3/PB4/PB5 设为 SPI1 AF5；PB14 设为普通输入上拉并映射到 EXTI14；SPI1 改成软件 NSS 从机，CS 的有效/结束完全由 PB14 电平和 EXTI14 处理。这个改法必须同步实测 MODF、首字节预装、DMA 停止时序和板载 Flash 总线冲突。

如果实际开发板上 PA4～PA7 不方便接线，允许改用 PB3/PB4/PB5/PB14，但这不是只改接线即可完成。至少需要同步修改：

- `spi_slave_init()` 的 GPIO 端口和管脚配置。
- SPI1 从机 NSS 策略：PB14 作为普通 GPIO 片选时，SPI 需改成软件 NSS 从机配置。
- EXTI 从 EXTI4 改到 EXTI14，并调整 SYSCFG 路由和 IRQHandler。
- 文档、Keil 工程资源说明和上板验证记录。

改用 PB 组前还要确认板载 W25Q Flash 或其他模块没有挂在同一组 SPI 线上；如果挂在一起，Jetson 驱动这组线时可能和 Flash 或原电路互相影响。

## 从机运行流程

1. `spi_slave_init()` 初始化引脚、SPI1、DMA2、EXTI4、命令服务和收发缓冲区。
2. Jetson 拉低 CS 并发送完整写命令，DMA 自动把 MOSI 字节写入接收数组。
3. CS 上升沿进入 `EXTI4_IRQHandler()`；中断停止 DMA，记录实际长度和硬件错误，将数据复制到单帧 `command_buffer`，发布 `ready` 后立即退出。
4. 主循环中的 `spi_slave_poll()` 发现 `ready`，调用 `service_transaction()` 完成格式、长度和 CRC 校验，再按 CMDID/SUBCMDID 执行回调。
5. 回调填写返回数据和实际长度，最后填写执行状态。协议层生成以 `0x60` 开始的变长返回帧，并重新装载发送 DMA；逻辑帧之后保持 `0xFF` 填充。
6. Jetson 第二次拉低 CS 产生时钟并读取响应。读事务结束后，从机恢复下一条写命令的接收状态。

中断不计算 CRC、不查命令表、不执行业务。当前是单帧邮箱设计；后台处理期间到来的额外事务不会覆盖当前命令，只增加 `spi_slave_dropped_transactions`。DMA 无法在限定次数内停止时增加 `spi_slave_dma_stop_faults`，并拒绝执行不完整命令。这两个全局计数可在调试器中观察。

## 编译与下载

使用 Keil MDK 打开 `stm32/spi_slave.uvprojx`，选择 `Jetson_SPI_Slave` 目标后编译并下载。工程使用项目内部相对路径，整个 `spi_jetson_stm32` 目录可以移动，但不能只复制 `stm32` 而丢失 `common` 和 `vendor`。

当前目标参数为 STM32F407ZGTx、8 MHz 外部晶振、168 MHz 系统时钟。更换芯片型号或晶振后，应同时检查 Keil 目标、启动文件、时钟参数、内存布局、DMA 映射和引脚复用。

如需重新生成工程，在项目根目录运行：

```sh
python3 tools/generate_keil.py
```

该命令会覆盖生成的 `stm32/spi_slave.uvprojx`。不要直接编辑生成工程中的文件列表后又运行生成脚本，否则手工改动会丢失。

## 添加业务命令

一般不需要修改 `spi_slave.c`。在 `../common/commands.c` 中增加回调并注册 CMDID/SUBCMDID。回调先填写 `response->data` 和 `response->size`，必须在所有退出路径最后填写 `response->status`；返回数据不得超过 251 字节，也不得保存请求/响应指针供回调结束后使用。

DONE 完成通知推荐使用 PB0 输出，接 Jetson J12 Pin 18。当前代码尚未启用该 GPIO；启用前仍按 10 ms 兼容等待运行。实现 DONE 后，PB0 初始化为低电平，只有 SPI/DMA 已准备好或响应已生成后才拉高，CS 进入事务时重新拉低。

计划中的 IRQ 使用 PB1/EXTI1，接 Jetson J12 Pin 22。EXTI1 只锁存取消请求：收帧阶段废弃当前 DMA 帧，执行阶段由业务回调在安全点检查并收尾，禁止从中断服务里强制跳出回调。IRQ 高电平至少保持 1 ms，中断响应固定 5 字节。完整规则见 [IRQ 命令中断管脚设计](IRQ_INTERRUPT_DESIGN.md)，当前代码尚未实现。

协议单元测试不覆盖真实 SPI 时序。下载后仍需用 Jetson 和逻辑分析仪验证 CS 窗口、首字节、DMA 长度、连续事务与错误恢复。
