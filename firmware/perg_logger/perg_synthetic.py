"""Synthetic calibration experiments; requested drag values are unverified."""

import csv
import json
import math
from pathlib import Path

from perg_signal import DEFAULT_BIAS, MAGNETS_PER_REV, load_session


def reference_coast_seconds(path):
    """Estimate the 700-to-300 RPM coast from raw signal frequency, not telemetry."""
    import numpy as np

    times, values = load_session(path)
    if len(times) < 2 or any(b <= a for a, b in zip(times, times[1:])):
        raise ValueError("Reference timestamps must increase")
    times = np.asarray(times) / 1e6
    values = np.asarray(values)
    estimates = []
    for start in np.arange(times[0], times[-1] - 2, 0.25):
        grid = start + np.arange(4000) / 2000
        wave = np.interp(grid, times, values)
        if np.percentile(wave, 95) - np.percentile(wave, 5) < 200:
            continue
        wave -= wave.mean()
        power = abs(np.fft.rfft(wave * np.hanning(len(wave)))) ** 2
        freq = np.fft.rfftfreq(len(wave), 1 / 2000)
        power[(freq < 2) | (freq > 200)] = 0
        peak = int(power.argmax())
        if power[peak] < 0.1 * power.sum():
            continue
        estimates.append((start + 1, freq[peak] * 60 / MAGNETS_PER_REV))
    if not estimates:
        raise ValueError("No clear pulses found in the reference")
    peak = max(range(len(estimates)), key=lambda i: estimates[i][1])
    crossings = {}
    for (t0, r0), (t1, r1) in zip(estimates[peak:], estimates[peak + 1 :]):
        for threshold in (700, 300):
            if threshold not in crossings and r0 >= threshold >= r1 and r1 < r0:
                crossings[threshold] = t0 + (r0 - threshold) / (r0 - r1) * (t1 - t0)
    if len(crossings) != 2 or crossings[300] <= crossings[700]:
        raise ValueError(
            "Reference must contain a clear coast-down from above 700 to below 300 RPM"
        )
    return crossings[300] - crossings[700]


def coast_interval(a, b, high, low):
    """Time for dRPM/dt = -(a*RPM^2 + b) to fall from high to low."""
    scale = math.sqrt(a / b)
    return (math.atan(high * scale) - math.atan(low * scale)) / math.sqrt(a * b)


def cmd_synthesize(args):
    """Generate a test waveform; the requested drag is a hypothesis, not a measured result."""
    if not all(
        math.isfinite(x) and x > 0
        for x in (
            args.reference_drag,
            args.target_drag,
            args.peak_rpm,
            args.amplitude_v,
            args.hold_seconds,
        )
    ):
        raise ValueError(
            "Drag, peak RPM, amplitude, and hold duration must be finite and positive"
        )
    if args.peak_rpm <= 700 or args.peak_rpm > 1200:
        raise ValueError("Choose a peak above 700 and no higher than 1200 RPM")
    if args.amplitude_v > 1.2:
        raise ValueError(
            "Amplitude must be at most 1.2 V peak with the default ADC/DAC mapping"
        )
    measured = reference_coast_seconds(args.reference)
    # A small constant bearing-friction term provides a finite, smooth stop.
    # Fit only the known 700->300 duration; this is not the monitor's algorithm.
    b = 1.0
    if measured >= 400 / b:
        raise ValueError("Reference coast is too long for this test model")
    low, high = 1e-12, 1.0
    for _ in range(80):
        mid = (low + high) / 2
        if coast_interval(mid, b, 700, 300) > measured:
            low = mid
        else:
            high = mid
    reference_a = (low + high) / 2
    a = reference_a * args.target_drag / args.reference_drag
    peak = args.peak_rpm
    coast_s = coast_interval(a, b, peak, 0)
    idle_s, spinup_s, hold_s, tail_s = 2.0, 6.0, args.hold_seconds, 5.0
    coast_start = idle_s + spinup_s + hold_s
    duration = coast_start + coast_s + tail_s
    output = Path(args.out)
    output.mkdir(parents=True, exist_ok=False)
    q, rate = math.sqrt(b / a), math.sqrt(a * b)
    angle = math.atan(peak / q)
    spinup_integral = peak * spinup_s / 2
    preceding_integral = spinup_integral + peak * hold_s
    with (
        (output / "perg_data.csv").open("x", newline="") as raw,
        (output / "profile.csv").open("x", newline="") as profile,
    ):
        writer, profile_writer = csv.writer(raw), csv.writer(profile)
        writer.writerow(["time_us", "adc_value"])
        profile_writer.writerow(["time_us", "rpm", "stage"])
        for time_us in range(0, math.ceil(duration * 1e6), 500):
            t = time_us / 1e6
            if t < idle_s:
                rpm, integral, stage = 0, 0, "idle"
            elif t < idle_s + spinup_s:
                u = t - idle_s
                rpm = peak * (1 - math.cos(math.pi * u / spinup_s)) / 2
                integral = (
                    peak
                    * (u - spinup_s / math.pi * math.sin(math.pi * u / spinup_s))
                    / 2
                )
                stage = "spinup"
            elif t < coast_start:
                rpm = peak
                integral = spinup_integral + peak * (t - idle_s - spinup_s)
                stage = "hold"
            elif t < coast_start + coast_s:
                theta = max(0, angle - rate * (t - coast_start))
                rpm = q * math.tan(theta)
                integral = (
                    preceding_integral + math.log(math.cos(theta) / math.cos(angle)) / a
                )
                stage = "coast"
            else:
                rpm, integral, stage = 0, 0, "stopped"
            # Constant amplitude through the calibration range, fading only
            # below 100 RPM. Phase is integrated analytically, not rounded RPM.
            amplitude = args.amplitude_v * min(1, rpm / 100)
            phase = 2 * math.pi * MAGNETS_PER_REV / 60 * integral
            adc = round(DEFAULT_BIAS + amplitude * math.sin(phase) * 4096 / 3.6)
            writer.writerow([time_us, adc])
            if time_us % 10000 == 0:
                profile_writer.writerow([time_us, f"{rpm:.4f}", stage])
    metadata = {
        "synthetic": True,
        "reference_csv": str(Path(args.reference).resolve()),
        "reference_drag_user_supplied": args.reference_drag,
        "requested_drag_unverified": args.target_drag,
        "reference_700_to_300_seconds": measured,
        "generated_700_to_300_seconds": coast_interval(a, b, 700, 300),
        "peak_rpm": peak,
        "duration_seconds": duration,
        "initial_idle_seconds": idle_s,
        "spinup_seconds": spinup_s,
        "peak_hold_seconds": hold_s,
        "final_idle_seconds": tail_s,
        "sample_rate_hz": 2000,
        "pulses_per_revolution_assumed": MAGNETS_PER_REV,
        "amplitude_peak_volts_nominal": args.amplitude_v,
        "rpm_decay_a": a,
        "rpm_decay_b": b,
        "model": "dRPM/dt = -(a*RPM^2+b); a scales with requested/reference drag",
        "limitation": "The monitor's drag mapping is unknown. Target drag is not guaranteed; record its accepted result.",
    }
    (output / "synthetic.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Synthetic data saved to {output / 'perg_data.csv'}")
    print(
        f"Reference 700->300: {measured:.3f}s; generated: {metadata['generated_700_to_300_seconds']:.3f}s"
    )
    print(f"Peak {peak:g} RPM; complete stop plus 5s idle; duration {duration:.2f}s")
    print(
        f"Requested drag {args.target_drag:g} is an UNVERIFIED hypothesis, not a calibrated result."
    )
