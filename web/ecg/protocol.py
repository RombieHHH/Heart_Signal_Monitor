# -*- coding: utf-8 -*-
"""
ecg_host.ecg.protocol
====================

上位机与下位机之间的二进制分块帧协议解析实现。

协议依据《实验三 心电信号测量与显示 预设计报告》第 10.1 节"二进制分块接口"定义。

帧结构（所有多字节字段均为小端序，单帧周期 100 ms，默认 50 个样本）：

    +-------------+----------+--------+--------+------+------+---------+---------+------------+-------+----------+--------+--------+
    | sync(2B)    | version  | type   | payload| flags|frame_seq| sample0 |sample_rate|  count   |  hr   |  sd_rr   |rmssd_rr| CRC16 |
    | 0xA5 0x5A   | (u8=1)   |(u8=1)  | len(u16)|(u16)| (u32)   | (u32)   | (u16)    | (u16)    | (u16) |  (u16)   | (u16)  | (u16) |
    +-------------+----------+--------+--------+------+------+---------+---------+------------+-------+----------+--------+--------+
    +----------------------------- 样本记录 count×10B -----------------------------+
    | raw(u16) | filtered(i16) | r_seq(u32) | quality(u8) | alarm(u8) |
    +------------------------------------------------------------------------------+

版本 1/type 1 是原始 10 字节记录协议。版本 2/type 2 是蓝牙紧凑协议：
250 Hz、每 400 ms 发送 100 个 8 位 ADC 电平，载荷 100 字节，总帧长 128 字节。
紧凑值由 12 位 ADC 右移 4 位得到，上位机以量化区间中点恢复。

字段说明：
    sync      同步字 0xA5 0x5A
    version   协议版本，当前为 1
    type      帧类型，1 = 波形帧
    payload_len  载荷长度（字节），波形帧 = count * 10
    flags     bit0 = 采集缺口（sample_index 不连续）
              bit1 = 发送丢帧（队列丢弃）
              其余位保留为 0
    frame_seq 帧号（递增）
    sample0   本帧首样本的采样序号
    sample_rate 原协议为 500 Hz；紧凑协议当前为 250 Hz
    count     本帧样本数，type 1 默认 50；type 2 当前为 100
    hr        心率，单位 0.1 BPM；0xFFFF 表示无效
    sd_rr     短窗 RR 标准差，单位 0.1 ms；0xFFFF 表示无效
    rmssd_rr  短窗 RMSSD，单位 0.1 ms；0xFFFF 表示无效

每条样本记录（10 字节）：
    raw       原始 ADC 采样值（去偏置前的 ADC count）
    filtered  去偏置后的滤波值，四舍五入饱和至 int16
    r_seq     检测确认时的真实采样序号；无 R 事件为 0xFFFFFFFF
    quality   信号质量标志：
              bit0 = 1 导联脱落
              bit1 = 1 削顶/饱和
              bit2 = 1 伪迹/检测不稳定
              bit3 = 1 学习中
    alarm     报警/有效状态：0=积累 1=信号无效 2=正常 3=疑似节律异常
"""

import struct
from dataclasses import dataclass, field
from typing import List, Optional

# 帧结构常量
SYNC_BYTE_0 = 0xA5
SYNC_BYTE_1 = 0x5A
VERSION_DEFAULT = 1
TYPE_WAVEFORM = 1
TYPE_COMPACT_LEVEL = 2

SAMPLE_RATE_DEFAULT = 500
SAMPLES_PER_FRAME_DEFAULT = 50

# 无效值
INVALID_U16 = 0xFFFF          # hr / sd_rr / rmssd_rr
NO_R_EVENT = 0xFFFFFFFF       # r_seq 无事件

# 固定尺寸（单位：字节）
HEADER_SIZE = 26              # sync2+ver1+type1+plen2+flags2+fseq4+s0_4+sr2+count2+hr2+sd2+rm2
RECORD_SIZE = 10              # 单条样本记录
CRC_SIZE = 2
MIN_FRAME_SIZE = HEADER_SIZE + CRC_SIZE          # 26 + 2 = 28
MAX_PAYLOAD = 500
MAX_LEGACY_COUNT = MAX_PAYLOAD // RECORD_SIZE     # type 1: 50 records
MAX_COMPACT_COUNT = MAX_PAYLOAD                   # type 2: 500 one-byte levels
MAX_COUNT = MAX_LEGACY_COUNT                      # compatibility for legacy tools


class Quality:
    """quality 位掩码。"""
    LEAD_OFF = (1 << 0)       # 导联脱落
    SATURATED = (1 << 1)      # 削顶/饱和
    ARTIFACT = (1 << 2)       # 伪迹/检测不稳定
    LEARNING = (1 << 3)       # 学习中


class AlarmState:
    """alarm 状态。"""
    ACCUMULATING = 0          # 数据积累中
    INVALID = 1               # 信号无效
    NORMAL = 2                # 正常
    ABNORMAL = 3              # 疑似节律异常


ALARM_TEXT = {
    AlarmState.ACCUMULATING: "数据积累中",
    AlarmState.INVALID: "信号无效",
    AlarmState.NORMAL: "正常",
    AlarmState.ABNORMAL: "疑似节律异常",
}

# 头部打包格式（小端）
_HEADER_STRUCT = struct.Struct("<HBBHHIIHHHHH")


@dataclass
class SampleRecord:
    """单条样本记录。"""
    raw: int = 0
    filtered: int = 0
    r_seq: Optional[int] = None      # 无 R 事件时为 None
    quality: int = 0
    alarm: int = 0
    point: int = 0                 # 0 none, 1 R, 2 P, 3 Q, 4 S, 5 T


@dataclass
class WaveformFrame:
    """解析后的完整波形帧。"""
    version: int = 1
    type: int = 1
    flags: int = 0
    frame_seq: int = 0
    sample0: int = 0
    sample_rate: int = SAMPLE_RATE_DEFAULT
    count: int = 0
    hr: Optional[float] = None       # BPM，0.1 分辨率
    sd_rr: Optional[float] = None    # ms，0.1 分辨率
    rmssd_rr: Optional[float] = None  # ms，0.1 分辨率
    samples: List[SampleRecord] = field(default_factory=list)

    @property
    def gap(self) -> bool:
        return bool(self.flags & 0x0001)

    @property
    def sent_dropped(self) -> bool:
        return bool(self.flags & 0x0002)


def crc16_ccitt_false(data: bytes) -> int:
    """CRC16 CCITT-FALSE：多项式 0x1021，初值 0xFFFF，无反射，异或输出 0。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class FrameParser:
    """流式帧解析器：从字节流中搜索同步字、解包并校验 CRC。"""

    def __init__(self):
        self._buf = bytearray()
        self.stats = {
            "frames_ok": 0,
            "frames_crc_error": 0,
            "frames_payload_error": 0,
            "frames_unsynced": 0,
            "recovered_bytes": 0,
        }

    def feed(self, data: bytes) -> List[WaveformFrame]:
        """喂入一段字节流，返回本次解析出的所有有效帧。"""
        self._buf.extend(data)
        frames = []
        while True:
            r = self._extract_one()
            if r is None:
                break
            if r is self.SEARCH_RETRY:
                continue
            assert isinstance(r, WaveformFrame)
            frames.append(r)
        return frames

    def _extract_one(self) -> Optional[WaveformFrame]:
        # 数据不足以解析头部
        if len(self._buf) < HEADER_SIZE:
            return None

        # 搜索同步字：错位时逐个字节丢弃
        if not (self._buf[0] == SYNC_BYTE_0 and self._buf[1] == SYNC_BYTE_1):
            self._buf.pop(0)
            self.stats["frames_unsynced"] += 1
            self.stats["recovered_bytes"] += 1
            return self.SEARCH_RETRY  # 触发外层循环继续搜索

        (sync, version, ftype, payload_len, flags,
         frame_seq, sample0, sample_rate, count,
         hr_raw, sd_raw, rm_raw) = _HEADER_STRUCT.unpack_from(self._buf, 0)

        # 校验载荷长度合法性
        expected_payload = count if ftype == TYPE_COMPACT_LEVEL else count * RECORD_SIZE
        valid_type = ((ftype == TYPE_WAVEFORM and version == 1) or
                      (ftype == TYPE_COMPACT_LEVEL and version == 2))
        max_count = MAX_COMPACT_COUNT if ftype == TYPE_COMPACT_LEVEL else MAX_LEGACY_COUNT
        if (not valid_type or payload_len > MAX_PAYLOAD or count > max_count
                or payload_len != expected_payload):
            self.stats["frames_payload_error"] += 1
            self._buf.pop(0)  # 丢弃一个字节重新搜索
            self.stats["recovered_bytes"] += 1
            return self.SEARCH_RETRY

        total = HEADER_SIZE + payload_len + CRC_SIZE
        if len(self._buf) < total:
            return None  # 整帧尚未收齐，等待更多数据

        # CRC 覆盖 version 到载荷结束（不含 sync，不含 CRC 本身）
        crc_received = struct.unpack_from("<H", self._buf, HEADER_SIZE + payload_len)[0]
        crc_calc = crc16_ccitt_false(bytes(self._buf[2:HEADER_SIZE + payload_len]))

        if crc_received != crc_calc:
            self.stats["frames_crc_error"] += 1
            # Lost bytes can put the next valid header inside this candidate.
            # Advance one byte so that header is not discarded with the bad frame.
            del self._buf[0]
            self.stats["recovered_bytes"] += 1
            return self.SEARCH_RETRY

        consumed = bytes(self._buf[:total])
        del self._buf[:total]
        self.stats["frames_ok"] += 1
        return self._build_frame(
            version, ftype, flags, frame_seq, sample0,
            sample_rate, count, hr_raw, sd_raw, rm_raw, consumed,
        )

    def _build_frame(self, version, ftype, flags, frame_seq, sample0,
                     sample_rate, count, hr_raw, sd_raw, rm_raw, full_frame):
        frame = WaveformFrame(
            version=version,
            type=ftype,
            flags=flags,
            frame_seq=frame_seq,
            sample0=sample0,
            sample_rate=sample_rate,
            count=count,
            hr=(hr_raw / 10.0 if hr_raw != INVALID_U16 else None),
            sd_rr=(sd_raw / 10.0 if sd_raw != INVALID_U16 else None),
            rmssd_rr=(rm_raw / 10.0 if rm_raw != INVALID_U16 else None),
        )

        payload = full_frame[HEADER_SIZE:HEADER_SIZE + (count if ftype == TYPE_COMPACT_LEVEL else count * RECORD_SIZE)]
        if ftype == TYPE_COMPACT_LEVEL:
            for value in payload:
                frame.samples.append(SampleRecord(raw=(value << 4) + 8))
            return frame
        off = 0
        for _ in range(count):
            raw, filtered_raw, r_seq, quality, alarm = struct.unpack_from(
                "<HhIBB", payload, off)
            off += RECORD_SIZE
            frame.samples.append(SampleRecord(
                raw=raw,
                filtered=filtered_raw,
                r_seq=(r_seq if r_seq != NO_R_EVENT else None),
                quality=quality,
                alarm=alarm,
            ))
        return frame

    # 用于指示"已消费部分字节，应继续循环"的内部哨兵
    SEARCH_RETRY = _SEARCH_RETRY = object()
