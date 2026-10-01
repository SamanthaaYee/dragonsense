"""
Perg Hall sensor recorder.

Reads CSV lines ("time_us,adc_value") streamed by the ESP32 sketch
perg_logger/perg_logger.ino and saves them to perg_data.csv until Ctrl+C.

Each row is one ADC sample of the Hall sensor voltage, taken every ~1 ms on
GPIO 27. Plotting adc_value against time_us shows the raw waveform. Each
flywheel magnet passing the sensor makes one pulse, so for later analysis:
    f   = 1 / Δt          (Δt = time between successive peaks)
    RPM = f / 8 * 60      (8 magnets on the flywheel)

Usage:
    python3 record_perg.py                 # auto-detect the ESP32 port
    python3 record_perg.py /dev/cu.usbserial-210

Requires pyserial:  python3 -m pip install pyserial
(The package is named "pyserial" but is imported as "serial".)
"""

import csv
import sys
import time

import serial
from serial.tools import list_ports

# Must match BAUD in perg_logger.ino
BAUD = 921600

FILENAME = "perg_data.csv"

# Show every Nth sample in the terminal (1 = every sample). Every sample is
# still saved to the CSV. Raise this if the samples/s rate drops while printing.
PRINT_EVERY = 1

# Width of the text bar drawn next to each value (full width = ADC 4095).
BAR_WIDTH = 50


def find_port():
    """Return the first USB-serial port (the ESP32's USB-UART chip)."""
    for p in list_ports.comports():
        if any(tag in p.device for tag in ("usbserial", "wchusbserial", "SLAB_USBtoUART", "ttyUSB")):
            return p.device
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


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    print(f"Connecting to {port} at {BAUD} baud")

    ser = serial.Serial(port, BAUD, timeout=1)

    # Opening the port resets the ESP32. Wait for it to boot, then discard the
    # boot messages so the file only contains samples.
    time.sleep(2)
    ser.reset_input_buffer()
    # The buffer reset usually cuts a line in half (e.g. "3,0"), so drop the
    # first partial line.
    ser.readline()

    print(f"Recording to {FILENAME}... press Ctrl+C to stop")

    count = 0
    start = time.time()
    last_report = start

    try:
        with open(FILENAME, "w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["time_us", "adc_value"])

            while True:
                row = parse_line(ser.readline())
                if row is None:
                    continue

                writer.writerow(row)
                count += 1

                # Live view: the bar length follows the ADC value, so Hall
                # pulses show up as bars growing and shrinking as you pedal.
                if count % PRINT_EVERY == 0:
                    bar = "#" * (row[1] * BAR_WIDTH // 4095)
                    print(f"{row[0]:>12}  {row[1]:>4}  {bar}")

                # Summary once per second
                now = time.time()
                if now - last_report >= 1:
                    rate = count / (now - start)
                    print(f"--- {count} samples ({rate:.0f}/s) ---")
                    file.flush()
                    last_report = now

    except KeyboardInterrupt:
        print(f"\nStopped. Saved {count} samples to {FILENAME}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
