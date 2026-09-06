# SPI 主方与从方基础配置记录（重记版）

本记录只对应当前代码和已确认的 SPI 基础配置。DONE 完成通知已有推荐管脚，但当前代码尚未启用；启用前仍按 10 ms 间隔联调。

## 一、主从分工

| 角色 | 设备 | 负责内容 |
|---|---|---|
| 主方 | Jetson Orin Nano | 提供 SCK，控制 CS，发送写帧，提供读时钟 |
| 从方 | STM32F407 | 接收/发送 SPI 数据，按 CS 边界保存帧并后台处理 |

没有主方的 SCK 和 CS，从方不会自行开始通信。

## 二、实际接线

```text
Jetson MOSI ───── STM32 PA7 / SPI1_MOSI
Jetson MISO ───── STM32 PA6 / SPI1_MISO
Jetson SCK  ───── STM32 PA5 / SPI1_SCK
Jetson CS0  ───── STM32 PA4 / SPI1_NSS
Jetson Pin18 ──── STM32 PB0 / DONE
Jetson GND  ───── STM32 GND
```

两端使用 3.3 V 逻辑。Jetson 排针是否已经启用 SPI pinmux，必须在目标机检查，不能只看 `/dev/spidev*` 是否存在。

DONE 是 STM32 输出到 Jetson 输入的普通 GPIO，不是 SPI 信号。高电平表示 STM32 已完成当前阶段并可继续，低电平表示未完成。DONE 建议外接 10 kΩ 下拉到 GND，避免 STM32 复位或未初始化时 Jetson 误判为完成。

当前 STM32 首选 PA4/PA5/PA6/PA7，因为 PA4 是 SPI1 硬件 NSS，并且可以接 EXTI4 做 CS 上升沿中断。本地寄存器版 `实验25 SPI实验` 的原始接法是 PB3/PB4/PB5/PB14：PB3=SCK、PB4=MISO、PB5=MOSI、PB14=CS。那份例程是 STM32 主机访问板载 W25Q Flash，PB14 是软件片选，不是 SPI1 硬件 NSS；只有 PA 组不方便接线时才考虑改用 PB 组。

## 三、主方：Jetson SPI 基础配置

代码文件：`jetson/main.c`

主方不在 C 程序里直接设置引脚复用；引脚复用由 Jetson 的设备树/Jetson-IO 完成。程序只打开已经存在的 SPI 设备节点，例如 `/dev/spidev0.0`。

Jetson 上先检查 40-pin Header 支持和当前启用状态：

```sh
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -l all
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -l enabled
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 19
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 21
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 23
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 24
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 18
```

本次目标机检查到 40-pin 支持 `spi1 (19,21,23,24,26)` 和 `spi3 (13,16,18,22,37)`，但当前未启用，19/21/23/24 均显示 `unused`。接线前先启用 `spi1`：

```sh
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -o dtbo '1=spi1'
sudo reboot
```

重启后再次检查 `config-by-function.py -l enabled` 和 19/21/23/24 的 pin 状态。只有确认 pinmux 已启用后，再使用 `/dev/spidev0.0` 或实际生成的设备节点测试。

DONE 使用 J12 Pin 18。不要启用会占用 13/16/18/22/37 的 `spi3`；用 `gpioinfo gpiochip0 | grep 'line 125'` 复查 Pin 18 对应的 `PY.03` 仍为空闲输入。

初始化步骤：

1. `open()` 打开 SPI 设备；
2. `flock()` 对设备加独占锁；
3. 通过 ioctl 设置 SPI 参数；
4. 使用 `SPI_IOC_MESSAGE(1)` 进行传输。

当前参数：

```text
模式：SPI_MODE_0
数据宽度：8 bit
位序：MSB first
频率：100 kHz
CS：低有效，由 spidev 自动控制
```

一次命令执行两次独立传输：

```text
第一次：发送完整 0x30 写帧，CS 拉低后再拉高
等待：10 ms
第二次：发送 0xFF 占位字节并读取 0x60 返回帧，CS 再拉低后拉高
```

`transfer()` 中的 `tx_buf`、`rx_buf`、`len`、`speed_hz` 和 `bits_per_word` 组成一次 SPI 事务。一次 ioctl 对应一个 CS 窗口。

## 四、从方：STM32 启动顺序

代码文件：`stm32/main.c`、`stm32/spi_slave.c`

```text
HAL_Init()
  → sys_stm32_clock_init(336, 8, 2, 7)
  → spi_slave_init()
  → while(1) spi_slave_poll()
```

这表示先初始化 HAL 和系统时钟，再初始化 SPI1、DMA 和 CS 中断，最后由主循环处理命令。

## 五、从方 GPIO 与 SPI 配置

| STM32 引脚 | 用途 |
|---|---|
| PA4 | SPI1_NSS/CS，低有效，连接 EXTI4 |
| PA5 | SPI1_SCK |
| PA6 | SPI1_MISO |
| PA7 | SPI1_MOSI |
| PB0 | DONE 输出，高表示完成/可继续 |

PA4～PA7 配置为 SPI1 AF5 复用功能；PA4 额外开启内部上拉。SPI1 参数与主方一致：从机、硬件 NSS、Mode 0、8 bit、MSB first，关闭 SPI 硬件 CRC。协议 CRC16 由软件层计算。

当前代码中的配置方法在 `stm32/spi_slave.c`：

1. 开启 GPIOA、SYSCFG、DMA2、SPI1 时钟。
2. PA4/PA5/PA6/PA7 配成 `GPIO_MODE_AF_PP`、`GPIO_AF5_SPI1`。
3. PA4 额外配置上拉，并通过 SYSCFG 映射到 EXTI4。
4. EXTI4 只打开上升沿；Jetson 释放 CS 时进入 `EXTI4_IRQHandler()`。
5. SPI1 配成从机、硬件 NSS、Mode 0；DMA2 Stream0/3 分别用于接收和发送。

原寄存器例程的配置方法在 `Drivers\BSP\SPI\spi.h`、`Drivers\BSP\SPI\spi.c` 和 `Drivers\SYSTEM\sys\sys.c`：先定义 PB3/PB4/PB5 为 SPI1 AF5，再用 `sys_gpio_set()` 配复用输出，用 `sys_gpio_af_set()` 写 AFR 选择 AF5；`norflash.h` 单独把 PB14 定义为普通 GPIO 片选。该方法服务于主机模式 Flash 访问，不能直接套到当前从机工程。

## 六、从方 DMA 配置

| DMA | 方向 | 作用 |
|---|---|---|
| DMA2 Stream0 Channel3 | SPI1 → 内存 | 接收 MOSI |
| DMA2 Stream3 Channel3 | 内存 → SPI1 | 发送 MISO |

接收缓冲区为 257 字节，用最后 1 字节发现超长事务；DMA 缓冲区必须位于普通 SRAM，不能位于 CCM。

## 七、从方 CS 上升沿和后台

Jetson 拉高 CS 后，STM32 进入 `EXTI4_IRQHandler()`。中断只做：停止 DMA、读取实际长度和错误、复制到 `command_buffer`、设置 `ready`。中断不做 CRC、不查命令、不调用业务回调。

随后 `spi_slave_poll()` 在后台执行：

```text
检查 0x30 帧头、0x0A 帧尾、长度、CRC
  → 查 CMDID
  → 查 SUBCMDID
  → 执行回调
  → 回调填写结果，最后填写 STATUS
  → 组装 0x60 响应并准备发送 DMA
```

## 八、初学者验证顺序

1. 确认共地和 3.3 V 电平。
2. 确认 MOSI、MISO、SCK、CS 没有接反。
3. 确认 Jetson pinmux 已启用，并使用正确的 `/dev/spidevB.C`。
4. 确认两端都是 Mode 0、8 bit、MSB first。
5. 用逻辑分析仪确认一次命令包含两个独立 CS 窗口。
6. 若启用 DONE，确认 Pin 18 没有被 `spi3` 占用，并确认 DONE 默认低电平。
7. 先用 100 kHz 和默认回显命令验证，再增加业务。

## 九、代码入口

| 内容 | 文件 |
|---|---|
| 主方 SPI 打开、参数和传输 | `jetson/main.c` |
| 从方启动和主循环 | `stm32/main.c` |
| 从方 GPIO、SPI、DMA、CS 中断 | `stm32/spi_slave.c` |
| 帧和 CRC | `common/protocol.c` |
| 命令表和回调 | `common/commands.c` |
