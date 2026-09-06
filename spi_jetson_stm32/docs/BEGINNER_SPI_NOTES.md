# SPI 主从入门笔记

本文给第一次接触本项目的人使用。它解释“哪一端配置什么”，以及一次命令从线缆到回调函数经历了什么。本文不新增协议字段，也不指定 READY/BUSY 物理管脚。

## 1. 先分清主方和从方

| 角色 | 本项目设备 | 谁产生时钟 | 谁控制 CS |
|---|---|---|---|
| 主方 | Jetson Orin Nano | Jetson | Jetson |
| 从方 | STM32F407 | 等待 Jetson | 由 Jetson 拉低/拉高 |

SPI 从方不能自己开始一帧数据；没有 Jetson 的 SCK 和 CS，从方不会完成一次事务。

## 2. 先接四根信号线和地线

当前项目的对应关系是：

```text
Jetson MOSI  ───────── STM32 PA7 / SPI1_MOSI
Jetson MISO  ───────── STM32 PA6 / SPI1_MISO
Jetson SCK   ───────── STM32 PA5 / SPI1_SCK
Jetson CS0   ───────── STM32 PA4 / SPI1_NSS
Jetson GND   ───────── STM32 GND
```

两端都使用 3.3 V 逻辑，不能把 5 V 信号直接接到 STM32 或 Jetson。CS 低电平表示当前 SPI 窗口有效，高电平表示窗口结束。

Jetson 40 针排针的 SPI0 默认对应 J12-19/21/23/24，但是否已通过 pinmux 启用必须在目标机确认；设备节点存在不代表排针已经输出 SPI。

## 3. 从方初始化顺序

打开 `stm32/spi_slave.uvprojx` 后，代码入口是 `stm32/main.c`：

1. `HAL_Init()` 初始化 HAL 和 SysTick。
2. `sys_stm32_clock_init(336, 8, 2, 7)` 设置 8 MHz 外部晶振、168 MHz 系统时钟。
3. `spi_slave_init()` 配置 SPI1、DMA2、CS 上升沿中断和协议服务。
4. 主循环只调用 `spi_slave_poll()`，不在中断中做 CRC 或业务处理。

## 4. STM32 GPIO 配置分别做什么

`spi_slave_init()` 先打开 GPIOA、SYSCFG、DMA2 和 SPI1 时钟，然后把 PA4～PA7 配置为 SPI1 的复用功能 AF5：

| 引脚 | SPI 作用 | 电气方向 |
|---|---|---|
| PA4 | NSS/CS，另接 EXTI4 观察上升沿 | 输入 |
| PA5 | SCK | 输入 |
| PA6 | MISO | STM32 输出 |
| PA7 | MOSI | STM32 输入 |

GPIO 使用推挽复用、高速、无普通上下拉；PA4 额外启用内部上拉，避免主机复位或断线时 CS 漂浮。PA4 保持 SPI 复用模式，同时由 EXTI4 感知 CS 从低变高。

## 5. STM32 SPI 参数

代码把 SPI1 设置为从机、硬件 NSS、Mode 0、8 bit、MSB first，并关闭硬件 CRC：

```text
主从角色：STM32 从机
CPOL/CPHA：0/0（Mode 0）
数据宽度：8 bit
位序：MSB first
NSS：硬件输入，低有效
SPI CRC：关闭，协议 CRC16 由软件计算
```

这些参数必须和 Jetson 的 spidev 设置一致；SPI 频率由主方决定，当前主方是 100 kHz。

## 6. 为什么还要 DMA

SPI 每收到一个字节就会产生数据，如果用普通中断逐字节搬运，容易在连续时钟下丢字节。当前从方使用 DMA：

| DMA | 方向 | 配置 |
|---|---|---|
| DMA2 Stream0 Channel3 | SPI1->内存 | 接收，普通模式，递增内存 |
| DMA2 Stream3 Channel3 | 内存->SPI1 | 发送，普通模式，递增内存 |

接收缓冲区是 257 字节：协议允许的最大写帧是 256 字节，多出的 1 字节用于发现超长事务。缓冲区必须位于普通 SRAM，不能放在 CCM，因为 F407 DMA 不能访问 CCM。

## 7. CS 上升沿中断做什么

Jetson 每次释放 CS 后，STM32 进入 `EXTI4_IRQHandler()`。这个中断只做四件事：

1. 清除 EXTI4 挂起标志；
2. 停止 DMA，读取实际收到的字节数和硬件错误；
3. 把 DMA 缓冲区复制到 `command_buffer`；
4. 设置 `ready`，然后立即退出中断。

中断不做 CRC、不解析 CMDID/SUBCMDID、不调用回调。这样可以缩短关中断时间，也避免业务代码阻塞 SPI。

## 8. 后台轮询做什么

主循环中的 `spi_slave_poll()` 发现 `command_buffer.ready` 后，调用共享服务层：

```text
收到快照
  → 检查 0x30 帧头、0x0A 帧尾、总长度和 CRC16
  → 查 CMDID 命令组
  → 查 SUBCMDID 子命令
  → 执行回调
  → 回调填写结果，最后填写 STATUS
  → 组装 0x60 响应并准备发送 DMA
```

处理完成前不会重新打开接收 DMA，因此新的事务不会覆盖当前命令；额外事务只计入丢弃计数。读事务结束后，驱动重新初始化 SPI 和 DMA，等待下一次写事务。

## 9. 初学者的最小检查清单

- 两端共地，信号都是 3.3 V；
- Jetson 的 SPI pinmux 已启用，设备节点与实际片选一致；
- Jetson 使用 Mode 0、8 bit、MSB first；
- STM32 工程目标为 STM32F407ZGTx，PA4～PA7 没有被其他代码重新初始化；
- DMA2 Stream0/3 和 EXTI4 没有被其他模块占用；
- 第一次 CS 窗口发送完整写帧，第二次 CS 窗口发送 `0xFF` 读取响应；
- 先用 100 kHz 和默认回显命令，再提高频率或增加业务回调；
- 用逻辑分析仪先确认 SCK、MOSI、MISO、CS 的方向和两个独立 CS 窗口。

## 10. 代码入口索引

| 要看什么 | 文件 |
|---|---|
| Jetson 打开和配置 SPI | `jetson/main.c` |
| STM32 GPIO、SPI、DMA、CS 中断 | `stm32/spi_slave.c` |
| STM32 启动和后台轮询 | `stm32/main.c` |
| 帧编解码与 CRC | `common/protocol.c` |
| 命令查找和回调 | `common/commands.c` |
| 主从整体时序 | `docs/MASTER_SLAVE.md` |
