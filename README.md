# Perg Logger

ESP32 firmware and Python tools for capturing a paddling ergometer's flywheel
sensor signal, estimating RPM, displaying Bluetooth telemetry, and replaying
recorded or synthetic sensor waveforms into the original monitor.

## Current firmware

- **Sensor ESP32** reads the flywheel Hall signal and four foot sensors.
  [Combined sensor logger setup](firmware/final_sensor_and_display_devboard/README.md)
- **Freenove ESP32-S3 LCD** shows workout stats, the estimated stroke force
  curve, and live left/right foot balance.
  [Force display setup](firmware/perg_freenove_force_display/README.md)

From the repository root, run the combined recorder with the sensor board's
USB port. Wait for the prompt, then tap **START** on the LCD. Tap **STOP** to
save the session.

```bash
python3 firmware/final_sensor_and_display_devboard/record_combined.py /dev/cu.usbserial-XXX
```

Earlier logger, display, and standalone foot-sensor sketches are in
[`firmware/Old Firmware`](<firmware/Old Firmware>).

## Project background

**Calibration remains experimental.** The monitor has not yet accepted a
replayed drag-59 calibration. See the [November 9 progress report](docs/experiments/2026-11-09.md)
for observed results, archived data, and known limitations.

The latest Anna and Irtaza collection is labeled **pace-based**. Seven raw and
telemetry recordings are preserved in the [pace-based archive](data/sessions/2026-11-15_pace_based/README.md).
See the [November 15 progress note](docs/experiments/2026-11-15-pace-based.md)
for the protocol label, timing findings, and serial connection troubleshooting.

## Project layout

```text
firmware/
  final_sensor_and_display_devboard/  Current Hall and foot-sensor logger
  perg_freenove_force_display/        Current Freenove LCD dashboard
  Old Firmware/            Earlier loggers, displays, and foot-sensor sketches
  ble_mac_address/         BLE address lookup sketch
  perg_signal_player/      DAC waveform player (separate board role)
record_perg.py             USB sample and telemetry recorder
analyze_perg.py            Offline waveform and stroke analysis
replay_lin_probe.py        Replay, synthetic trials, optional LIN decoding
perg_signal.py             CSV validation and timestamp-based resampling
perg_synthetic.py          Synthetic coast-down model and generation
tests/                    Hardware-free workflow tests
requirements.txt          Python dependencies
data/
  examples/perg_data.csv   Original recording used for the example plot
  sessions/anna/           Saved Anna raw samples and telemetry
  sessions/misha/          Saved Misha raw samples and telemetry
  sessions/2026-11-09/     Three archived recordings and checksum manifest
  sessions/2026-11-15_pace_based/  Seven pace-based recordings and manifest
  synthetic/2026-11-09/    Two generated trials with explicit outcome records
  recordings/             Named captures (local, ignored by Git)
docs/images/              Original example analysis plot
outputs/                  Generated analysis files (ignored by Git)
```

Each firmware folder contains a matching `.ino` file so Arduino IDE can open
and compile the sketches separately. Open only the sketch for the board's role.

## Setup

```bash
python3 -m pip install -r requirements.txt
```

For the original CYD display, install **TFT_eSPI by Bodmer** in Arduino IDE. Configure
the library's `User_Setup.h` with the ILI9341 driver and pin definitions shown
in `firmware/Old Firmware/perg_stat_display/perg_stat_display.ino`. Enable `LOAD_GLCD`,
`LOAD_FONT2`, `LOAD_FONT4`, and `LOAD_FONT7`. Definitions in the sketch alone
do not configure the separately compiled library. Library updates may reset
this setup.

## Record a session with the original hardware

Upload `firmware/Old Firmware/perg_full_logger/perg_full_logger.ino` to the sensor ESP32
and `firmware/Old Firmware/perg_stat_display/perg_stat_display.ino` to the CYD. The logger
is configured for 2 kHz raw sampling and 10 telemetry updates per second.
Its USB baud rate is 460800, the recorder's default:

```bash
python3 record_perg.py
```

With multiple boards connected, specify the sensor logger's port:

```bash
python3 record_perg.py /dev/cu.usbserial-10
```

For the original USB-only `firmware/Old Firmware/perg_logger/perg_logger.ino` sketch,
which samples at 1 kHz, select its matching baud rate and start immediately:

```bash
python3 record_perg.py /dev/cu.usbserial-210 --baud 921600 --start-immediately
```

The recorder asks for a recording name before connecting, for example
`calibration_01`. It saves raw samples to `data/recordings/calibration_01/perg_data.csv`
and mirrored telemetry to `data/recordings/calibration_01/perg_telemetry.csv`, relative
to the project directory regardless of the terminal's working directory.
Existing recording folders are refused so previous captures are not overwritten.
With the full logger, the recorder waits for LCD Start before creating the
files. Tap LCD Stop to finish, or press Ctrl+C to exit manually.

You can also supply the name directly:

```bash
python3 record_perg.py --name calibration_01
```

### Use the LCD Start/Stop button

Upload **both** updated sketches: `perg_full_logger` to the sensor ESP32 and
`perg_stat_display` to the CYD LCD. The display uses the XPT2046 resistive touch
controller on the ESP32-2432S028R CYD (CLK 25, MISO 39, MOSI 32, CS 33, IRQ 36).
Install **XPT2046_Touchscreen by Paul Stoffregen** alongside TFT_eSPI; the touch
bus is separate from the display bus. The pinout follows the
[CYD board reference](https://github.com/caseynielson/esp_28_lcd_exp), using the
[touch library's custom SPI support](https://github.com/PaulStoffregen/XPT2046_Touchscreen).

On the LCD's first boot, press and release each of the two calibration crosses.
The calibration is saved. Hold a finger on the screen while resetting the LCD
to repeat calibration. Keep TFT_eSPI's existing display configuration.

Start/Stop is in the top-right corner. It is disabled until the updated logger
reports its session state. Start resets elapsed time and distance, then begins
counting. Stop freezes elapsed time and distance; RPM, pace, voltage, and watts
remain live. The button waits for the logger's acknowledgment before changing
state. Movement or noise alone never starts the timer. Each Start begins a new
session, rather than resuming the previous one. Without a computer the button
still controls the LCD session.

The saved capture follows the LCD buttons by default:

```bash
python3 record_perg.py /dev/cu.usbserial-10
```

Enter a fresh recording name. Wait for `Ready: tap START on the LCD`, then tap
Start and paddle. Tap Stop to finish: the recorder closes both files and exits.
Run the command again with a new name before the next capture. No timed reset
button presses are needed. Starting the recorder in this mode arms a new session
and clears the timer/distance, so run it before paddling. If you cancel with
Ctrl+C, files close but the LCD session continues until you tap Stop.

Both CSV `time_us` columns are relative to the logger's receipt of the Start
command. Bluetooth adds a small delay after your tap. The previous
`--start-on-motion` flag is accepted as an alias, but now also waits for the
button; automatic motion triggering has been replaced.

Use `--start-immediately` to capture idle samples with boot-relative timestamps
or to record without a display. A Stop message from a connected display still
ends the capture.

Every received sample is saved; the terminal shows every 100th raw sample.
The USB-only sketch produces a telemetry file containing just the header.
The full logger records generated telemetry even when Bluetooth is disconnected;
these records do not confirm delivery to the display. Bluetooth connection
attempts and writes run in a separate task.

At 460800 baud, 8N1 serial carries at most 46,080 bytes/s. Raw CSV at 2 kHz
needs up to 34,000 bytes/s plus telemetry; 10 kHz CSV would need up to
170,000 bytes/s and requires a different format or faster reliable link.

## Replay a capture into the original Perg monitor

`firmware/perg_signal_player/perg_signal_player.ino` is separate firmware for a replay
ESP32, not the LCD. It generates a waveform on GPIO 25 (DAC1) for the monitor's
SPD input through the replay coupling circuit. Uploading it to the sensor
ESP32 replaces its logger firmware. Confirm the physical replay circuit and
which USB port belongs to the player before running output.

The [replay wiring notes](docs/replay_wiring.md) document the existing circuit
assumptions and unverified electrical details. The sensor recording circuit
is not the replay circuit. Optional LIN sniffer firmware is not included.

Validate a capture without connecting to hardware:

```bash
python3 replay_lin_probe.py run auto --csv "data/sessions/2026-11-09/68kg_drag59_test2/perg_data.csv" --once --dry-run
```

The Python tool resamples by timestamps to the player's 1 kHz DAC clock, so
2 kHz logger captures retain their duration. Gaps over 2 ms hold the previous
value. The ADC-to-DAC conversion uses an approximate bias/gain, not a measured
calibration of the complete replay circuit.

Once the replay hardware is connected and the player sketch uploaded, replace
`PLAYER_PORT` with its USB port and run:

```bash
python3 replay_lin_probe.py run PLAYER_PORT --csv "data/sessions/2026-11-09/68kg_drag59_test2/perg_data.csv" --once
```

No LIN sniffer is needed for playback alone. `--once` waits for the queued
signal to finish, then stops the DAC and closes USB. Ctrl+C stops early.
Without `--once`, playback loops until Ctrl+C, or for `--seconds N` if given.

### Synthetic calibration experiments

Generate a clean spin-up, hold, complete coast-down, and five seconds
at rest. The example below measures the raw waveform's 700-to-300 RPM interval
from the recording the user identified as drag 59. It does not use the logger's
calculated RPM, watts, or fixed drag constant.

```bash
python3 replay_lin_probe.py synthesize --reference "data/sessions/2026-11-09/68kg_drag59/perg_data.csv" --reference-drag 59 --target-drag 59 --peak-rpm 840 --hold-seconds 5 --out outputs/synthetic_drag59_trial03
python3 replay_lin_probe.py run auto --csv outputs/synthetic_drag59_trial03/perg_data.csv --once --dry-run
```

Output folders must be new. Each contains `perg_data.csv` (synthetic ADC-format
signal at 2 kHz), `profile.csv` (intended RPM and stages at 100 Hz), and
`synthetic.json` (parameters, reference, and limitations). The replay tool
converts the signal to the player's existing 1 kHz clock.

With the existing replay circuit connected and the original monitor in drag
calibration mode, run once on the actual player port:

```bash
python3 replay_lin_probe.py run PLAYER_PORT --csv outputs/synthetic_drag59_trial03/perg_data.csv --once
```

Check that the monitor itself displays more than 700 RPM. Increase the peak
or hold duration if its display remains below that threshold.

The model is `dRPM/dt = -(a*RPM^2 + b)`, with a fixed illustrative bearing term
`b = 1 RPM/s`. It fits `a` to the reference coast interval, then scales `a` by
requested/reference drag. Higher requested values therefore decelerate faster.
This is a test hypothesis, not the proprietary monitor's verified drag formula.
Amplitude is constant through the calibration range and fades below 100 RPM.
The eight-pulses-per-revolution assumption is unchanged.

Record the number only when the monitor stops flashing and accepts calibration.
A number that remains flashing is not an accepted measurement and must not be
used as a calibration reference. An accepted synthetic run can become the next
`--reference`, with its observed number as `--reference-drag`, to refine another
target experimentally. Synthetic trials do not establish the actual machine's
physical drag, and the generated target number is never guaranteed.

## Analyze a recording

Analyze the original example without hardware:

```bash
python3 analyze_perg.py data/examples/perg_data.csv
```

Analyze a named capture or a saved session:

```bash
python3 analyze_perg.py data/recordings/calibration_01/perg_data.csv
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
| `time_us` | ESP32 microseconds since LCD Start by default, or since boot with `--start-immediately` |
| `adc_value` | Raw 12-bit ADC reading (0–4095), not calibrated voltage or force |

Telemetry CSVs contain
`time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts,stroke_rate_spm`.
The recorder also accepts older telemetry, leaving stroke rate blank.
Pace is seconds per 500 m; period is the interval between sensor pulses.
The display also shows peak watts over the last 1.25 seconds.

RPM conversion assumes eight pulses per flywheel revolution:
`RPM = 60 / (8 × pulse_interval_seconds)`. Confirm this against the hardware.
The full logger now uses the analysis in `perg_rpm_stroke_rate_v2.xlsx`:

- Crossings: arm below ADC 1670, then count upward crossings of ADC 1820,
  interpolating their timestamps between samples. Eight accepted pulse
  intervals make one turn. Distance is turns × **0.2481 m/rev**, counted only
  while the session runs; Stop freezes the total.
- RPM: average per-pulse RPM within ±0.15 s. Catch detection looks for a
  minimum within ±0.3 s, with at least an 8 RPM range. These centered windows
  need about 0.45 s of lookahead. Startup waits for complete windows; equal
  minima count only once. Stroke rate is `60 / catch_interval_s`, averaged
  across up to four intervals as in the workbook's Console check tab.
- Watts: **0.0001578 × average_omega³**, where omega is per-pulse RPM × 2π/60
  and the average uses pulses in the trailing 1.5 s. Watts remain zero until
  the first full averaging window is available.
- Pace: **500 × Δtime / Δdistance**, measuring complete flywheel turns over
  approximately 1.5 s, in seconds per 500 m. The window extends to the first
  whole turn covering 1.5 s; startup uses available whole turns and shows
  no pace until one complete turn is available. Timing from the same magnet
  to itself cancels uneven magnet spacing. Both endpoints use sensor pulse
  timestamps so telemetry timing cannot introduce pace jitter.
  This is recent pace, not the session average. It stays live outside a
  session; the session distance still freezes on Stop. No pulses for 0.7 s
  clears RPM, watts, pace and stroke detection history. Stroke rate also
  expires after 5 s without a detected catch, even if the wheel keeps spinning.

The CYD shows `PACE /500m` as mm:ss and `SPM` in the former pulse-period
position. Pulse period remains in the telemetry. Upload both updated sketches.
The fitted constants come from one **drag-61** session and may need refitting
for other drag settings; ADC midline/hysteresis may also need adjustment for
another sensor setup. `PergMetrics.h` holds these settings. The older
`perg_bt_logger` sketch still uses its original model.

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

## Development checks

```bash
python3 -m unittest discover -s tests -v
python3 -m compileall -q record_perg.py replay_lin_probe.py perg_signal.py perg_synthetic.py
```

Compile changed firmware with Arduino IDE or Arduino CLI (`esp32:esp32:esp32`).
The display needs TFT_eSPI and XPT2046_Touchscreen. A successful build does not
validate the analog replay circuit or the monitor's drag algorithm. This cleanup
does not upload firmware or run signals on connected hardware.

VS Code associates `.ino` files with C++. Machine-specific include paths,
compilation databases, and Arduino build caches remain local and ignored by Git.
If the editor reports missing ESP32 headers, use your installed Arduino build
configuration; Arduino IDE compilation is independent of those editor warnings.
