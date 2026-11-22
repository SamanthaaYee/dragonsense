"""Detect accidental edits to the archived experimental evidence."""

import csv
import hashlib
import json
import unittest
from pathlib import Path

from perg_signal import load_session

ROOT = Path(__file__).resolve().parents[1]


class ArchivedDataTests(unittest.TestCase):
    def test_pace_collection_integrity_and_recorded_timing_findings(self):
        base = ROOT / "data/WattageTests/2026-11-15_pace_based"
        manifest = json.loads((base / "manifest.json").read_text())
        for session in manifest["recordings"]:
            for filename, expected in session["files"].items():
                path = base / session["folder"] / filename
                with self.subTest(path=str(path)):
                    self.assertEqual(
                        hashlib.sha256(path.read_bytes()).hexdigest(),
                        expected["sha256"],
                    )
                    with path.open(newline="") as file:
                        rows = csv.DictReader(file)
                        self.assertEqual(rows.fieldnames, expected["columns"])
                        times = []
                        invalid_adc = 0
                        for row in rows:
                            times.append(int(row["time_us"]))
                            if "adc_value" in row:
                                invalid_adc += not 0 <= int(row["adc_value"]) <= 4095
                    self.assertEqual(len(times), expected["rows"])
                    self.assertEqual(times[0], expected["first_time_us"])
                    self.assertEqual(times[-1], expected["last_time_us"])
                    gaps = [b - a for a, b in zip(times, times[1:])]
                    # Preserve the reported outlier rather than rewriting the
                    # evidence to satisfy the strict replay loader.
                    self.assertEqual(
                        sum(gap < 0 for gap in gaps), expected["backward_timestamps"]
                    )
                    self.assertEqual(max(gaps), expected["max_forward_gap_us"])
                    if filename == "perg_data.csv":
                        self.assertEqual(invalid_adc, expected["adc_out_of_range"])
                        self.assertEqual(
                            sum(gap > 1500 for gap in gaps),
                            expected["gaps_over_1500_us"],
                        )

    def test_recording_checksums_and_sample_counts(self):
        base = ROOT / "data/WattageTests/2026-11-09"
        manifest = json.loads((base / "manifest.json").read_text())
        for session in manifest["recordings"]:
            for filename, expected in session["files"].items():
                p = base / session["folder"] / filename
                with self.subTest(path=str(p)):
                    self.assertEqual(
                        hashlib.sha256(p.read_bytes()).hexdigest(), expected["sha256"]
                    )
            times, values = load_session(base / session["folder"] / "perg_data.csv")
            self.assertEqual(len(values), session["files"]["perg_data.csv"]["rows"])
            self.assertEqual(
                times[-1] - times[0],
                round(session["files"]["perg_data.csv"]["duration_seconds"] * 1e6),
            )

    def test_synthetic_artifact_checksums(self):
        for trial in (ROOT / "data/synthetic/2026-11-09").glob("drag59_trial*"):
            checksums = json.loads((trial / "checksums.json").read_text())
            for filename, expected in checksums.items():
                with self.subTest(trial=trial.name, filename=filename):
                    self.assertEqual(
                        hashlib.sha256((trial / filename).read_bytes()).hexdigest(),
                        expected,
                    )


if __name__ == "__main__":
    unittest.main()
