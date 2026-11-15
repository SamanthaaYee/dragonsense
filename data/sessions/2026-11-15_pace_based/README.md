# Pace-based paddling data collection

Archived November 15, 2026 (America/Toronto). The user identified this collection
as **pace-based**. It is a separate collection from the November 9 drag/replay
experiments.

All seven session folders contain byte-for-byte copies of their local
`data/live/` raw ADC and telemetry CSVs. Original session names are preserved,
including capitalization, `kg` labels and `60drag` labels. These names do not
record the prescribed pace. Individual target paces, pace interval boundaries,
and the meaning of each `kg` label were not supplied.

The [manifest](manifest.json) records the protocol, original names, SHA-256
hashes, row counts and timing checks. `pace_s` is the firmware's estimated
recent pace in seconds per 500 m; it must not be treated as a supplied target
pace. Raw `time_us,adc_value` data and calculated telemetry remain separate.

| Original session name | Raw ADC rows | Endpoint span (s) | Telemetry rows | Timing findings |
| --- | ---: | ---: | ---: | --- |
| Anna45kg60drag | 265,960 | 132.979195 | 1,332 | No backward timestamps or raw gaps over 1.5 ms. |
| Anna55kg60drag | 134,800 | 67.399508 | 677 | No backward timestamps or raw gaps over 1.5 ms. |
| Anna65kg60drag | 135,680 | 67.839498 | 682 | No backward timestamps or raw gaps over 1.5 ms. |
| Anna68kg60drag | 131,050 | 65.529508 | 658 | Two raw gaps over 1.5 ms; largest is 3 ms. |
| Irtaza50kg60drag | 136,699 | 68.349342 | 686 | One anomalous raw timestamp, followed by a backward jump. |
| Irtaza60kg60drag | 136,460 | 68.229506 | 685 | No backward timestamps or raw gaps over 1.5 ms. |
| irtaza68kg60drag | 293,200 | 146.599505 | 1,469 | No backward timestamps or raw gaps over 1.5 ms. |

Endpoint span is `(last_time_us - first_time_us) / 1e6`, not the sum of all
timestamp differences or a claim that every intervening timestamp is valid.
In `Irtaza50kg60drag`, raw data row 21,128 (one-based, excluding the header)
contains `4107585030` microseconds between `10563099` and `10564599`. This
outlier is preserved and identified in the manifest; monotonic-timestamp
analysis/replay will reject the uncorrected raw file. The telemetry timestamps
in all seven sessions are monotonic. No raw ADC values are outside 0–4095.

No timestamps, ADC values, distances, or pace estimates were corrected,
filtered, re-zeroed, or regenerated for this archive. The date above identifies
the archive, not an independently established capture date for every file.

See [the progress note](../../../docs/experiments/2026-11-15-pace-based.md)
for the recording workflow and serial-port troubleshooting.
