#pragma once
#include <stddef.h>
#include <stdint.h>

// BLE writes are packets, not lines. A CSV line may span several 20-byte
// packets, and a packet may contain more than one line. Drop oversized lines
// through their newline so a damaged suffix is never interpreted as telemetry.
class BleLineFramer {
 public:
  void reset() { used = 0; overflow = false; }
  template <typename Sink>
  void feed(const uint8_t *bytes, size_t length, Sink sink) {
    for (size_t i = 0; i < length; ++i) {
      char c = bytes[i];
      if (c == '\n') {
        text[used] = '\0';
        if (!overflow && used) sink(text);
        reset();
      } else if (c != '\r') {
        if (c == '\0' || used >= sizeof(text) - 1) overflow = true;
        else if (!overflow) text[used++] = c;
      }
    }
  }
 private:
  char text[112] = {};
  size_t used = 0;
  bool overflow = false;
};
