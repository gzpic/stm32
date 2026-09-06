# DONE 握手管脚设计

状态：设计稿；本版确定推荐板级管脚，但尚未改变当前默认通信代码。

## 设计目标

在不改变现有两次 SPI 操作和帧格式的前提下，增加一个可选的从机完成通知。该信号命名为 DONE，由 STM32 驱动、Jetson 采样：

| 电平 | 含义 |
|---|---|
| 高 | DONE：STM32 已完成当前阶段，响应已准备好，或已恢复到可接收下一条命令 |
| 低 | NOT_DONE：STM32 正在接收、解析、执行、准备响应、读事务进行中或复位初始化中 |

这是一个单向信号：STM32 驱动，Jetson 只作为输入采样。它不是 SPI 数据，也不替代 CS；CS 仍然是每次 SPI 事务的边界。名称使用 DONE，不再使用 BUSY 管脚命名，避免把低电平状态当作另一个主动握手命令。

## 最终推荐管脚

| 信号 | STM32F407 | Jetson Orin Nano 40-pin J12 | 电气方向 |
|---|---|---|---|
| DONE | PB0，普通 GPIO 推挽输出 | Pin 18，GPIO 输入，当前可作为 `gpiochip0` line 125 / `PY.03` 检查 | STM32 -> Jetson |
| GND | GND | 任意 GND | 共地 |

DONE 使用 3.3 V 电平。建议外接 10 kΩ 下拉到 GND，使 STM32 复位、下载、未初始化或断线时 Jetson 不会误读为 DONE。STM32 初始化早期先把 PB0 配成输出低电平，只有 SPI 从机和 DMA 已装载、或响应已准备好时才拉高。

## 管脚选择依据

STM32 侧选择 PB0 的依据：

- 当前 SPI 默认占用 PA4=NSS、PA5=SCK、PA6=MISO、PA7=MOSI，DONE 不能复用这些线。
- 本地寄存器版 `实验25 SPI实验` 的 SPI Flash 资源为 PB3=SCK、PB4=MISO、PB5=MOSI、PB14=CS，PB0 不属于这组原始 SPI Flash 线。
- DONE 是 STM32 输出到 Jetson 输入，不需要 STM32 端 EXTI，也不需要定时器/复用功能；普通 GPIO 输出即可。
- PB0 在当前 `spi_jetson_stm32` 工程中未被使用，新增影响面小。

Jetson 侧选择 J12 Pin 18 的依据：

- 计划启用的 40-pin SPI 功能是 `spi1 (19,21,23,24,26)`，Pin 18 不在这组 SPI 线上，不会占用 SCK/MOSI/MISO/CS。
- 目标机检查结果显示 Pin 18 当前为 `unused`。Jetson.GPIO 的板型表中 Pin 18 对应 `PY.03`，可用 `gpioinfo gpiochip0` 查到 line 125 当前为 unused input。
- Pin 26 虽然在 SPI 接线附近，但属于 `spi1` 的 CS1 候选，不能拿来做 DONE；Pin 13/16/18/22/37 属于另一组 `spi3` 候选，在不启用 `spi3` 时可作为 GPIO 候选，其中 Pin 18 物理位置接近 SPI 区域，接线较短。

如果实际板卡上 PB0 没有引出，备选顺序是 PB1、PC0、PC1 等普通空闲 GPIO；如果 Jetson Pin 18 已被其他功能占用，优先改用 Pin 22 或 Pin 16。任何替换都要同时更新板级表、Jetson-IO/gpioinfo 记录和实测结果。

## 时序

1. STM32 复位、初始化或 DMA 异常时保持 DONE=低。
2. SPI 从机接收已重新装载、没有待处理请求时置 DONE=高，表示可开始写事务。
3. Jetson 发起写事务前，必须观察 DONE=高；CS 拉低后 STM32 立即置 DONE=低。
4. CS 上升沿中断只保存 DMA 快照并保持 DONE=低；后台完成校验、回调和响应 DMA 准备后置 DONE=高。
5. Jetson 观察到 DONE=高 后发起读事务；读事务期间再次置 DONE=低，读 CS 上升沿后恢复接收并在确认状态后置 DONE=高。
6. DONE 等待必须带超时；超时只报告通信未就绪，不自动重发可能已经执行的写命令。

DONE 的状态变化应在对应 SPI CS 边沿之外完成建立时间。Jetson 不得把一次事务前残留的高电平当作本次命令已经完成；实现时应在发起写事务前清空/记录 GPIO 边沿事件，按“低→高”完成周期确认，或使用等价的序号/状态机制。

## 软件边界

- 共享协议层不读取或生成 DONE 字节，帧格式保持不变。
- STM32 SPI 驱动只调用抽象接口 `done_set(0/1)`；接口由板级适配层实现。
- Jetson 主机把 DONE 作为可选配置；未启用 DONE 时保持现有 10 ms 兼容等待。
- 板级配置至少包含：STM32 端 GPIO 端口/编号和复位默认状态、Jetson 物理排针号、Linux GPIO chip/line、pinmux 状态、电平标准、外部上下拉及是否与其他外设复用。

## 配置和检查方法

STM32 侧实现 DONE 时，PB0 配置为普通推挽输出，初始化阶段先输出低电平。只在 `arm()` 完成 SPI/DMA 接收准备或响应 DMA 已准备好后输出高电平；CS 进入事务时输出低电平。

Jetson 侧实现 DONE 时，J12 Pin 18 配置为 GPIO 输入。用 Jetson-IO 确认没有启用会占用 Pin 18 的 `spi3`，再用 `gpioinfo` 确认 line 未被其他驱动占用：

```sh
sudo python3 /opt/nvidia/jetson-io/config-by-function.py -l enabled
sudo python3 /opt/nvidia/jetson-io/config-by-pin.py -p 18
gpioinfo gpiochip0 | grep 'line 125'
```

预期 Pin 18 不被 `spi3` 或其他功能占用，`gpioinfo` 显示 `PY.03` 为 unused input。若设备树更新后 line 号变化，以 Jetson-IO 和 `gpioinfo` 的实际输出为准。

## 启用前验收

必须先提交实际两端资料和测量记录：

- STM32 开发板完整型号/版本、PB0 的连接网络、排针针脚和已装模块；
- Jetson 载板型号、J12 Pin 18、Jetson-IO 当前功能和 `gpioinfo` 的占用状态；
- 两端均为 3.3 V、无反向驱动；复位、掉电和拔线时不会把 Jetson 输入误判为 DONE；
- 示波器/逻辑分析仪确认 CS 与 DONE 的先后关系、响应准备时间和超时行为。

这些证据齐全后，可以把 PB0/J12 Pin 18 写入代码作为默认 DONE 管脚。当前最终设计结论是：DONE=STM32 PB0 输出，接 Jetson J12 Pin 18 输入，低为未完成，高为完成/可继续。
