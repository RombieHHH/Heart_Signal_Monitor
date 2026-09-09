# -*- coding: utf-8 -*-
"""
ecg_host.ecg.reader
===================

蓝牙串口读取线程：从虚拟串口（透传蓝牙模块）读字节流，交给 FrameParser 解析，
把解析出的波形帧写入 DataStore，并交给 CsvRecorder 记录。

- 自动识别：port='auto' 时按目标蓝牙模块的 ADDR/NAME（0023:00:0032AA / HSM）
  从系统串口列表中挑选配对出的虚拟串口。
- 自动重连：串口异常/断开后按重试间隔重连。
- 连续无有效帧超过阈值时，在状态中标记连接中断（前端据此提示）。
"""

import logging
import re
import threading
import time
from typing import Optional

import serial
from serial.tools import list_ports

from .datastore import DataStore
from .protocol import FrameParser
from .recorder import CsvRecorder

log = logging.getLogger("ecg.reader")

INACTIVE_TIMEOUT_S = 1.0      # 100 ms 一帧；连续 1 s 无有效帧视为断开
RECONNECT_DELAY_S = 2.0

# 目标蓝牙模块：主机配对后虚拟为 BTHENUM 串口
TARGET_BT_NAME = "HSM"
TARGET_BT_ADDRESS = "0023:00:0032AA"      # 设备信息形如 ADDR:0023:00:0032AA
_TARGET_BT_ADDR_KEY = re.sub(r"[^0-9A-Fa-f]", "", TARGET_BT_ADDRESS).upper()
_BT_HINTS = ("BTHENUM", "BLUETOOTH", "蓝牙")


def list_serial_ports():
    """枚举本机可用的串口，返回 [(device, description), ...]。"""
    try:
        return [(p.device, p.description) for p in list_ports.comports()]
    except Exception as exc:
        log.warning("failed to enumerate serial ports: %s", exc)
        return []


def _port_text(p) -> str:
    """把 pyserial 端口信息拼接成统一的大写文本，供地址/名称匹配。"""
    fields = [p.device, getattr(p, "name", "") or "",
              p.description or "", p.hwid or ""]
    mfr = getattr(p, "manufacturer", None)
    if mfr:
        fields.append(mfr)
    return " ".join(str(f) for f in fields).upper()


def _bt_match_score(p) -> int:
    """端口与目标蓝牙模块的匹配度：地址 > 名称 > 蓝牙串口特征。"""
    text = _port_text(p)
    score = 0
    if _TARGET_BT_ADDR_KEY in text:
        score += 4      # 硬件 ID 中带模块蓝牙地址，最可靠
    if TARGET_BT_NAME.upper() in text:
        score += 3      # 描述/名称带 HSM
    if any(h in text for h in _BT_HINTS):
        score += 1      # 蓝牙虚拟串口（弱特征，仅作兜底排序）
    return score


def _auto_select_port():
    """按 ADDR/NAME 挑选目标蓝牙虚拟串口；无法唯一识别时返回 None 等待重试。"""
    try:
        ports = list(list_ports.comports())
    except Exception as exc:
        log.warning("failed to enumerate serial ports: %s", exc)
        return None
    if not ports:
        log.info("no serial port found, retrying later")
        return None

    scored = [(_bt_match_score(p), p) for p in ports]
    best_score, best = max(scored, key=lambda item: item[0])

    # 地址或名称精确命中：直接使用
    if best_score >= 3:
        log.info("目标蓝牙模块匹配度 %d：%s (%s)",
                 best_score, best.device, best.description)
        return best

    # 无精确特征时仅在候选唯一时才自动打开，避免误连其他蓝牙设备
    if len(ports) == 1:
        log.info("仅发现一个串口 %s，使用之", ports[0].device)
        return ports[0]
    bt_candidates = [p for score, p in scored if score >= 1]
    if len(bt_candidates) == 1:
        log.info("仅发现一个蓝牙串口 %s，使用之", bt_candidates[0].device)
        return bt_candidates[0]

    log.warning("未识别到目标蓝牙模块（%s / %s），本机可用串口较多：%s；"
                "请确认模块已配对/连接，程序将自动重试",
                TARGET_BT_ADDRESS, TARGET_BT_NAME,
                ", ".join(p.device for _, p in scored))
    return None


class SerialReader(threading.Thread):
    """后台读取蓝牙串口的线程，daemon 运行。"""

    def __init__(self, store: DataStore, recorder: Optional[CsvRecorder] = None,
                 port: Optional[str] = "auto", baudrate: int = 115200,
                 name: str = "SerialReader"):
        super().__init__(name=name, daemon=True)
        self.store = store
        self.recorder = recorder
        self.port = port or "auto"
        self.baudrate = baudrate
        self.parser = FrameParser()
        self._stop_evt = threading.Event()
        self._port_handle: Optional[serial.Serial] = None

    # ---- 控制 ----
    def stop(self):
        self._stop_evt.set()

    def set_port(self, port: str):
        """允许运行时切换端口；重启一次读取循环以应用新端口。"""
        self.port = port
        if self._port_handle is not None:
            try:
                self._port_handle.close()
            except Exception:
                pass
            self._port_handle = None

    # ---- 主循环 ----
    def run(self):
        log.info("SerialReader started (port=%s, baud=%d)", self.port, self.baudrate)
        while not self._stop_evt.is_set():
            try:
                self._work_cycle()
            except Exception as exc:  # 捕获未知异常避免线程退出
                log.exception("reader cycle error: %s", exc)
            self.store.update_status(connected=False)
            if not self._stop_evt.is_set():
                self._stop_evt.wait(RECONNECT_DELAY_S)

    def _work_cycle(self):
        conn = self._open_serial()
        if conn is None:
            return
        self._port_handle = conn
        self.parser = FrameParser()
        self.store.update_status(
            connected=False,
            serial_open=True,
            port=conn.port or self.port,
            baudrate=self.baudrate,
            last_frame_time=None,  # 重新连接后重置，避免误判
            bytes_received=0,
        )
        self.store.merge_parser_stats(self.parser.stats)
        bytes_received = 0
        log.info("connected to serial %s", conn.port)

        try:
            while not self._stop_evt.is_set():
                # 读取时阻塞等待，设置超时以响应停止事件
                chunk = conn.read(min(4096, max(1, conn.in_waiting)))
                if not chunk:
                    # 无数据：若连续超时未收到有效帧，标记断连
                    if self._inactive_timed_out():
                        self.store.update_status(connected=False)
                    continue
                bytes_received += len(chunk)
                for frame in self.parser.feed(chunk):
                    recv_ts = time.time()
                    self.store.push_frame(frame)
                    if self.recorder is not None:
                        self.recorder.record(frame, recv_ts)
                self.store.merge_parser_stats(self.parser.stats)
                self.store.update_status(connected=not self._inactive_timed_out(),
                                         bytes_received=bytes_received)
        except serial.SerialException as exc:
            log.warning("serial read error: %s", exc)
        finally:
            try:
                conn.close()
            except Exception:
                pass
            self._port_handle = None
            self.store.update_status(connected=False, serial_open=False)

    # ---- 辅助 ----
    def _open_serial(self):
        """打开串口；port='auto' 时按目标蓝牙模块 ADDR/NAME 自动识别。"""
        target = self.port
        if target == "auto":
            picked = _auto_select_port()
            if picked is None:
                return None
            target = picked.device
            log.info("auto-selected port %s (%s)", target, picked.description)
        try:
            return serial.Serial(
                port=target,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.3,
            )
        except (serial.SerialException, OSError) as exc:
            log.warning("cannot open %s: %s", target, exc)
            self.store.update_status(connected=False, serial_open=False,
                                     port=target, baudrate=self.baudrate)
            return None

    def _inactive_timed_out(self) -> bool:
        """距离最后有效帧超过 INACTIVE_TIMEOUT_S 视为断连。"""
        lt = self.store.status.last_frame_time
        if lt is None:
            return True
        return (time.time() - lt) > INACTIVE_TIMEOUT_S
