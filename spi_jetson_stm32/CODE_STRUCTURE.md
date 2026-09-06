# 代码目录与模块结构

本文说明 `spi_jetson_stm32` 的代码边界、依赖方向和主要调用流程。项目当前采用“硬件通信层 → 协议服务层 → 命令业务层”三层结构；Jetson 和 STM32 共享纯 C 的帧编解码代码。

## 目录树

```text
spi_jetson_stm32/
├── README.md                 # 使用入口：构建、运行和硬件范围
├── REQUIREMENTS.md           # 已确认需求、暂定参数和验收条件
├── PROTOCOL.md               # 线上帧格式、CRC、状态码和两次 SPI 时序
├── PROTOCOL_LAYER.md         # 中断/后台分工、命令表和回调规则
├── CODE_STRUCTURE.md         # 本文：代码目录和模块依赖
├── REVIEW.md                 # 代码检视记录
├── VALIDATION.md             # 已执行与待执行的验证记录
├── Makefile                  # Jetson 程序和主机协议测试构建入口
│
├── docs/                     # 平台说明、接线和可选 GPIO 设计
│   ├── MASTER_SLAVE.md       # 主从系统总体说明
│   ├── STM32.md              # STM32 从机说明
│   ├── JETSON.md             # Jetson 主机说明
│   ├── READY_BUSY_DESIGN.md  # DONE 完成通知管脚设计
│   └── IRQ_INTERRUPT_DESIGN.md # IRQ 命令中断设计（尚未实现）
│
├── common/                   # 与操作系统、MCU 外设无关的共享代码
│   ├── protocol.h            # 帧常量、请求视图和编解码接口
│   ├── protocol.c            # 写帧/返回帧编解码及 CRC16
│   ├── service.h             # 单请求服务状态和协议层入口
│   ├── service.c             # 校验事务、调用命令层、组装返回帧
│   ├── commands.h            # 命令组、子命令、回调和响应结构定义
│   └── commands.c            # CMDID/SUBCMDID 查表及示例回调
│
├── stm32/                    # STM32F407 从机专用代码
│   ├── README.md             # 指向 docs/STM32.md 的代码目录入口
│   ├── main.c                # 固件入口和非阻塞后台轮询
│   ├── spi_slave.h           # SPI 从机通信层公开接口
│   ├── spi_slave.c           # SPI1、DMA、CS 上升沿中断和命令 buffer
│   └── spi_slave.uvprojx     # 可直接打开的 Keil MDK 工程
│
├── jetson/                   # Jetson Orin Nano 主机专用代码
│   ├── README.md             # 指向 docs/JETSON.md 的代码目录入口
│   ├── main.c                # spidev 配置、写事务、等待、读事务及校验
│   └── parse_number.h        # 命令行十进制/十六进制参数解析
│
├── tests/
│   └── test_protocol.c       # 协议、CRC、命令分发和服务状态测试
│
├── tools/
│   ├── generate_keil.py      # 从项目内部模板重新生成 Keil 工程
│   └── import_hal.py         # 仅在刷新厂商依赖时导入原例程文件
│
├── vendor/
│   ├── README.md             # 厂商依赖来源和许可说明
│   └── hal_example/          # Keil 工程所需 HAL、CMSIS、启动和系统代码
│
└── build/                    # 本机构建产物，运行 make 后生成，不提交
    ├── spi_request           # Jetson ARM64 主机程序
    └── test_protocol         # 主机协议测试程序
```

仓库根目录的 `.github/workflows/spi-check.yml` 负责 Linux 主机编译、协议测试、内存检查、Keil 工程生成一致性以及命令行参数检查。

## 分层职责

| 层 | 文件 | 负责 | 不负责 |
|---|---|---|---|
| STM32 通信层 | `stm32/spi_slave.c` | SPI/DMA 初始化、CS 上升沿捕获、复制事务快照、后台重新装载 DMA | CRC、命令查找和业务执行 |
| 协议服务层 | `common/protocol.c`、`common/service.c` | 帧头/帧尾/长度/CRC 校验、请求解析、状态管理、返回帧组装 | 直接访问 STM32 寄存器或 Linux 设备 |
| 命令业务层 | `common/commands.c` | 按 CMDID 和 SUBCMDID 查表、执行回调、填写结果和最终状态 | SPI 收发和帧 CRC |
| Jetson 通信层 | `jetson/main.c` | 配置 spidev，完成一次写事务和一次读事务，校验返回帧 | STM32 业务命令的具体实现 |

依赖方向保持单向：

```text
stm32/main.c
    └── stm32/spi_slave.c
            └── common/service.c
                    ├── common/protocol.c
                    └── common/commands.c

jetson/main.c
    └── common/protocol.c

tests/test_protocol.c
    ├── common/protocol.c
    ├── common/service.c
    └── common/commands.c
```

`common` 不包含 HAL、CMSIS、Linux `ioctl` 或板级引脚定义，因此协议和命令分发可以直接在开发机及 Jetson 上测试。`vendor` 只为 STM32 固件构建提供依赖，不参与 Jetson 程序。

## 一次命令的代码路径

1. Jetson 的 `main()` 调用 `proto_write()` 生成 `0x30` 写命令帧，通过第一次 SPI 事务发送。
2. STM32 DMA 接收字节；CS 上升沿进入 `EXTI4_IRQHandler()`，中断停止 DMA，将数据、实际长度和硬件错误复制到单帧 `command_buffer`，设置 `ready` 后退出。
3. STM32 主循环调用 `spi_slave_poll()`，后台取得稳定快照并交给 `service_transaction()`。
4. `service_transaction()` 调用 `proto_parse_write()` 检查帧头、帧尾、声明长度和 CRC。失败时直接准备协议错误响应，不调用业务回调。
5. 校验成功后，`command_dispatch()` 先查找 CMDID 命令组，再查找 SUBCMDID 条目并调用回调。
6. 回调填写 `response->data` 和 `response->size`，在最后填写 `response->status`。协议层将其组装为变长 `0x60` 返回帧并计算 CRC，通信层重新装载发送 DMA。
7. Jetson 等待约定间隔后发起第二次 SPI 事务，MOSI 发送 256 个 `0xFF` 以产生足够时钟，同时从 MISO 读取“逻辑响应 + 填充”。
8. Jetson 使用 `proto_parse_reply()` 从缓冲区找出 CRC 正确的真实帧长度，再读取状态及实际返回数据。

## 修改位置速查

| 要修改的内容 | 主要位置 | 同步事项 |
|---|---|---|
| 新增业务命令 | `common/commands.c` | 注册唯一的 CMDID/SUBCMDID，回调最后填写状态，并增加测试 |
| 修改线上帧字段 | `common/protocol.h/.c` | 同步 Jetson、STM32、`PROTOCOL.md`、`REQUIREMENTS.md` 和测试 |
| 修改响应长度 | `common/protocol.h` | 同步 DMA 读取长度、结果容量、文档和边界测试 |
| 修改 SPI 引脚/DMA | `stm32/spi_slave.c` | 同步 Keil 资源配置、接线文档并上板验证 |
| 修改 SPI 模式/频率/间隔 | `jetson/main.c` | 同步 STM32 配置和 `PROTOCOL.md`，用逻辑分析仪验证 |
| 修改 STM32 工程文件列表 | `tools/generate_keil.py` | 重新生成 `stm32/spi_slave.uvprojx` 并检查差异 |

## 构建边界

- Jetson：`make all` 只编译 `jetson/main.c` 与共享 `common/protocol.c`。
- 协议测试：`make test` 在主机环境编译全部 `common` 模块，不需要 SPI 硬件。
- STM32：Keil 工程编译 `stm32`、`common` 和 `vendor/hal_example` 中被引用的文件。
- `build/`、Keil 输出和调试器个人设置属于生成物，不提交仓库。

当前结构适合串行的“一次写命令、一次读响应”。若将来需要多个并发请求、变长响应或长耗时命令，应先扩展请求关联、队列及 DONE 完成通知协议，再调整通信层。
