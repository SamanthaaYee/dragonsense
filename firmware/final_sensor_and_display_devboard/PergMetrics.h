#pragma once

#include <stdint.h>
#include <math.h>

// perg_rpm_stroke_rate_v2.xlsx, Settings B5:B13 (drag 61).
// All timestamps use unsigned subtraction so micros() rollover is safe.
class PergMetrics {
 public:
  static constexpr float MIDLINE = 1820.0f;
  static constexpr float HYSTERESIS = 150.0f;
  static constexpr float METRES_PER_REV = 0.2481f;
  static constexpr float WATTS_K = 0.0001578f;
  static constexpr uint32_t POWER_WINDOW_US = 1500000;
  static constexpr uint32_t PACE_WINDOW_US = 1500000;
  static constexpr uint32_t STOP_US = 700000;

  float rpm = 0, periodMs = 0, strokeRate = 0;
  uint32_t pulses = 0;

  bool latestStroke(uint32_t &start, uint32_t &end) const {
    if (catchCount < 2) return false;
    start = catches[catchCount - 2];
    end = catches[catchCount - 1];
    return true;
  }

  bool latestRevolution(uint32_t &midpoint, float &turnRpm) const {
    if (raw.count < 9) return false;
    const Point &last = raw.at(raw.count - 1);
    const Point &first = raw.at(raw.count - 9);
    const uint32_t duration = last.time - first.time;
    // Time a full revolution to cancel uneven magnet spacing.
    midpoint = first.time + duration / 2;
    turnRpm = 60000000.0f / duration;
    return true;
  }

  void sample(uint32_t time, int adc) {
    tick(time);
    if (adc < MIDLINE - HYSTERESIS) armed = true;
    if (haveSample && armed && previousAdc < MIDLINE && adc >= MIDLINE) {
      float fraction = (MIDLINE - previousAdc) / (adc - previousAdc);
      uint32_t crossing = previousTime + (uint32_t)(fraction * (uint32_t)(time - previousTime));
      pulse(crossing);
      armed = false;
    }
    previousTime = time;
    previousAdc = adc;
    haveSample = true;
  }

  void tick(uint32_t now) {
    if (havePulse && (uint32_t)(now - lastPulse) > STOP_US) {
      raw.clear();
      smooth.clear();
      havePulse = false;
      haveCatch = false;
      catchCount = 0;
      rpm = periodMs = strokeRate = 0;
    }
    if (haveCatch && (uint32_t)(now - lastCatch) > 5000000UL) {
      haveCatch = false;
      catchCount = 0;
      strokeRate = 0;
    }
  }

  float watts(uint32_t now) const {
    if (!havePulse || !raw.count || (uint32_t)(now - raw.at(0).time) < POWER_WINDOW_US)
      return 0;
    float sum = 0;
    int count = 0;
    for (int i = raw.count - 1; i >= 0; --i) {
      const Point &p = raw.at(i);
      if ((uint32_t)(now - p.time) >= POWER_WINDOW_US) break;
      sum += p.value;
      ++count;
    }
    float omega = count ? sum / count * 6.283185307f / 60.0f : 0;
    return WATTS_K * omega * omega * omega;
  }

  float pace(uint32_t now) const {
    if (!havePulse || raw.count < 9 || (uint32_t)(now - lastPulse) > STOP_US)
      return 0;
    // Compare the same magnet across whole turns to cancel uneven magnet
    // spacing. Extend the window to the first whole turn covering 1.5 s;
    // during startup use all available whole turns (at least one).
    // End at the latest pulse, not the telemetry timestamp: packet timing
    // must not change the measured speed or produce a sawtooth pace.
    int last = raw.count - 1;
    int first = last;
    while (first >= 8) {
      first -= 8;
      if ((uint32_t)(raw.at(last).time - raw.at(first).time) >= PACE_WINDOW_US)
        break;
    }
    float distance = ((last - first) / 8) * METRES_PER_REV;
    float seconds = (uint32_t)(raw.at(last).time - raw.at(first).time) / 1000000.0f;
    return 500.0f * seconds / distance;
  }

  // Exposed separately to allow replay of interpolated pulse timestamps.
  void pulse(uint32_t time) {
    tick(time);
    if (!havePulse) {
      havePulse = true;
      lastPulse = time;
      return; // first crossing establishes the distance origin
    }
    uint32_t interval = time - lastPulse;
    if (interval <= 4000) return; // reject double triggers; keep timing origin
    lastPulse = time;
    if (interval >= 300000) {
      raw.clear();
      smooth.clear();
      rpm = periodMs = strokeRate = 0;
      haveCatch = false;
      catchCount = 0;
      return;
    }
    ++pulses; // one accepted magnet interval = 1/8 turn
    periodMs = interval / 1000.0f;
    raw.push({time, 7500000.0f / interval});

    // Workbook F: centered +/-0.15 s average, emitted 0.15 s late.
    for (int i = 0; i < raw.count; ++i) {
      Point &p = raw.at(i);
      if (p.done || (uint32_t)(time - p.time) < 150000) continue;
      p.done = true;
      if ((uint32_t)(p.time - raw.at(0).time) < 150000) continue;
      float sum = 0;
      int count = 0;
      for (int j = 0; j < raw.count; ++j) {
        if (absDelta(raw.at(j).time, p.time) <= 150000) {
          sum += raw.at(j).value;
          ++count;
        }
      }
      rpm = sum / count;
      smooth.push({p.time, rpm});
      detectCatches(p.time);
    }
  }

 private:
  struct Point {
    uint32_t time;
    float value;
    bool done;
    Point(uint32_t t = 0, float v = 0) : time(t), value(v), done(false) {}
  };
  // 640 entries cover >2.5 s even at the 4 ms minimum pulse interval.
  struct History {
    Point data[640];
    int head = 0, count = 0;
    Point &at(int i) { return data[(head + i) % 640]; }
    const Point &at(int i) const { return data[(head + i) % 640]; }
    void clear() { head = count = 0; }
    void push(Point p) {
      if (count == 640) { head = (head + 1) % 640; --count; }
      at(count++) = p;
    }
  } raw, smooth;
  bool armed = false, haveSample = false, havePulse = false, haveCatch = false;
  int previousAdc = 0;
  uint32_t previousTime = 0, lastPulse = 0, lastCatch = 0;
  uint32_t catches[5] = {};
  int catchCount = 0;

  static uint32_t absDelta(uint32_t a, uint32_t b) {
    int32_t d = (int32_t)(a - b);
    return d < 0 ? (uint32_t)(-(int64_t)d) : (uint32_t)d;
  }

  void detectCatches(uint32_t latest) {
    for (int i = 0; i < smooth.count; ++i) {
      Point &p = smooth.at(i);
      if (p.done || (uint32_t)(latest - p.time) < 300000) continue;
      p.done = true;
      if ((uint32_t)(p.time - smooth.at(0).time) < 300000) continue;
      float low = p.value, high = p.value;
      bool earlierEqual = false;
      for (int j = 0; j < smooth.count; ++j) {
        const Point &q = smooth.at(j);
        if (absDelta(q.time, p.time) > 300000) continue;
        if (q.value < low) low = q.value;
        if (q.value > high) high = q.value;
        if (j < i && q.value == p.value) earlierEqual = true;
      }
      if (p.value != low || high - low < 8.0f || earlierEqual) continue;
      // Console check K averages the last four catch-to-catch intervals.
      if (catchCount == 5) {
        for (int j = 1; j < 5; ++j) catches[j - 1] = catches[j];
        --catchCount;
      }
      catches[catchCount++] = p.time;
      lastCatch = p.time;
      haveCatch = true;
      if (catchCount >= 2)
        strokeRate = 60000000.0f * (catchCount - 1) /
                     (uint32_t)(p.time - catches[0]);
    }
  }
};
