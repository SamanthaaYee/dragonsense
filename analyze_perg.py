"""
Analyze a Perg recording made by record_perg.py.

From the raw Hall sensor waveform (perg_data.csv) this computes:
  1. Hall pulses      - one per magnet passing the sensor
  2. Hall frequency   - f = 1 / Δt between consecutive pulses
  3. Flywheel RPM     - RPM = f / 8 * 60   (8 magnets)
  4. Acceleration     - rate of change of RPM (drive > 0, recovery < 0)
  5. Strokes          - each stroke is one RPM rise (drive) + fall (recovery)
  6. Stroke rate      - strokes per minute
  7. Drive/recovery   - time of each phase and their ratio

Usage:
    python3 analyze_perg.py                  # analyzes perg_data.csv
    python3 analyze_perg.py other_file.csv

Outputs:
    perg_analysis.png   - plots of waveform, RPM, acceleration, stroke rate
    perg_strokes.csv    - one row per detected stroke
"""

import csv
import sys

import matplotlib.pyplot as plt
import numpy as np
from scipy.signal import find_peaks

MAGNETS = 8

# A pulse starts when the ADC rises above PULSE_THRESHOLD and ends when it
# drops below half of it. The negative half of the signal is clipped to 0 by
# the ESP32, so between pulses the signal sits at 0 and a low threshold works.
PULSE_THRESHOLD = 50

# An interval between pulses is only trusted if no sampling gap inside it is
# longer than this. Longer gaps mean a pulse could have been missed.
MAX_SAMPLE_GAP_US = 3000

# RPM is smoothed over this window before looking for strokes, so single
# noisy pulse intervals don't look like strokes.
SMOOTH_S = 0.15

# Stroke detection. These are starting guesses; tune them with real data.
MIN_STROKE_PERIOD_S = 0.6       # 100 strokes/min max
MIN_STROKE_PROMINENCE_RPM = 5   # how big an RPM dip must be to count as a stroke


def load(path):
    """Return (t_us, adc) arrays with corrupted serial rows removed."""
    with open(path) as f:
        rows = [(int(a), int(b)) for a, b in list(csv.reader(f))[1:]]
    t_us = np.array([r[0] for r in rows], dtype=np.int64)
    adc = np.array([r[1] for r in rows], dtype=float)

    # Serial glitches produce rows like "3,0" (a boot fragment) or two lines
    # merged into one huge timestamp. A real sample always has a neighbour
    # within 100 ms, so keep only rows that do.
    dt = np.diff(t_us)
    close = (dt > 0) & (dt < 100_000)
    keep = np.zeros(len(t_us), dtype=bool)
    keep[:-1] |= close
    keep[1:] |= close
    keep &= (adc >= 0) & (adc <= 4095)
    t_us, adc = t_us[keep], adc[keep]

    # Drop anything still out of order.
    ordered = np.concatenate([[True], np.diff(t_us) > 0])
    return t_us[ordered], adc[ordered]


def check_sampling(t_us):
    dt = np.diff(t_us)
    median = np.median(dt)
    gaps = dt > MAX_SAMPLE_GAP_US
    print(f"Samples:       {len(t_us)} over {(t_us[-1] - t_us[0]) / 1e6:.1f} s "
          f"({len(t_us) / ((t_us[-1] - t_us[0]) / 1e6):.0f}/s)")
    print(f"Sample period: median {median:.0f} µs, {gaps.sum()} gaps > {MAX_SAMPLE_GAP_US} µs")
    if gaps.mean() > 0.01:
        print("WARNING: sampling has frequent gaps (baud rate too low?). "
              "Pulses inside gaps are missed, so RPM results are unreliable.")
    return gaps


def find_pulses(t_us, adc):
    """Return (pulse_times_us, pulse_peaks) using threshold crossings with hysteresis.

    The pulse time is the interpolated moment the signal rises through
    PULSE_THRESHOLD. A rising edge is sharper than the rounded top, so it
    gives more consistent timing than the peak sample.
    """
    times, peaks = [], []
    in_pulse = False
    peak = 0
    for i in range(1, len(adc)):
        if not in_pulse and adc[i] >= PULSE_THRESHOLD > adc[i - 1]:
            frac = (PULSE_THRESHOLD - adc[i - 1]) / (adc[i] - adc[i - 1])
            times.append(t_us[i - 1] + frac * (t_us[i] - t_us[i - 1]))
            in_pulse = True
            peak = adc[i]
        elif in_pulse:
            peak = max(peak, adc[i])
            if adc[i] < PULSE_THRESHOLD / 2:
                peaks.append(peak)
                in_pulse = False
    if in_pulse:
        peaks.append(peak)
    return np.array(times), np.array(peaks)


def pulse_rpm(pulse_us, t_us, gaps):
    """RPM for each valid pulse interval. Returns (mid_times_s, rpm)."""
    # Count sampling gaps up to each sample, so we can tell whether a gap falls
    # between two pulses.
    gap_count = np.concatenate([[0], np.cumsum(gaps)])
    idx = np.searchsorted(t_us, pulse_us)
    gaps_between = gap_count[idx[1:]] - gap_count[idx[:-1]]

    dt_s = np.diff(pulse_us) / 1e6
    valid = (gaps_between == 0) & (dt_s > 0)
    freq = 1 / dt_s[valid]
    rpm = freq / MAGNETS * 60
    mid_s = (pulse_us[:-1][valid] + pulse_us[1:][valid]) / 2 / 1e6
    return mid_s, rpm


def detect_strokes(t_s, rpm_s):
    """Find strokes on a smoothed, evenly sampled RPM curve.

    A stroke starts at an RPM minimum (catch), speeds up to a maximum (end of
    drive), then slows down to the next minimum (end of recovery).
    """
    dt = t_s[1] - t_s[0]
    distance = max(1, int(MIN_STROKE_PERIOD_S / dt))
    mins, _ = find_peaks(-rpm_s, distance=distance, prominence=MIN_STROKE_PROMINENCE_RPM)

    strokes = []
    for a, b in zip(mins[:-1], mins[1:]):
        top = a + np.argmax(rpm_s[a:b])
        drive = t_s[top] - t_s[a]
        recovery = t_s[b] - t_s[top]
        strokes.append({
            "start_s": round(t_s[a], 3),
            "stroke_rate_spm": round(60 / (t_s[b] - t_s[a]), 1),
            "drive_s": round(drive, 3),
            "recovery_s": round(recovery, 3),
            "recovery_to_drive": round(recovery / drive, 2) if drive > 0 else None,
            "min_rpm": round(rpm_s[a], 1),
            "max_rpm": round(rpm_s[top], 1),
        })
    return mins, strokes


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "perg_data.csv"
    t_us, adc = load(path)
    t0 = t_us[0]
    t_s = (t_us - t0) / 1e6

    gaps = check_sampling(t_us)

    pulse_us, pulse_peaks = find_pulses(t_us, adc)
    print(f"Hall pulses:   {len(pulse_us)} (max ADC {adc.max():.0f})")

    mid_s, rpm = pulse_rpm(pulse_us, t_us, gaps)
    mid_s -= t0 / 1e6

    fig, axes = plt.subplots(4, 1, figsize=(12, 10), sharex=True)
    ax_wave, ax_rpm, ax_acc, ax_rate = axes

    ax_wave.plot(t_s, adc, lw=0.5)
    ax_wave.plot((pulse_us - t0) / 1e6, np.full(len(pulse_us), PULSE_THRESHOLD), "r|", ms=8, label="pulse")
    ax_wave.set_ylabel("ADC")
    ax_wave.set_title(f"Raw Hall waveform ({path})")
    ax_wave.legend(loc="upper right")

    strokes = []
    if len(rpm) < 10:
        print("Not enough valid pulse intervals to compute RPM.")
        for ax in (ax_rpm, ax_acc, ax_rate):
            ax.text(0.5, 0.5, "Not enough valid pulses (check sampling gaps)",
                    ha="center", va="center", transform=ax.transAxes)
    else:
        print(f"Hall freq:     {np.median(rpm) * MAGNETS / 60:.1f} Hz median")
        print(f"RPM:           median {np.median(rpm):.0f}, max {rpm.max():.0f}")

        # Resample RPM onto an even 100 Hz grid and smooth it, so acceleration
        # and stroke detection aren't thrown off by uneven pulse spacing.
        grid = np.arange(mid_s[0], mid_s[-1], 0.01)
        rpm_grid = np.interp(grid, mid_s, rpm)
        win = max(1, int(SMOOTH_S / 0.01))
        # Pad with edge values so the ends don't get pulled toward 0
        padded = np.pad(rpm_grid, (win // 2, win - 1 - win // 2), mode="edge")
        rpm_smooth = np.convolve(padded, np.ones(win) / win, mode="valid")
        accel = np.gradient(rpm_smooth, grid)  # RPM per second

        mins, strokes = detect_strokes(grid, rpm_smooth)

        ax_rpm.plot(mid_s, rpm, ".", ms=2, alpha=0.4, label="per pulse")
        ax_rpm.plot(grid, rpm_smooth, lw=1.2, label="smoothed")
        ax_rpm.plot(grid[mins], rpm_smooth[mins], "kv", ms=5, label="stroke start")
        ax_rpm.set_ylabel("Flywheel RPM")
        ax_rpm.legend(loc="upper right")

        ax_acc.plot(grid, accel, lw=0.8)
        ax_acc.axhline(0, color="gray", lw=0.5)
        ax_acc.fill_between(grid, accel, 0, where=accel > 0, alpha=0.3, color="green", label="speeding up (drive)")
        ax_acc.fill_between(grid, accel, 0, where=accel < 0, alpha=0.3, color="red", label="slowing (recovery)")
        ax_acc.set_ylabel("RPM / s")
        ax_acc.legend(loc="upper right")

        if strokes:
            ax_rate.plot([s["start_s"] for s in strokes], [s["stroke_rate_spm"] for s in strokes], "o-")
        ax_rate.set_ylabel("Strokes / min")

    ax_rate.set_xlabel("Time (s)")
    fig.tight_layout()
    fig.savefig("perg_analysis.png", dpi=120)
    print("Saved plots:   perg_analysis.png")

    if strokes:
        with open("perg_strokes.csv", "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=strokes[0].keys())
            writer.writeheader()
            writer.writerows(strokes)
        rates = [s["stroke_rate_spm"] for s in strokes]
        ratios = [s["recovery_to_drive"] for s in strokes if s["recovery_to_drive"]]
        print(f"Strokes:       {len(strokes)}, median rate {np.median(rates):.0f} spm, "
              f"median recovery:drive {np.median(ratios):.2f}")
        print("Saved strokes: perg_strokes.csv")
    else:
        print("Strokes:       none detected")

    plt.show()


if __name__ == "__main__":
    main()
