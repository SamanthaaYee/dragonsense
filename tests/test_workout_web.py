"""Check web/workout.js under Node, and that it agrees with the display's parser.

See docs/workout_push.md. Skipped when node or a C++ compiler is unavailable.
"""

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKOUT_JS = ROOT / "web/workout.js"
WORKOUT_HEADER = ROOT / "firmware/perg_freenove_force_display/Workout.h"

LONGEST = "W1" + ";D10000,1800" * 20
VECTORS = [
    "W1",
    "W1;T240,60;T240,60;D500,90",
    "W1;T10,0;T3600,1800;D50,0;D10000,0",
    LONGEST,
    LONGEST + "0",
    "W1" + ";T60,0" * 21,
    "W1;T60,0;T9,0" + ";T60,0" * 20,
    "",
    "W",
    "X1",
    "W2;T60,0",
    "w1;T60,0",
    "W1;",
    "W1;T60",
    "W1;T60,",
    "W1;X60,0",
    "W1;t60,0",
    "W1;T060,0",
    "W1;T60,00",
    "W1;T60,0;",
    "W1 ;T60,0",
    "W1;T60,0x",
    "W1;T-60,0",
    "W1;T123456,0",
    "W12",
    "W1;T60;0",
    "W1;T60,0\n",
    "W1;T9,0",
    "W1;T3601,0",
    "W1;D49,0",
    "W1;D10001,0",
    "W1;T60,1801",
    "W1;T0,0",
    "foobar",
]

NODE_UNIT_CHECKS = r"""
const assert = require('assert');
const W = require(process.argv[1]);

// Durations: seconds or m:ss; distances: whole metres.
assert.strictEqual(W.parseDuration('90'), 90);
assert.strictEqual(W.parseDuration(' 1:30 '), 90);
assert.strictEqual(W.parseDuration('0:05'), 5);
assert.strictEqual(W.parseDuration('60:00'), 3600);
assert.strictEqual(W.parseDuration('0'), 0);
for (const bad of ['', '1:5', '1:60', '1.5', '-5', 'abc', '1:30:00', '1234']) {
  assert.strictEqual(W.parseDuration(bad), null, bad);
}
assert.strictEqual(W.parseDistance('500'), 500);
for (const bad of ['', '5.5', '-1', '123456', '1e3', '500m']) {
  assert.strictEqual(W.parseDistance(bad), null, bad);
}
assert.strictEqual(W.formatDuration(90), '1:30');
assert.strictEqual(W.formatDuration(5), '0:05');
assert.strictEqual(W.formatDuration(3600), '60:00');

// Encoding builds the canonical line and validates every piece.
const pieces = [
  { kind: 'T', target: 240, rest: 60 },
  { kind: 'D', target: 500, rest: 90 },
];
const frozen = Object.freeze(pieces.map((p) => Object.freeze({ ...p })));
const encoded = W.encode(frozen);
assert.deepStrictEqual(encoded, { line: 'W1;T240,60;D500,90', error: null });
assert.deepStrictEqual(W.decode(encoded.line), { error: 'none', pieces });
assert.deepStrictEqual(W.encode([]), { line: 'W1', error: null });

const tooMany = Array.from({ length: 21 }, () => ({ kind: 'T', target: 60, rest: 0 }));
assert.match(W.encode(tooMany).error, /20 pieces/);
assert.match(W.encode([{ kind: 'T', target: 9, rest: 0 }]).error, /Piece 1.*0:10/);
assert.match(W.encode([pieces[0], { kind: 'D', target: 49, rest: 0 }]).error, /Piece 2.*50/);
assert.match(W.encode([{ kind: 'T', target: 60, rest: 1801 }]).error, /Piece 1.*rest/i);
assert.match(W.encode([{ kind: 'X', target: 60, rest: 0 }]).error, /Piece 1/);
assert.match(W.encode([{ kind: 'T', target: 60.5, rest: 0 }]).error, /Piece 1/);
assert.match(W.encode([{ kind: 'T', target: null, rest: 0 }]).error, /Piece 1/);
assert.match(W.encode('nope').error, /list/);

// Every valid encoding is accepted by the same rules the display uses.
const longest = Array.from({ length: 20 }, () => ({ kind: 'D', target: 10000, rest: 1800 }));
const big = W.encode(longest);
assert.strictEqual(big.line.length, W.LIMITS.maxLine);
assert.strictEqual(W.decode(big.line).error, 'none');

// FNV-1a 32 reference vectors, as 8 lowercase hex digits.
assert.strictEqual(W.checksum(''), '811c9dc5');
assert.strictEqual(W.checksum('a'), 'e40c292c');
assert.strictEqual(W.checksum('foobar'), 'bf9cf968');

// Display status values.
assert.deepStrictEqual(W.parseStatus('OK,3,0123abcd'),
  { state: 'ok', count: 3, hash: '0123abcd' });
assert.deepStrictEqual(W.parseStatus('ERR,busy'), { state: 'error', code: 'busy' });
assert.deepStrictEqual(W.parseStatus('RECV'), { state: 'pending' });
assert.deepStrictEqual(W.parseStatus('READY'), { state: 'pending' });
assert.deepStrictEqual(W.parseStatus('OK,x,1'), { state: 'unknown', raw: 'OK,x,1' });
assert.match(W.describeDisplayError('busy'), /Stop/);
assert.match(W.describeDisplayError('syntax'), /syntax/);

// Confirmation must match both the hash and the piece count of what was sent.
assert.strictEqual(W.confirms('W1;T60,0', `OK,1,${W.checksum('W1;T60,0')}`), true);
assert.strictEqual(W.confirms('W1;T60,0', `OK,2,${W.checksum('W1;T60,0')}`), false);
assert.strictEqual(W.confirms('W1;T60,0', `OK,1,${W.checksum('W1;T61,0')}`), false);
assert.strictEqual(W.confirms('W1', `OK,0,${W.checksum('W1')}`), true);
"""

NODE_VECTORS = r"""
const fs = require('fs');
const W = require(process.argv[1]);
const lines = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
for (const line of lines) {
  const result = W.decode(line);
  console.log(`${result.error} ${result.pieces.length} ${W.checksum(line)}`);
}
"""

CPP_VECTORS = r"""
#include <cstdio>
int main() {
  for (const char *line : LINES) {
    const workout::ParseResult r = workout::parse(line);
    std::printf("%s %u %08x\n", workout::errorCode(r.error), (unsigned)r.plan.count,
                (unsigned)workout::checksum(line));
  }
}
"""


def run_node(script: str, *args: str) -> str:
    result = subprocess.run(
        ["node", "-e", script, str(WORKOUT_JS), *args],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return result.stdout


class WorkoutBluetoothContractTests(unittest.TestCase):
    def test_workout_upload_uses_a_separate_characteristic(self):
        display = (
            WORKOUT_HEADER.parent / "perg_freenove_force_display.ino"
        ).read_text()
        logger = (
            ROOT
            / "firmware/final_sensor_and_display_devboard/final_sensor_and_display_devboard.ino"
        ).read_text()
        page = (ROOT / "web/index.html").read_text()

        def uuid(source, name):
            return re.search(rf"""\b{name}\s*=\s*['"]([0-9a-f-]+)['"]""", source)[1]

        self.assertEqual(uuid(display, "WORKOUT_UUID"), uuid(page, "WORKOUT_UUID"))
        self.assertEqual(
            uuid(display, "FORCE_TELEMETRY_UUID"), uuid(logger, "FORCE_TELEMETRY_UUID")
        )
        self.assertNotIn(
            uuid(display, "WORKOUT_UUID"),
            {
                uuid(logger, name)
                for name in ("FORCE_TELEMETRY_UUID", "TELEMETRY_UUID", "COMMAND_UUID")
            },
        )


@unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
class WorkoutWebTests(unittest.TestCase):
    def test_encoding_validation_and_status(self):
        run_node(NODE_UNIT_CHECKS)

    @unittest.skipUnless(shutil.which("c++"), "Native C++ compiler is not installed")
    def test_javascript_and_firmware_agree(self):
        with tempfile.TemporaryDirectory() as tmp:
            vectors = Path(tmp) / "vectors.json"
            vectors.write_text(json.dumps(VECTORS))
            javascript = run_node(NODE_VECTORS, str(vectors))

            literals = ",\n".join(json.dumps(line) for line in VECTORS)
            source = Path(tmp) / "vectors.cpp"
            source.write_text(
                "#include " + json.dumps(str(WORKOUT_HEADER)) + "\n"
                "static const char *LINES[] = {\n" + literals + "\n};\n" + CPP_VECTORS
            )
            executable = Path(tmp) / "vectors"
            build = subprocess.run(
                ["c++", "-std=c++17", str(source), "-o", str(executable)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            firmware = subprocess.run(
                [str(executable)], capture_output=True, text=True, check=True
            ).stdout

        self.assertEqual(len(javascript.splitlines()), len(VECTORS))
        for line, js_row, cpp_row in zip(
            VECTORS, javascript.splitlines(), firmware.splitlines()
        ):
            self.assertEqual(js_row, cpp_row, repr(line))


if __name__ == "__main__":
    unittest.main()
