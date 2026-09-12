# SPI 测试步骤与记录

状态：第一步协议测试基线已通过；第二步 Jetson 回环的数据一致性已通过；第三步 Jetson/STM32 板间 SPI 未通过。由于没有逻辑分析仪，尚未定位到具体信号线或时序原因，当前不能把板间 SPI 记为可用。测试按“纯软件基线 -> Jetson 回环 -> Jetson/STM32 基本通信 -> 异常与压力 -> DONE/IRQ”顺序进行。

## 测试分层

| 步骤 | 测试对象 | 是否需要硬件 | 主要结论 |
|---|---|---|---|
| 1 | 共享协议、CRC、命令分发和服务状态 | 否 | 建立修改前的软件基线 |
| 2 | Jetson spidev 与 MOSI/MISO 回环 | Jetson、跳线、逻辑分析仪 | 确认 pinmux、Mode 0、位序、频率和 CS 窗口 |
| 3 | Jetson 与 STM32 回显通信 | 两块板、逻辑分析仪 | 确认真实 SPI1、DMA、EXTI4 和两次事务 |
| 4 | 错误帧、边界、连续传输和频率扫描 | 两块板、逻辑分析仪 | 验证错误恢复和稳定工作范围 |
| 5 | DONE 与 IRQ | 两块板、额外 GPIO、逻辑分析仪 | 验证握手、取消和固定 5 字节中断响应 |

“单元测试通过”只能证明纯 C 逻辑符合预期，不能证明 SPI 引脚、电气、pinmux、DMA 首字节和 CS 时序正确。基本 SPI 验收必须至少完成步骤 1～3。

## 第一步：建立协议测试基线

### 目标

在不连接 Jetson 和 STM32 的条件下，确认当前仓库的帧编解码、CRC、命令分发和响应状态是稳定的。以后每次修改 SPI 状态机、DONE 或 IRQ 前后都运行同一组测试，若结果变化，应先解释变化再上板。

### 已有测试资产

- `tests/test_protocol.c`：协议和服务测试入口。
- `Makefile`：生成 `build/test_protocol` 并执行。
- `.github/workflows/spi-check.yml`：Linux 编译、普通测试和 ASan/UBSan 内存检查。

现有用例已经覆盖：

- CRC16/MODBUS 标准向量 `123456789 -> 0x4B37`。
- 写 DATA 长度 0～248 字节及最大 256 字节写帧。
- 返回 RESULT 长度 0～251 字节及 5～256 字节逻辑响应。
- DATA/RESULT 中包含 `0x30`、`0xFF`、`0x0A`。
- 坏帧尾、坏 CRC、声明长度错误和传输错误。
- CMDID/SUBCMDID 双重匹配、未知命令和回调执行次数。
- 回调遗漏 STATUS、返回长度越界、未读响应消费和合法新写覆盖旧响应。
- 响应任意 bit 被破坏后 CRC 校验失败。

### 执行环境准备

优先在 Jetson 或任意 Linux 环境执行，需要：

```sh
cc --version
make --version
```

当前 Windows 工作机的普通终端没有可直接调用的 `make`、`gcc` 或 `clang`，WSL 当前也不可用，因此不能把本机未执行写成测试通过。仓库已有 GitHub Actions，推送后也可用 Linux CI 运行同一基线。

### 普通基线测试

进入项目目录后执行：

```sh
cd spi_jetson_stm32
make clean
make test
```

通过标准：

```text
protocol/service tests passed
```

同时要求进程返回码为 0、编译无 warning（当前使用 `-Wall -Wextra -Werror`），不能只看到最后一行文字就判定通过。

### 内存安全测试

普通测试通过后执行：

```sh
cc -std=c99 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -Icommon \
  tests/test_protocol.c \
  common/protocol.c common/service.c common/commands.c \
  -o build/test_protocol_sanitized
./build/test_protocol_sanitized
```

通过标准：输出 `protocol/service tests passed`，进程返回码为 0，并且 AddressSanitizer、UndefinedBehaviorSanitizer 没有报告。

### 第一步执行记录

执行时填写，不使用以前机器上的结果代替本次基线：

| 项目 | 本次记录 |
|---|---|
| Git 提交 | `a059d9c`（测试源码基线；测试目录由本地工作区复制） |
| 执行设备/系统 | `jetson`；Linux 5.15.148-tegra；aarch64；Ubuntu 22.04 用户环境 |
| C 编译器及版本 | GCC 11.4.0；GNU Make 4.3 |
| `make test` | 通过；`-Wall -Wextra -Werror` 编译无告警，输出 `protocol/service tests passed` |
| ASan/UBSan | 通过；返回码 0，无 sanitizer 报告 |
| 开始时间 | Jetson 设备时钟 `2026-03-15T00:30:28+08:00` |
| 原始输出保存位置 | Jetson `/home/chen/stm32-spi-test/test-results/step1-20260315.log` |
| 日志 SHA-256 | `6bdaa1d3a752483d8977205e79ca57068622615e63e9dc5c6e33693abe5c295c` |
| 结论 | 第一步通过，可以准备第二步 Jetson SPI 回环 |

注意：执行测试时 Jetson 系统时间显示 2026-03-15，与当前工作机日期不一致。记录保留设备原始时间；后续上板测试前应先确认 Jetson 时间同步状态。

第一步停止条件：任一断言失败、编译 warning、非零返回码或 sanitizer 报告。发生时先保留完整输出并定位软件问题，不进入 Jetson 回环和板间测试。

## 第二步：Jetson SPI 回环

断开 STM32，确认 Jetson 断电后再将所选 SPI 的 MOSI 与 MISO 短接。上电后用测试程序发送固定模式和不同长度，接收数据必须与发送数据逐字节一致：

```text
00 FF 30 0A 55 AA
```

至少测试 1、2、8、32、256 字节，各循环 1000 次。逻辑分析仪同时确认 Mode 0、SCK 空闲低、上升沿采样、每字节 8 个时钟、100 kHz 初始频率以及每次 ioctl 只有一个 CS 低窗口。回环只能确认 Jetson 主机侧，不能确认 STM32。

本项目提供专用回环工具，不能使用会忽略第一次 RX 数据并等待 STM32 协议响应的 `spi_request` 代替：

```sh
make build/spi_loopback
./build/spi_loopback /dev/spidev0.0 10 100000
./build/spi_loopback /dev/spidev0.0 1000 100000
```

参数依次为设备节点、每种长度的循环次数和请求频率。工具依次测试 1、2、6、8、32、256 字节，每次使用变化的数据模式，首轮包含 `00 FF 30 0A 55 AA`。任一短传输或字节不一致立即失败并报告长度、轮次、偏移和 TX/RX 值。

### 第二步执行记录

| 项目 | 本次记录 |
|---|---|
| Jetson pinmux | `spi1 (19,21,23,24,26)` 已启用；19=`spi1_dout`，21=`spi1_din` |
| 接线 | J12 Pin 19 MOSI 与 Pin 21 MISO 短接；STM32 未参与 |
| 设备和配置 | `/dev/spidev0.0`；Mode 0；8 bit；MSB first；请求 100 kHz |
| 冒烟测试 | 6 种长度各 10 次，全部通过 |
| 正式测试 | 6 种长度各 1000 次，共 6000 次事务，全部通过 |
| 数据结论 | TX/RX 逐字节一致，短传输和 mismatch 均为 0 |
| 总耗时 | `3.309 s`；明显短于请求 100 kHz 下 305000 字节所需的理论 24.4 s |
| 波形结论 | 待逻辑分析仪确认实际 SCK、Mode 0 边沿、时钟数和 CS 窗口，当前不得声称实际频率为 100 kHz |
| 原始日志 | Jetson `/home/chen/stm32-spi-test/test-results/step2-loopback-20260315.log` |
| 日志 SHA-256 | `95f941544cf54bb60086a94771507b93f0c63c834a1ff27e6b92efb35543041d` |
| 当前结论 | 数据回环通过；第二步等待逻辑分析仪波形验收 |

### 补充测试：STM32 无 DMA 基础从机

当正式 DMA/EXTI 从机无法完成第三步时，先使用独立工程 `stm32/spi_basic_test.uvprojx` 排除协议和 DMA 影响。该固件使用 PA5=SCK、PA6=MISO、PA7=MOSI以及 RXNE 中断，并将软件 NSS 保持为选中状态以单独验证时钟和数据线；PA4只作为输入观察 CS。Mode 0、8 bit、MSB first。每收到一个字节，PA6固定返回 `0xA5`，最多保留复位后收到的前 64 个字节。

构建并下载 `stm32/OutputBasic/spi_basic_test.hex` 后，在 Jetson 执行：

```sh
make build/spi_basic_test
./build/spi_basic_test /dev/spidev0.0 byte
```

Jetson 分 32 个单字节事务发送 `00` 到 `1F`，事务间隔 10 ms；通过标准是 RX 32 字节全部为 `A5`，并输出 `PASS`。事务完成后可暂停 STM32，在 Keil Watch 查看：

```text
basic_spi_transaction_count
basic_spi_nonempty_transaction_count
basic_spi_empty_transaction_count
basic_spi_last_size
basic_spi_last_rx
basic_spi_overrun_count
```

正确接收时 `basic_spi_last_size=32`，`basic_spi_last_rx[0..31]=00..1F`，且 `basic_spi_overrun_count=0`。传输期间不能停在 SPI1 中断断点，否则会人为制造溢出；应在全部事务完成后暂停查看。该固件仅用于低速接线诊断，不能替代正式 DMA 从机的功能和压力测试。

### 板间 SPI 定位过程与结果

本轮定位按从主机到从机、从复杂固件到最小固件的顺序进行，过程和边界如下。

1. **软件协议基线**：Jetson 上的协议、CRC、命令分发和响应测试通过，排除了纯 C 协议逻辑的明显错误，但该步骤不接触硬件 SPI。
2. **Jetson pinmux 与设备节点**：启用 J12 的 `spi1 (19,21,23,24,26)`，确认 `/dev/spidev0.0` 可用。后续还启用了第二组 `spi3 (13,16,18,22,37)` 作为交叉检查；设备节点存在只说明驱动已注册，不等于排针波形正确。
3. **Jetson 自回环**：短接 Pin 19/MOSI 与 Pin 21/MISO，1、2、6、8、32、256 字节共 6000 次事务全部逐字节一致。这证明所选 Jetson SPI1 的数据输出、数据输入和 spidev 传输路径能形成闭环。
4. **正式 STM32 DMA 从机**：按 PA4=NSS、PA5=SCK、PA6=MISO、PA7=MOSI 接线并共地。Jetson 收到的数据不能组成合法响应；Keil/DAP 观察到接收数组为 `0x00`，有效接收长度在不同尝试中为 0 或极小值，未得到完整命令帧。因此不能把 Jetson 端显示的变化字节解释为 STM32 返回值，它们可能来自未被有效驱动的 MISO、残留数据或错误采样。
5. **去除 DMA 和协议层**：新增 `spi_basic_test.uvprojx`，只使用 SPI1 RXNE 中断收字节并固定返回 `0xA5`。测试仍未观察到稳定的接收计数和 `00..1F` 数据，说明问题不只位于正式协议解析或 DMA 收尾逻辑。
6. **DAP 与固件检查**：CMSIS-DAP 能连接 STM32F407ZGTx，最小固件能够编译、整片擦除、下载和复位；因此下载器连接和 MCU 基本运行条件成立。调试时只在事务结束后暂停，避免断点本身阻塞 SPI 中断。
7. **万用表静态检查**：检查了两板共地、3.3 V 电源以及相关 GPIO 的高低电平切换，静态电压未发现明显电源错误。更换跳线和改用另一组候选 SCK/SPI 管脚后，STM32 端仍未收到有效字节。万用表只能显示平均或静态电压，不能确认 SCK 是否有正确脉冲、边沿、频率和 CS 建立/保持时间。
8. **频率疑点**：Jetson 程序请求 100 kHz，但回环总耗时表明实际控制器频率明显更高；目标机曾显示最低实际 SPI 时钟约为 3.1875 MHz。该频率必须以逻辑分析仪或示波器实测为准，不能继续按请求值 100 kHz 推导中断和时序裕量。

本轮已经排除或确认的范围：

| 项目 | 结果 |
|---|---|
| 协议纯软件测试 | 通过 |
| Jetson SPI1 pinmux、spidev 与 MOSI/MISO 回环 | 数据一致性通过 |
| STM32 工程编译、DAP 下载和 MCU 复位 | 通过 |
| 电源、共地和静态 3.3 V 电平 | 未发现异常 |
| Jetson 到 STM32 的 SCK/CS 是否到达正确引脚 | 未确认 |
| SCK 实际频率、Mode 0 边沿和 CS 时序 | 无分析仪，未确认 |
| STM32 板间 SPI 接收与固定 `0xA5` 返回 | 未通过 |
| 正式 DMA、协议响应、DONE 和 IRQ | 未进入验收 |

**最终结论：Jetson 自回环可用，但 Jetson/STM32 板间 SPI 尚未打通。** 现有证据不足以在 SCK、CS、排针映射、实际频率和从机采样之间继续缩小范围。后续若恢复 SPI，应先用逻辑分析仪同时观察 Jetson 端与 STM32 引脚处的 CS、SCK、MOSI、MISO；第一目标是确认最小固件收到单字节并返回 `0xA5`，之后才恢复 DMA 和正式协议。

## 第三步：Jetson 与 STM32 基本通信

按接线文档连接 PA4=NSS、PA5=SCK、PA6=MISO、PA7=MOSI并共地，先不接 DONE 和 IRQ。使用现有 `CMDID=0x01、SUBCMDID=0x00` 回显命令：

1. 发送空 DATA，验证最短 8 字节写帧。
2. 发送 `00 FF 30 0A 55 AA`，验证特殊字节原样返回。
3. 发送 248 字节 DATA，验证最大写帧和 DMA 长度。
4. 连续执行 1000 次，记录成功数、CRC 错误、超时、短传输及 STM32 丢弃计数。

逻辑分析仪必须看到两个独立 CS 低窗口：第一个窗口发送完整写帧，第二个窗口产生响应读取时钟。当前普通响应仍读取 256 字节；逻辑响应后的 `0xFF` 是填充。

## 第四步：异常、边界和压力

依次注入坏 CRC、错误 LEN、截断写帧、257 字节超长事务、读事务长度错误和处理中额外事务。确认错误命令不执行、响应状态正确、DMA 能恢复且下一条合法命令成功。保持 Mode 0 不变，再从 100 kHz 逐档提高频率；每档至少连续 1000 次，以零错误的最高档作为候选频率，并由逻辑分析仪复核。

无 DONE 时再测试事务间隔 10、5、2、1 ms。10 ms 是当前协议基线；更短间隔只有在连续测试和波形均通过后才能记录为实测能力，不能直接改成协议保证。

## 第五步：DONE 与 IRQ

基本 SPI 完全通过后才接入 PB0/DONE 和 PB1/IRQ。DONE 测试覆盖复位默认低、写前就绪、处理期间低、响应就绪高和读后恢复。IRQ 测试分别在 CS 低的收帧阶段、CS 高的执行阶段和普通响应未读阶段触发，检查至少 1 ms 高脉冲、原命令停止/结果作废、固定响应 `60 05 E9 B3 0A`、恰好 40 个读时钟以及高优先级命令随后成功。

IRQ 当前只有设计文档，代码尚未实现；在实现状态值、EXTI1、协作取消和 5 字节读事务前，该步骤必须保持“待执行”。

## 结果保存规则

每次上板测试至少保存：Git 提交号、两端固件/程序版本、接线照片、Jetson pinmux 和设备节点、测试参数、终端原始输出、逻辑分析仪原始采样文件及结论。摘要追加到 `VALIDATION.md`，大体积波形文件不要直接提交仓库，可记录其外部存放位置和校验值。
