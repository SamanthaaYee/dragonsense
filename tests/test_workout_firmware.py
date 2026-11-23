"""Compile the display's workout parser and runner natively, without hardware.

See docs/workout_push.md for the format and runner semantics under test.
"""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

WORKOUT_HEADER = (
    Path(__file__).resolve().parents[1]
    / "firmware/perg_freenove_force_display/Workout.h"
)

HARNESS = r"""
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
using namespace workout;

static Sample on(float t, float d) { return Sample{true, t, d}; }
static Sample off(float t, float d) { return Sample{false, t, d}; }
static std::string text(const Plan &p, const RunState &s, const Sample &x) {
  return describe(p, s, x).text;
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
static std::string repeat(const char *piece, int n) {
  std::string line = "W1";
  for (int i = 0; i < n; ++i) line += piece;
  return line;
}

static void parsing() {
  ParseResult r = parse("W1;T240,60;T240,60;D500,90");
  assert(r.error == Error::None && r.plan.count == 3);
  assert(r.plan.pieces[0].kind == Kind::Time && r.plan.pieces[0].target == 240);
  assert(r.plan.pieces[0].restS == 60);
  assert(r.plan.pieces[2].kind == Kind::Distance && r.plan.pieces[2].target == 500);
  assert(r.plan.pieces[2].restS == 90);

  r = parse("W1");  // clear
  assert(r.error == Error::None && r.plan.count == 0);
  assert(parse("W1;T10,0;T3600,1800;D50,0;D10000,0").error == Error::None);

  struct Bad { const char *line; Error error; };
  const Bad bad[] = {
    {"", Error::Version}, {"W", Error::Version}, {"X1", Error::Version},
    {"W2;T60,0", Error::Version}, {"w1;T60,0", Error::Version},
    {"W1;", Error::Syntax}, {"W1;T60", Error::Syntax}, {"W1;T60,", Error::Syntax},
    {"W1;X60,0", Error::Syntax}, {"W1;t60,0", Error::Syntax},
    {"W1;T060,0", Error::Syntax}, {"W1;T60,00", Error::Syntax},
    {"W1;T60,0;", Error::Syntax}, {"W1 ;T60,0", Error::Syntax},
    {"W1;T60,0x", Error::Syntax}, {"W1;T-60,0", Error::Syntax},
    {"W1;T123456,0", Error::Syntax}, {"W12", Error::Syntax},
    {"W1;T60;0", Error::Syntax}, {"W1;T60,0\n", Error::Syntax},
    {"W1;T9,0", Error::Range}, {"W1;T3601,0", Error::Range},
    {"W1;D49,0", Error::Range}, {"W1;D10001,0", Error::Range},
    {"W1;T60,1801", Error::Range}, {"W1;T0,0", Error::Range},
  };
  for (const Bad &b : bad) assert(parse(b.line).error == b.error);

  // 21 pieces is too many even though the line is short.
  assert(parse(repeat(";T60,0", 21).c_str()).error == Error::Count);
  // The longest valid line is exactly MAX_LINE characters.
  const std::string longest = repeat(";D10000,1800", 20);
  assert(longest.size() == MAX_LINE);
  assert(parse(longest.c_str()).error == Error::None);
  assert(parse((longest + "0").c_str()).error == Error::Length);
  // Earlier errors win, in reading order: piece 2's range before the count.
  assert(parse(("W1;T60,0;T9,0" + repeat(";T60,0", 20).substr(2)).c_str()).error
         == Error::Range);

  assert(std::string(errorCode(Error::Busy)) == "busy");
  assert(std::string(errorCode(Error::Length)) == "length");
  assert(std::string(errorCode(Error::Count)) == "count");
}

static void formatting() {
  char buf[MAX_LINE + 1];
  const std::string longest = repeat(";D10000,1800", 20);
  assert(format(parse(longest.c_str()).plan, buf, sizeof buf) == longest.size());
  assert(longest == buf);
  assert(format(parse("W1").plan, buf, sizeof buf) == 2 && std::string(buf) == "W1");
  assert(format(parse("W1;T60,0").plan, buf, 4) == 0);  // does not fit
  assert(format(parse("W1;T60,0").plan, buf, 9) == 8 && std::string(buf) == "W1;T60,0");

  // FNV-1a 32 reference vectors.
  assert(checksum("") == 0x811c9dc5u);
  assert(checksum("a") == 0xe40c292cu);
  assert(checksum("foobar") == 0xbf9cf968u);
}

static void fullWorkout() {
  const Plan plan = parse("W1;T60,30;D100,0;D100,20;T10,5").plan;
  RunState s;
  assert(s.phase == Phase::Idle && !inProgress(s.phase));
  s = advance(plan, s, off(0, 0));
  assert(s.phase == Phase::Idle);
  assert(text(plan, s, off(0, 0)) == "READY 4 PCS");
  assert(describe(plan, s, off(0, 0)).fraction == 0.0f);

  s = advance(plan, s, on(0, 0));
  assert(s.phase == Phase::Work && s.index == 0 && inProgress(s.phase));
  assert(text(plan, s, on(0, 0)) == "P1/4 1:00");
  assert(near(describe(plan, s, on(30, 0)).fraction, 0.5f));

  s = advance(plan, s, on(59.9f, 50));
  assert(s.phase == Phase::Work);
  assert(text(plan, s, on(59.9f, 50)) == "P1/4 0:01");  // rounds up, never 0:00

  // Time pieces end exactly at start + target, not at the telemetry time.
  s = advance(plan, s, on(60.1f, 51));
  assert(s.phase == Phase::Rest && s.index == 0 && near(s.startS, 60));
  assert(text(plan, s, on(60.1f, 51)) == "REST 0:30");
  assert(near(describe(plan, s, on(75, 55)).fraction, 0.5f));

  s = advance(plan, s, on(90, 60));
  assert(s.phase == Phase::Work && s.index == 1 && near(s.startS, 90));
  assert(near(s.startM, 60));  // rest distance never counts
  assert(text(plan, s, on(90, 60)) == "P2/4 100m");
  assert(text(plan, s, on(100, 159.5f)) == "P2/4 1m");

  // Distance piece with no rest carries the overshoot into the next piece.
  s = advance(plan, s, on(101, 163));
  assert(s.phase == Phase::Work && s.index == 2 && near(s.startM, 160));
  assert(near(s.startS, 101));
  assert(text(plan, s, on(101, 163)) == "P3/4 97m");

  s = advance(plan, s, on(130, 260));
  assert(s.phase == Phase::Rest && s.index == 2 && near(s.startS, 130));
  assert(text(plan, s, on(130, 260)) == "REST 0:20");

  s = advance(plan, s, on(150, 280));
  assert(s.phase == Phase::Work && s.index == 3 && near(s.startM, 280));
  assert(text(plan, s, on(150, 280)) == "P4/4 0:10");

  // Last piece: the rest after it is ignored.
  s = advance(plan, s, on(160, 290));
  assert(s.phase == Phase::Finishing && inProgress(s.phase));
  assert(text(plan, s, on(160, 290)) == "DONE");
  assert(describe(plan, s, on(160, 290)).fraction == 1.0f);
  s = advance(plan, s, on(170, 300));
  assert(s.phase == Phase::Finishing);

  s = advance(plan, s, off(170, 300));
  assert(s.phase == Phase::Finished && !inProgress(s.phase));
  assert(text(plan, s, off(170, 300)) == "DONE");
  s = advance(plan, s, off(170, 300));
  assert(s.phase == Phase::Finished);

  // The next START runs the workout again from the first piece.
  s = advance(plan, s, on(0, 0));
  assert(s.phase == Phase::Work && s.index == 0 && near(s.startS, 0));
}

static void interruptions() {
  const Plan plan = parse("W1;T10,5;T10,5;T10,0").plan;

  // Stopping mid-workout aborts back to Idle.
  RunState s = advance(plan, RunState{}, on(0, 0));
  s = advance(plan, s, on(12, 0));
  assert(s.phase == Phase::Rest);
  s = advance(plan, s, off(12.5f, 0));
  assert(s.phase == Phase::Idle);

  // One delayed update can cross several boundaries.
  s = advance(plan, RunState{}, on(0, 0));
  s = advance(plan, s, on(26, 0));
  assert(s.phase == Phase::Rest && s.index == 1 && near(s.startS, 25));
  s = advance(plan, RunState{}, on(0, 0));
  s = advance(plan, s, on(500, 0));
  assert(s.phase == Phase::Finishing);

  // A missed Stop/Start (elapsed jumps back) restarts from piece 1.
  s = advance(plan, RunState{}, on(0, 0));
  s = advance(plan, s, on(18, 0));
  assert(s.phase == Phase::Work && s.index == 1);
  s = advance(plan, s, on(2, 0));
  assert(s.phase == Phase::Work && s.index == 0 && near(s.startS, 2));
  // ...but small timing jitter does not.
  s = advance(plan, s, on(1.5f, 0));
  assert(s.phase == Phase::Work && near(s.startS, 2));

  // Booting mid-session starts at the current elapsed time.
  s = advance(plan, RunState{}, on(300, 900));
  assert(s.phase == Phase::Work && near(s.startS, 300) && near(s.startM, 900));

  // Without a plan nothing runs and the title shows.
  const Plan empty = parse("W1").plan;
  s = advance(empty, RunState{}, on(0, 0));
  assert(s.phase == Phase::Idle);
  assert(text(empty, s, on(0, 0)) == "PERG");

  const Plan single = parse("W1;T3600,0").plan;
  s = advance(single, RunState{}, off(0, 0));
  assert(text(single, s, off(0, 0)) == "READY 1 PC");
  s = advance(single, s, on(0, 0));
  assert(text(single, s, on(0, 0)) == "P1/1 60:00");
  // A sample slightly before the piece start never shows more than the target.
  assert(text(single, s, on(-0.5f, 0)) == "P1/1 60:00");

  const Plan widest = parse(repeat(";D10000,0", 20).c_str()).plan;
  s = advance(widest, RunState{}, on(0, 0));
  assert(text(widest, s, on(0, 0)) == "P1/20 10000m");
  assert(std::strlen(describe(widest, s, on(0, 0)).text) < sizeof(Banner{}.text));
}

int main() {
  parsing();
  formatting();
  fullWorkout();
  interruptions();
}
"""


@unittest.skipUnless(shutil.which("c++"), "Native C++ compiler is not installed")
class WorkoutFirmwareTests(unittest.TestCase):
    def test_parse_format_runner_and_status_text(self):
        source_text = "#include " + json.dumps(str(WORKOUT_HEADER)) + "\n" + HARNESS
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "workout.cpp"
            executable = Path(tmp) / "workout"
            source.write_text(source_text)
            # Apple's sanitizer runtime can be incompatible with the host ABI.
            # CI still exercises AddressSanitizer and UndefinedBehaviorSanitizer.
            sanitizers = (
                ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
                if sys.platform == "linux"
                else []
            )
            build = subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    *sanitizers,
                    str(source),
                    "-o",
                    str(executable),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
