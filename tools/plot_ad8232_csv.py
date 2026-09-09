#!/usr/bin/env python3
"""Display the first 20 seconds of AD8232 raw ADC data from a CSV file."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path


DEFAULT_DURATION_SECONDS = 30.0
DEFAULT_PANEL_SECONDS = 5.0
DEFAULT_SAMPLE_RATE_HZ = 500.0
ADC_MIN_VALUE = 0
ADC_MAX_VALUE = 4095


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Plot the first 20 seconds of raw AD8232 samples. By default, "
            "the newest ad8232_raw_*.csv in the current directory is used."
        )
    )
    parser.add_argument(
        "csv_file",
        nargs="?",
        type=Path,
        help="AD8232 CSV file. Defaults to the newest capture in this folder.",
    )
    parser.add_argument(
        "-d",
        "--duration",
        type=float,
        default=DEFAULT_DURATION_SECONDS,
        help="Seconds to display from the start (default: %(default)s).",
    )
    parser.add_argument(
        "-r",
        "--sample-rate",
        type=float,
        default=DEFAULT_SAMPLE_RATE_HZ,
        help="Fallback sample rate when the CSV has no nominal time column.",
    )
    parser.add_argument(
        "--panel-seconds",
        type=float,
        default=DEFAULT_PANEL_SECONDS,
        help="Seconds shown in each subplot (default: %(default)s).",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Optional PNG output path. The plot window is still displayed.",
    )
    parser.add_argument(
        "--full-range",
        action="store_true",
        help="Use the full ADC range instead of suppressing isolated outliers.",
    )

    arguments = parser.parse_args()
    if arguments.duration <= 0.0:
        parser.error("--duration must be greater than zero")
    if arguments.sample_rate <= 0.0:
        parser.error("--sample-rate must be greater than zero")
    if arguments.panel_seconds <= 0.0:
        parser.error("--panel-seconds must be greater than zero")
    return arguments


def choose_input_path(requested_path: Path | None) -> Path:
    if requested_path is not None:
        path = requested_path.expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(f"CSV file not found: {path}")
        return path

    candidates = sorted(
        Path.cwd().glob("ad8232_raw_*.csv"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        raise FileNotFoundError(
            "No ad8232_raw_*.csv file was found in the current directory."
        )
    return candidates[0].resolve()


def load_samples(
    csv_path: Path,
    duration_seconds: float,
    fallback_sample_rate_hz: float,
) -> tuple[list[float], list[int], str, int]:
    times: list[float] = []
    samples: list[int] = []
    invalid_rows = 0

    with csv_path.open("r", newline="", encoding="utf-8-sig") as csv_file:
        reader = csv.DictReader(csv_file)
        fieldnames = reader.fieldnames or []
        if "raw_adc" not in fieldnames:
            raise ValueError("CSV is missing the required raw_adc column")

        if "nominal_sample_seconds" in fieldnames:
            time_source = "nominal_sample_seconds"
        elif "sample_index" in fieldnames:
            time_source = "sample_index"
        else:
            raise ValueError(
                "CSV must contain nominal_sample_seconds or sample_index"
            )

        for row_number, row in enumerate(reader, start=2):
            try:
                sample = int(row["raw_adc"])
                if not ADC_MIN_VALUE <= sample <= ADC_MAX_VALUE:
                    raise ValueError

                if time_source == "nominal_sample_seconds":
                    sample_time = float(row[time_source])
                else:
                    sample_time = int(row[time_source]) / fallback_sample_rate_hz
            except (KeyError, TypeError, ValueError):
                invalid_rows += 1
                continue

            if sample_time < 0.0:
                invalid_rows += 1
                continue
            if sample_time >= duration_seconds:
                break

            times.append(sample_time)
            samples.append(sample)

    if not samples:
        raise ValueError(
            f"No valid samples were found in the first {duration_seconds:g}s"
        )

    return times, samples, time_source, invalid_rows


def percentile(values: list[int], fraction: float) -> float:
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def display_limits(samples: list[int], full_range: bool) -> tuple[float, float]:
    if full_range:
        lower = float(min(samples))
        upper = float(max(samples))
    else:
        lower = percentile(samples, 0.005)
        upper = percentile(samples, 0.995)

    span = max(upper - lower, 1.0)
    padding = span * 0.08
    return max(ADC_MIN_VALUE, lower - padding), min(ADC_MAX_VALUE, upper + padding)


def plot_samples(
    csv_path: Path,
    times: list[float],
    samples: list[int],
    duration_seconds: float,
    panel_seconds: float,
    full_range: bool,
    output_path: Path | None,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise RuntimeError(
            "matplotlib is required. Install it with: python -m pip install matplotlib"
        ) from error

    panel_count = max(1, math.ceil(duration_seconds / panel_seconds))
    figure_height = max(5.0, panel_count * 2.1)
    figure, axes = plt.subplots(
        panel_count,
        1,
        figsize=(14, figure_height),
        sharey=True,
        squeeze=False,
    )
    axes_list = [row[0] for row in axes]
    y_min, y_max = display_limits(samples, full_range)

    for panel_index, axis in enumerate(axes_list):
        start_time = panel_index * panel_seconds
        end_time = min(start_time + panel_seconds, duration_seconds)
        panel_times = []
        panel_samples = []

        for sample_time, sample in zip(times, samples):
            if start_time <= sample_time < end_time:
                panel_times.append(sample_time)
                panel_samples.append(sample)

        axis.plot(
            panel_times,
            panel_samples,
            color="#006D77",
            linewidth=0.8,
            antialiased=True,
        )
        axis.set_xlim(start_time, end_time)
        axis.set_ylim(y_min, y_max)
        axis.set_ylabel("ADC")
        axis.grid(True, which="major", color="#D7DEE5", linewidth=0.65)
        axis.minorticks_on()
        axis.grid(True, which="minor", color="#EEF1F4", linewidth=0.4)
        axis.set_title(f"{start_time:g}–{end_time:g} s", loc="left", fontsize=10)

    axes_list[-1].set_xlabel("Time (s)")
    figure.suptitle(
        f"AD8232 raw ECG — first {duration_seconds:g} s\n{csv_path.name}",
        fontsize=14,
    )
    figure.tight_layout(rect=(0.02, 0.02, 1.0, 0.95))

    if output_path is not None:
        resolved_output = output_path.expanduser().resolve()
        resolved_output.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(resolved_output, dpi=160, bbox_inches="tight")
        print(f"Saved plot to: {resolved_output}")

    plt.show()


def main() -> int:
    arguments = parse_arguments()

    try:
        csv_path = choose_input_path(arguments.csv_file)
        times, samples, time_source, invalid_rows = load_samples(
            csv_path,
            arguments.duration,
            arguments.sample_rate,
        )
        print(f"CSV: {csv_path}")
        print(
            f"Displaying {len(samples)} samples from the first "
            f"{arguments.duration:g} seconds using {time_source}."
        )
        if invalid_rows:
            print(f"Skipped {invalid_rows} invalid CSV rows.")

        plot_samples(
            csv_path=csv_path,
            times=times,
            samples=samples,
            duration_seconds=arguments.duration,
            panel_seconds=arguments.panel_seconds,
            full_range=arguments.full_range,
            output_path=arguments.output,
        )
    except (FileNotFoundError, OSError, RuntimeError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
