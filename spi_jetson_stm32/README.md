# Jetson Orin Nano / STM32F407 I2C 项目

当前正式通信使用 Jetson I2C 主机和 STM32F407 I2C1 从机：Jetson J12 Pin 5/SCL 接 PB6，Pin 3/SDA 接 PB7，Pin 6 共地；从机 7 bit 地址固定为 `0x42`，设备节点为 `/dev/i2c-7`。两线为开漏 3.3 V 总线，使用 Jetson 侧约 2.2 kOhm 上拉，不接 DONE GPIO。

帧格式、CRC 和命令号沿用原协议。每次交互先由 Jetson 写完整命令帧，随后读取 256 字节响应窗口；STM32 未完成时返回 `STATUS=0x06`，主机只重试读取，绝不重发写命令。详细约定见 [协议文档](PROTOCOL.md) 顶部的 v0.4 I2C 章节。

三份核心说明并列放在 `docs/`：

| 文档 | 内容 |
|---|---|
| [主从总体说明](docs/MASTER_SLAVE.md) | 系统角色、接线、两次 SPI 时序、帧概览和主从协作 |
| [STM32 从机说明](docs/STM32.md) | 固件文件、硬件资源、中断/后台流程、Keil 构建和命令扩展 |
| [Jetson 主机说明](docs/JETSON.md) | Linux 编译、spidev 使用、参数、返回码和联调方法 |
| [IRQ 命令中断设计](docs/IRQ_INTERRUPT_DESIGN.md) | IRQ 管脚、1 ms 脉宽、取消边界和固定 5 字节中断响应 |
| [SPI 测试步骤与记录](docs/SPI_TEST_PLAN.md) | 协议基线、Jetson 回环、板间回显、压力测试及 DONE/IRQ 验证顺序 |
| [I2C 配置及测试记录](docs/I2C_TEST_RECORD.md) | PB6/PB7 选型、Jetson I2C7 配置、最小从机与双向读写实测结果 |

需求边界见 [项目需求说明](REQUIREMENTS.md)，线上字段见 [协议文档](PROTOCOL.md)，代码入口和依赖方向见 [代码目录与模块结构](CODE_STRUCTURE.md)。

第一次接触 SPI 主从配置时，先阅读 [SPI 主从入门笔记](docs/BEGINNER_SPI_NOTES.md)。

## 软件测试

在本目录执行 `make test`，可在 macOS/Linux 上测试共享协议和业务状态转换。

现有 I2C 链路的实测范围见 [I2C 配置及测试记录](docs/I2C_TEST_RECORD.md)。协议单元测试不能替代真实 I2C 上板验证。

Jetson 回环使用 `build/spi_loopback`，把 J12 Pin 19/MOSI 与 Pin 21/MISO 短接后运行 `./build/spi_loopback /dev/spidev0.0 1000 100000`。该工具检查全双工 TX/RX 数据一致性；SCK 实际频率和边沿仍需逻辑分析仪确认。

## Jetson 主机

主机端文件和执行流程见 [Jetson 主机说明](docs/JETSON.md)。

目标系统需提供 C 编译器和 Linux I2C 开发头文件。在 Jetson 上本目录执行：

```sh
make
./build/i2c_request /dev/i2c-7 1 0 0x30 0xff 0x0a
```

命令参数为设备、CMDID、SUBCMD、零个或多个载荷字节；十六进制需要 `0x` 前缀。示例预期输出为 `status=0 data[3]: 30 FF 0A`。返回码 0=成功，1=设备/传输/响应校验错误，2=参数错误，3=从机报告业务错误。

响应为 `60 STATUS RESULT[N] CRC_LO CRC_HI 0A`，逻辑长度为 5～256 字节。I2C 读取固定请求 256 字节，逻辑帧后的 `0xFF` 是填充。单次写操作失败时执行状态可能不确定，程序不自动重试。

## Windows 本地命令客户端

本机可通过 SSH 调用 Jetson 上已编译的 `build/i2c_request`，无需在 Windows 直接访问 I2C。常用手动命令：

```powershell
& "D:\conda\python.exe" tools\jetson_command_client.py arm-cortex-temp
& "D:\conda\python.exe" tools\jetson_command_client.py light
& "D:\conda\python.exe" tools\jetson_command_client.py identity
& "D:\conda\python.exe" tools\jetson_command_client.py echo 0x30 0xFF 0x0A
```

脚本可作为模块导入，命名函数例如 `getArmCortexTemp()`、`getLightLevel()`、`getDht11Temp()`、`getDeviceIdentity()` 和 `echo(payload)`。`getArmCortexTemp()` 和 `getLightLevel()` 分别发送 `F0/03` 的 DATA=`00`、DATA=`01`，并校验读帧 RESULT 的首字节 type。函数不需要位置参数；如需指定 Jetson 或 I2C 设备，可传输参数，例如 `getArmCortexTemp(host="jetson", device="/dev/i2c-7")`。原始命令可使用 `raw CMDID SUBID [BYTE ...]`。

## STM32 从机

从机端文件、硬件资源和后台处理流程见 [STM32 从机说明](docs/STM32.md)。

打开 `stm32/i2c_slave.uvprojx`，在 Keil MDK 中编译和下载。所需 HAL、CMSIS 和原例程系统依赖已随项目放入 `vendor/hal_example`，工程使用项目内部相对路径，可以整体移动。

目标沿用 STM32F407ZGTx、8 MHz HSE、168 MHz 配置；其他 STM32F4 型号/晶振需要修改目标、启动文件和时钟。PB6/PB7 使用 I2C1 AF4 开漏复用，I2C1 事件与错误中断专用；不得配置为推挽输出或启用内部上下拉。

`stm32/main.c` 是入口；`stm32/i2c_slave.c` 在 I2C 中断收集命令并在 STOP 后发布，后台轮询完成校验、回调和响应组帧；`common/service.c` 负责传输无关的命令处理；`common/commands.c` 提供 CMDID/SUBCMDID 命令表；`common/protocol.c` 是主从共享编解码。

计划增加的 IRQ 命令中断使用 STM32 PB1/EXTI1 接 Jetson J12 Pin 22，主机输出高脉冲至少保持 1 ms。中断后的 `CMD_INTERRUPTED_ERR` 返回固定 5 字节，只需 5 个 `0xFF` 提供读时钟。详见 [IRQ 命令中断设计](docs/IRQ_INTERRUPT_DESIGN.md)；当前代码尚未实现。

可以运行 `python3 tools/generate_keil.py` 重新生成工程，该操作覆盖生成的 `stm32/i2c_slave.uvprojx`，保留 vendor 中的模板。只有刷新厂商依赖时才需要原始示例，使用 `python3 tools/import_hal.py [原例程目录]`。原始厂商代码的版权与许可沿用原文件。

## 验证范围

当前测试结果见 [验证记录](VALIDATION.md)。主机单元测试不代表已完成上板联调。硬件验收项目见协议文档末尾；需要确认真实 Jetson pinmux/片选时序，以及 STM32 DMA 首字节预装和连续事务行为。
