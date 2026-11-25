# Pace-based collection and recorder updates — November 15, 2026

## Collection label

The user identified the latest Anna and Irtaza paddling data collection as
**pace-based**. Archive and commit labels use that description. It is distinct
from the earlier drag-factor replay/calibration experiments.

Seven raw/telemetry pairs are archived under
[data/sessions/2026-11-15_pace_based](../../data/sessions/2026-11-15_pace_based/README.md).
Their original labels are `Anna45kg60drag`, `Anna55kg60drag`, `Anna65kg60drag`,
`Anna68kg60drag`, `Irtaza50kg60drag`, `Irtaza60kg60drag` and `irtaza68kg60drag`.
Each archived file matches its local source byte-for-byte, with SHA-256 hashes
and timing findings in the manifest.

Prescribed pace values and interval boundaries were not provided. They are not
inferred from the session names or from measured `pace_s`. The CSVs also do not
identify the firmware commit used for each capture. Retaining these limits
avoids confusing an estimated result with an experimental target.

## Recording workflow

The sensor logger is the ESP32 connected to the Perg Hall sensor on GPIO 27
and GND. The other ESP32 is the statistics display. Unplugging only the sensor
logger confirmed that its current USB port was `/dev/cu.usbserial-10`; the
remaining USB device was `/dev/cu.usbserial-210`. Port names can change after
reconnecting, so identify the board before choosing the recording port.

The full logger variants use 460800 baud. With `--start-on-button`, the
recorder arms the logger, waits for the screen's Start command, records raw
ADC plus generated telemetry, and saves when Stop is received. Example:

```bash
python3 record_perg.py /dev/cu.usbserial-10 --start-on-button --name NEW_SESSION_NAME
```

The logger must report readiness before starting the trial. Existing recording
names are refused so previous captures are not overwritten.

## Serial configuration failure

An earlier attempt named `Anna45kg` failed at serial-port configuration with
`termios.error: (22, 'Invalid argument')`, before the recording directory was
created. Local checks reproduced the rejection on `/dev/cu.usbserial-10` even
at lower baud rates; no other process had that port open at the time. Reopening
the correct sensor port after USB reconnection was the next troubleshooting
step. This observation does not establish a baud-rate-only cause.

`record_perg.py` now handles serial-open/configuration errors with a concise
message explaining how to close competing tools, reconnect USB, list ports,
and retry at the baud rate matching the uploaded sketch. It does not silently
change baud rates or create empty recording files after a connection failure.
This change improves diagnostics; it cannot repair a USB driver or cable.

## Timing findings and validation

The archive preserves an isolated timestamp outlier in `Irtaza50kg60drag`
and two sampling gaps in `Anna68kg60drag`. The archive README gives their
locations and measured sizes. No corrections were applied. All seven telemetry
files have monotonic timestamps, and every raw ADC value is in range.

Checks cover archive hashes/row counts/timing findings and serial-configuration
failure cleanup, alongside the existing recorder, firmware calculation, BLE
framing, and replay tests. These checks establish file integrity and software
behavior; they do not establish that a prescribed pace was maintained.
