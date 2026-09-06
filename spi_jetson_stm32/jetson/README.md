# Jetson Orin Nano 主机代码说明

本目录实现 Linux `spidev` 主机程序。程序根据命令行参数构造写命令帧，完成第一次 SPI 写事务；等待 STM32 后台处理后，再执行第二次 SPI 事务读取变长逻辑响应并校验。帧编解码复用 `../common/protocol.c`。

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

这些值必须和 STM32 端及协议文档保持一致。当前命令行没有提供修改频率和等待时间的选项。

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

联调时建议先使用 100 kHz 和默认回显命令，通过逻辑分析仪确认两个独立 CS 低窗口、写帧 `0x30`、返回帧 `0x60`、第二次事务的 256 字节时钟、正确的变长帧尾及至少 10 ms 的高电平间隔，再逐步提高频率或增加业务命令。
