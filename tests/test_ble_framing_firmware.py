"""Exercise packet fragmentation and reconnect boundaries in the S3 receiver."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(shutil.which("c++"), "Native C++ compiler is not installed")
class BleFramingFirmwareTests(unittest.TestCase):
    def test_fragmentation_overflow_and_reconnect(self):
        header = (
            Path(__file__).resolve().parents[1]
            / "firmware/Old Firmware/perg_freenove_esp32_s3_display/BleLineFramer.h"
        )
        harness = "#include " + json.dumps(str(header)) + "\n"
        harness += r"""
#include <cassert>
#include <string>
#include <vector>
int main() {
  const std::string state = "session_state,1";
  const std::string values = "750.0,161.2,25.4,0.123,10.0,8.2,76,50.0";
  const std::string message = state + "\n" + values + "\n";
  // Check every possible split, including packets straddling the newline.
  for (size_t split = 0; split <= message.size(); ++split) {
    BleLineFramer framer;
    std::vector<std::string> lines;
    auto sink = [&](const char *s) { lines.emplace_back(s); };
    framer.feed((const uint8_t *)message.data(), split, sink);
    framer.feed((const uint8_t *)message.data()+split, message.size()-split, sink);
    assert(lines.size() == 2 && lines[0] == state && lines[1] == values);
  }
  BleLineFramer framer;
  std::vector<std::string> lines;
  auto feed = [&](const std::string &s) {
    framer.feed((const uint8_t *)s.data(), s.size(),
                [&](const char *line) { lines.emplace_back(line); });
  };
  feed(std::string(112, '9')); // overlong prefix without a newline
  feed(values + "\n\r\n" + state + "\r\n");
  assert(lines.size() == 1 && lines[0] == state); // discard suffix and resync
  feed("broken,partial");
  framer.reset(); // disconnect must not concatenate two sessions
  feed(values + "\n");
  assert(lines.size() == 2 && lines[1] == values);
  feed(std::string("bad\0suffix\n", 11) + state + "\n");
  assert(lines.size() == 3 && lines[2] == state); // embedded NUL is invalid
  // A maximum-length valid line still leaves space for the terminator.
  feed(std::string(111, '1') + "\n");
  assert(lines.size() == 4 && lines[3].size() == 111);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "framer.cpp"
            executable = Path(tmp) / "framer"
            source.write_text(harness)
            subprocess.run(
                ["c++", "-std=c++11", str(source), "-o", str(executable)],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
