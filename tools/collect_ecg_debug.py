#!/usr/bin/env python3
"""Capture 100 Hz ECGDBG diagnostics emitted by MCU USART2."""

import argparse
import csv
from datetime import datetime
from pathlib import Path
import time

import serial


def main():
    parser = argparse.ArgumentParser(description="采集 MCU USART2 ECG 调试数据")
    parser.add_argument("port", help="USART2 对应串口，例如 COM7")
    parser.add_argument("-d", "--duration", type=float, default=60.0)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()
    output = args.output or Path(
        "ecg_debug_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".csv"
    )
    valid = invalid = 0
    deadline = time.monotonic() + args.duration
    with serial.Serial(args.port, 115200, timeout=0.2) as port, \
            output.open("x", newline="", encoding="utf-8") as file:
        port.reset_input_buffer()
        writer = csv.writer(file)
        writer.writerow(("host_time", "sample_index", "raw_adc",
                         "display_filtered", "qrs_filtered",
                         "quality_flags", "leads_off", "signal_valid",
                         "learning_complete", "hr_valid", "rr_count",
                         "detector_threshold", "dropped_samples"))
        while time.monotonic() < deadline:
            line = port.readline().decode("ascii", errors="ignore").strip()
            if not line:
                continue
            fields = line.split(",")
            if len(fields) not in (7, 13) or fields[0] != "ECGDBG":
                invalid += 1
                continue
            try:
                values = [int(value) for value in fields[1:]]
            except ValueError:
                invalid += 1
                continue
            if len(fields) == 7:
                values = values[:5] + ["", "", "", "", "", ""] + values[5:]
            writer.writerow((f"{time.time():.3f}", *values))
            valid += 1
    print(f"保存 {valid} 条调试记录到 {output}；忽略 {invalid} 条无效行")


if __name__ == "__main__":
    main()
