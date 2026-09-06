# 协议层与命令分发

## 执行流程

```text
SPI + DMA 接收
    ↓ CS 上升沿，进入 EXTI4 中断
停止 DMA → 记录实际长度/硬件错误 → 复制到 command_buffer → 发布 ready → 退出中断
    ↓ 主循环 spi_slave_poll()
检查硬件错误 → 帧头/帧尾/长度/CRC → cmdid 命令组 → subcmdid 条目 → callback
    ↓
生成响应 → CS 为高时重新装载 DMA → 等待主机第二次 SPI 读取
```

中断只保存事务快照，最多复制 257 字节，不做 CRC、协议解析和业务执行。DMA 停止等待有次数上限；停止失败时记录 `spi_slave_dma_stop_faults`，停止接收且不执行命令，需排查硬件并重新初始化。

后台调用 `service_transaction()`，通过 `proto_parse_write()` 检查整体帧格式、实际长度与声明总长度以及 CRC。任一校验失败返回状态 1，不进入回调。解析成功后 `command_dispatch()` 在 `command_group` 数组中匹配 CMDID，再在组内匹配 SUBCMDID；均匹配且回调非空才执行。找不到返回状态 2。

## 添加命令

在 `common/commands.c` 增加回调，并在对应组的 `command_entry` 数组中注册 `{subcmdid, callback, context}`。新命令组注册为 `{cmdid, entries, count}`。每个 `(cmdid, subcmdid)` 应唯一；若重复，执行第一个匹配条目。

回调收到解析后的 `proto_request`，其中 `data/size` 为请求载荷。回调类型为 `void callback(const proto_request *, command_response *, void *)`；先写 `response->data`（最多 251 字节）及实际 `response->size`，最后写 `response->status`。每条退出路径都应填写最终状态；初始长度为 0、状态为执行失败 `0x04`。回调返回后，后台按实际长度组装 `60 STATUS RESULT[N] CRC_LO CRC_HI 0A`，CRC 包含最终状态和实际数据。不得保存请求或响应指针供回调返回后使用，不得越过输出容量。默认已注册 `CMDID=1, SUBCMDID=0` 回显。

也可以用 `service_init_commands()` 注入其他静态命令表及上下文，必须在启用通信前初始化；表与上下文生命周期必须覆盖服务运行时间。

## 缓冲区和并发约定

单帧邮箱对应当前“一写一读”的串行协议。中断在数据与元数据复制完成后发布 `ready`，后台读屏障后消费。处理期间 DMA 停止、邮箱保持占用；额外 CS 上升沿仅增加 `spi_slave_dropped_transactions`，不覆盖邮箱。主循环即使反复等待 CS 拉高也不会重复执行回调。

回调运行时中断保持开启；只有后台重新装载 DMA 的短临界区关闭中断，并恢复进入前的中断状态。不要在其他中断或多个任务中调用后台轮询。

当前沿用两次 SPI 间至少 10 ms 的高电平间隔，回调必须在该时间预算内完成。单帧邮箱不能接收连续流水写入；需要这种能力时应另行扩展队列及响应关联协议。主机违反间隔、在重新装载时拉低 CS，不保证该次事务有效，须校验响应。读事务也会产生 CS 上升沿快照，但由服务状态机消费，不作为命令执行。
