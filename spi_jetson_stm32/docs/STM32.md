# STM32F407 从机说明

本目录实现 STM32F407 的 SPI1 从机通信层。它负责通过 DMA 接收 Jetson 写命令，在 CS 上升沿保存一份稳定的事务快照，并在主循环中交给共享协议层解析和执行。协议格式和业务命令本身位于 `../common`。

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
| SPI 接收 DMA | DMA2 Stream0 Channel3 |
| SPI 发送 DMA | DMA2 Stream3 Channel3 |

通信使用 Mode 0、8 bit、MSB first。两块板必须共地并使用 3.3 V 电平；建议给 NSS 外接 10 kΩ 上拉。上述引脚、DMA 流和 EXTI4 不能再被其他模块占用。

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

当前没有 READY 握手引脚，Jetson 两次事务之间暂定至少等待 10 ms。因此回调不能长期阻塞；需要耗时操作时，应先扩展 READY/完成通知或异步任务协议。

协议单元测试不覆盖真实 SPI 时序。下载后仍需用 Jetson 和逻辑分析仪验证 CS 窗口、首字节、DMA 长度、连续事务与错误恢复。
