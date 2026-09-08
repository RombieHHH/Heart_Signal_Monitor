#!/usr/bin/env python3
"""Collect AD8232 raw ADC samples from a serial port into a CSV file."""

from __future__ import annotations

import argparse
import csv
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any


DEFAULT_BAUD_RATE = 115200
DEFAULT_DURATION_SECONDS = 120.0
SERIAL_READ_TIMEOUT_SECONDS = 0.2
CSV_FLUSH_INTERVAL_SAMPLES = 1000
PROGRESS_INTERVAL_SECONDS = 10.0


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Collect newline-delimited AD8232 ADC values from a serial port "
            "and save them as CSV."
        )
    )
    parser.add_argument(
        "port",
        nargs="?",
        help="Serial port connected to USART2, for example COM3.",
    )
    parser.add_argument(
        "-b",
        "--baud-rate",
        type=int,
        default=DEFAULT_BAUD_RATE,
        help="Serial baud rate (default: %(default)s).",
    )
    parser.add_argument(
        "-d",
        "--duration",
        type=float,
        default=DEFAULT_DURATION_SECONDS,
        help="Capture duration in seconds (default: %(default)s).",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help=(
            "Output CSV path. By default, a timestamped file is created in "
            "the current directory."
        ),
    )
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="List available serial ports and exit.",
    )

    arguments = parser.parse_args()
    if not arguments.list_ports and not arguments.port:
        parser.error("port is required unless --list-ports is used")
    if arguments.baud_rate <= 0:
        parser.error("--baud-rate must be greater than zero")
    if arguments.duration <= 0.0:
        parser.error("--duration must be greater than zero")

    return arguments


def load_pyserial() -> tuple[Any, Any]:
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError(
            "pyserial is required. Install it with: python -m pip install pyserial"
        ) from error

    return serial, list_ports


def list_serial_ports(list_ports: Any) -> int:
    ports = sorted(list_ports.comports(), key=lambda port: port.device)
    if not ports:
        print("No serial ports found.")
        return 0

    for port in ports:
        description = port.description or "Unknown device"
        print(f"{port.device}: {description}")
    return 0


def choose_output_path(requested_path: Path | None) -> Path:
    if requested_path is not None:
        return requested_path.expanduser().resolve()

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    candidate = Path.cwd() / f"ad8232_raw_{timestamp}.csv"
    suffix = 1
    while candidate.exists():
        candidate = Path.cwd() / f"ad8232_raw_{timestamp}_{suffix}.csv"
        suffix += 1
    return candidate


def parse_sample(raw_line: bytes) -> int | None:
    text = raw_line.decode("ascii", errors="ignore").strip()
    if not text:
        return None

    try:
        return int(text, 10)
    except ValueError:
        return None


def collect_samples(
    serial_module: Any,
    port: str,
    baud_rate: int,
    duration_seconds: float,
    output_path: Path,
) -> int:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    sample_count = 0
    invalid_line_count = 0
    interrupted = False
    start_monotonic = 0.0
    elapsed = 0.0

    try:
        with serial_module.Serial(
            port=port,
            baudrate=baud_rate,
            bytesize=serial_module.EIGHTBITS,
            parity=serial_module.PARITY_NONE,
            stopbits=serial_module.STOPBITS_ONE,
            timeout=SERIAL_READ_TIMEOUT_SECONDS,
        ) as serial_port:
            serial_port.reset_input_buffer()
            # Synchronize to the next complete CR/LF-delimited sample. The
            # first bytes received after opening may belong to a partial line.
            serial_port.readline()

            with output_path.open("x", newline="", encoding="utf-8") as csv_file:
                writer = csv.writer(csv_file)
                writer.writerow(
                    ("sample_index", "elapsed_seconds", "timestamp", "raw_adc")
                )

                start_wall_time = datetime.now().astimezone()
                start_monotonic = time.monotonic()
                deadline = start_monotonic + duration_seconds
                next_progress = start_monotonic + PROGRESS_INTERVAL_SECONDS

                print(
                    f"Capturing {port} at {baud_rate} baud for "
                    f"{duration_seconds:g} seconds..."
                )
                print(f"CSV: {output_path}")

                try:
                    while True:
                        now = time.monotonic()
                        remaining = deadline - now
                        if remaining <= 0.0:
                            break

                        serial_port.timeout = min(
                            SERIAL_READ_TIMEOUT_SECONDS, remaining
                        )
                        raw_line = serial_port.readline()
                        received_at = time.monotonic()
                        if received_at > deadline:
                            break

                        if not raw_line:
                            continue

                        sample = parse_sample(raw_line)
                        if sample is None:
                            invalid_line_count += 1
                            continue

                        elapsed = received_at - start_monotonic
                        timestamp = start_wall_time + timedelta(seconds=elapsed)
                        writer.writerow(
                            (
                                sample_count,
                                f"{elapsed:.6f}",
                                timestamp.isoformat(timespec="milliseconds"),
                                sample,
                            )
                        )
                        sample_count += 1

                        if sample_count % CSV_FLUSH_INTERVAL_SAMPLES == 0:
                            csv_file.flush()

                        if received_at >= next_progress:
                            print(
                                f"{elapsed:6.1f} s: {sample_count} samples "
                                f"received"
                            )
                            next_progress += PROGRESS_INTERVAL_SECONDS
                except KeyboardInterrupt:
                    interrupted = True
                finally:
                    csv_file.flush()

    except FileExistsError:
        print(f"Output file already exists: {output_path}", file=sys.stderr)
        return 1
    except serial_module.SerialException as error:
        print(f"Serial port error: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"File error: {error}", file=sys.stderr)
        return 1

    if start_monotonic != 0.0:
        elapsed = min(time.monotonic() - start_monotonic, duration_seconds)
    sample_rate = sample_count / elapsed if elapsed > 0.0 else 0.0
    state = "Capture stopped" if interrupted else "Capture complete"
    print(
        f"{state}: {sample_count} valid samples, "
        f"{invalid_line_count} invalid lines, {sample_rate:.1f} samples/s."
    )
    print(f"Saved to: {output_path}")
    return 130 if interrupted else 0


def main() -> int:
    arguments = parse_arguments()

    try:
        serial_module, list_ports = load_pyserial()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 2

    if arguments.list_ports:
        return list_serial_ports(list_ports)

    output_path = choose_output_path(arguments.output)
    return collect_samples(
        serial_module=serial_module,
        port=arguments.port,
        baud_rate=arguments.baud_rate,
        duration_seconds=arguments.duration,
        output_path=output_path,
    )


if __name__ == "__main__":
    raise SystemExit(main())
