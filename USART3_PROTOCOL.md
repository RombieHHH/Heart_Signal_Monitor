# USART3 数据回传

USART3 使用 115200 baud、8 数据位、无校验、1 停止位、无流控。HC06 的 UART 端也必须配置为 115200。
PB10（TX）接 USB 串口接收端 RX，并连接 GND；上位机使用用户提供的 `protocol.py` 中 `FrameParser.feed()` 接收二进制流。

MCU 保持 500 Hz 采样和本地显示；相邻两个 ADC 点先平均，再以 250 Hz 回传量化电平。
每 400 ms 发送 100 个 8 位 ADC 电平，每帧 128 字节：26 字节头、100 字节电平、2 字节 CRC；平均约 320 B/s，多字节字段为小端序。
CRC16/CCITT-FALSE 覆盖 version 到最后一条样本，不包含同步字和 CRC。

- version=2，type=2，payload_len=100，sample_rate=250，count=100。
- 每个载荷字节为 `raw_adc >> 4`，上位机恢复为 `(value << 4) + 8`，最大量化误差 8 ADC counts。
- 头部 hr、sd_rr、rmssd_rr 固定为 0xFFFF；MCU 不通过蓝牙发送计算结果。
- 上位机根据电平重新完成去基线、低通、P/Q/R/S/T、心率、SDNN、RMSSD、质量和报警计算。

使用独立发送缓冲区和 `HAL_UART_Transmit_IT`，每 2 ms 提交 1 字节，一帧约 256 ms 发完。接收端按字节流拼帧。合并帧后固定头和 CRC 的频率减半，250 Hz 模式约 320 B/s。

发送忙时丢弃新帧，不等待串口；下一成功提交的帧携带 flags bit1，并保留帧号缺口。ADC 序号在采集中产生，队列丢样和主动清空不会压缩时间轴。遇到采样序号跳变，舍弃未满的旧帧，下一帧设置 bit0；若舍弃了旧帧，也设置 bit1。采集停止时不会生成虚构样本。

验证命令：

```text
python tools/test_ecg_protocol.py C:/Users/Rombie/Desktop/protocol.py
python tools/test_ecg.py
```

协议测试编译实际 C 编码器，再由上位机解析器以分片字节流解码，检查 CRC、字段、饱和/舍入、R 序号、缺口、丢帧和序号回绕。硬件串口波形与实际接收需烧录后验证。

原始二进制抓包可用 `python tools/analyze_ecg_capture.py <capture.bin>` 检查；工具不会打开串口或忽略 CRC。
