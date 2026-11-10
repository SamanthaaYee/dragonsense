#!/usr/bin/env python3
"""
Perg replay + LIN probe.

Replays a recorded Hall-sensor CSV into the
Perg monitor's SPD jack through the ESP32 running perg_signal_player.ino,
An optional, separately supplied LIN sniffer can capture the monitor's RJ-11
traffic. Sniffer firmware is not included in this repository.

Subcommands:
    probe PLAYER_PORT SNIFFER_PORT
        Try a range of baud rates on both tapped LIN lines and report which
        one produces valid-looking frames.
    run PLAYER_PORT [SNIFFER_PORT]
        Replay a CSV; --once finishes one pass. --dry-run opens no ports.
    synthesize --reference CSV --reference-drag N --target-drag N --out DIR
        Generate a synthetic coast-down hypothesis for monitor testing.
    decode CAPTURE_FILE
        Re-analyse a saved capture (autobaud, LIN frames, per-ID stats).
    setdac PLAYER_PORT VALUE
        Hold the SPD output at a fixed 0-255 DAC level (manual level tests).
    sweep PLAYER_PORT
        Play a synthetic constant-frequency pulse train while stepping the
        amplitude up, to find the amplitude the monitor responds to.

LIN background: LIN (Local Interconnect Network) is a single-wire
master/slave bus, typically 2400-20000 baud, idling high (recessive).
A frame = break (~13 dominant bits) + sync byte 0x55 + protected ID + 1-8
data bytes + checksum. Whether the monitor is a master polling some other
unit, a slave being polled, or just wired for firmware updates, a passive
capture will show which.

Requires pyserial (imported as "serial") and CSV columns "time_us,adc_value".
"""

import argparse
import statistics
import sys
import time

import serial
from serial.tools import list_ports

from perg_signal import (
    DEFAULT_BIAS,
    DEFAULT_GAIN,
    PLAYBACK_HZ,
    load_session,
    session_entries,
    to_dac,
)
from perg_synthetic import cmd_synthesize

# Must match BAUD in perg_signal_player.ino / perg_lin_sniffer.ino.
PLAYER_BAUD = 115200
SNIFFER_BAUD = 115200

# Pacing: keep the player's ring level (entries sent - entries played) in
# [LOW, HIGH] so it neither starves nor overflows.
LEVEL_LOW = 4000
LEVEL_HIGH = 8000
PACKET_SAMPLES = 240

# --------------------------------------------------------------------------
# Serial links
# --------------------------------------------------------------------------


def find_port():
    """Never guess which board should generate a signal when several are connected."""
    ports = [
        p.device
        for p in list_ports.comports()
        if any(
            tag in p.device
            for tag in ("usbserial", "wchusbserial", "SLAB_USBtoUART", "ttyUSB")
        )
    ]
    if len(ports) == 1:
        return ports[0]
    if ports:
        sys.exit(
            "Multiple USB serial ports found: "
            + ", ".join(ports)
            + ". Specify the player port."
        )
    sys.exit("No ESP32 serial port found. Plug in the player or specify its port.")


class PlayerLink:
    """Binary packet link to perg_signal_player.ino."""

    MAGIC = b"\xa5\x5a"

    CMD_PLAY = 0x01
    CMD_START = 0x10
    CMD_STOP = 0x11
    CMD_FLUSH = 0x12
    CMD_SET_DAC = 0x14
    CMD_STATUS = 0x15

    def __init__(self, port):
        self.ser = serial.Serial(port, PLAYER_BAUD, timeout=2)
        time.sleep(2)  # opening the port may reset the ESP32; let it boot
        self.ser.reset_input_buffer()
        # _wait_ready catches the boot banner if the port open resets the
        # board. On boards without auto-reset (some CH340 boards) nothing
        # re-prints, so fall back to pinging it with a STATUS packet.
        ready = self._wait_ready()
        for _ in range(3):
            if ready:
                break
            ready = self.status()
        if not ready:
            self.ser.close()
            sys.exit(
                "Player did not answer (look for 'perg_signal_player "
                "ready'). Is it running perg_signal_player.ino? "
                "(Pressing EN/reset just after launching the command "
                "also re-prints the banner.)"
            )

    def _wait_ready(self, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self.ser.readline()
            if b"ready" in line or b"K " in line:
                return True
        return False

    def send(self, cmd, payload=b""):
        if len(payload) > 1024:
            raise ValueError(
                "Player packets cannot exceed the firmware's 1024-byte buffer"
            )
        frame = (
            self.MAGIC
            + bytes([cmd, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
            + payload
        )
        self.ser.write(frame)
        self.ser.flush()

    def status(self):
        """Ask for a status line and wait for it (used as a handshake)."""
        self.ser.reset_input_buffer()
        self.send(self.CMD_STATUS)
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            line = self.ser.readline().decode(errors="ignore").strip()
            if line.startswith("K error"):
                raise RuntimeError(line)
            if line.startswith("K level="):
                return line
        return None

    def start(self, loop_mode):
        payload = bytes([1 if loop_mode else 0])
        self.send(self.CMD_START, payload)

    def stop(self):
        self.send(self.CMD_STOP)

    def flush(self):
        self.send(self.CMD_FLUSH)

    def set_dac(self, value):
        self.send(self.CMD_SET_DAC, bytes([value & 0xFF]))


class SnifferLink:
    """ASCII line link to perg_lin_sniffer.ino."""

    def __init__(self, port):
        self.ser = serial.Serial(port, SNIFFER_BAUD, timeout=0.05)
        time.sleep(2)
        self.ser.reset_input_buffer()

    def command(self, line, expect_reply=True):
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        if not expect_reply:
            return None
        end = time.monotonic() + 1.5
        while time.monotonic() < end:
            raw = self.ser.readline()
            if raw.startswith(b"K "):
                return raw.decode(errors="ignore").strip()
        return None

    def drain(self):
        """Return the sniffer lines received since the last drain."""
        lines = []
        while True:
            raw = self.ser.readline()
            if not raw:
                break
            lines.append(raw.decode(errors="ignore").rstrip("\r\n"))
        return lines


# --------------------------------------------------------------------------
# Session loading and DAC mapping
# --------------------------------------------------------------------------


def median_filter(values, n):
    """Odd-width median filter; n<=1 returns values unchanged."""
    if n <= 1:
        return values
    half = n // 2
    out = []
    for i in range(len(values)):
        w = values[max(0, i - half) : i + half + 1]
        out.append(statistics.median(w))
    return out


def chunks(iterable, n):
    """Yield lists of up to n items."""
    buf = []
    for item in iterable:
        buf.append(item)
        if len(buf) >= n:
            yield buf
            buf = []
    if buf:
        yield buf


# --------------------------------------------------------------------------
# LIN decoding
# --------------------------------------------------------------------------

LIN_BAUD_CANDIDATES = [2400, 4800, 9600, 10400, 19200, 20000]


def lin_checksum(data, pid=None, enhanced=False):
    """LIN one's-complement checksum with end-around carry."""
    s = (pid if enhanced else 0) or 0
    for b in data:
        s += b
        if s > 255:
            s -= 255
    return (~s) & 0xFF


def pid_parity_ok(pid):
    """Check the two LIN parity bits over their specified identifier subsets."""
    bits = [(pid >> i) & 1 for i in range(6)]
    p0 = bits[0] ^ bits[1] ^ bits[2] ^ bits[4]
    p1 = 1 ^ bits[1] ^ bits[3] ^ bits[4] ^ bits[5]
    return ((pid >> 6) & 1) == p0 and ((pid >> 7) & 1) == p1


def extract_frames(times, values):
    """Scan a byte stream (t_us, value) for LIN frames.

    A frame starts with a break (seen as a 0x00 byte), then the sync byte
    0x55, then PID + data + checksum. Data length is resolved by finding the
    length whose classic or enhanced checksum validates. Returns frame dicts.
    """
    frames = []
    n = len(values)
    i = 0
    while i < n:
        if (
            values[i] == 0x55
            and i > 0
            and values[i - 1] == 0x00
            and times[i] - times[i - 1] < 20000
        ):
            pid = values[i + 1] if i + 1 < n else None
            if pid is not None:
                matched = None
                for length in range(1, 9):
                    end = i + 2 + length
                    if end >= n:
                        break
                    data = values[i + 2 : end]
                    csum = values[end]
                    gap_ok = times[end] - times[i] < 100000  # 100 ms cap
                    if not gap_ok:
                        break
                    if csum == lin_checksum(
                        data, enhanced=False
                    ) or csum == lin_checksum(data, pid=pid, enhanced=True):
                        matched = (length, data, csum)
                        break
                if matched:
                    length, data, csum = matched
                    frames.append(
                        {
                            "t": times[i - 1],
                            "pid": pid,
                            "data": data,
                            "csum": csum,
                            "parity": pid_parity_ok(pid),
                        }
                    )
                    i = i + 2 + length + 1
                    continue
        i += 1
    return frames


def autobaud_edges(events):
    """Estimate baud from dominant pulse widths (edge mode events).

    LIN's sync byte 0x55 produces 1-bit dominant pulses, so baud ~ 1/width.
    Returns a list of (baud, count) candidates, most frequent first.
    """
    widths = [w for _, _, w in events if 20 < w < 5000]
    if not widths:
        return []
    clusters = []
    for w in sorted(widths):
        for c in clusters:
            if abs(w - c[0]) < c[0] * 0.15:
                c[1].append(w)
                c[0] = statistics.median(c[1])
                break
        else:
            clusters.append([w, [w]])
    out = []
    for centre, group in clusters:
        bit_us = centre
        if bit_us > 0:
            baud = 1e6 / bit_us
            if 200 <= baud <= 250000:
                out.append((round(baud), len(group)))
    out.sort(key=lambda e: -e[1])
    return out


def summarise_frames(frames, label):
    """Print per-PID stats: count, interval, checksum mode, data ranges."""
    by_pid = {}
    for f in frames:
        by_pid.setdefault(f["pid"], []).append(f)
    print(f"\n--- LIN frames on {label}: {len(frames)} total ---")
    if not by_pid:
        print("    (no frames decoded)")
    for pid in sorted(by_pid):
        fs = by_pid[pid]
        parity = fs[0]["parity"]
        intervals = [
            b["t"] - a["t"] for a, b in zip(fs, fs[1:]) if 0 < b["t"] - a["t"] < 5000000
        ]
        med_iv = statistics.median(intervals) if intervals else None
        lengths = {len(f["data"]) for f in fs}
        rng = [
            (
                min(f["data"][i] for f in fs if len(f["data"]) > i),
                max(f["data"][i] for f in fs if len(f["data"]) > i),
            )
            for i in range(max(lengths))
        ]
        print(
            f"  PID 0x{pid:02X}{' ' if parity else '!'}"
            f" n={len(fs)} len={sorted(lengths)}"
            + (f" every~{med_iv / 1000:.1f}ms" if med_iv else "")
        )
        for i, (lo, hi) in enumerate(rng):
            if lo != hi:
                print(f"      byte[{i}]: {lo}..{hi}")
        # Two-byte little-endian interpretations, for value hunting.
        for off in range(0, max(lengths) - 1):
            vals = [
                f["data"][off] | (f["data"][off + 1] << 8)
                for f in fs
                if len(f["data"]) >= off + 2
            ]
            if vals and (max(vals) - min(vals)) > 0:
                print(f"      u16LE[{off}]: {min(vals)}..{max(vals)}")


def parse_capture_file(path):
    """Read a sniffer log back into per-line byte streams + edge events."""
    lines = {"A": [], "B": []}
    edges = []
    rails = []
    for raw in open(path):
        raw = raw.strip()
        if not raw or raw.startswith("#"):
            continue
        parts = raw.split()
        if parts[0] in ("A", "B") and len(parts) == 3:
            try:
                lines[parts[0]].append((int(parts[1]), int(parts[2], 16)))
            except ValueError:
                pass
        elif parts[0] == "E" and len(parts) == 4:
            try:
                edges.append((int(parts[1]), int(parts[2]), int(parts[3])))
            except ValueError:
                pass
        elif parts[0] == "RAIL":
            rails.append(raw)
    return lines, edges, rails


def analyse_lines(lines, edges, label=""):
    """Decode both lines and report; edges give the baud candidates."""
    if edges:
        print("\n--- Autobaud candidates (dominant pulse width -> 1 bit) ---")
        for baud, count in autobaud_edges(edges)[:5]:
            print(f"  ~{baud} baud  ({count} pulses)")
    for tag in ("A", "B"):
        stream = lines[tag]
        if not stream:
            print(f"\nLine {tag}: no bytes captured")
            continue
        times = [t for t, _ in stream]
        values = [v for _, v in stream]
        frames = extract_frames(times, values)
        valid = sum(1 for f in frames if f["parity"])
        rate = len(stream) / max(times[-1] - times[0], 1) * 1e6
        print(
            f"\nLine {tag}: {len(stream)} bytes ({rate:.1f}/s), "
            f"{len(frames)} frames ({valid} with valid PID parity)"
        )
        summarise_frames(frames, f"line {tag}" + (f" {label}" if label else ""))


# --------------------------------------------------------------------------
# Replay runner
# --------------------------------------------------------------------------


class Replayer:
    """Streams ring entries to the player while pacing the ring level."""

    def __init__(self, player, entries, loop_mode):
        self.player = player
        self.entries = entries
        self.loop_mode = loop_mode
        self.sent_entries = 0
        self.start_s = None

    def _pump(self, entry_chunks):
        """Send packets while keeping the ring level inside [LOW, HIGH]."""
        for chunk in entry_chunks:
            payload = b"".join(
                (e & 0xFF).to_bytes(1, "little")
                + ((e >> 8) & 0xFF).to_bytes(1, "little")
                for e in chunk
            )
            self.player.send(PlayerLink.CMD_PLAY, payload)
            self.sent_entries += len(chunk)
            level = self.sent_entries - (time.monotonic() - self.start_s) * PLAYBACK_HZ
            if level > LEVEL_HIGH:
                # Playback drains 1 entry/ms; sleep until level == LOW.
                time.sleep((level - LEVEL_LOW) / PLAYBACK_HZ)


# --------------------------------------------------------------------------
# Subcommands
# --------------------------------------------------------------------------


def cmd_probe(args):
    player = PlayerLink(args.player)
    sniffer = SnifferLink(args.sniffer)
    print("Probing both tapped LIN lines at several baud rates; leave the")
    print("monitor running normally (or press keys on it) while this runs.\n")
    player.set_dac(128)  # parked at idle: DAC mid-scale = 0 V on the jack
    results = []
    for baud in LIN_BAUD_CANDIDATES:
        reply = sniffer.command(f"b{baud}")
        if reply is None:
            print(f"  {baud}: sniffer did not answer b{baud}")
            continue
        sniffer.command("s")  # stream mode
        time.sleep(1.5)
        stream_lines = sniffer.drain()
        sniffer.command("x")
        per_line = {"A": [], "B": []}
        for line in stream_lines:
            parts = line.split()
            if len(parts) == 3 and parts[0] in per_line:
                try:
                    per_line[parts[0]].append((int(parts[1]), int(parts[2], 16)))
                except ValueError:
                    pass
        row = [f"  {baud:>6} baud:"]
        best = 0
        for tag in ("A", "B"):
            stream = per_line[tag]
            if not stream:
                row.append(f" {tag}: silent")
                continue
            times = [t for t, _ in stream]
            values = [v for _, v in stream]
            frames = extract_frames(times, values)
            good = sum(1 for f in frames if f["parity"])
            rate = len(stream) / max(times[-1] - times[0], 1) * 1e6
            row.append(f" {tag}: {rate:.0f} B/s, {len(frames)} frames, {good} valid")
            best = max(best, good)
        results.append((best, baud))
        print("".join(row))
    results.sort(reverse=True)
    if results and results[0][0] > 0:
        print(
            f"\nBest guess: {results[0][1]} baud "
            f"({results[0][0]} valid frames). "
            "Verify with 'run' or 'decode'."
        )
    else:
        print(
            "\nNo valid LIN frames at any baud. Try edge mode to find the "
            "baud: sniffer.command 'e', run for a few seconds, then 'decode'."
        )
    player.stop()


def cmd_run(args):
    print(f"Loading {args.csv}")
    times, values = load_session(args.csv)
    if args.smooth:
        print(f"  applying median filter n={args.smooth}; this can alter pulse timing")
        values = median_filter(values, args.smooth)
    dac = to_dac(values, args.bias, args.gain)
    entries = list(session_entries(times, dac))
    duration_s = (times[-1] - times[0]) / 1e6
    print(
        f"  {len(values)} recorded samples over {duration_s:.3f}s; "
        f"{len(entries)} playback ticks ({len(entries) / 1000:.3f}s at 1 kHz)"
    )
    print(f"  bias={args.bias}, gain={args.gain}")
    if args.dry_run:
        print("Dry run complete. No serial ports opened and no signal sent.")
        return

    print(f"Connecting to signal-player ESP32 on {args.player}...")
    player = PlayerLink(args.player)
    sniffer = SnifferLink(args.sniffer) if args.sniffer else None
    if sniffer:
        sniffer.command(f"b{args.baud}")

    if sniffer:
        print("Sniffer stream mode; capturing to " + args.out)
        sniffer.command("s")
        log = open(args.out, "w")
    else:
        print("No sniffer attached: replaying without LIN capture.")

    replayer = Replayer(player, entries, loop_mode=not args.once)
    limit = (
        f"for {args.seconds:.1f}s"
        if args.seconds
        else ("until Ctrl+C" if replayer.loop_mode else "one complete pass")
    )
    print(f"Replaying {limit}... Ctrl+C to stop\n")
    print("  queued signal duration | sniffer bytes seen")
    try:
        idle = [0]

        def poll():
            if not sniffer:
                return 0
            lines = sniffer.drain()
            log.write("\n".join(lines) + "\n")
            idle[0] = (
                sum(1 for line in lines if line.startswith(("A ", "B "))) + idle[0]
            )
            return idle[0]

        # Replay with sniffer draining interleaved: the pacing sleeps leave
        # plenty of gaps for poll().
        player.flush()
        replayer.start_s = time.monotonic()
        player.start(replayer.loop_mode)
        sent = 0
        entry_iter = iter(entries)
        done = False
        while not done:
            chunk = next(chunks(entry_iter, PACKET_SAMPLES), None)
            if chunk is None:
                if replayer.loop_mode:
                    entry_iter = iter(entries)
                    chunk = next(chunks(entry_iter, PACKET_SAMPLES), None)
                    if chunk is None:
                        break  # empty session file: nothing to play
                else:
                    done = True
                    break
            payload = b"".join(
                (e & 0xFF).to_bytes(1, "little")
                + ((e >> 8) & 0xFF).to_bytes(1, "little")
                for e in chunk
            )
            player.send(PlayerLink.CMD_PLAY, payload)
            sent += len(chunk)
            level = sent - (time.monotonic() - replayer.start_s) * PLAYBACK_HZ
            if level > LEVEL_HIGH:
                time.sleep((level - LEVEL_LOW) / PLAYBACK_HZ)
            poll()
            t_s = sent / PLAYBACK_HZ
            print(f"\r  {t_s:7.1f}s | bytes={idle[0]:>7} ", end="", flush=True)
            if args.seconds and t_s >= args.seconds:
                done = True
        # Sending the last packet does not mean it has played: several seconds
        # can remain queued. Wait for the hardware ring to empty before STOP.
        print("\nFinishing the queued signal...")
        drain_deadline = time.monotonic() + sent / PLAYBACK_HZ + 10
        while True:
            status = player.status()
            if status:
                fields = dict(
                    part.split("=", 1) for part in status.split()[1:] if "=" in part
                )
                if fields.get("level") == "0":
                    break
            if time.monotonic() >= drain_deadline:
                raise RuntimeError(
                    "Player did not confirm an empty buffer; stopping output"
                )
            poll()
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\nStopped by Ctrl+C")
    finally:
        try:
            player.stop()
        finally:
            player.ser.close()
            if sniffer:
                sniffer.command("x")
                log.close()
                sniffer.ser.close()

    if sniffer:
        print(f"\nCapture saved to {args.out}. Decoding...")
        lines, edges, rails = parse_capture_file(args.out)
        analyse_lines(lines, edges)
        if rails:
            print(f"\nRail samples: {rails[-3:]}")
    else:
        print("\nDone (no sniffer attached: no LIN capture was written).")


def cmd_decode(args):
    lines, edges, rails = parse_capture_file(args.capture)
    analyse_lines(lines, edges)
    if rails:
        print("\nRail readings (6V tap):")
        for r in rails:
            print(f"  {r}")


def cmd_setdac(args):
    player = PlayerLink(args.player)
    player.set_dac(args.value)
    print(
        f"DAC held at {args.value} "
        f"(jack ~{(args.value - 128) * 3.3 / 255:.2f} V). Ctrl+C to exit."
    )
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        player.set_dac(128)


def cmd_sweep(args):
    """Constant-frequency pulse train, amplitude stepping up every step."""
    player = PlayerLink(args.player)
    amp_max = args.amplitude
    print(
        f"Sweeping: {args.freq} Hz square pulses off the 0 V idle, "
        f"{amp_max} counts (~{amp_max * 3.3 / 255:.2f} V) max, "
        f"step every {args.step} s. Watch the monitor's RPM display."
    )
    player.flush()
    player.start(False)
    for frac in (0.25, 0.5, 0.75, 1.0):
        entries = []
        for i in range(round(PLAYBACK_HZ * args.step)):
            phase = (i * args.freq / PLAYBACK_HZ) % 1.0
            high = phase < 0.5
            amplitude = round(amp_max * frac)
            entries.append(128 + amplitude if high else 128 - amplitude)
        print(
            f"  amplitude {frac * 100:.0f}% "
            f"(~{frac * amp_max * 3.3 / 255:.2f} V pulses)"
        )
        replayer = Replayer(player, entries, loop_mode=False)
        replayer.sent_entries = 0
        replayer.start_s = time.monotonic()
        replayer._pump(chunks(entries, PACKET_SAMPLES))
        time.sleep(args.step)
    player.stop()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="try baud rates on both tapped LIN lines")
    p.add_argument("player", help="player ESP32 serial port (or auto)")
    p.add_argument("sniffer", help="sniffer ESP32 serial port (or auto)")

    p = sub.add_parser("decode", help="re-analyse a saved capture")
    p.add_argument("capture", help="log file written by 'run'")

    p = sub.add_parser(
        "synthesize",
        help="create synthetic spin-up/coast-down data for a drag experiment",
    )
    p.add_argument(
        "--reference", required=True, help="raw CSV from a known-drag calibration"
    )
    p.add_argument(
        "--reference-drag",
        type=float,
        required=True,
        help="drag observed on the real monitor for the reference",
    )
    p.add_argument(
        "--target-drag",
        type=float,
        required=True,
        help="hypothesized target; requires validation on the monitor",
    )
    p.add_argument(
        "--peak-rpm",
        type=float,
        default=760,
        help="peak above the 700 RPM calibration threshold (default 760)",
    )
    p.add_argument(
        "--hold-seconds",
        type=float,
        default=5,
        help="hold peak speed so the monitor can register it (default 5)",
    )
    p.add_argument(
        "--amplitude-v",
        type=float,
        default=0.6,
        help="nominal peak signal voltage (default 0.6)",
    )
    p.add_argument(
        "--out", required=True, help="new output folder; existing folders are refused"
    )

    p = sub.add_parser("run", help="replay the session, optionally sniffing LIN")
    p.add_argument("player", help="player ESP32 serial port (or auto)")
    p.add_argument(
        "sniffer",
        nargs="?",
        default=None,
        help="sniffer ESP32 serial port (or auto); omit to replay without the sniffer",
    )
    p.add_argument("--csv", default="data/live/perg_data.csv")
    p.add_argument(
        "--baud",
        type=int,
        default=9600,
        help="LIN baud for the sniffer UARTs (default 9600)",
    )
    p.add_argument(
        "--seconds",
        type=float,
        default=0,
        help="queue N seconds of signal (0 = full CSV with --once, otherwise loop until Ctrl+C)",
    )
    p.add_argument("--out", default="lin_capture.log")
    p.add_argument(
        "--bias",
        type=int,
        default=DEFAULT_BIAS,
        help="ADC counts used as the signal's zero (default 1870)",
    )
    p.add_argument(
        "--gain",
        type=float,
        default=DEFAULT_GAIN,
        help="ADC->DAC gain (default 0.068 preserves the "
        "recorded volt-per-count; raise if the monitor "
        "shows nothing)",
    )
    p.add_argument(
        "--smooth",
        type=int,
        default=0,
        help="optional median filter width (odd, e.g. 3 or 5)",
    )
    p.add_argument(
        "--once", action="store_true", help="play the session once instead of looping"
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="validate the CSV and playback timing without opening any serial port",
    )

    p = sub.add_parser("setdac", help="hold the SPD output at a fixed level")
    p.add_argument("player", help="player ESP32 serial port (or auto)")
    p.add_argument("value", type=int, help="DAC value 0-255")

    p = sub.add_parser("sweep", help="constant-frequency pulses, amplitude stepping up")
    p.add_argument("player", help="player ESP32 serial port (or auto)")
    p.add_argument(
        "--freq",
        type=float,
        default=91.0,
        help="pulse frequency in Hz (default 91, the session's steady rate)",
    )
    p.add_argument(
        "--amplitude",
        type=int,
        default=120,
        help="max pulse height above the 0 V idle, DAC counts (default 120)",
    )
    p.add_argument(
        "--step", type=float, default=2.0, help="seconds per amplitude step (default 2)"
    )

    args = ap.parse_args()
    if args.cmd in ("probe", "run", "setdac", "sweep"):
        if args.player in (None, "auto") and not getattr(args, "dry_run", False):
            args.player = find_port()
    if args.cmd == "run" and args.sniffer == "auto" and not args.dry_run:
        args.sniffer = find_port()
    if args.cmd == "probe":
        cmd_probe(args)
    elif args.cmd == "decode":
        cmd_decode(args)
    elif args.cmd == "synthesize":
        cmd_synthesize(args)
    elif args.cmd == "run":
        cmd_run(args)
    elif args.cmd == "setdac":
        cmd_setdac(args)
    elif args.cmd == "sweep":
        cmd_sweep(args)


if __name__ == "__main__":
    main()
