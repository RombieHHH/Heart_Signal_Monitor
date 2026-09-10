# -*- coding: utf-8 -*-
"""
ecg_host.ecg.datastore
======================

线程安全的数据缓冲与系统状态。

- 环形缓冲区保存最近采样的 filtered/raw 波形，供 Web 端实时绘制（默认 4 s 窗口）。
- 记录最近一次帧的统计量（hr/sd_rr/rmssd_rr/alarm）、解析器统计、串口状态。
- 通过锁保护，读取线程写入、WebSocket 广播线程读取。
"""

import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Dict, Optional

from .protocol import WaveformFrame
from .processing import HostECGProcessor

# 默认波形可视窗口样本数：4 s * 250 Hz = 1000
DEFAULT_WINDOW_SAMPLES = 1000


@dataclass
class SystemStatus:
    """串口/系统运行状态快照，供 Web 端展示。"""
    connected: bool = False
    serial_open: bool = False
    bytes_received: int = 0
    port: Optional[str] = None
    baudrate: int = 115200
    last_frame_time: Optional[float] = None     # 最近一次有效帧时间戳
    frames_ok: int = 0
    frames_crc_error: int = 0
    frames_payload_error: int = 0
    frames_unsynced: int = 0
    recovered_bytes: int = 0
    frame_sequence_gaps: int = 0
    recent_frame_success_percent: float = 100.0
    recent_frame_errors: int = 0
    sample_index: int = 0
    frame_seq: int = 0
    sample_rate: int = 500


class DataStore:
    """线程安全的数据缓冲与状态仓库。"""

    def __init__(self, window_samples: int = DEFAULT_WINDOW_SAMPLES):
        self._lock = threading.Lock()
        self._capacity = window_samples
        self._filtered = []      # 环缓冲（list 模拟，先进先出，长度不超过 cap）
        self._raw = []
        self._r_marks = []       # 与波形等长对齐的 R 事件标记（0/1）
        self._point_marks = []   # 0 none, 1 R, 2 P, 3 Q, 4 S, 5 T
        self._source_indices = []
        self.latest: Optional[WaveformFrame] = None
        self.processor = HostECGProcessor(250)
        self._processor_rate = 250
        self._pending_display = deque()
        self._landmarks = {}
        self._previous_frame_seq = None
        self._last_display_index = None
        self._display_stream_index = -1
        self.status = SystemStatus()
        self.status.sample_rate = 500

    # ---------- 写入（串口读取线程） ----------
    def push_frame(self, frame: WaveformFrame) -> None:
        """记录一帧数据到环形缓冲并刷新状态。"""
        with self._lock:
            self.latest = frame
            st = self.status
            st.sample_rate = frame.sample_rate
            st.frame_seq = frame.frame_seq
            st.last_frame_time = time.time()

            if frame.type == 2:
                if frame.sample_rate != self._processor_rate:
                    self._processor_rate = frame.sample_rate
                    self.processor = HostECGProcessor(frame.sample_rate)
                    self._pending_display.clear()
                    self._landmarks.clear()
                sequence_gap = (self._previous_frame_seq is not None and
                    frame.frame_seq != ((self._previous_frame_seq + 1) & 0xFFFFFFFF))
                if frame.gap or frame.sent_dropped or sequence_gap:
                    if sequence_gap:
                        missing = ((frame.frame_seq - self._previous_frame_seq - 1) & 0xFFFFFFFF)
                        st.frame_sequence_gaps += missing if missing < 10000 else 1
                    self.processor.handle_gap()
                    self._pending_display.clear()
                    self._landmarks.clear()
                for offset, rec in enumerate(frame.samples):
                    sequence = frame.sample0 + offset
                    result = self.processor.push(rec.raw, sequence)
                    rec.filtered = result.filtered
                    rec.r_seq = result.r_seq
                    rec.quality = result.quality
                    rec.alarm = result.alarm
                    frame.hr, frame.sd_rr, frame.rmssd_rr = result.hr, result.sdnn, result.rmssd
                    for marked_sequence, point in result.landmarks:
                        self._landmarks[marked_sequence] = point
                    self._pending_display.append((sequence, rec.raw, rec.filtered))
            self._previous_frame_seq = frame.frame_seq
            if frame.type == 2:
                delay = round(0.5 * frame.sample_rate)
                while len(self._pending_display) > delay:
                    sequence, raw, filtered = self._pending_display.popleft()
                    point = self._landmarks.pop(sequence, 0)
                    self._filtered.append(filtered)
                    self._raw.append(raw)
                    self._point_marks.append(point)
                    self._r_marks.append(1 if point == 1 else 0)
                    self._source_indices.append(sequence)
                    self._last_display_index = sequence
                    self._display_stream_index += 1
            else:
                for offset, rec in enumerate(frame.samples):
                    self._filtered.append(rec.filtered)
                    self._raw.append(rec.raw)
                    point = 1 if rec.r_seq is not None else 0
                    self._point_marks.append(point)
                    self._r_marks.append(1 if point == 1 else 0)
                    self._source_indices.append(frame.sample0 + offset)
                    self._display_stream_index += 1
                self._last_display_index = frame.sample0 + frame.count - 1
            if self._last_display_index is not None:
                st.sample_index = self._last_display_index
            # 裁剪到窗口容量
            excess = len(self._filtered) - self._capacity
            if excess > 0:
                del self._filtered[:excess]
                del self._raw[:excess]
                del self._r_marks[:excess]
                del self._point_marks[:excess]
                del self._source_indices[:excess]

    def update_status(self, **kwargs) -> None:
        with self._lock:
            for k, v in kwargs.items():
                setattr(self.status, k, v)

    def mark_disconnected(self, serial_open: Optional[bool] = None) -> None:
        """标记实时传输中断，并清除所有不再属于实时数据的显示状态。"""
        with self._lock:
            self.status.connected = False
            self.status.recent_frame_success_percent = 0.0
            self.status.recent_frame_errors = 0
            if serial_open is not None:
                self.status.serial_open = serial_open
            self.status.sample_index = 0
            self.latest = None
            self._filtered.clear()
            self._raw.clear()
            self._r_marks.clear()
            self._point_marks.clear()
            self._source_indices.clear()
            self._pending_display.clear()
            self._landmarks.clear()
            self._previous_frame_seq = None
            self._last_display_index = None
            self._display_stream_index = -1
            self.processor = HostECGProcessor(self._processor_rate)

    def merge_parser_stats(self, parser_stats: Dict[str, int]) -> None:
        with self._lock:
            st = self.status
            st.frames_ok = parser_stats["frames_ok"]
            st.frames_crc_error = parser_stats["frames_crc_error"]
            st.frames_payload_error = parser_stats["frames_payload_error"]
            st.frames_unsynced = parser_stats["frames_unsynced"]
            st.recovered_bytes = parser_stats["recovered_bytes"]

    # ---------- 读取（广播线程） ----------
    def snapshot(self, window_seconds: Optional[float] = None) -> Dict:
        """生成一帧 Web 广播所需的全部数据快照。

        window_seconds 若为 None，则返回整段缓冲。
        """
        with self._lock:
            st = self.status
            # 依据窗口秒数截取尾部样本
            n = len(self._filtered)
            if window_seconds is not None:
                keep = int(window_seconds * st.sample_rate)
                keep = max(0, min(keep, n))
                start = n - keep
            else:
                start = 0

            data = {
                "connected": st.connected,
                "serial_open": st.serial_open,
                "bytes_received": st.bytes_received,
                "port": st.port,
                "baudrate": st.baudrate,
                "sample_rate": st.sample_rate,
                "sample_index": st.sample_index,
                "frame_seq": st.frame_seq,
                "last_frame_age": (
                    (time.time() - st.last_frame_time) * 1000.0
                    if st.last_frame_time is not None else None
                ),
                "statistics": {
                    "frames_ok": st.frames_ok,
                    "frames_crc_error": st.frames_crc_error,
                    "frames_payload_error": st.frames_payload_error,
                    "frames_unsynced": st.frames_unsynced,
                    "recovered_bytes": st.recovered_bytes,
                    "frame_sequence_gaps": st.frame_sequence_gaps,
                    "recent_frame_success_percent": st.recent_frame_success_percent,
                    "recent_frame_errors": st.recent_frame_errors,
                },
                "metrics": self._latest_metrics_locked(st),
                "samples": {
                    "start_index": self._source_indices[start] if n else None,
                    "end_index": self._source_indices[-1] if n else None,
                    "stream_start_index": self._display_stream_index - n + 1 + start if n else None,
                    "stream_end_index": self._display_stream_index if n else None,
                    "filtered": self._filtered[start:],
                    "raw": self._raw[start:],
                    "r_marks": self._r_marks[start:],
                    "point_marks": self._point_marks[start:],
                },
            }
            return data

    def _latest_metrics_locked(self, st) -> Dict:
        """最近一帧的心率 / HRV / 报警状态。"""
        f = self.latest
        return {
            "hr_bpm": f.hr if f else None,
            "sd_rr_ms": f.sd_rr if f else None,
            "rmssd_rr_ms": f.rmssd_rr if f else None,
            "alarm": f.samples[-1].alarm if f and f.samples else None,
            "quality": f.samples[-1].quality if f and f.samples else None,
        }
