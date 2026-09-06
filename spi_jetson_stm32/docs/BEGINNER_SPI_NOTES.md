# SPI 基础配置记录（重记版）

本记录只描述当前代码已经做的事情。READY/BUSY 和具体扩展板管脚暂不写入。

## 一、从方启动入口

文件：`stm32/main.c`

```text
HAL_Init()
  → sys_stm32_clock_init(336, 8, 2, 7)
  → spi_slave_init()
  → while(1) spi_slave_poll()
```

也就是说，SPI 从方先完成系统时钟，再初始化 SPI 外设，最后由主循环处理收到的命令。

## 二、从方使用的 SPI 引脚

文件：`stm32/spi_slave.c`

| STM32 引脚 | 用途 |
|---|---|
| PA4 | SPI1_NSS/CS，低有效，同时观察 CS 上升沿 |
| PA5 | SPI1_SCK |
| PA6 | SPI1_MISO |
| PA7 | SPI1_MOSI |

PA4～PA7 都配置为 SPI1 的 AF5 复用功能。PA4 额外打开内部上拉，减少 CS 在主机未驱动时的漂浮。

## 三、从方 SPI 参数

STM32 SPI1 被配置为：

```text
从机、硬件 NSS、SPI Mode 0、8 bit、MSB first、关闭 SPI 硬件 CRC
```

协议中的 CRC16 是软件计算的校验字段，不是 SPI 外设的硬件 CRC 功能。

## 四、为什么使用 DMA

SPI 字节流由 DMA 自动搬运，不在每个字节上进入 CPU 中断：

| DMA | 作用 |
|---|---|
| DMA2 Stream0 Channel3 | SPI1 接收，写入 SRAM 缓冲区 |
| DMA2 Stream3 Channel3 | 从响应缓冲区发送到 SPI1 |

接收缓冲区为 257 字节，用于识别超过协议最大长度的事务；DMA 缓冲区必须放在普通 SRAM，不能放在 CCM。

## 五、CS 上升沿中断

Jetson 将 CS 拉高表示本次 SPI 窗口结束，STM32 进入 `EXTI4_IRQHandler()`。

中断只负责：

1. 停止 DMA；
2. 记录实际接收长度和硬件错误；
3. 复制数据到 `command_buffer`；
4. 发布 `ready` 标志并退出。

中断不做 CRC、命令查找和回调。

## 六、后台处理

`spi_slave_poll()` 在主循环中执行：

```text
取出 buffer
  → 检查帧头、帧尾、声明长度和 CRC
  → 查 CMDID
  → 查 SUBCMDID
  → 执行回调
  → 回调填写 RESULT，最后填写 STATUS
  → 准备 0x60 响应
```

这样可以把“快速中断”和“可能耗时的业务处理”分开。

## 七、初学者检查顺序

1. 确认 Jetson 和 STM32 共地，信号电平均为 3.3 V。
2. 确认 MOSI、MISO、SCK、CS 没有接反。
3. 确认 Jetson SPI pinmux 已启用，并且使用了正确的 `/dev/spidevB.C`。
4. 确认 Jetson 和 STM32 都是 Mode 0、8 bit、MSB first。
5. 用逻辑分析仪确认 CS 拉低期间有 SCK，且一次命令有两个独立 CS 窗口。
6. 先以 100 kHz 测试，再考虑提高频率或增加业务命令。

## 八、代码入口

| 内容 | 文件 |
|---|---|
| STM32 启动和主循环 | `stm32/main.c` |
| GPIO、SPI、DMA、CS 中断 | `stm32/spi_slave.c` |
| Jetson spidev 初始化和传输 | `jetson/main.c` |
| 帧和 CRC | `common/protocol.c` |
| 命令表和回调 | `common/commands.c` |
