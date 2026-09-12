# 验证记录

日期：2026-09-05；环境：macOS。

| 检查 | 结果 |
|---|---|
| `make test` | 通过：CRC 标准向量、0～248 字节写载荷、256 字节总长度小端编码、逐位破坏校验、特殊字节、响应及重新同步 |
| AddressSanitizer / UndefinedBehaviorSanitizer | 通过：同一协议/状态转换测试，无报告 |
| 新增 `stm32/main.c`、`stm32/spi_slave.c` Cortex-M4 语法检查 | 通过，非完整链接 |
| 生成 Keil 工程所有源文件路径存在性 | 通过 |
| Keil 完整编译 | Windows MDK 5.43a、Arm Compiler 6.24 构建通过：0 errors、25 条厂商依赖告警，生成 AXF 和 HEX |
| Keil 下载/烧录 | 未执行：无连接硬件 |
| Jetson Linux 主机程序编译 | 未执行：当前为 macOS，缺少 Linux SPI 开发环境 |
| 实际 SPI 通信、DMA、NSS 时序 | 未执行：无连接硬件 |

ARM 语法检查使用本地 HAL/CMSIS 设备头和 Clang `--target=arm-none-eabi -mcpu=cortex-m4 -mthumb`。原例程仅提供 Keil 相关 CMSIS 编译器头，因此检查时在临时目录补充官方 [CMSIS 5.9.0 cmsis_gcc.h](https://github.com/ARM-software/CMSIS_5/blob/5.9.0/CMSIS/Core/Include/cmsis_gcc.h)，没有改动原例程；对厂商 ADC 内联函数关闭 unused-parameter 告警，其余使用 `-Wall -Wextra -Werror`。这不能替代实际 Keil 构建验证。

当前默认 CRC 算法、字段字节序、最大响应长度、示例业务和 10 ms 时序约定均见 `PROTOCOL.md`；这些是补充设计，需与实际通信对象保持一致。

协议层更新验证：命令表测试覆盖 CMDID 与 SUBCMDID 双重匹配、未知组/子命令不触发回调、坏 CRC/坏尾/硬件错误不触发回调、读事务完成后保留自定义命令表。`make test` 与 AddressSanitizer/UndefinedBehaviorSanitizer 均通过。中断快照更新后，STM32 源码再次通过同条件 Cortex-M4 语法检查。单帧邮箱的真实中断并发、DMA 停止延迟及溢出计数行为仍需上板验证。

返回帧更新验证：回调改为最后填写 `command_response.status`。测试确认回调填写的成功状态和数据进入响应、未填写状态时返回 `0x04`、修改状态字节会导致 CRC 校验失败。更新后 `make test` 与 AddressSanitizer/UndefinedBehaviorSanitizer 均通过。没有改变线上帧长度或现有字段偏移，未执行目标固件完整编译和上板联调。

v0.3 变长响应：取消固定 36 字节帧。测试覆盖 0～251 字节 RESULT、5～256 字节逻辑帧、RESULT 内含 `0x0A`、帧后 `0xFF` 填充、回调遗漏状态、回调越界长度和逐位破坏 CRC。第二次 SPI 事务改为统一产生 256 字节时钟；真实硬件上的最大读事务和边界识别仍待联调。

2026-09-06 上传前复查：已将依赖纳入 `vendor/hal_example`，重新生成 Keil 工程并检查引用路径。新增数字解析及合法 CRC/非法声明长度测试通过；内存检查与基于随项目依赖的 Cortex-M4 语法检查通过。详见 `REVIEW.md`。新增 Linux CI 用于在目标操作系统上编译主机程序及持续检查，实际运行结果以 GitHub Actions 为准。

v0.2 帧头变更：返回帧头统一为 `0x60`，共享编码和解码同时更新。`make test` 通过，覆盖新帧头生成及旧 `0xFF` 帧头即使 CRC 正确也被拒绝。MOSI 占位值与空闲发送值仍为 `0xFF`；主从必须同步升级，实际硬件联调状态不变。

2026-09-06 Windows MDK5 验证：生成工程由不可用的 Arm Compiler 5.06u7 更新为本机已安装的 Arm Compiler 6.24，随后通过 μVision 5.43a 命令行完整编译和链接。结果为 0 errors、25 warnings；告警来自归档的 HAL、ADC 内联函数和 USART 示例中的未使用参数。产物大小为 Code=10314、RO-data=470、RW-data=16、ZI-data=2224，并成功生成 `spi_slave.axf` 与 `spi_slave.hex`。本次结果验证构建，不代表已完成烧录或 SPI 硬件联调。

第一步 SPI 测试基线：通过 `ssh jetson` 在 aarch64 Jetson（Linux 5.15.148-tegra、GCC 11.4.0、GNU Make 4.3）执行 `make clean`、`make test` 和 ASan/UBSan 测试。普通测试在 `-Wall -Wextra -Werror` 下编译无告警，两次测试均输出 `protocol/service tests passed`、返回码为 0，sanitizer 无报告。原始日志位于 Jetson `/home/chen/stm32-spi-test/test-results/step1-20260315.log`，SHA-256 为 `6bdaa1d3a752483d8977205e79ca57068622615e63e9dc5c6e33693abe5c295c`。Jetson 设备时钟显示 2026-03-15，与当前工作机日期不一致，故保留设备原始时间并要求后续测试前检查时间同步。该结果只建立纯软件协议基线，不代表物理 SPI 已通过。

第二步 Jetson SPI 回环：通过 Jetson-IO 应用并重启后，J12 Header 已启用 `spi1 (19,21,23,24,26)`；Pin 19=`spi1_dout`、Pin 21=`spi1_din`。在 Pin 19/21 短接状态下，新增 `spi_loopback` 通过 `/dev/spidev0.0`、Mode 0、8 bit、MSB first、请求 100 kHz 依次测试 1、2、6、8、32、256 字节。冒烟测试每种长度 10 次通过，正式测试每种长度 1000 次、共 6000 次事务全部通过，无短传输和字节不一致。正式测试总耗时 3.309 s，明显短于请求 100 kHz 传输 305000 字节的理论 24.4 s，故只能确认数据回环正确，实际 SCK 频率、Mode 0 边沿、时钟数和 CS 窗口仍待逻辑分析仪验收。原始日志位于 Jetson `/home/chen/stm32-spi-test/test-results/step2-loopback-20260315.log`，SHA-256 为 `95f941544cf54bb60086a94771507b93f0c63c834a1ff27e6b92efb35543041d`。

2026-09-12 板间 SPI 定位：正式 DMA 从机和无 DMA/RXNE 最小从机均未收到稳定的完整数据，固定 `0xA5` 返回也未通过。DAP 下载、MCU 复位、Jetson 自回环、共地和静态电平均已检查；没有逻辑分析仪，尚不能确认 STM32 引脚处的 SCK、CS、MOSI、MISO 波形。当前只能判定 Jetson 回环可用，不能判定 Jetson/STM32 板间 SPI 可用。详细过程见 `docs/SPI_TEST_PLAN.md`。

2026-09-12 基础 I2C 验证：Jetson J12 Pin 5/SCL、Pin 3/SDA、Pin 6/GND 分别连接 STM32 PB6/I2C1_SCL、PB7/I2C1_SDA、GND；Jetson 使用 `/dev/i2c-7`，STM32 最小 I2C1 从机地址为 `0x42`。读取寄存器 0 得到预置值 `0xA5`，向寄存器 2 写入 `0x5A` 后回读仍为 `0x5A`，DAP 计数和内存值同步变化。基础双向链路通过，正式业务协议及压力、异常和恢复测试尚未执行。详细配置见 `docs/I2C_TEST_RECORD.md`。

2026-09-12 I2C 扩展验证：Keil 重新构建并用 CMSIS-DAP 下载 `i2c_basic_test` 后，连续 16 字节读写、跨 16 字节边界回绕、重复起始读、错误地址 NACK、256 字节写和 256 字节读均通过。随机长度 1--16 字节、随机起始寄存器的 2256 组读写（其中 2000 组压力循环）均无字节错误；连续复位 STM32 5 次后，Jetson 每次均重新读到初始化值 `A5 01`。现有 I2C 物理链路和最小从机中断收发可用于下一阶段的正式协议迁移；正式命令帧、BUSY 状态、超时策略、掉电/拔线恢复和业务并发仍未验收。详细配置及命令见 `docs/I2C_TEST_RECORD.md`。

2026-09-12 正式 I2C 帧读写验证：使用 `tools/i2c_frame_test.py` 通过 Jetson `i2ctransfer` 发送原始命令帧，再以 CMSIS-DAP 读取 STM32 `command_rx` RAM 缓冲区逐字节比较；随后读取 256 字节响应窗口并比较“响应帧加 FF 填充”。空载荷为写 8、读 5 字节逻辑帧；特殊字节载荷为写 13、读 10 字节逻辑帧；248 字节载荷为写 256、读 253 字节逻辑帧，三项均一致。该测试直接覆盖读写帧头、命令号、16 bit 小端长度、子命令、载荷、CRC16、帧尾和读填充的正确性。

2026-09-12 DHT11 温度命令：新增 `CMDID=0xF0`、`SUBCMD=0x01`，使用原实验31的 DHT11 PG9 时序读取，成功时返回 1 字节整数摄氏温度。Keil 构建通过 0 errors、1 条既有 HAL 头文件 warning；Jetson 协议单元测试通过。当前实板连续 3 次、间隔 2 秒调用均返回 `STATUS=0x04`，表明 PG9 未获得有效 DHT11 响应或校验数据，不能将其解释为温度值。需要检查 DHT11 模块已接入 PG9、供电和共地正确。

2026-09-13 板载传感器命令：将内部温度与 LS1 光照统一为 `F0/03`，写帧 DATA 的唯一 type 字节为 `00`（内部温度）或 `01`（光照）；成功读帧 RESULT 的首字节回显相同 type。实板原始 RESULT 分别为 `00 9D 0B`（`29.73°C`）和 `01 13`（光照 `19/100`），本机 Python 客户端也分别读取到 `31.66°C` 与 `19`。`F0/02` 已不再注册。Keil 完整编译、Jetson `make test`、`make all` 和真实 SSH -> Jetson -> I2C -> STM32 链路均通过。内部温度转换沿用原实验20的典型参数，表示 MCU 结温近似值，未经单板校准；光照值不是 lux。详细帧和结果见 `docs/I2C_TEST_RECORD.md`。
