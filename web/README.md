# ECG 上位机（Python 后端）

本上位机根据《实验三 心电信号测量与显示 预设计报告》实现，通过蓝牙透传虚拟串口接收下位机（STM32F103RBT6 + AD8232）心电数据，完成协议解析、实时显示（WebSocket 推送）和 CSV 记录；仓库自带 ECG 实时监控页面 `web/index.html`，后端启动后直接在 `/` 提供。

## 目录结构

```
ecg_host/
├── main.py                 # 入口：参数解析 + 启动读取线程 + FastAPI
├── requirements.txt
├── web/index.html          # ECG 实时监控前端页面（后端自动托管）
├── recordings/             # CSV 记录输出目录（运行时自动创建）
└── ecg/
    ├── protocol.py         # 二进制帧协议解析（同步字/CRC16）
    ├── reader.py           # 蓝牙串口读取线程（自动选口/重连）
    ├── datastore.py        # 线程安全环形缓冲与状态
    ├── recorder.py         # 会话 CSV + 数据缺口记录
    └── server.py           # FastAPI REST + WebSocket 广播
```

## 安装

```bash
cd ecg_host
python -m pip install -r requirements.txt
```

## 运行

```bash
# 常规启动：auto 自动识别 HSM 蓝牙虚拟串口（模块→PC 方向，默认 921600）
python main.py

# 手动指定串口 / 波特率
python main.py --port COM29 --baud 921600

# 关闭记录、启用调试日志
python main.py --no-record -v --host 0.0.0.0
```

### 本机完整演示（已验证可用）

> 实测发现：Windows 蓝牙“从模块 → 电脑”方向会被系统低功耗轮询限速
> （约 260 B/s，帧到达后 CRC 全错）；而“电脑 → 模块”方向稳定无损。
> 因此演示时让 mock 走 COM31（电脑蓝牙发往模块），
> 后端读取模块 UART TX 回传到 USB-TTL（COM29）的数据。

```bash
# 终端 1：mock 把帧写入蓝牙虚拟串口（电脑 → 模块，可靠方向）
python mock_mcu.py --port COM31

# 终端 2：后端读取模块 UART TX 输出（USB-TTL COM29）
python main.py --port COM29
```

> 最终 STM32 实机如果必须使用“模块 → 电脑”方向，需要让模块作为蓝牙
> 主设备主动连接电脑的传入 COM 口，或更换不受 Windows 轮询限制的适配器。

启动后可访问：

| 接口 | 说明 |
| --- | --- |
| `/` | ECG 实时监控前端页面（web/index.html） |
| `/health` | 健康检查 |
| `/api/snapshot` | 一次性数据快照（JSON） |
| `/api/ports` | 本机串口列表 |
| `/api/session` | 会话/协议约定 |
| `/ws` | WebSocket 实时推送（发送 `{"type":"subscribe"}` 订阅） |

## 数据内容与格式

### 下行口（下位机 → 上位机）二进制帧

小端序，单帧周期 100 ms，默认 50 个样本，固定开销 26 字节头 + 载荷 500 字节 + CRC2 字节 = 528 字节：

| 字段 | 类型/字节 | 含义 |
| --- | --- | --- |
| sync | 2 | 同步字 `0xA5 0x5A` |
| version / type | u8 / u8 | 版本 1；类型 1=波形帧 |
| payload_len / flags | u16 / u16 | 载荷长度；bit0=采集缺口 bit1=发送丢帧 |
| frame_seq / sample0 | u32 / u32 | 帧号；本帧首样本采样序号 |
| sample_rate / count | u16 / u16 | 500 Hz；样本数 50 |
| hr / sd_rr / rmssd_rr | u16×3 | 0.1 BPM / 0.1 ms；`0xFFFF` 无效 |
| sample records | count×10 | 见下 |
| CRC16 | u16 | CCITT-FALSE（poly 0x1021，init 0xFFFF，覆盖 version~载荷） |

样本记录（10 B）：`raw(u16)`，`filtered(i16)`，`r_seq(u32，无事件=0xFFFFFFFF)`，`quality(u8)`，`alarm(u8)`。

- `quality` 位：bit0 导联脱落，bit1 削顶，bit2 伪迹/检测不稳，bit3 学习中
- `alarm`：0 积累，1 信号无效，2 正常，3 疑似节律异常
- CRC 错误整帧丢弃；凭 sample0/frame_seq 判别缺口（详见 `recorder`）。

### 上行 WebSocket / REST 数据（JSON）

`/api/snapshot` 与 `/ws` 推送统一的快照结构：

```jsonc
{
  "connected": true,              // 串口连接状态
  "port": "COM5", "baudrate": 115200,
  "sample_rate": 500,
  "sample_index": 123456,         // 最近样本序号
  "frame_seq": 2469,
  "last_frame_age": 42.1,         // 距最后有效帧时间(ms)
  "statistics": {                 // 解析累计统计
    "frames_ok": 2469, "frames_crc_error": 0,
    "frames_payload_error": 0, "frames_unsynced": 0,
    "recovered_bytes": 0
  },
  "metrics": {                    // 最近一帧测量值
    "hr_bpm": 72.0, "sd_rr_ms": 8.3, "rmssd_rr_ms": 9.1,
    "alarm": 2, "quality": 0
  },
  "samples": {                    // 波形（默认最近 4s/2000 点）
    "start_index": 121457,
    "filtered": [ ... ],          // 整形数组
    "raw": [ ... ],
    "r_marks": [0, 1, 0, ...]     // R 事件标记(0/1)，与数组对齐
  }
}
```

`alarm` 无有效样本时为 `null`；`hr/sd_rr/rmssd_rr` 无效时为 `null`。