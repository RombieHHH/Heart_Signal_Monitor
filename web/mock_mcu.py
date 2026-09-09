# -*- coding: utf-8 -*-
"""
mock_mcu.py
===========

模拟下位机（STM32 + AD8232）：

    PC COM29
       │ TTL
       ▼
    蓝牙模块
       │ Bluetooth SPP
       ▼
    PC COM31

按《预设计报告》10.1 节二进制帧协议，持续发送 ECG 波形帧。

默认：
    COM29
    115200 baud
    500 Hz
    每帧 50 点
    100 ms / frame
    HR = 72 BPM
    RR jitter = 8 ms

重要：
    这里不再使用原来的“10 字节 / 1.9 ms”发送方式。

    每帧 528 字节在 UART 上连续发送：
        528 * 10 / 115200 ≈ 45.83 ms

    之后等待剩余时间，约 94.27 ms，
    再发送下一帧。

    这样更接近 MCU 真正的 UART 连续发送行为，
    也避免 Windows / USB-UART / 蓝牙模块对大量小 write()
    的缓冲与聚合造成影响。

用法：

    python mock_mcu.py

    python mock_mcu.py --port COM29

    python mock_mcu.py --port COM29 --hr 72 --rr-jitter 8

    python mock_mcu.py --port COM29 --baud 115200 --verbose

按 Ctrl+C 停止。
"""

import argparse
import logging
import math
import random
import struct
import time

import serial

from ecg.protocol import RECORD_SIZE, NO_R_EVENT, crc16_ccitt_false


log = logging.getLogger("mock_mcu")


# ============================================================
# 基本参数
# ============================================================

FS = 500                     # ECG 采样率 Hz
SAMPLES_PER_FRAME = 50       # 每帧样点数
FRAME_PERIOD_S = 0.1         # 每帧 100 ms

# UART 使用 8N1：
# 1 byte = 10 bit
UART_BITS_PER_BYTE = 10


# ============================================================
# ECG 波形合成
# ============================================================

def _gauss(x, center, sigma, amp):
    """
    高斯函数。
    """
    dx = (x - center) / max(sigma, 1e-9)
    return amp * math.exp(-0.5 * dx * dx)


def build_beat(samples_per_beat: int) -> list:
    """
    构造一拍 PQRST ECG。

    返回：
        list[float]

    特点：
        R 峰约为 1.0
        基线约为 0
    """

    n = samples_per_beat

    wave = []

    for i in range(n):
        f = i / n

        amp = 0.0

        # P
        amp += _gauss(
            f,
            center=0.18,
            sigma=0.030,
            amp=0.12,
        )

        # Q
        amp += _gauss(
            f,
            center=0.36,
            sigma=0.012,
            amp=-0.15,
        )

        # R
        amp += _gauss(
            f,
            center=0.40,
            sigma=0.020,
            amp=1.0,
        )

        # S
        amp += _gauss(
            f,
            center=0.44,
            sigma=0.015,
            amp=-0.25,
        )

        # T
        amp += _gauss(
            f,
            center=0.55,
            sigma=0.050,
            amp=0.30,
        )

        wave.append(amp)

    return wave


class MockECGStream:
    """
    持续产生 ECG 样本。

    支持：
        HR
        RR jitter
        SDNN / SD RR
        RMSSD
    """

    def __init__(
        self,
        hr=72,
        rr_jitter_ms=8,
        amplitude=1500,
        baseline=2048,
    ):
        self.fs = FS

        self.amplitude = amplitude
        self.baseline = baseline

        self._hr = hr
        self.rr_jitter_ms = rr_jitter_ms

        self.base_rr_ms = 60000.0 / hr

        # 当前 beat
        self._beat = []

        # 当前 beat 剩余样点
        self._beats_left = 0

        # 当前 beat 的 R 峰下标（跨帧保存，保证每个 beat 标一次 R）
        self._r_peak_index = None

        # 最近 RR
        self._rr_deque = []

    # --------------------------------------------------------
    # 创建新 beat
    # --------------------------------------------------------

    def _make_beat(self):
        """
        根据目标 HR 和 RR jitter 创建一拍。
        """

        if self.rr_jitter_ms > 0:
            jitter = (
                random.uniform(-1.0, 1.0)
                * self.rr_jitter_ms
            )
        else:
            jitter = 0.0

        rr_ms = max(
            200.0,
            self.base_rr_ms + jitter,
        )

        n = max(
            1,
            int(round(self.fs * rr_ms / 1000.0)),
        )

        self._beat = build_beat(n)

        self._beats_left = n

        self._r_peak_index = self._next_r_peak_index()

        self._rr_deque.append(rr_ms)

        if len(self._rr_deque) > 30:
            self._rr_deque.pop(0)

    # --------------------------------------------------------
    # HR
    # --------------------------------------------------------

    @property
    def hr(self) -> int:
        """
        根据最近 RR 计算 HR。
        """

        if not self._rr_deque:
            return int(round(self._hr))

        avg_rr = (
            sum(self._rr_deque)
            / len(self._rr_deque)
        )

        return int(
            round(60000.0 / avg_rr)
        )

    # --------------------------------------------------------
    # SD RR
    # --------------------------------------------------------

    @property
    def sd_rr(self) -> float:
        """
        RR 标准差，单位 ms。
        """

        if len(self._rr_deque) < 3:
            return 0.0

        mean = (
            sum(self._rr_deque)
            / len(self._rr_deque)
        )

        variance = sum(
            (x - mean) ** 2
            for x in self._rr_deque
        ) / (
            len(self._rr_deque) - 1
        )

        return math.sqrt(variance)

    # --------------------------------------------------------
    # RMSSD
    # --------------------------------------------------------

    @property
    def rmssd_rr(self) -> float:
        """
        RR 相邻差值 RMS，单位 ms。
        """

        if len(self._rr_deque) < 3:
            return 0.0

        s = 0.0

        for a, b in zip(
            self._rr_deque,
            self._rr_deque[1:],
        ):
            s += (b - a) ** 2

        return math.sqrt(
            s / (len(self._rr_deque) - 1)
        )

    # --------------------------------------------------------
    # 查找 R 峰
    # --------------------------------------------------------

    def _next_r_peak_index(self) -> int:
        """
        返回当前 beat 中 R 峰样点下标。

        直接扫描模板，寻找最大值。
        """

        best = 0
        maximum = -1e9

        for i, value in enumerate(self._beat):
            if value > maximum:
                maximum = value
                best = i

        return best

    # --------------------------------------------------------
    # 生成样点
    # --------------------------------------------------------

    def next_pack(self, n=SAMPLES_PER_FRAME):
        """
        返回 n 个 ECG 样本。

        每个样本：

            (
                raw,
                filtered,
                r_index_or_None
            )

        raw：
            0 ~ 4095

        filtered：
            int16

        r_index：
            当前 beat 中 R 峰位置
        """

        out = []

        for _ in range(n):

            # 当前 beat 用完，生成下一拍
            if self._beats_left <= 0:
                self._make_beat()

            # 当前 beat 的 sample index
            idx = (
                len(self._beat)
                - self._beats_left
            )

            # 波形值
            value = self._beat[idx]

            self._beats_left -= 1

            # ------------------------------------------------
            # RAW
            # ------------------------------------------------

            raw = round(
                self.baseline
                + self.amplitude * value
            )

            raw = max(
                0,
                min(4095, raw)
            )

            # ------------------------------------------------
            # Filtered
            # ------------------------------------------------

            filtered = round(
                self.amplitude * value
            )

            filtered = max(
                -32768,
                min(32767, filtered)
            )

            # ------------------------------------------------
            # R event
            # ------------------------------------------------

            if (
                self._r_peak_index is not None
                and idx == self._r_peak_index
            ):
                r_index = idx
            else:
                r_index = None

            out.append(
                (
                    raw,
                    filtered,
                    r_index,
                )
            )

        return out


# ============================================================
# 帧打包
# ============================================================

def build_frame(
    frame_seq,
    sample0,
    samples,
    hr_raw,
    sd_raw,
    rm_raw,
):
    """
    根据协议打包一帧。

    samples：

        (
            raw,
            filtered,
            r_index
        )

    帧格式：

        Header
        +
        sample records
        +
        CRC16

    总长度：

        26 + 50 * 10 + 2
        = 528 bytes
    """

    count = len(samples)

    payload = bytearray()

    # --------------------------------------------------------
    # Sample records
    # --------------------------------------------------------

    for raw, filtered, r_index in samples:

        if r_index is not None:
            r_seq = (
                r_index
                + sample0
            )
        else:
            r_seq = NO_R_EVENT

        # RECORD_SIZE = 10 bytes
        #
        # <HhIBB
        #
        # H  raw
        # h  filtered
        # I  r_seq
        # B  quality
        # B  alarm
        #
        payload += struct.pack(
            "<HhIBB",
            raw,
            filtered,
            r_seq,
            0,      # quality = 0
            2,      # alarm = 2
        )

    # --------------------------------------------------------
    # Header
    # --------------------------------------------------------

    hdr = struct.pack(
        "<HBBHHIIHHHHH",

        0x5AA5,               # magic
        1,                    # version
        1,                    # type

        count * RECORD_SIZE,  # payload_len

        0,                    # reserved

        frame_seq,            # uint32
        sample0,              # uint32

        FS,                   # sample rate
        count,                # sample count

        hr_raw,               # HR * 10
        sd_raw,               # SD RR * 10
        rm_raw,               # RMSSD * 10
    )

    # --------------------------------------------------------
    # CRC
    # --------------------------------------------------------

    crc = crc16_ccitt_false(
        hdr[2:] + payload
    )

    # --------------------------------------------------------
    # 完整帧
    # --------------------------------------------------------

    frame = (
        hdr
        + payload
        + struct.pack("<H", crc)
    )

    return frame


# ============================================================
# UART 发送
# ============================================================

def write_frame(
    ser: serial.Serial,
    frame: bytes,
) -> int:
    """
    连续发送整个 frame。

    不进行 10B/1.9ms 分片。

    返回：
        实际写入字节数
    """

    total = len(frame)

    sent = 0

    # --------------------------------------------------------
    # 一次 write
    # --------------------------------------------------------

    n = ser.write(frame)

    if n is None:
        n = 0

    sent += n

    # --------------------------------------------------------
    # flush
    # --------------------------------------------------------
    #
    # 注意：
    # pyserial 的 flush() 会等待输出缓冲区发送出去。
    #
    # 这比只调用 write() 更适合这里做测试，
    # 可以避免 Python 层提前进入下一帧。
    # --------------------------------------------------------

    ser.flush()

    return sent


# ============================================================
# 参数
# ============================================================

def parse_args(argv=None):

    parser = argparse.ArgumentParser(
        description=(
            "下位机 ECG 帧模拟器（发往蓝牙模块 TTL）"
        )
    )

    parser.add_argument(
        "--port",
        default="COM29",
        help="蓝牙模块 TTL 所接串口，默认 COM29",
    )

    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="波特率，默认 115200",
    )

    parser.add_argument(
        "--hr",
        type=float,
        default=72,
        help="目标心率 BPM，默认 72",
    )

    parser.add_argument(
        "--rr-jitter",
        type=float,
        default=8.0,
        help="RR 抖动 ms，默认 8；设为 0 关闭",
    )

    parser.add_argument(
        "--amplitude",
        type=int,
        default=1500,
        help="QRS 幅度，默认 1500",
    )

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="开启详细日志",
    )

    return parser.parse_args(argv)


# ============================================================
# Main
# ============================================================

def main(argv=None):

    args = parse_args(argv)

    # --------------------------------------------------------
    # Logging
    # --------------------------------------------------------

    logging.basicConfig(
        level=(
            logging.DEBUG
            if args.verbose
            else logging.INFO
        ),
        format=(
            "%(asctime)s "
            "[%(levelname)s] "
            "%(message)s"
        ),
    )

    # --------------------------------------------------------
    # ECG generator
    # --------------------------------------------------------

    gen = MockECGStream(
        hr=args.hr,
        rr_jitter_ms=max(
            0.0,
            args.rr_jitter,
        ),
        amplitude=args.amplitude,
    )

    write_ok = 0
    write_err = 0

    # --------------------------------------------------------
    # 串口
    # --------------------------------------------------------

    try:

        with serial.Serial(
            port=args.port,
            baudrate=args.baud,

            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,

            timeout=0.2,

            # 明确关闭流控
            xonxoff=False,
            rtscts=False,
            dsrdtr=False,
        ) as ser:

            log.info(
                "mock 发送到 %s @ %d baud",
                args.port,
                args.baud,
            )

            log.info(
                "UART: 8N1, "
                "xonxoff=False, "
                "rtscts=False, "
                "dsrdtr=False"
            )

            log.info(
                "每帧: %d bytes, %d samples, %.1f ms",
                26
                + SAMPLES_PER_FRAME * RECORD_SIZE
                + 2,
                SAMPLES_PER_FRAME,
                FRAME_PERIOD_S * 1000.0,
            )

            # 理论 UART 发送时间
            expected_tx_time = (
                (
                    26
                    + SAMPLES_PER_FRAME * RECORD_SIZE
                    + 2
                )
                * UART_BITS_PER_BYTE
                / args.baud
            )

            log.info(
                "理论 UART 发送时间约 %.3f ms",
                expected_tx_time * 1000.0,
            )

            # ------------------------------------------------
            # frame / sample sequence
            # ------------------------------------------------

            frame_seq = 0
            sample0 = 0

            # 使用绝对时间调度，避免 sleep 累积误差
            next_slot = time.perf_counter()

            while True:

                # ==================================================
                # 1. 生成 50 个样本
                # ==================================================

                samples = gen.next_pack(
                    SAMPLES_PER_FRAME
                )

                # ==================================================
                # 2. 生成帧
                # ==================================================

                frame = build_frame(
                    frame_seq,
                    sample0,
                    samples,

                    int(
                        round(
                            gen.hr * 10.0
                        )
                    ),

                    int(
                        round(
                            gen.sd_rr * 10.0
                        )
                    ),

                    int(
                        round(
                            gen.rmssd_rr * 10.0
                        )
                    ),
                )

                # --------------------------------------------------
                # 检查帧长度
                # --------------------------------------------------

                expected_len = (
                    26
                    + SAMPLES_PER_FRAME
                    * RECORD_SIZE
                    + 2
                )

                if len(frame) != expected_len:

                    log.error(
                        "帧长度异常：%d != %d",
                        len(frame),
                        expected_len,
                    )

                # ==================================================
                # 3. 连续发送整帧
                # ==================================================

                tx_start = time.perf_counter()

                try:

                    n = write_frame(
                        ser,
                        frame,
                    )

                except serial.SerialTimeoutException as exc:

                    write_err += 1

                    log.error(
                        "串口写超时：%s",
                        exc,
                    )

                    n = 0

                tx_elapsed = (
                    time.perf_counter()
                    - tx_start
                )

                # ==================================================
                # 4. 统计
                # ==================================================

                if n == len(frame):
                    write_ok += 1
                else:
                    write_err += 1

                    log.warning(
                        "帧#%d 写入不完整：%d/%d",
                        frame_seq,
                        n,
                        len(frame),
                    )

                # ==================================================
                # 5. 日志
                # ==================================================

                if (
                    frame_seq < 5
                    or frame_seq % 100 == 0
                ):

                    log.info(
                        (
                            "帧#%d "
                            "sample0=%d "
                            "HR=%d "
                            "SD_RR=%.2fms "
                            "RMSSD=%.2fms "
                            "len=%d "
                            "tx=%.3fms "
                            "OK=%d "
                            "ERR=%d"
                        ),

                        frame_seq,
                        sample0,
                        gen.hr,
                        gen.sd_rr,
                        gen.rmssd_rr,

                        len(frame),

                        tx_elapsed * 1000.0,

                        write_ok,
                        write_err,
                    )

                # ==================================================
                # 6. 下一帧
                # ==================================================

                frame_seq = (
                    frame_seq + 1
                ) & 0xFFFFFFFF

                sample0 += SAMPLES_PER_FRAME

                # --------------------------------------------------
                # 绝对时间调度
                # --------------------------------------------------

                next_slot += FRAME_PERIOD_S

                delay = (
                    next_slot
                    - time.perf_counter()
                )

                if delay > 0:

                    time.sleep(delay)

                else:

                    # 如果由于某种原因发送超过 100ms，
                    # 不让调度时间无限落后。
                    #
                    # 重新基准化。
                    next_slot = (
                        time.perf_counter()
                    )

    # ------------------------------------------------------------
    # 串口打开失败
    # ------------------------------------------------------------

    except serial.SerialException as exc:

        log.error(
            "无法打开串口 %s：%s",
            args.port,
            exc,
        )

        log.info(
            "请确认："
            "蓝牙模块 TTL 已连接到该串口，"
            "且 COM29 未被其他程序占用。"
        )

    # ------------------------------------------------------------
    # Ctrl+C
    # ------------------------------------------------------------

    except KeyboardInterrupt:

        log.info(
            "mock 停止。"
            "累计成功发送 %d 帧，失败 %d 帧。",
            write_ok,
            write_err,
        )


# ============================================================
# Entry
# ============================================================

if __name__ == "__main__":
    main()
