"""Hardware-free checks for capture safety, replay timing, and trial generation."""

import contextlib
import csv
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

import perg_signal as signal
import perg_synthetic as synthetic
import record_perg as recorder
import replay_lin_probe as replay


class SignalTests(unittest.TestCase):
    def test_both_sample_rates_preserve_duration(self):
        self.assertEqual(
            list(signal.session_entries([0, 1000, 2000], [10, 20, 30])), [10, 20, 30]
        )
        self.assertEqual(
            list(
                signal.session_entries([0, 500, 1000, 1500, 2000], [10, 15, 20, 25, 30])
            ),
            [10, 20, 30],
        )
        self.assertEqual(
            list(signal.session_entries([0, 10000], [10, 30])), [10] * 10 + [30]
        )

    def test_invalid_csv_cannot_silently_change_replay_timing(self):
        samples = [
            "bad,header\n1,2\n",
            "time_us,adc_value\n0,1870\n",
            "time_us,adc_value\n0,1870\n500,bad\n",
            "time_us,adc_value\n0,1870\n500,4096\n",
            "time_us,adc_value\n1000,1870\n0,1870\n",
        ]
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "bad.csv"
            for contents in samples:
                with self.subTest(contents=contents):
                    p.write_text(contents)
                    with self.assertRaises(ValueError):
                        signal.load_session(p)

    def test_dry_run_never_opens_hardware(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "signal.csv"
            p.write_text("time_us,adc_value\n0,1870\n500,1900\n1000,1870\n")
            args = SimpleNamespace(
                csv=p, smooth=0, bias=1870, gain=0.0679, dry_run=True
            )
            with patch.object(
                replay, "PlayerLink", side_effect=AssertionError("Opened hardware")
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    replay.cmd_run(args)


class RecorderTests(unittest.TestCase):
    def test_existing_recording_is_never_replaced(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            patch.object(recorder, "OUTPUT_DIR", Path(tmp)),
        ):
            original = Path(tmp) / "existing"
            original.mkdir()
            (original / "perg_data.csv").write_text("original")
            with self.assertRaises(SystemExit):
                recorder.choose_recording_dir("existing")
            with patch("builtins.input", side_effect=["../bad", "existing", "new"]):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(recorder.choose_recording_dir(), Path(tmp) / "new")
            self.assertEqual((original / "perg_data.csv").read_text(), "original")

    def test_button_records_only_session_and_closes_both_files(self):
        class Stream:
            def __init__(self):
                self.lines = iter(
                    [
                        b"boot\n",
                        b"session_ready,1000\n",
                        b"1500,1870\n",
                        b"session_start,2000\n",
                        b"2500,1900\n",
                        b"telemetry,3000,100,120,0,0,75,0.001,50\n",
                        b"session_stop,3000\n",
                        b"3500,1950\n",
                    ]
                )
                self.closed = False
                self.commands = []

            def read_until(self, *args, **kwargs):
                return next(self.lines)

            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.commands.append(data)

            def flush(self):
                pass

            def close(self):
                self.closed = True

        stream = Stream()
        with tempfile.TemporaryDirectory() as tmp:
            with (
                patch.object(recorder, "OUTPUT_DIR", Path(tmp)),
                patch.object(recorder.serial, "Serial", return_value=stream),
                patch.object(recorder.time, "sleep"),
                patch.object(recorder, "wait_for_sample", return_value=[500, 1870]),
                patch(
                    "sys.argv",
                    ["record_perg.py", "fake", "--name", "test", "--start-on-button"],
                ),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                recorder.main()
            with (Path(tmp) / "test/perg_data.csv").open() as f:
                self.assertEqual(
                    list(csv.DictReader(f)), [{"time_us": "500", "adc_value": "1900"}]
                )
            with (Path(tmp) / "test/perg_telemetry.csv").open() as f:
                self.assertEqual(list(csv.DictReader(f))[0]["time_us"], "1000")
        self.assertEqual(stream.commands, [b"A"])
        self.assertTrue(stream.closed)
        self.assertEqual(next(stream.lines), b"3500,1950\n")

    def test_arming_retry_stops_after_readiness(self):
        class Stream:
            now = 0
            ready_writes = None

            def __init__(self):
                self.writes = []
                self.lines = iter(
                    [b"1000,1870\n"] * 4
                    + [b"session_ready,2000\n"]
                    + [b"3000,1870\n"] * 5
                    + [b"session_start,10000\n"]
                )

            def write(self, data):
                self.writes.append(data)

            def flush(self):
                pass

            def read_until(self, *args, **kwargs):
                self.now += 0.6
                line = next(self.lines)
                if line.startswith(b"session_ready"):
                    self.ready_writes = len(self.writes)
                return line

        stream = Stream()
        with (
            patch.object(recorder.time, "monotonic", side_effect=lambda: stream.now),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(recorder.wait_for_session_start(stream), 10000)
        self.assertGreater(len(stream.writes), 1)
        self.assertEqual(len(stream.writes), stream.ready_writes)


class ReplayTests(unittest.TestCase):
    def test_lin_checksum_and_pid_vectors(self):
        self.assertEqual(replay.lin_checksum([0x4A, 0x55, 0x93, 0xE5]), 0xE6)
        self.assertEqual(
            replay.lin_checksum([0x4A, 0x55, 0x93, 0xE5], pid=0x92, enhanced=True), 0x54
        )
        for pid in [0x92, 0x3C, 0x7D]:
            self.assertTrue(replay.pid_parity_ok(pid))
            self.assertFalse(replay.pid_parity_ok(pid ^ 0x40))
            self.assertFalse(replay.pid_parity_ok(pid ^ 0x80))
        frame = [0, 0x55, 0x92, 0x4A, 0x55, 0x93, 0xE5, 0xE6]
        decoded = replay.extract_frames(list(range(0, 8000, 1000)), frame)
        self.assertEqual(decoded[0]["data"], frame[3:7])
        self.assertTrue(decoded[0]["parity"])

    def test_auto_port_refuses_multiple_boards(self):
        ports = [SimpleNamespace(device=f"/dev/cu.usbserial-{x}") for x in [10, 210]]
        with (
            patch.object(replay.list_ports, "comports", return_value=ports),
            self.assertRaises(SystemExit),
        ):
            replay.find_port()

    def test_stop_waits_for_the_hardware_buffer(self):
        class Player:
            CMD_PLAY = 1

            def __init__(self, port):
                self.statuses = iter([500, 100, 0])
                self.last_level = None
                self.samples = 0
                self.stopped = False
                self.ser = SimpleNamespace(close=lambda: None)

            def flush(self):
                pass

            def start(self, loop):
                pass

            def send(self, command, payload):
                self.samples += len(payload) // 2

            def status(self):
                self.last_level = next(self.statuses)
                return f"K level={self.last_level} playing=1"

            def stop(self):
                if self.last_level != 0:
                    raise AssertionError("Stopped before buffer drained")
                self.stopped = True

        player = Player("fake")
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "signal.csv"
            p.write_text("time_us,adc_value\n0,1870\n500,1900\n1000,1870\n")
            args = SimpleNamespace(
                csv=p,
                smooth=0,
                bias=1870,
                gain=0.0679,
                dry_run=False,
                player="fake",
                sniffer=None,
                once=True,
                seconds=0,
            )
            with (
                patch.object(replay, "PlayerLink", return_value=player) as constructor,
                patch.object(replay.time, "sleep"),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                constructor.CMD_PLAY = 1
                replay.cmd_run(args)
        self.assertTrue(player.stopped)
        self.assertEqual(player.samples, 2)


class SyntheticTests(unittest.TestCase):
    def test_complete_monotone_coast_and_known_peak_frequency(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "trial"
            args = SimpleNamespace(
                reference="reference.csv",
                reference_drag=59,
                target_drag=59,
                peak_rpm=840,
                hold_seconds=5,
                amplitude_v=0.6,
                out=output,
            )
            with (
                patch.object(
                    synthetic, "reference_coast_seconds", return_value=12.3866666667
                ),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                synthetic.cmd_synthesize(args)
            metadata = json.loads((output / "synthetic.json").read_text())
            self.assertAlmostEqual(
                metadata["generated_700_to_300_seconds"], 12.3866666667
            )
            times, adc = signal.load_session(output / "perg_data.csv")
            self.assertEqual(set(b - a for a, b in zip(times, times[1:])), {500})
            self.assertEqual(adc[:4000], [1870] * 4000)
            self.assertEqual(adc[-10000:], [1870] * 10000)
            x = np.asarray(adc[16000:18000], dtype=float)
            x -= x.mean()
            frequency = np.fft.rfftfreq(len(x), 0.0005)[
                np.argmax(abs(np.fft.rfft(x * np.hanning(len(x)))))
            ]
            self.assertAlmostEqual(frequency, 840 * 8 / 60, delta=1)
            with (output / "profile.csv").open() as f:
                profile = list(csv.DictReader(f))
            coast = [float(x["rpm"]) for x in profile if x["stage"] == "coast"]
            self.assertTrue(all(a >= b for a, b in zip(coast, coast[1:])))
            self.assertEqual(len([x for x in profile if x["stage"] == "hold"]), 500)
            self.assertLess(
                synthetic.coast_interval(metadata["rpm_decay_a"] * 2, 1, 700, 300),
                metadata["generated_700_to_300_seconds"],
            )
            with (
                patch.object(
                    synthetic, "reference_coast_seconds", return_value=12.3866666667
                ),
                self.assertRaises(FileExistsError),
            ):
                synthetic.cmd_synthesize(args)


if __name__ == "__main__":
    unittest.main()
