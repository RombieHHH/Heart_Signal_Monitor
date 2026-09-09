# USART3 数据回传

USART3 使用工程现有配置：921600 baud、8 数据位、无校验、1 停止位、无流控。
PB10（TX）接 USB 串口接收端 RX，并连接 GND；上位机使用用户提供的 `protocol.py` 中 `FrameParser.feed()` 接收二进制流。

500 Hz 采样，每 50 个连续样本发送一帧，正常每 100 ms 一帧，每帧 528 字节。
26 字节头、500 字节样本记录、2 字节 CRC；多字节字段为小端序。
CRC16/CCITT-FALSE 覆盖 version 到最后一条样本，不包含同步字和 CRC。

- raw 为 ADC 原值；filtered 为当前采样对应的去偏置滤波值，四舍五入并饱和至 int16，不使用 LCD 的 250 样本延迟数据。
- r_seq 在检测确认的记录中携带 R 峰对应的全局 ADC 采样序号，无事件为 0xFFFFFFFF。
- 心率使用显示心率，SD RR 使用短窗 SDNN，RMSSD 使用短窗 RMSSD；统一乘 10 四舍五入，无效值为 0xFFFF。头部指标取帧末状态。
- quality 映射导联脱落、削顶/超量程、伪迹/失效、学习中；alarm 为积累 0、无效 1、正常 2、疑似异常 3。

使用独立发送缓冲区和 `HAL_UART_Transmit_IT`。发送忙时丢弃新帧，不等待串口；下一成功提交的帧携带 flags bit1，并保留帧号缺口。ADC 序号在采集中产生，队列丢样和主动清空不会压缩时间轴。遇到采样序号跳变，舍弃未满的旧帧，下一帧设置 bit0；若舍弃了旧帧，也设置 bit1。采集停止时不会生成虚构样本。

验证命令：

```text
python tools/test_ecg_protocol.py C:/Users/Rombie/Desktop/protocol.py
python tools/test_ecg.py
```

协议测试编译实际 C 编码器，再由上位机解析器以分片字节流解码，检查 CRC、字段、饱和/舍入、R 序号、缺口、丢帧和序号回绕。硬件串口波形与实际接收需烧录后验证。
