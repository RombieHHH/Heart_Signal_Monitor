# Heart Signal Monitor

基于 STM32F103 与 AD8232 的便携式单导联心电信号监测系统。项目包含嵌入式采集与显示固件、心电信号处理算法、蓝牙串口协议，以及 Python/FastAPI 网页上位机。

本项目用于课程设计与工程验证，不用于医疗诊断。模拟器实测和人体静息心电实测均已通过视频完成课程验收。

## 主要功能

- TIM3 触发 ADC1，配合循环 DMA 以 500 Hz 连续采集 AD8232 输出。
- 0.5--40 Hz 显示通道与 5--15 Hz QRS 检测通道。
- 自适应 QRS/R 峰检测、BPM 计算和 P/Q/R/S/T 形态标记。
- 基于连续 30 个 RR 间期的 SDNN、RMSSD 与演示性节律异常提示。
- 导联脱落、削顶、平直、丢样及采集停止检测与自动恢复。
- 240 像素 TFT 实时波形、心率、RR、HRV 和状态显示。
- USART3 紧凑二进制帧、CRC16 校验、采样序号与传输缺口检测。
- Python 上位机重新计算指标，通过 REST/WebSocket 提供实时网页显示和 CSV 记录。
- 上位机自动忽略接近正常 RR 两倍或三倍的漏峰间期，避免显示心率被偶发漏检减半，并输出忽略计数。

## 系统组成

```text
电极/模拟器
    -> AD8232 模拟前端
    -> STM32F103 ADC + DMA（500 Hz）
       -> 滤波、QRS、BPM、HRV、PQRST
       -> TFT 本地显示与蜂鸣提示
       -> USART3/蓝牙（250 Hz 紧凑帧）
          -> Python/FastAPI
             -> WebSocket 网页监测与 CSV 记录
```

主要硬件接口：

| 功能 | 接口 | 说明 |
| --- | --- | --- |
| 心电输入 | PB0 / ADC1_IN8 | AD8232 OUT |
| 导联状态 | PC6、PC7 | LO-、LO+ |
| TFT | SPI2，PB13/PB15 | SCK、MOSI |
| 无线串口 | USART3，PB10/PB11 | 115200 baud，8N1 |
| 调试串口 | USART2，PA2/PA3 | 诊断或原始采样 |
| 蜂鸣器 | PA11 / TIM1_CH4 | 2 kHz PWM |

## 目录结构

```text
Inc/、Src/                 STM32 外设与信号处理模块
hardware/                  AD8232、LCD 与 ECG 应用层
Drivers/                   STM32 HAL 与 CMSIS
cmake/                     ARM 工具链和 CubeMX 构建配置
web/                       Python 后端、网页前端与模拟数据源
tools/                     协议、算法、采集和分析工具
Heart_Signal_Monitor.ioc   STM32CubeMX 工程
ACCEPTANCE.md              验收项目与测试说明
USART3_PROTOCOL.md         蓝牙串口帧协议
ALGORITHM_INTERFACE.md     算法模块接口
```

## 固件构建

需要 CMake、Ninja 和 `arm-none-eabi-gcc`，并确保工具链位于 `PATH`。

```powershell
cmake --preset Release
cmake --build --preset Release
```

构建结果位于 `build/Release/`，包括 ELF、HEX 和 BIN 文件。BIN 的烧录起始地址为 `0x08000000`。

Debug 构建：

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

## 网页上位机

安装依赖：

```powershell
cd web
python -m pip install -r requirements.txt
```

连接实际串口并启动：

```powershell
python main.py --port COM6 --baud 115200
```

也可以使用自动选口或关闭 CSV 记录：

```powershell
python main.py --port auto
python main.py --port COM6 --no-record -v
```

启动后访问 `http://127.0.0.1:8000/`。主要接口包括：

| 路径 | 用途 |
| --- | --- |
| `/` | 实时心电监测页面 |
| `/health` | 服务健康检查 |
| `/api/snapshot` | 当前状态与数据快照 |
| `/api/ports` | 本机串口列表 |
| `/api/session` | 会话与协议参数 |
| `/ws` | WebSocket 实时数据 |

无硬件时可在两个终端中运行模拟发送端和后端：

```powershell
python mock_mcu.py --port COM31
python main.py --port COM29
```

具体串口连接方式和 Windows 蓝牙链路注意事项见 [`web/README.md`](web/README.md)。

## 测试

算法回归测试会编译并执行实际 C 模块：

```powershell
python tools/test_ecg.py --cc gcc
```

协议测试：

```powershell
python tools/test_ecg_bt_protocol.py
python tools/test_ecg_protocol.py web/ecg/protocol.py
```

测试覆盖多档心率、倒置信号、基线漂移、50 Hz 干扰、滤波频响、HRV 报警、导联与丢样恢复、序号回绕、显示增益、PQRST 标记和协议 CRC。模拟器与人体实测已通过视频完成整机功能验收。

## 数据协议

MCU 保持 500 Hz 本地采样，相邻两点平均后以 250 Hz 发送 8 位量化电平。每帧包含 100 个样本，总长 128 字节，使用 CRC16/CCITT-FALSE 校验。上位机根据恢复后的电平重新计算滤波、PQRST、BPM、HRV、质量和报警状态。

完整字段定义、缺口标志与序号规则见 [`USART3_PROTOCOL.md`](USART3_PROTOCOL.md)。

## 安全说明

- 人体测量必须采用安全隔离供电，避免电脑、调试器、示波器或非隔离电源形成危险通路。
- HRV 阈值、PQRST 标记和“疑似节律异常”状态仅用于教学演示。
- 本项目未经临床验证，不得用于诊断、治疗或替代合规医疗设备。
