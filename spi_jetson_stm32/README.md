# Jetson Orin Nano / STM32F407 SPI 新项目

先阅读 [项目需求说明](REQUIREMENTS.md)，了解已确认需求、当前实现约定及验收范围，再查看 [协议文档](PROTOCOL.md)。本版包含协议编解码、示例回显业务、Linux 主机命令行程序和 STM32F407 SPI1 DMA 从机。

## 软件测试

在本目录执行 `make test`，可在 macOS/Linux 上测试共享协议和业务状态转换。

## Jetson 主机

目标系统需提供 C 编译器和 Linux SPI 开发头文件。在 Jetson 上本目录执行：

```sh
make
./build/spi_request /dev/spidevB.C 1 0 0x30 0xff 0x0a
```

将 `/dev/spidevB.C` 替换为实际启用的 SPI 设备。命令参数为设备、CMDID、SUBCMD、零个或多个载荷字节；十六进制需要 `0x` 前缀。示例预期输出 `status=0 data: 30 FF 0A 00 ...`（共 31 个数据字节）。程序每次执行一次写、一次读，100 kHz、Mode 0、8 bit、MSB first。返回码 0=成功，1=设备/传输/响应校验错误，2=参数错误，3=从机报告业务错误。

响应为 `60 STATUS RESULT[31] CRC_LO CRC_HI 0A`，共 36 字节。回调先填返回数据，最后填执行状态，后台随后计算 CRC；具体字段与状态码见协议文档。不同长度的业务响应尚未约定。单次写操作失败时执行状态可能不确定，程序不自动重试。

## STM32 从机

打开 `stm32/spi_slave.uvprojx`，在 Keil MDK 中编译和下载。所需 HAL、CMSIS 和原例程系统依赖已随项目放入 `vendor/hal_example`，工程使用项目内部相对路径，可以整体移动。没有复制历史 HEX 作为新固件。

目标沿用 STM32F407ZGTx、8 MHz HSE、168 MHz 配置；其他 STM32F4 型号/晶振需要修改目标、启动文件和时钟。PA4/5/6/7 用于 SPI1，DMA2 Stream0/3 与 EXTI4 专用。新增 SPI 数据路径使用 CMSIS 寄存器控制 DMA；系统初始化及 GPIO 复用现有 HAL。不可同时初始化原来 SPI 主机驱动或占用这些 DMA/中断。

`stm32/main.c` 是新入口；`stm32/spi_slave.c` 在 CS 上升沿中断保存快照，后台轮询消费；`common/service.c` 负责校验与响应；`common/commands.c` 提供 CMDID/SUBCMDID 命令表和业务回调；`common/protocol.c` 是主从共享编解码。详见 [协议层设计与添加命令](PROTOCOL_LAYER.md)。DMA 缓冲区保持在 SRAM1/2，不能放在 CCM。主循环需满足协议文档的 10 ms 片选间隔，暂不包含 READY 引脚握手。

可以运行 `python3 tools/generate_keil.py` 重新生成工程，该操作覆盖生成的 `stm32/spi_slave.uvprojx`，保留 vendor 中的模板。只有刷新厂商依赖时才需要原始示例，使用 `python3 tools/import_hal.py [原例程目录]`。原始厂商代码的版权与许可沿用原文件。

## 验证范围

当前测试结果见 [验证记录](VALIDATION.md)。主机单元测试不代表已完成上板联调。硬件验收项目见协议文档末尾；需要确认真实 Jetson pinmux/片选时序，以及 STM32 DMA 首字节预装和连续事务行为。
