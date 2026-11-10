"""Detect accidental edits to the archived experimental evidence."""

import hashlib
import json
import unittest
from pathlib import Path

from perg_signal import load_session

ROOT = Path(__file__).resolve().parents[1]


class ArchivedDataTests(unittest.TestCase):
    def test_recording_checksums_and_sample_counts(self):
        base = ROOT / "data/sessions/2026-11-09"
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
