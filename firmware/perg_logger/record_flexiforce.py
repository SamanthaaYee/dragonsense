"""
Record FlexiForce data from flexiforce_esp32.ino, then plot it.

Start: run the script. Stop: Ctrl+C. The CSV and plot are saved to
data/recordings/flexiforce_<timestamp>.csv/.png and the plot is shown.

Usage:
    python3 record_flexiforce.py /dev/cu.usbserial-210
"""

import csv
import sys
import time
from pathlib import Path

import matplotlib.pyplot as plt
import serial

BAUD = 115200
COLUMNS = ["L_bot", "L_top", "R_top", "R_bot", "balance"]
OUTPUT_DIR = Path(__file__).resolve().parent / "data" / "recordings"


def parse(line: str) -> list[float] | None:
    """'L_bot:1.2,L_top:3.4,...,balance:-0.5' -> [1.2, 3.4, ..., -0.5], or None if malformed."""
    try:
        fields = dict(part.split(":") for part in line.strip().split(","))
        return [float(fields[name]) for name in COLUMNS]
    except (ValueError, KeyError):
        return None


def record(port: str) -> list[list[float]]:
    rows = []
    start = time.monotonic()
    with serial.Serial(port, BAUD, timeout=1) as ser:
        print("Recording... press Ctrl+C to stop.")
        try:
            while True:
                values = parse(ser.readline().decode(errors="ignore"))
                if values:
                    rows.append([round(time.monotonic() - start, 3), *values])
        except KeyboardInterrupt:
            pass
    return rows


def plot(rows: list[list[float]], png_path: Path) -> None:
    t = [r[0] for r in rows]
    left = [r[1] + r[2] for r in rows]
    right = [r[3] + r[4] for r in rows]
    balance = [r[5] for r in rows]

    fig, (ax_left, ax_right, ax_balance) = plt.subplots(3, 1, sharex=True, figsize=(10, 8))
    ax_left.plot(t, left)
    ax_left.set_ylabel("Left force (lb)")
    ax_right.plot(t, right)
    ax_right.set_ylabel("Right force (lb)")
    ax_balance.plot(t, balance, color="black")
    ax_balance.axhline(0, color="gray", linewidth=0.8)
    ax_balance.set_ylabel("Balance, lb (+right / -left)")
    ax_balance.set_xlabel("Time (s)")
    fig.tight_layout()
    fig.savefig(png_path)
    plt.show()


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("Usage: python3 record_flexiforce.py <serial-port>")

    rows = record(sys.argv[1])
    if not rows:
        sys.exit("No data received.")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    base = OUTPUT_DIR / time.strftime("flexiforce_%Y%m%d_%H%M%S")
    with open(base.with_suffix(".csv"), "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["time_s", *COLUMNS])
        writer.writerows(rows)
    print(f"Saved {len(rows)} rows to {base.with_suffix('.csv')}")
    plot(rows, base.with_suffix(".png"))


if __name__ == "__main__":
    main()
