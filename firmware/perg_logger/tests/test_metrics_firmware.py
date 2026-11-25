"""Run the firmware calculations with a native compiler, without hardware."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import record_perg


class TelemetryTests(unittest.TestCase):
    def test_old_and_new_telemetry(self):
        old = b"telemetry,1000,750,161.2,10,0.2,10,1,76"
        self.assertEqual(record_perg.parse_telemetry(old)[-1], "")
        self.assertEqual(record_perg.parse_telemetry(old + b",48.5")[-1], "48.5")
        self.assertIsNone(record_perg.parse_telemetry(old + b",nan"))


@unittest.skipUnless(shutil.which("c++"), "Native C++ compiler is not installed")
class MetricsFirmwareTests(unittest.TestCase):
    def test_calibration_strokes_idle_and_rollover(self):
        header = (
            Path(__file__).resolve().parents[1]
            / "firmware/Old Firmware/perg_full_logger/PergMetrics.h"
        )
        harness = "#include " + json.dumps(str(header)) + "\n"
        harness += r"""
#include <cassert>
#include <cstdio>
int main() {
  // Constant 750 RPM: 100 pulses/s, 3.10125 m/s, ~76.45 W.
  for (uint32_t origin : {1000U, 0xffff0000U}) {
    PergMetrics m;
    uint32_t t = origin;
    for (int i = 0; i <= 300; ++i) { m.pulse(t); t += 10000; }
    assert(m.pulses == 300);
    assert(fabs(m.rpm - 750) < 0.01);
    assert(fabs(m.pace(t - 10000) - 500 / 3.10125) < 0.02);
    assert(fabs(m.watts(t - 10000) - 76.4504) < 0.02);
    assert(m.strokeRate == 0); // steady spin is not a stroke
    m.tick(t + PergMetrics::STOP_US);
    assert(m.rpm == 0 && m.watts(t) == 0 && m.pace(t) == 0);
    m.pulse(t + 1000000); // new origin, no invented distance across the gap
    m.pulse(t + 1010000);
    assert(m.pulses == 301);
  }
  // Hysteresis and interpolation: crossings at 1500 and 11500 us.
  PergMetrics detector;
  detector.sample(1000, 1600); detector.sample(2000, 2040);
  detector.sample(3000, 1810); detector.sample(4000, 1830);
  assert(detector.pulses == 0); // near-midline noise cannot rearm
  detector.sample(11000, 1600); detector.sample(12000, 2040);
  assert(detector.pulses == 1 && fabs(detector.periodMs - 10) < 0.001);
  for (int i = 0; i < 2000; ++i) detector.sample(13000 + i * 500, 1820 + i % 3);
  assert(detector.rpm == 0 && detector.pulses == 1);

  // A 1.2 s RPM cycle should produce 50 strokes/min after window warmup.
  PergMetrics rowing;
  double time = 1000000;
  for (int i = 0; i < 3000; ++i) {
    double rpm = 750 + 80 * cos(6.283185307 * time / 1200000);
    time += 7500000 / rpm;
    rowing.pulse((uint32_t)time);
  }
  assert(fabs(rowing.strokeRate - 50) < 0.5);

  // Uneven magnet spacing at a steady 750 RPM. Full turns must give a
  // stable pace at every magnet and every telemetry phase, across rollover.
  for (uint32_t origin : {1000U, 0xffff0000U}) {
    PergMetrics uneven;
    uint32_t t = origin;
    const uint32_t intervals[] = {6000,14000,7000,13000,8000,12000,9000,11000};
    uneven.pulse(t);
    for (int i = 0; i < 1000; ++i) {
      t += intervals[i % 8];
      uneven.pulse(t);
      if (i < 8) assert(uneven.pace(t) == 0); // require a complete turn
      else {
        assert(fabs(uneven.pace(t) - 500 / 3.10125) < 0.02);
        assert(uneven.pace(t) == uneven.pace(t + 3000));
      }
    }
    // Adapt to half speed once the old measurements leave the window.
    for (int i = 0; i < 100; ++i) { t += 20000; uneven.pulse(t); }
    assert(fabs(uneven.pace(t) - 500 / 1.550625) < 0.02);
    assert(uneven.pace(t + PergMetrics::STOP_US + 1) == 0);
  }
}
"""
        harness = "#include <initializer_list>\n" + harness
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "metrics.cpp"
            executable = Path(tmp) / "metrics"
            source.write_text(harness)
            subprocess.run(
                [
                    "c++",
                    "-std=c++11",
                    "-Wall",
                    "-Wextra",
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
