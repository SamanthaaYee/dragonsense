"""
Perg Hall sensor recorder.

Reads CSV lines ("time_us,adc_value") streamed by perg_logger.ino or
perg_full_logger.ino and saves them to data/live/perg_data.csv until Ctrl+C.
The full logger also mirrors its Bluetooth telemetry over USB. Tagged
telemetry lines are saved to data/live/perg_telemetry.csv with the ESP32 timestamp,
RPM, pace, distance, Hall voltage, pulse period, elapsed time, and watts.

Each row is one ADC sample of the Hall sensor voltage on GPIO 27, sampled
at 2 kHz by the full logger (1 kHz by the original logger). Plotting
adc_value against time_us shows the raw waveform. Each
flywheel magnet passing the sensor makes one pulse, so for later analysis:
    f   = 1 / Δt          (Δt = time between successive peaks)
    RPM = f / 8 * 60      (8 magnets on the flywheel)

Usage:
    python3 record_perg.py                 # auto-detect the ESP32 port
    python3 record_perg.py /dev/cu.usbserial-210
    python3 record_perg.py /dev/cu.usbserial-10 --baud 921600  # original sketch

Requires pyserial:  python3 -m pip install pyserial
(The package is named "pyserial" but is imported as "serial".)
"""

import argparse
import csv
import math
from pathlib import Path
import sys
import time

import serial
from serial.tools import list_ports

# Matches the full logger. Use --baud 921600 for the original perg_logger.ino.
BAUD = 460800
CONNECT_TIMEOUT_S = 8
MAX_LINE_BYTES = 128

OUTPUT_DIR = Path(__file__).resolve().parent / "data" / "live"
FILENAME = OUTPUT_DIR / "perg_data.csv"
TELEMETRY_FILENAME = OUTPUT_DIR / "perg_telemetry.csv"
TELEMETRY_COLUMNS = [
    "time_us", "rpm", "pace_s", "distance_m", "hall_v", "period_ms",
    "elapsed_s", "watts",
]

# Show every Nth sample in the terminal (1 = every sample). Every sample is
# still saved to the CSV. Raise this if the samples/s rate drops while printing.
PRINT_EVERY = 100

# Width of the text bar drawn next to each value (full width = ADC 4095).
BAR_WIDTH = 50


def find_port():
    """Auto-select only when there is a single USB-serial candidate."""
    ports = [p.device for p in list_ports.comports()
             if any(tag in p.device for tag in
                    ("usbserial", "wchusbserial", "SLAB_USBtoUART", "ttyUSB"))]
    if len(ports) == 1:
        return ports[0]
    if len(ports) > 1:
        sys.exit("Multiple serial ports found: " + ", ".join(ports)
                 + "\nPass the logger's port explicitly: python3 record_perg.py PORT")
    sys.exit("No ESP32 serial port found. Plug in the board or pass the port as an argument.")


def parse_line(raw):
    """Return [time_us, adc_value] as ints, or None for boot noise/partial lines."""
    parts = raw.decode(errors="ignore").strip().split(",")
    if len(parts) != 2:
        return None
    try:
        return [int(parts[0]), int(parts[1])]
    except ValueError:
        return None


def parse_telemetry(raw):
    """Validate tagged USB telemetry and retain the transmitted precision."""
    try:
        parts = raw.decode("ascii").strip().split(",")
        if len(parts) != 9 or parts[0] != "telemetry":
            return None
        int(parts[1])
        if not all(math.isfinite(float(value)) for value in parts[2:]):
            return None
        return parts[1:]
    except (UnicodeDecodeError, ValueError):
        return None


def wait_for_sample(ser):
    """Require actual sample data before declaring a connection or saving."""
    deadline = time.monotonic() + CONNECT_TIMEOUT_S
    received = b""
    while time.monotonic() < deadline:
        # Bound reads so garbage without newlines cannot stall startup.
        raw = ser.read_until(b"\n", size=MAX_LINE_BYTES)
        if raw:
            received = raw[:80]
        row = parse_line(raw)
        if raw.endswith(b"\n") and row is not None:
            return row

    detail = (f"Received bytes, but no valid time_us,adc_value CSV. "
              f"Last bytes: {received!r}" if received else "No serial data received.")
    raise RuntimeError(
        f"No valid samples within {CONNECT_TIMEOUT_S} seconds. {detail}\n"
        f"Check the selected port and upload perg_logger.ino or "
        f"perg_full_logger.ino with BAUD = {ser.baudrate}. "
        "Close Arduino Serial Monitor before recording."
    )


def main():
    parser = argparse.ArgumentParser(description="Record Perg Hall samples and telemetry over USB.")
    parser.add_argument("port", nargs="?", help="USB serial port; auto-detect if omitted")
    parser.add_argument("--baud", type=int, default=BAUD,
                        help=f"must match the uploaded sketch (default: {BAUD})")
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")
    port = args.port or find_port()
    print(f"Connecting to {port} at {args.baud} baud")

    ser = serial.Serial(port, args.baud, timeout=1)

    count = 0
    telemetry_count = 0
    try:
        # Opening the port may reset the board. Discard boot output and the
        # first potentially partial line, then confirm the expected stream.
        time.sleep(2)
        ser.reset_input_buffer()
        ser.read_until(b"\n", size=MAX_LINE_BYTES)
        first_row = wait_for_sample(ser)
        print(f"Connected to {port}: receiving Hall sensor samples")
        print(f"Recording to {FILENAME}... press Ctrl+C to stop")
        print(f"Bluetooth telemetry mirrored over USB goes to {TELEMETRY_FILENAME}")
        start = time.time()
        last_report = start

        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
        with open(FILENAME, "w", newline="") as file, \
                open(TELEMETRY_FILENAME, "w", newline="") as telemetry_file:
            writer = csv.writer(file)
            telemetry_writer = csv.writer(telemetry_file)
            telemetry_writer.writerow(TELEMETRY_COLUMNS)
            writer.writerow(["time_us", "adc_value"])
            writer.writerow(first_row)
            count = 1

            while True:
                raw = ser.read_until(b"\n", size=MAX_LINE_BYTES)
                if not raw.endswith(b"\n"):
                    continue
                telemetry = parse_telemetry(raw)
                row = parse_line(raw) if telemetry is None else None
                if telemetry is not None:
                    telemetry_writer.writerow(telemetry)
                    telemetry_count += 1
                elif row is not None:
                    writer.writerow(row)
                    count += 1
                else:
                    continue

                # Live view: the bar length follows the ADC value, so Hall
                # pulses show up as bars growing and shrinking as you pedal.
                if row is not None and count % PRINT_EVERY == 0:
                    bar = "#" * (row[1] * BAR_WIDTH // 4095)
                    print(f"{row[0]:>12}  {row[1]:>4}  {bar}")

                # Summary once per second
                now = time.time()
                if now - last_report >= 1:
                    rate = count / (now - start)
                    print(f"--- {count} samples ({rate:.0f}/s), "
                          f"{telemetry_count} telemetry updates ---")
                    file.flush()
                    telemetry_file.flush()
                    last_report = now

    except KeyboardInterrupt:
        if count:
            print(f"\nStopped. Saved {count} samples to {FILENAME}")
            print(f"Saved {telemetry_count} telemetry updates to {TELEMETRY_FILENAME}")
        else:
            print("\nStopped before recording started.")
    except RuntimeError as error:
        sys.exit(str(error))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
