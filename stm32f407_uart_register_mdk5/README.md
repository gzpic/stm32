# STM32F407 寄存器版 UART 接收演示

本工程用于 Jetson Orin Nano 与 STM32F407 通过 UART 通信，STM32 端使用寄存器方式配置 USART1。

## 串口配置

- USART1：PA9/TX、PA10/RX
- 115200 baud，8N1，无流控
- 以 `\r\n` 作为一帧文本的结束符
- 有效载荷最大 199 字节

Jetson 作为发送端时至少连接：

```text
Jetson J12 Pin 8 / UART1_TXD  -> STM32 PA10 / USART1_RX
Jetson J12 Pin 6 或 Pin 9 GND -> STM32 GND
```

信号为 3.3V TTL，不能连接 5V TTL 或 RS-232 电平。

## 可视化效果

- 红灯常亮表示 STM32 程序已经运行。
- 每收到一条完整消息，绿灯翻转一次。
- 支持 `LED ON`、`LED OFF`、`LED TOGGLE` 三条命令。
- 收到的正文通过 ITM/SWO 输出到 MDK5 Debug (printf) Viewer。

## MDK5 使用

打开 `Projects/MDK-ARM/atk_f407.uvprojx`，编译并下载。进入调试前，在
`Options for Target -> Debug -> Settings -> Trace` 中启用 Trace，将 Core Clock 设置为
`168000000`，并启用 ITM Stimulus Port 0。调试时打开
`View -> Serial Windows -> Debug (printf) Viewer`。

调试器还必须连接 STM32F407 的 PB3/SWO；只有 SWDIO、SWCLK 和 GND 时，LED 功能正常，
但 Debug (printf) Viewer 不会显示字符。

Jetson 端测试命令：

```bash
./uart_send --device /dev/ttyTHS1 --text "LED TOGGLE" --count 20 --interval-ms 500
```

预期 MDK5 输出：

```text
STM32 register UART RX ready, 115200 8N1
RX[1] (10 bytes): LED TOGGLE
RX[2] (10 bytes): LED TOGGLE
```

