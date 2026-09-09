# -*- coding: utf-8 -*-
"""
ecg_host.ecg.recorder
=====================

CSV 数据记录模块。

依据报告第 10.2 节，按会话将接收到的数据保存为 CSV 文件。字段：
    sample_index, t_s, raw_adc, filtered_count, r_event_index,
    hr_bpm, sd_rr_ms, rmssd_rr_ms, quality, alarm, frame_seq

数据缺口（采集缺口 / 发送丢帧）单独记录在 *_gaps.csv 中：
    frame_seq, gap_type, expected_index, actual_index
"""

import csv
import os
from datetime import datetime
from typing import List, Optional

from .protocol import WaveformFrame


def _fmt(value) -> str:
    """数值转字符串；None 记为空（对应协议无效值）。"""
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


class CsvRecorder:
    """将会话中的所有样本追加写入 CSV。"""

    def __init__(self, directory: str, name: str = "ecg"):
        os.makedirs(directory, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        base = os.path.join(directory, f"{name}_{stamp}")
        self.wave_path = base + ".csv"
        self.gap_path = base + "_gaps.csv"

        self._seq_seen = 0          # 上一帧号，用于推断缺口
        self._prev_last_index: Optional[int] = None
        self._first_frame = True
        self._closed = False

        self._wf = open(self.wave_path, "w", newline="", encoding="utf-8")
        self._w = csv.writer(self._wf)
        self._w.writerow([
            "sample_index", "t_s", "raw_adc", "filtered_count", "r_event_index",
            "hr_bpm", "sd_rr_ms", "rmssd_rr_ms", "quality", "alarm", "frame_seq",
        ])

        self._gf = open(self.gap_path, "w", newline="", encoding="utf-8")
        self._g = csv.writer(self._gf)
        self._g.writerow(["frame_seq", "gap_type", "expected_index", "actual_index"])

    @property
    def files(self) -> List[str]:
        return [self.wave_path, self.gap_path]

    def record(self, frame: WaveformFrame, recv_time: float) -> None:
        """写一帧（将收到帧前预测的缺口一并记录）。"""
        if self._closed:
            return
        self._detect_gaps(frame)
        t_s = f"{recv_time:.3f}"

        for i, s in enumerate(frame.samples):
            idx = frame.sample0 + i
            self._w.writerow([
                idx,
                t_s,
                s.raw if s.raw is not None else "",
                s.filtered if s.filtered is not None else "",
                s.r_seq if s.r_seq is not None else "",
                _fmt(frame.hr),
                _fmt(frame.sd_rr),
                _fmt(frame.rmssd_rr),
                s.quality,
                s.alarm,
                frame.frame_seq,
            ])
        self._seq_seen = frame.frame_seq
        self._prev_last_index = frame.sample0 + frame.count - 1
        self._first_frame = False

    def _detect_gaps(self, frame: WaveformFrame) -> None:
        """根据帧号 / 序号不连续，写一条缺口记录。"""
        if self._first_frame:
            return
        # 帧号缺口
        if frame.frame_seq != (self._seq_seen + 1) & 0xFFFFFFFF:
            self._g.writerow([frame.frame_seq, "frame_seq_gap",
                              self._seq_seen + 1, frame.frame_seq])
        # 采样序号缺口
        if self._prev_last_index is not None:
            expected = self._prev_last_index + 1
            if frame.sample0 > expected:
                self._g.writerow([frame.frame_seq, "sample_index_gap",
                                  expected, frame.sample0])

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._wf.close()
        self._gf.close()