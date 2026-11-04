# Perg Logger

ESP32 firmware and Python tools for capturing a paddling ergometer's flywheel
sensor signal, estimating RPM, and displaying Bluetooth telemetry.

## Project layout

```text
firmware/
  perg_logger/             Original 1 kHz USB raw-sample logger
  perg_bt_logger/          1 kHz Bluetooth telemetry logger
  perg_full_logger/        Combined 2 kHz USB + Bluetooth logger
  perg_stat_display/       CYD Bluetooth display
record_perg.py             USB sample and telemetry recorder
analyze_perg.py            Offline waveform and stroke analysis
requirements.txt          Python dependencies
data/
  examples/perg_data.csv   Original recording used for the example plot
  sessions/anna/           Saved Anna raw samples and telemetry
  sessions/misha/          Saved Misha raw samples and telemetry
  live/                   Latest captures (local, ignored by Git)
docs/images/              Original example analysis plot
outputs/                  Generated analysis files (ignored by Git)
```

Each firmware folder contains a matching `.ino` file so Arduino IDE can open
and compile the sketches separately. Open only the sketch for the board's role.

## Setup

```bash
python3 -m pip install -r requirements.txt
```

For the CYD display, install **TFT_eSPI by Bodmer** in Arduino IDE. Configure
the library's `User_Setup.h` with the ILI9341 driver and pin definitions shown
in `firmware/perg_stat_display/perg_stat_display.ino`. Enable `LOAD_GLCD`,
`LOAD_FONT2`, `LOAD_FONT4`, and `LOAD_FONT7`. Definitions in the sketch alone
do not configure the separately compiled library. Library updates may reset
this setup.

## Record a session

Upload `firmware/perg_full_logger/perg_full_logger.ino` to the sensor ESP32
and `firmware/perg_stat_display/perg_stat_display.ino` to the CYD. The logger
is configured for 2 kHz raw sampling and 10 telemetry updates per second.
Its USB baud rate is 460800, the recorder's default:

```bash
python3 record_perg.py
```

With multiple boards connected, specify the sensor logger's port:

```bash
python3 record_perg.py /dev/cu.usbserial-10
```

For the original USB-only `firmware/perg_logger/perg_logger.ino` sketch,
which samples at 1 kHz, select its matching baud rate:

```bash
python3 record_perg.py /dev/cu.usbserial-210 --baud 921600
```

The recorder validates incoming samples before opening its output files.
It saves raw samples to `data/live/perg_data.csv` and mirrored telemetry to
`data/live/perg_telemetry.csv`, relative to the project directory regardless
of the terminal's working directory. Both files are overwritten when a valid
recording starts. Copy captures into a named folder under `data/sessions/`
to preserve them before the next recording. Stop with Ctrl+C.

Every received sample is saved; the terminal shows every 100th raw sample.
The USB-only sketch produces a telemetry file containing just the header.
The full logger records generated telemetry even when Bluetooth is disconnected;
these records do not confirm delivery to the display. Bluetooth connection
attempts and writes run in a separate task.

At 460800 baud, 8N1 serial carries at most 46,080 bytes/s. Raw CSV at 2 kHz
needs up to 34,000 bytes/s plus telemetry; 10 kHz CSV would need up to
170,000 bytes/s and requires a different format or faster reliable link.

## Analyze a recording

Analyze the original example without hardware:

```bash
python3 analyze_perg.py data/examples/perg_data.csv
```

Analyze the latest capture or a saved session:

```bash
python3 analyze_perg.py
python3 analyze_perg.py data/sessions/misha/perg_data.csv
```

Plots are written to `outputs/perg_analysis.png`. If strokes are detected,
their summaries are written to `outputs/perg_strokes.csv`. Each run overwrites
the plot; a run with no strokes does not replace a previous stroke summary.

The offline analyzer uses a fixed threshold designed for the original
zero-baseline, clipped waveform. It still needs adaptation for biased
recordings; its results on those recordings should not be used as validated
RPM or stroke measurements. The Bluetooth firmware uses adaptive detection.

## Data format and interpretation

Raw sample CSVs contain:

| Column | Meaning |
| --- | --- |
| `time_us` | ESP32 microseconds since boot, not wall-clock time |
| `adc_value` | Raw 12-bit ADC reading (0–4095), not calibrated voltage or force |

Telemetry CSVs contain
`time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts`.
Pace is seconds per 500 m; period is the interval between sensor pulses.
The display also shows peak watts over the last 1.25 seconds.

RPM conversion assumes eight pulses per flywheel revolution:
`RPM = 60 / (8 × pulse_interval_seconds)`. Confirm this against the hardware.
Power, pace, and distance use an adjustable drag model and need calibration
against the Perg display. No calibration from the video comparisons has
been applied to the firmware.

Anna's and Misha's session folders preserve the supplied files without
altering their values. The latest local raw capture duplicates Misha's saved
raw recording. The original example has 29,675 samples after filtering over
about 35.3 seconds, 590 detected pulses, and 2,816 sampling gaps longer than
3 ms. Those statistics describe the original example, not the newer sessions.

## Sensor wiring

The original unconditioned input connects Perg signal (3.5 mm left terminal)
through 10 kOhm to GPIO 27, and Perg ground to ESP32 ground; the right terminal
is unused. Negative voltages are clipped by the ADC. Later work uses a biased
signal; the newer firmware's voltage conversion assumes a baseline of 1870
ADC counts and an approximate 3.6 V scale, which still require calibration.
Check the actual conditioning circuit before relying on firmware wiring comments.

GPIO 27 is ADC2. If Wi-Fi is added, move the analog input to a suitable ADC1
pin and update the sketch.

## Original example output

![Original Hall waveform analysis](docs/images/perg_analysis.png)

The original example's lower panels are empty because too few pulse intervals
survive the sampling-gap checks. It is an early signal-capture example.
