"""Validated Hall-sensor CSV input and conversion to the replay DAC timeline."""

import csv

DEFAULT_BIAS = 1870
DEFAULT_GAIN = 0.0679
MAGNETS_PER_REV = 8
PLAYBACK_HZ = 1000


def load_session(path):
    """Return (times_us, adc_values) from a time_us,adc_value CSV."""
    times, values = [], []
    with open(path, newline="") as file:
        reader = csv.DictReader(file)
        if not {"time_us", "adc_value"}.issubset(reader.fieldnames or []):
            raise ValueError("CSV must contain time_us and adc_value columns")
        for row in reader:
            try:
                timestamp = int(row["time_us"])
                value = int(row["adc_value"])
            except (TypeError, ValueError) as error:
                raise ValueError(
                    f"Invalid sample at CSV line {reader.line_num}"
                ) from error
            if timestamp < 0 or not 0 <= value <= 4095:
                raise ValueError(f"Sample out of range at CSV line {reader.line_num}")
            if times and timestamp <= times[-1]:
                raise ValueError(
                    f"Timestamps must increase (CSV line {reader.line_num}); check for a board reset"
                )
            times.append(timestamp)
            values.append(value)
    if len(times) < 2:
        raise ValueError("Recording needs at least two samples")
    return times, values


def to_dac(values, bias, gain):
    """Map ADC counts to 0-255 DAC counts, centred on 128 (mid-scale).

    bias (default 1870) is the recording's idle level: the logger sampled
    through a ~1.65 V bias network (the ADC can't read negative), so
    subtracting it recovers the pickup's real voltage — idle = 0 V.
    gain preserves volts (ADC mV/count / DAC mV/count). The result is
    offset by 128 because the replay wiring AC-couples the DAC into the
    SPD jack (series cap + 100 k bleed): mid-scale DAC = 0 V on the
    jack, and the bipolar waveform swings both ways around it.
    """
    out = []
    for v in values:
        d = (v - bias) * gain + 128
        out.append(max(0, min(255, int(round(d)))))
    return out


def session_entries(times, dac_values):
    """Resample timestamped data to the player's fixed 1 ms DAC timeline.

    This preserves duration for both the original 1 kHz and full logger's
    2 kHz recordings. Interpolate normal sample jitter; hold the previous
    value across gaps over 2 ms rather than inventing a waveform there.
    """
    if len(times) < 2 or len(times) != len(dac_values):
        raise ValueError("Recording needs at least two matching timestamps and samples")
    if any(b <= a for a, b in zip(times, times[1:])):
        raise ValueError("Recording timestamps must increase; check for a board reset")
    index = 0
    for t in range(times[0], times[-1] + 1, 1_000_000 // PLAYBACK_HZ):
        while index + 1 < len(times) and times[index + 1] <= t:
            index += 1
        value = dac_values[index]
        if index + 1 < len(times):
            gap = times[index + 1] - times[index]
            if gap <= 2000:
                fraction = (t - times[index]) / gap
                value += fraction * (dac_values[index + 1] - value)
        yield max(0, min(255, round(value)))
