# Perg Logger

ESP32 and Python tools for recording and analyzing the raw Hall-effect sensor
signal from a Perg rowing machine flywheel.

The ESP32 sketch samples the Hall sensor on GPIO 27 at 1 kHz and streams CSV
rows over USB serial. The Python recorder saves those rows to `perg_data.csv`,
and the analyzer detects pulses, estimates flywheel RPM, and attempts to
summarize rowing strokes.

## Files

- `perg_logger.ino` - ESP32 sketch that samples the Hall sensor and streams
  `time_us,adc_value`.
- `record_perg.py` - serial logger that records the ESP32 stream to
  `perg_data.csv`.
- `analyze_perg.py` - analysis script that creates plots and stroke summaries.
- [`perg_data.csv`](perg_data.csv) - raw sample recording included so the analysis
  can be reproduced without connecting the hardware.
- [`perg_analysis.png`](perg_analysis.png) - example analysis plot from that recording.

## Wiring

- Perg 3.5 mm left/signal terminal to ESP32 GPIO 27 through a 10 kOhm resistor.
- Perg 3.5 mm ground terminal to ESP32 ground.
- The right terminal is not used.

## Setup

```bash
python3 -m pip install -r requirements.txt
```

To analyze the included example without hardware, run:

```bash
python3 analyze_perg.py perg_data.csv
```

To capture a new session, upload `perg_logger.ino` to the ESP32, then record data
(the recorder overwrites `perg_data.csv`, so copy the example first if needed):

```bash
python3 record_perg.py
```

If auto-detection does not find the ESP32, pass the serial port explicitly:

```bash
python3 record_perg.py /dev/cu.usbserial-210
```

Analyze the captured CSV:

```bash
python3 analyze_perg.py
```

## What the CSV contains

The included `perg_data.csv` is a raw sensor recording, with one sample per row:

| Column | Meaning |
| --- | --- |
| `time_us` | ESP32 timestamp in microseconds since boot, not a date or wall-clock time. |
| `adc_value` | Raw 12-bit ADC reading (0–4095), not a calibrated voltage or force. |

As the flywheel turns, magnets pass the sensor and produce pulses. The analyzer
finds rising threshold crossings, measures the time between pulses, and uses
`RPM = 60 / (8 × pulse_interval_seconds)`, assuming eight magnets per revolution.
With enough valid intervals, it also estimates acceleration, stroke rate, and
drive/recovery timing from changes in flywheel speed.

After filtering, this example contains 29,675 samples over about 35.3 seconds,
with 590 detected pulses and 2,816 sampling gaps longer than 3 ms. The raw file
also contains an initial serial fragment. The analyzer
filters invalid samples and rejects pulse intervals containing gaps longer than
3 ms. It is an early signal-capture example; it does not provide validated
rowing performance measurements.

## Example Output

![Perg Hall sensor waveform and analysis result](perg_analysis.png)

Caption: Raw Perg Hall-effect sensor recording. The top panel shows ADC pulses
from flywheel magnets; red tick marks show detected pulse crossings. The lower
RPM, acceleration, and stroke-rate panels are empty for this recording because
the analyzer found too few valid pulse intervals after checking sampling gaps.

## Notes

- The current RPM conversion assumes 8 magnets on the flywheel.
- The ESP32 ADC clips negative voltage, so this captures the positive half of
  the Hall signal. A DC-bias circuit would improve future measurements.
- The example `perg_data.csv` and `perg_analysis.png` are tracked in this repository.
- `perg_strokes.csv` is generated only when strokes are detected and is ignored by git.
- Both Python scripts write their output files in the current working directory.
