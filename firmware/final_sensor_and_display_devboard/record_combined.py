"""Record Hall samples, Perg telemetry, and foot force from the combined ESP32."""

import argparse
import csv
import math
import sys
import time
from pathlib import Path

import serial

# Reuse the existing Perg serial protocol and keep its recorder independent.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import record_perg as perg


FOOT_COLUMNS = ["time_us", "L_bot_lb", "L_top_lb", "R_top_lb", "R_bot_lb", "balance_lb"]
FOOT_REPORT_INTERVAL_S = 0.25
ADC_LABELS = ("L_bot GPIO14", "L_top GPIO32", "R_top GPIO33", "R_bot GPIO34")
MICROS_MODULUS = 1 << 32


def parse_foot(raw):
    try:
        tag, timestamp, *values = raw.decode("ascii").strip().split(",")
        if tag == "foot" and len(values) == len(FOOT_COLUMNS) - 1:
            timestamp_us = int(timestamp)
            if 0 <= timestamp_us < MICROS_MODULUS and all(
                math.isfinite(float(value)) for value in values
            ):
                return [timestamp_us, *values]
    except (UnicodeDecodeError, ValueError):
        pass
    return None


def format_adc_snapshot(raw):
    try:
        tag, *feet = raw.decode("ascii").strip().split(",")
        if tag != "adc_diag" or len(feet) != 3 * len(ADC_LABELS):
            return None
        readings = []
        for i, label in enumerate(ADC_LABELS):
            count, mv, tare = feet[3 * i:3 * i + 3]
            readings.append(f"{label} {int(count)} ({float(mv):.1f} mV, tare {float(tare):.1f})")
        return " | ".join(readings)
    except (UnicodeDecodeError, ValueError):
        return None


def diagnose(port):
    with serial.Serial(port, perg.BAUD, timeout=0.2) as stream:
        time.sleep(2)
        stream.reset_input_buffer()
        print("Turn the flywheel slowly or press one foot sensor at a time. Ctrl+C to finish.")
        next_request = time.monotonic()
        deadline = next_request + 4
        hall_min, hall_max = None, None
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_request:
                stream.write(b"D")
                next_request = now + 0.25
            raw = stream.read_until(b"\n", size=perg.MAX_LINE_BYTES)
            if raw.startswith(b"adc_diag,") and raw.endswith(b"\n"):
                reading = format_adc_snapshot(raw)
                if reading is not None:
                    hall_range = "no samples" if hall_min is None else f"{hall_min}–{hall_max}"
                    print(f"Hall GPIO27 {hall_range} | {reading}", flush=True)
                    hall_min, hall_max = None, None
                    deadline = time.monotonic() + 4
            elif raw.endswith(b"\n") and raw[:1].isdigit():
                sample = perg.parse_line(raw)
                if sample is not None:
                    adc = sample[1]
                    hall_min = adc if hall_min is None else min(hall_min, adc)
                    hall_max = adc if hall_max is None else max(hall_max, adc)
        raise RuntimeError("No ADC snapshot received. Upload the updated combined sketch and check the port.")


def record(port, directory, start_immediately):
    print(f"Connecting to {port} at {perg.BAUD} baud")
    with serial.Serial(port, perg.BAUD, timeout=1) as stream:
        time.sleep(2)
        stream.reset_input_buffer()
        stream.read_until(b"\n", size=perg.MAX_LINE_BYTES)
        first_hall = perg.wait_for_sample(stream)
        start_us = 0 if start_immediately else perg.wait_for_session_start(stream)

        directory.mkdir(parents=True, exist_ok=False)
        hall_count = telemetry_count = foot_count = 0
        with (
            (directory / "perg_data.csv").open("x", newline="") as hall,
            (directory / "perg_telemetry.csv").open("x", newline="") as telemetry,
            (directory / "foot_force.csv").open("x", newline="") as foot,
        ):
            hall_writer, telemetry_writer, foot_writer = map(csv.writer, (hall, telemetry, foot))
            hall_writer.writerow(["time_us", "adc_value"])
            telemetry_writer.writerow(perg.TELEMETRY_COLUMNS)
            foot_writer.writerow(FOOT_COLUMNS)
            if start_immediately:
                hall_writer.writerow(first_hall)
                hall_count += 1
            print(f"Recording to {directory}. Tap LCD Stop or press Ctrl+C to finish.")
            last_flush = time.monotonic()
            next_foot_report = last_flush

            try:
                while True:
                    raw = stream.read_until(b"\n", size=perg.MAX_LINE_BYTES)
                    if not raw.endswith(b"\n"):
                        continue
                    if raw.startswith(b"session_stop,"):
                        try:
                            int(raw.split(b",", 1)[1])
                        except ValueError:
                            continue
                        print("LCD STOP received.")
                        break
                    if raw.startswith(b"foot,"):
                        row, writer = parse_foot(raw), foot_writer
                    elif raw.startswith(b"telemetry,"):
                        row, writer = perg.parse_telemetry(raw), telemetry_writer
                    else:
                        row, writer = perg.parse_line(raw), hall_writer
                    if row is None:
                        continue
                    if not start_immediately:
                        row[0] = (int(row[0]) - start_us) % MICROS_MODULUS
                    writer.writerow(row)
                    hall_count += writer is hall_writer
                    telemetry_count += writer is telemetry_writer
                    foot_count += writer is foot_writer
                    now = time.monotonic()
                    if writer is foot_writer and now >= next_foot_report:
                        _, left_bottom, left_top, right_top, right_bottom, balance = row
                        print(
                            f"Foot (lb) | L bottom {left_bottom} | L top {left_top} | "
                            f"R top {right_top} | R bottom {right_bottom} | Balance R-L {balance}",
                            flush=True,
                        )
                        next_foot_report = now + FOOT_REPORT_INTERVAL_S
                    if now - last_flush >= 1:
                        for file in (hall, telemetry, foot):
                            file.flush()
                        last_flush = now
            except KeyboardInterrupt:
                print("\nRecording stopped.")
        print(f"Saved {hall_count} Hall, {telemetry_count} telemetry, and {foot_count} foot rows.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", help="USB port of the combined ESP32")
    parser.add_argument("--name", help="new test folder name; prompts if omitted")
    parser.add_argument("--output-dir", type=Path, help="parent folder (default: data/recordings)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--start-immediately", action="store_true", help="record without LCD Start")
    mode.add_argument("--diagnose", action="store_true", help="show live ADC values without recording")
    args = parser.parse_args()
    try:
        if args.diagnose:
            diagnose(args.port or perg.find_port())
        else:
            directory = perg.choose_recording_dir(args.name, output_dir=args.output_dir)
            record(args.port or perg.find_port(), directory, args.start_immediately)
    except KeyboardInterrupt:
        print("\nDiagnostic stopped." if args.diagnose else "\nStopped before recording started.")
    except (OSError, RuntimeError, perg.SerialConfigurationError) as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
