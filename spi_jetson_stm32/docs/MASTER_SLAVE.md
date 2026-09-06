# Jetson 与 STM32 SPI 主从总体说明

本文从系统角度说明 Jetson Orin Nano 主机与 STM32F407 从机如何协作。平台专用的构建、硬件资源和代码细节分别见同目录的 [STM32 从机说明](STM32.md) 与 [Jetson 主机说明](JETSON.md)。线上字段的完整定义见 [帧协议](../PROTOCOL.md)，需求来源及暂定参数见 [需求说明](../REQUIREMENTS.md)。

## 系统角色

| 节点 | SPI 角色 | 主要职责 |
|---|---|---|
| Jetson Orin Nano | 主机 | 控制 SCK/CS，构造并发送命令，提供读时钟，解析 STM32 响应 |
| STM32F407 | 从机 | DMA 接收命令，在 CS 上升沿保存快照，后台校验并执行回调，准备响应 |

SPI 从机不能主动发起传输。每次完整交互由 Jetson 发起两次独立 SPI 事务：第一次发送写命令，第二次产生时钟并读取响应。

## 接线和默认参数

```text
Jetson SCK   ───────── STM32 PA5 / SPI1_SCK
Jetson MOSI  ───────── STM32 PA7 / SPI1_MOSI
Jetson MISO  ───────── STM32 PA6 / SPI1_MISO
Jetson CS    ───────── STM32 PA4 / SPI1_NSS + EXTI4
Jetson DONE  ───────── STM32 PB0 / DONE
Jetson IRQ   ───────── STM32 PB1 / EXTI1（计划）
Jetson GND   ───────── STM32 GND
```

当前使用 3.3 V、SPI Mode 0、8 bit、MSB first、100 kHz、CS 低有效。DONE 为 STM32 输出到 Jetson 输入，高电平表示完成/可继续，低电平表示未完成。当前代码尚未启用 DONE，启用前仍按 10 ms 间隔运行。Jetson 的实际排针、SPI 控制器和 `/dev/spidevB.C` 取决于载板、JetPack 和 pinmux。

### 管脚选择依据

当前固件默认选择 STM32F407 的 SPI1 标准硬件 NSS 组合：PA4=NSS、PA5=SCK、PA6=MISO、PA7=MOSI，全部为 AF5。选择这组管脚的主要原因是 PA4 可以作为 SPI1 硬件 NSS，同时接 EXTI4 检测 CS 上升沿，和本项目“以 CS 边沿界定一次事务”的从机设计一致，代码改动和时序风险最低。

本地寄存器版 `实验25 SPI实验` 使用的是另一组 SPI1 复用管脚：PB3=SCK、PB4=MISO、PB5=MOSI，PB14 为软件 GPIO 片选。该例程面向 STM32 作为主机访问板载 W25Q Flash，不是 Jetson 主机、STM32 从机结构；PB14 也不是 SPI1 硬件 NSS。因此这组管脚只能作为“PA4～PA7 不方便接出时”的备选方案。若改用 PB3/PB4/PB5/PB14，需要同步修改 `stm32/spi_slave.c` 的 GPIO、NSS/SSM、EXTI14、中断入口和文档，并确认板载 SPI Flash 不会与 Jetson 共用总线造成冲突。

实际板级选择顺序：优先使用 PA4/PA5/PA6/PA7；若开发板排针不方便或被其他外设占用，再评估 PB3/PB4/PB5/PB14；任何改管脚都必须用示波器或逻辑分析仪确认 SCK、MOSI、MISO、CS 时序。

## 两次 SPI 事务

```text
Jetson                           STM32
  │                                │
  ├─ CS↓，发送 0x30 写命令 ───────→│ DMA 接收
  ├─ CS↑ ─────────────────────────→│ 中断保存命令快照
  │                                │ 后台校验、查表、执行回调
  │         等待至少 10 ms          │ 组装 0x60 变长响应
  ├─ CS↓，发送 256 个 0xFF ───────→│ 提供 MISO 响应字节
  │←──── 逻辑响应 + 0xFF 填充 ─────┤
  ├─ CS↑ ─────────────────────────→│ 消费响应并恢复接收
  │                                │
```

写命令帧：

```text
30 CMDID LEN_LO LEN_HI SUBCMDID DATA[N] CRC_LO CRC_HI 0A
```

返回帧：

```text
60 STATUS RESULT[N] CRC_LO CRC_HI 0A
```

返回逻辑帧为 5～256 字节，不固定为 36 字节。回调填写 RESULT 和实际长度，最后填写 STATUS。因为返回帧没有长度字段，而 SPI 主机必须预先决定时钟数，Jetson 第二次事务统一产生 256 字节时钟；逻辑帧后的 `0xFF` 是物理填充，不属于响应和 CRC。

## STM32 处理边界

CS 上升沿中断只停止 DMA、记录接收长度和硬件错误、复制事务数据到单帧 buffer，然后设置 ready 并退出。CRC、帧解析、命令查找和业务回调全部在主循环后台执行。

后台先检查写帧头、帧尾、实际长度、声明总长度和 CRC。校验通过后，先按 CMDID 找命令组，再按 SUBCMDID 找组内回调。回调处理业务，填写返回数据与长度，并在最后填写状态。协议层检查返回长度，生成帧头、CRC 和帧尾，再由 SPI DMA 在第二次事务中送出。

当前使用单帧邮箱，只支持串行的一写一读。buffer 占用期间的新事务不会覆盖正在处理的命令。DONE 完成通知推荐使用 STM32 PB0 输出接 Jetson J12 Pin 18 输入；语义、启用条件和板级配置要求见 [DONE 握手管脚设计](READY_BUSY_DESIGN.md)。当前代码尚未启用 DONE，默认仍依赖 10 ms 间隔。

## 返回状态

| STATUS | 含义 |
|---|---|
| `0x00` | 命令执行成功 |
| `0x01` | 帧格式、长度、CRC 或传输错误 |
| `0x02` | CMDID/SUBCMDID 没有匹配命令 |
| `0x03` | 命令参数错误 |
| `0x04` | 命令执行失败，或回调遗漏状态 |
| `0x05` | `CMD_INTERRUPTED_ERR`，IRQ 请求中止命令（计划，当前未实现） |

主机应先验证返回帧头、帧尾、CRC 和帧后填充，再读取 STATUS。STATUS 非 `0x00` 时，不应将 RESULT 当成成功数据使用。写传输失败后不自动重试，因为 STM32 可能已经执行该命令。

计划中的 IRQ 由 Jetson Pin 22 输出到 STM32 PB1/EXTI1，高电平至少保持 1 ms。中断响应固定为 `60 05 E9 B3 0A`，主机仅用 5 个 `0xFF` 读取，不走普通响应的 256 字节填充。收帧阶段与执行阶段的取消边界见 [IRQ 命令中断管脚设计](IRQ_INTERRUPT_DESIGN.md)。

## 代码对应关系

| 流程 | Jetson | 共享层 | STM32 |
|---|---|---|---|
| 生成写帧 | `jetson/main.c` | `common/protocol.c` | — |
| SPI 收发 | `jetson/main.c` | — | `stm32/spi_slave.c` |
| CS 上升沿快照 | — | — | `EXTI4_IRQHandler()` |
| 校验与服务状态 | — | `common/protocol.c`、`common/service.c` | `spi_slave_poll()` 调用 |
| 命令查找与回调 | — | `common/commands.c` | 后台执行 |
| 生成并解析响应 | `jetson/main.c` | `common/protocol.c` | `common/service.c` 组帧 |

完整目录关系见 [代码目录与模块结构](../CODE_STRUCTURE.md)。

## 当前验证范围

协议、CRC、0～251 字节返回数据、命令双重匹配、错误回调长度、响应边界及内存检查已有自动测试；Jetson ARM64 程序已完成编译。STM32 工程已用 Arm Compiler 6 完整构建过。

实际接线后的 SPI 电气与时序仍需上板验证，包括两个 CS 窗口、DMA 实际长度、最短/最长响应、`0xFF` 填充、连续命令、错误恢复及提高频率后的稳定性。验证记录见 [VALIDATION.md](../VALIDATION.md)。
