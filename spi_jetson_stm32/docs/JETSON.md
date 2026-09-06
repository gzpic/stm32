# Jetson Orin Nano 主机说明

本目录实现 Linux `spidev` 主机程序。程序根据命令行参数构造写命令帧，完成第一次 SPI 写事务；等待 STM32 后台处理后，再执行第二次 SPI 事务读取变长逻辑响应并校验。帧编解码复用 `../common/protocol.c`。

主从之间的完整交互先看 [主从总体说明](MASTER_SLAVE.md)；从机硬件和处理流程见 [STM32 从机说明](STM32.md)。

## 文件职责

| 文件 | 作用 |
|---|---|
| `main.c` | 打开并锁定 spidev、配置 SPI、发送写帧、读取返回帧、校验并打印结果 |
| `parse_number.h` | 将命令行中的十进制或带 `0x` 前缀的十六进制参数转换成字节 |

## 编译

在 Jetson 上进入项目根目录执行：

```sh
make clean
make all
```

生成的 ARM64 程序位于：

```text
build/spi_request
```

运行 `make test` 可以测试共享协议、CRC、命令分发和服务状态，不需要连接 STM32。该测试不能代替真实 SPI 联调。

## 使用方法

```sh
./build/spi_request /dev/spidevB.C CMDID SUBCMDID [DATA_BYTE ...]
```

CMDID、SUBCMDID 和每个数据字节可以写成十进制或带 `0x` 前缀的十六进制。示例：

```sh
./build/spi_request /dev/spidev0.0 1 0 0x30 0xff 0x0a
```

程序会完成以下操作：

1. 生成 `30 CMDID LEN_LO LEN_HI SUBCMDID DATA CRC_LO CRC_HI 0A` 写帧。
2. 以一次独立 SPI 事务发送完整写帧，并释放 CS。
3. 等待 10 ms，给 STM32 后台解析和执行命令。
4. 再发送 256 个 `0xFF` 占位字节以覆盖最大帧时钟，同时从 MISO 读取响应和帧后填充。
5. 找出并校验 `60 STATUS RESULT[N] CRC_LO CRC_HI 0A` 的真实长度，输出状态和 N 字节结果。

`0xFF` 只是第二次事务中 MOSI 的时钟占位值；STM32 返回帧头是 `0x60`。

## 默认 SPI 参数

| 参数 | 当前值 |
|---|---|
| 模式 | Mode 0 |
| 位宽 | 8 bit |
| 位顺序 | MSB first |
| 频率 | 100 kHz |
| CS | 低有效，每次系统调用对应一个独立片选窗口 |
| 启动等待 | 打开设备并配置后等待 100 ms |
| 写后等待 | 10 ms |
| 读后等待 | 10 ms |

这些值必须和 STM32 端及协议文档保持一致。DONE 完成通知当前尚未在代码中启用；启用前主机继续使用 10 ms 兼容等待，不访问任意 GPIO。管脚选择和启用条件见 [DONE 握手管脚设计](READY_BUSY_DESIGN.md)。

## 输出与返回码

成功时输出类似：

```text
status=0 data: 30 FF 0A 00 00 ...
```

| 进程返回码 | 含义 |
|---|---|
| 0 | SPI 响应有效，STM32 报告命令成功 |
| 1 | 打开、配置、传输或返回帧校验失败 |
| 2 | 命令行参数错误 |
| 3 | 返回帧有效，但 STM32 报告业务错误 |

程序对设备文件申请非阻塞独占锁，避免本程序的多个实例同时访问同一从机。SPI 写传输出现不确定结果时不会自动重试，因为命令可能已经在 STM32 上执行。

## 设备与联调

`/dev/spidevB.C` 取决于 Jetson 载板、JetPack 和 pinmux，示例设备名不能直接视为实际配置。先确认对应 SPI 控制器和片选已在设备树中启用，并检查设备节点权限。Jetson 的 SCK/MOSI/MISO/CS 分别连接 STM32 的 PA5/PA7/PA6/PA4，两块板共地。

当前目标机可通过 `ssh jetson` 检查到 Jetson Linux R36.4.7，系统存在 `/dev/spidev0.0`、`/dev/spidev0.1`、`/dev/spidev1.0`、`/dev/spidev1.1`。这只能说明内核有 spidev 节点，不能单独证明 40-pin 排针已经复用为 SPI。

使用 Jetson-IO 检查 40-pin Header：

```sh
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -l enabled
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -l all
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 19
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 21
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 23
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 24
```

本次检查结果显示 40-pin Header 暂无启用功能，19/21/23/24/26 均为 `unused`；Jetson-IO 支持的 40-pin SPI 功能包括 `spi1 (19,21,23,24,26)` 和 `spi3 (13,16,18,22,37)`。因此接线前应先启用 `spi1`，重启后再确认：

```sh
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -o dtbo '1=spi1'
sudo reboot
```

Jetson 侧的管脚复用不由本项目 C 程序设置。`jetson/main.c` 只通过 Linux `spidev` 打开设备节点，并用 `ioctl()` 配置 Mode 0、8 bit、MSB first 和 100 kHz；SCK/MOSI/MISO/CS 实际落到哪个 40-pin 物理针脚，由 Jetson-IO 生成的设备树覆盖和重启后的 pinmux 决定。启用后建议按下面顺序复查：

```sh
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -l enabled
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 19
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 21
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 23
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 24
ls -l /dev/spidev*
```

若这些 pin 仍显示 `unused`，不要接线测试；先修正 Jetson-IO 配置或设备树覆盖。若 spidev 节点存在但 pin 仍未启用，程序可能能打开设备，但 40-pin 排针不会输出预期 SPI 波形。

启用 `spi1` 后，优先使用 J12 物理 19/21/23/24 连接 STM32 默认 PA 组：

| Jetson J12 | Jetson SPI 信号 | STM32 默认信号 |
|---|---|---|
| Pin 19 | MOSI | PA7 / SPI1_MOSI |
| Pin 21 | MISO | PA6 / SPI1_MISO |
| Pin 23 | SCK | PA5 / SPI1_SCK |
| Pin 24 | CS0 | PA4 / SPI1_NSS |
| Pin 18 | GPIO 输入 / DONE | PB0 / DONE |
| Pin 22 | GPIO 输出 / IRQ | PB1 / EXTI1（计划，当前代码未启用） |
| 任意 GND | GND | GND |

若 STM32 端改用本地 `实验25 SPI实验` 的 PB3/PB4/PB5/PB14 备选组，Jetson 侧仍可使用同一组 J12 `spi1` 管脚，但 STM32 固件必须同步改为 PB 组；不能只改接线。

DONE 管脚不是 SPI 片选。它是 STM32 到 Jetson 的普通 GPIO 完成通知：高电平表示 STM32 已完成当前阶段并可继续，低电平表示未完成。当前最终推荐为 STM32 PB0 接 Jetson J12 Pin 18；Pin 18 当前可通过 `gpioinfo gpiochip0 | grep 'line 125'` 检查为 `PY.03` unused input。启用 DONE 前，确认没有启用会占用 Pin 18 的 `spi3`，并给 DONE 线外接 10 kΩ 下拉到 GND。

计划中的 IRQ 方向与 DONE 相反：Jetson J12 Pin 22 输出到 STM32 PB1/EXTI1，空闲低，中断时在物理引脚上保持高电平至少 1 ms（建议 1～2 ms）后恢复低。IRQ 中断响应固定为 5 字节，主机只发送 5 个 `0xFF` 读取。当前一次性 spidev 写事务无法保证在内核传输中途立即停钟和释放 CS，启用前必须先完成可取消传输方案。详见 [IRQ 命令中断管脚设计](IRQ_INTERRUPT_DESIGN.md)。

联调时建议先使用 100 kHz 和默认回显命令，通过逻辑分析仪确认两个独立 CS 低窗口、写帧 `0x30`、返回帧 `0x60`、第二次事务的 256 字节时钟、正确的变长帧尾及至少 10 ms 的高电平间隔，再逐步提高频率或增加业务命令。
