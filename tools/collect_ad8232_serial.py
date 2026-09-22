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
DEFAULT_SAMPLE_RATE_HZ = 500.0
SERIAL_READ_TIMEOUT_SECONDS = 0.05
SERIAL_READ_CHUNK_SIZE = 4096
SERIAL_PENDING_LIMIT_BYTES = 65536
CSV_FLUSH_INTERVAL_SAMPLES = 1000
PROGRESS_INTERVAL_SECONDS = 10.0
ADC_MIN_VALUE = 0
ADC_MAX_VALUE = 4095


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
        "-r",
        "--sample-rate",
        type=float,
        default=DEFAULT_SAMPLE_RATE_HZ,
        help=(
            "Expected ADC sample rate in Hz, used for completeness checks and "
            "the nominal_sample_seconds column (default: %(default)s)."
        ),
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
    if arguments.sample_rate <= 0.0:
        parser.error("--sample-rate must be greater than zero")

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
    try:
        text = raw_line.decode("ascii").strip()
    except UnicodeDecodeError:
        return None

    if not text or not text.isdecimal():
        return None

    try:
        sample = int(text, 10)
    except ValueError:
        return None

    if sample < ADC_MIN_VALUE or sample > ADC_MAX_VALUE:
        return None
    return sample


def take_complete_lines(pending: bytearray) -> list[bytes]:
    """Remove and return all LF-terminated lines currently in pending."""
    last_newline = pending.rfind(b"\n")
    if last_newline < 0:
        return []

    complete = bytes(pending[: last_newline + 1])
    del pending[: last_newline + 1]
    return complete.splitlines()


def collect_samples(
    serial_module: Any,
    port: str,
    baud_rate: int,
    duration_seconds: float,
    sample_rate_hz: float,
    output_path: Path,
) -> int:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    sample_count = 0
    invalid_line_count = 0
    interrupted = False
    start_monotonic = 0.0
    elapsed = 0.0
    pending = bytearray()
    synchronized = False
    partial_line_discarded = False
    next_flush_sample = CSV_FLUSH_INTERVAL_SAMPLES

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

            with output_path.open("x", newline="", encoding="utf-8") as csv_file:
                writer = csv.writer(csv_file)
                writer.writerow(
                    (
                        "sample_index",
                        "elapsed_seconds",
                        "timestamp",
                        "raw_adc",
                        "nominal_sample_seconds",
                    )
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
                        raw_chunk = serial_port.read(SERIAL_READ_CHUNK_SIZE)
                        received_at = time.monotonic()

                        if not raw_chunk:
                            continue

                        pending.extend(raw_chunk)

                        # Opening a running serial stream may begin in the
                        # middle of a number. Discard only that first fragment.
                        if not synchronized:
                            first_newline = pending.find(b"\n")
                            if first_newline < 0:
                                if len(pending) > SERIAL_PENDING_LIMIT_BYTES:
                                    pending.clear()
                                    invalid_line_count += 1
                                continue
                            del pending[: first_newline + 1]
                            synchronized = True

                        lines = take_complete_lines(pending)
                        if len(pending) > SERIAL_PENDING_LIMIT_BYTES:
                            pending.clear()
                            synchronized = False
                            invalid_line_count += 1

                        receive_elapsed = received_at - start_monotonic
                        timestamp = start_wall_time + timedelta(
                            seconds=receive_elapsed
                        )
                        timestamp_text = timestamp.isoformat(timespec="milliseconds")
                        output_rows = []

                        for raw_line in lines:
                            sample = parse_sample(raw_line)
                            if sample is None:
                                invalid_line_count += 1
                                continue

                            nominal_sample_seconds = sample_count / sample_rate_hz
                            output_rows.append(
                                (
                                    sample_count,
                                    f"{receive_elapsed:.6f}",
                                    timestamp_text,
                                    sample,
                                    f"{nominal_sample_seconds:.6f}",
                                )
                            )
                            sample_count += 1

                        if output_rows:
                            writer.writerows(output_rows)

                        if sample_count >= next_flush_sample:
                            csv_file.flush()
                            while sample_count >= next_flush_sample:
                                next_flush_sample += CSV_FLUSH_INTERVAL_SAMPLES

                        if received_at >= next_progress:
                            elapsed = receive_elapsed
                            print(
                                f"{elapsed:6.1f} s: {sample_count} samples "
                                f"received"
                            )
                            while received_at >= next_progress:
                                next_progress += PROGRESS_INTERVAL_SECONDS
                except KeyboardInterrupt:
                    interrupted = True
                finally:
                    partial_line_discarded = bool(pending)
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
    expected_samples = round(duration_seconds * sample_rate_hz)
    completeness = (
        (sample_count / expected_samples) * 100.0 if expected_samples > 0 else 0.0
    )
    state = "Capture stopped" if interrupted else "Capture complete"
    print(
        f"{state}: {sample_count} valid samples, "
        f"{invalid_line_count} invalid lines, {sample_rate:.1f} samples/s."
    )
    print(
        f"Expected about {expected_samples} samples at {sample_rate_hz:g} Hz; "
        f"capture completeness: {completeness:.2f}%."
    )
    if partial_line_discarded:
        print("One incomplete trailing serial line was discarded.")
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
        sample_rate_hz=arguments.sample_rate,
        output_path=output_path,
    )


if __name__ == "__main__":
    raise SystemExit(main())
