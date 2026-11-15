"""
Perg Hall sensor recorder.

Reads CSV lines ("time_us,adc_value") streamed by perg_logger.ino or
perg_full_logger.ino and saves them to data/live/NAME/perg_data.csv until Ctrl+C.
The full logger also mirrors its Bluetooth telemetry over USB. Tagged
telemetry lines are saved to data/live/NAME/perg_telemetry.csv with the ESP32 timestamp,
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
    python3 record_perg.py --name calibration_01
    python3 record_perg.py --start-on-button
    python3 record_perg.py /dev/cu.usbserial-10 --baud 921600  # original sketch

Requires pyserial:  python3 -m pip install pyserial
(The package is named "pyserial" but is imported as "serial".)
"""

import argparse
import csv
import math
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

# Matches the full logger. Use --baud 921600 for the original perg_logger.ino.
BAUD = 460800
CONNECT_TIMEOUT_S = 8
MAX_LINE_BYTES = 128

OUTPUT_DIR = Path(__file__).resolve().parent / "data" / "live"
TELEMETRY_COLUMNS = [
    "time_us",
    "rpm",
    "pace_s",
    "distance_m",
    "hall_v",
    "period_ms",
    "elapsed_s",
    "watts",
    "stroke_rate_spm",
]

# Show every Nth sample in the terminal (1 = every sample). Every sample is
# still saved to the CSV. Raise this if the samples/s rate drops while printing.
PRINT_EVERY = 100

# Width of the text bar drawn next to each value (full width = ADC 4095).
BAR_WIDTH = 50


def choose_recording_dir(name=None):
    """Choose a new named capture without replacing an existing folder."""
    supplied_name = name is not None
    while True:
        if not supplied_name:
            try:
                name = input("Save recording as (e.g. calibration_01): ")
            except (EOFError, KeyboardInterrupt):
                sys.exit("\nRecording cancelled before connecting.")
        name = name.strip()
        if not name or name in (".", "..") or any(c in name for c in "/\\"):
            error = "Enter a recording name without path separators."
        elif (OUTPUT_DIR / name).exists():
            error = f"Recording '{name}' already exists. Choose a different name."
        else:
            return OUTPUT_DIR / name
        if supplied_name:
            sys.exit(error)
        print(error)


def find_port():
    """Auto-select only when there is a single USB-serial candidate."""
    ports = [
        p.device
        for p in list_ports.comports()
        if any(
            tag in p.device
            for tag in ("usbserial", "wchusbserial", "SLAB_USBtoUART", "ttyUSB")
        )
    ]
    if len(ports) == 1:
        return ports[0]
    if len(ports) > 1:
        sys.exit(
            "Multiple serial ports found: "
            + ", ".join(ports)
            + "\nPass the logger's port explicitly: python3 record_perg.py PORT"
        )
    sys.exit(
        "No ESP32 serial port found. Plug in the board or pass the port as an argument."
    )


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
        if len(parts) not in (9, 10) or parts[0] != "telemetry":
            return None
        int(parts[1])
        if not all(math.isfinite(float(value)) for value in parts[2:]):
            return None
        # Older firmware has no stroke-rate field. Preserve its unknown value.
        return parts[1:] + ([""] if len(parts) == 9 else [])
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

    detail = (
        f"Received bytes, but no valid time_us,adc_value CSV. Last bytes: {received!r}"
        if received
        else "No serial data received."
    )
    raise RuntimeError(
        f"No valid samples within {CONNECT_TIMEOUT_S} seconds. {detail}\n"
        f"Check the selected port and upload perg_logger.ino or "
        f"perg_full_logger.ino with BAUD = {ser.baudrate}. "
        "Close Arduino Serial Monitor before recording."
    )


def wait_for_session_start(ser):
    """Arm the updated full logger and wait for the LCD Start button."""
    ser.write(b"A")
    ser.flush()
    print("Preparing the LCD Start/Stop button...")
    ready = False
    deadline = time.monotonic() + CONNECT_TIMEOUT_S
    next_arm_attempt = time.monotonic() + 1
    while True:
        if not ready:
            now = time.monotonic()
            if now > deadline:
                raise RuntimeError(
                    "No session_ready reply after repeated attempts. Check the "
                    "sensor ESP32's USB connection and confirm it has the updated "
                    "Start/Stop perg_full_logger sketch."
                )
            if now >= next_arm_attempt:
                # Arming is idempotent. Retry only before readiness; never reset
                # a session after telling the user to press Start.
                ser.write(b"A")
                ser.flush()
                next_arm_attempt = now + 1
        raw = ser.read_until(b"\n", size=MAX_LINE_BYTES)
        if not raw.endswith(b"\n"):
            continue
        parts = raw.decode(errors="ignore").strip().split(",")
        if len(parts) == 2 and parts[0] in ("session_ready", "session_start"):
            try:
                start_us = int(parts[1])
            except ValueError:
                continue
            if parts[0] == "session_ready":
                ready = True
                print(
                    "Ready: tap START on the LCD, then paddle. Tap STOP to finish and save."
                )
            elif ready:
                return start_us


def main():
    parser = argparse.ArgumentParser(
        description="Record Perg Hall samples and telemetry over USB."
    )
    parser.add_argument(
        "port", nargs="?", help="USB serial port; auto-detect if omitted"
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=BAUD,
        help=f"must match the uploaded sketch (default: {BAUD})",
    )
    parser.add_argument("--name", help="new recording name; prompts if omitted")
    parser.add_argument(
        "--start-on-button",
        "--start-on-motion",
        dest="session_control",
        action="store_true",
        help="LCD Start/Stop controls capture with session-relative timestamps "
        "(--start-on-motion is a legacy alias; motion no longer starts the timer)",
    )
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")
    recording_dir = choose_recording_dir(args.name)
    filename = recording_dir / "perg_data.csv"
    telemetry_filename = recording_dir / "perg_telemetry.csv"
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
        start_us = 0
        if args.session_control:
            start_us = wait_for_session_start(ser)
            first_row = None
        start = time.time()
        last_report = start

        # Reserve the folder exclusively, including if another recorder started
        # after the name check. Never truncate an existing recording.
        recording_dir.mkdir(parents=True, exist_ok=False)
        with (
            open(filename, "x", newline="") as file,
            open(telemetry_filename, "x", newline="") as telemetry_file,
        ):
            print(f"Recording to {filename}... press Ctrl+C to stop")
            print(f"Bluetooth telemetry mirrored over USB goes to {telemetry_filename}")
            writer = csv.writer(file)
            telemetry_writer = csv.writer(telemetry_file)
            telemetry_writer.writerow(TELEMETRY_COLUMNS)
            writer.writerow(["time_us", "adc_value"])
            if first_row is not None:
                writer.writerow([(first_row[0] - start_us) % (1 << 32), first_row[1]])
                count = 1

            while True:
                raw = ser.read_until(b"\n", size=MAX_LINE_BYTES)
                if not raw.endswith(b"\n"):
                    continue
                if args.session_control and raw.startswith(b"session_stop,"):
                    try:
                        int(raw.decode("ascii").strip().split(",")[1])
                    except (ValueError, UnicodeDecodeError, IndexError):
                        continue
                    print(f"\nLCD STOP received. Saved {count} samples to {filename}")
                    print(
                        f"Saved {telemetry_count} telemetry updates to {telemetry_filename}"
                    )
                    break
                telemetry = parse_telemetry(raw)
                row = parse_line(raw) if telemetry is None else None
                if telemetry is not None:
                    if args.session_control:
                        telemetry[0] = str((int(telemetry[0]) - start_us) % (1 << 32))
                    telemetry_writer.writerow(telemetry)
                    telemetry_count += 1
                elif row is not None:
                    if args.session_control:
                        row[0] = (row[0] - start_us) % (1 << 32)
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
                    print(
                        f"--- {count} samples ({rate:.0f}/s), "
                        f"{telemetry_count} telemetry updates ---"
                    )
                    file.flush()
                    telemetry_file.flush()
                    last_report = now

    except KeyboardInterrupt:
        if count:
            print(f"\nStopped. Saved {count} samples to {filename}")
            print(f"Saved {telemetry_count} telemetry updates to {telemetry_filename}")
        else:
            print("\nStopped before recording started.")
    except RuntimeError as error:
        sys.exit(str(error))
    except OSError as error:
        sys.exit(f"Could not save recording: {error}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
