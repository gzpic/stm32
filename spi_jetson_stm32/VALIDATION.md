# 验证记录

日期：2026-09-05；环境：macOS。

| 检查 | 结果 |
|---|---|
| `make test` | 通过：CRC 标准向量、0～248 字节写载荷、256 字节总长度小端编码、逐位破坏校验、特殊字节、响应及重新同步 |
| AddressSanitizer / UndefinedBehaviorSanitizer | 通过：同一协议/状态转换测试，无报告 |
| 新增 `stm32/main.c`、`stm32/spi_slave.c` Cortex-M4 语法检查 | 通过，非完整链接 |
| 生成 Keil 工程所有源文件路径存在性 | 通过 |
| Keil 完整编译和下载 | 未执行：本机无 Keil |
| Jetson Linux 主机程序编译 | 未执行：当前为 macOS，缺少 Linux SPI 开发环境 |
| 实际 SPI 通信、DMA、NSS 时序 | 未执行：无连接硬件 |

ARM 语法检查使用本地 HAL/CMSIS 设备头和 Clang `--target=arm-none-eabi -mcpu=cortex-m4 -mthumb`。原例程仅提供 Keil 相关 CMSIS 编译器头，因此检查时在临时目录补充官方 [CMSIS 5.9.0 cmsis_gcc.h](https://github.com/ARM-software/CMSIS_5/blob/5.9.0/CMSIS/Core/Include/cmsis_gcc.h)，没有改动原例程；对厂商 ADC 内联函数关闭 unused-parameter 告警，其余使用 `-Wall -Wextra -Werror`。这不能替代实际 Keil 构建验证。

当前默认 CRC 算法、字段字节序、固定响应长度、示例业务和 10 ms 时序约定均见 `PROTOCOL.md`；这些是补充设计，需与实际通信对象保持一致。

协议层更新验证：命令表测试覆盖 CMDID 与 SUBCMDID 双重匹配、未知组/子命令不触发回调、坏 CRC/坏尾/硬件错误不触发回调、读事务完成后保留自定义命令表。`make test` 与 AddressSanitizer/UndefinedBehaviorSanitizer 均通过。中断快照更新后，STM32 源码再次通过同条件 Cortex-M4 语法检查。单帧邮箱的真实中断并发、DMA 停止延迟及溢出计数行为仍需上板验证。

返回帧更新验证：回调改为最后填写 `command_response.status`。测试确认回调填写的成功状态和数据进入响应、未填写状态时返回 `0x04`、修改状态字节会导致 CRC 校验失败。更新后 `make test` 与 AddressSanitizer/UndefinedBehaviorSanitizer 均通过。没有改变线上帧长度或现有字段偏移，未执行目标固件完整编译和上板联调。

2026-09-06 上传前复查：已将依赖纳入 `vendor/hal_example`，重新生成 Keil 工程并检查引用路径。新增数字解析及合法 CRC/非法声明长度测试通过；内存检查与基于随项目依赖的 Cortex-M4 语法检查通过。详见 `REVIEW.md`。新增 Linux CI 用于在目标操作系统上编译主机程序及持续检查，实际运行结果以 GitHub Actions 为准。
