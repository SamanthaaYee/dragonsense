#pragma once

#include <stdint.h>

// Relative drive force inferred from flywheel speed. RPM/s is proportional
// to torque for a given flywheel, with drag estimated from its recovery.
class StrokeForce {
 public:
  static constexpr int POINTS = 32;
  uint8_t curve[POINTS] = {};
  bool curveReady = false;

  void reset(uint32_t time) {
    head = count = 0;
    sessionStart = lastEnd = time;
    curveReady = false;
  }

  void sampleSpeed(uint32_t time, float rpm) {
    if (count == 0 && (int32_t)(time - sessionStart) < 0) return;
    if (count && (uint32_t)(time - at(count - 1).time) < 20000) return;
    if (count == CAPACITY) {
      head = (head + 1) % CAPACITY;
      --count;
    }
    history[(head + count++) % CAPACITY] = {time, rpm};
  }

  void finish(uint32_t start, uint32_t end) {
    const uint32_t duration = end - start;
    if (end == lastEnd || duration == 0 || duration > 5000000 || count < 2 ||
        (int32_t)(start - at(0).time) < 0 ||
        (int32_t)(at(count - 1).time - end) < 0) return;
    lastEnd = end;

    // Interpolate the catch-to-catch RPM trace on a uniform time grid.
    constexpr int STEPS = 64;
    float speeds[STEPS + 1];
    int peak = 0;
    for (int step = 0, i = 0; step <= STEPS; ++step) {
      const uint32_t target = start + duration * step / STEPS;
      while (i + 1 < count - 1 && (int32_t)(target - at(i + 1).time) >= 0) ++i;
      const Sample &before = at(i), &after = at(i + 1);
      if (after.time - before.time > 250000) return;
      const float fraction = (float)(target - before.time) / (after.time - before.time);
      speeds[step] = before.rpm + fraction * (after.rpm - before.rpm);
      if (speeds[step] > speeds[peak]) peak = step;
    }
    // The speed maximum approximates the drive's end. Use the middle
    // half of the recovery to estimate dRPM/dt = -drag * RPM^2.
    if (peak < 4 || peak > STEPS - 4 || speeds[peak] - speeds[0] < 8.0f) return;
    const int recoveryFirst = peak + (STEPS - peak) / 4;
    const int recoveryLast = STEPS - (STEPS - peak) / 4;
    const float secondsPerStep = (duration / 1000000.0f) / STEPS;
    const float coastSeconds = (recoveryLast - recoveryFirst) * secondsPerStep;
    const float drag = speeds[recoveryFirst] > speeds[recoveryLast]
        ? (1.0f / speeds[recoveryLast] - 1.0f / speeds[recoveryFirst]) / coastSeconds
        : 0.0f;

    float effort[STEPS];
    for (int step = 0; step <= peak; ++step) {
      const int first = step < 2 ? 0 : step - 2;
      const int last = step + 2;
      const float acceleration = (speeds[last] - speeds[first]) / ((last - first) * secondsPerStep);
      const float drive = acceleration + drag * speeds[step] * speeds[step];
      effort[step] = drive > 0 ? drive : 0;
    }

    float points[POINTS];
    float peakEffort = 0;
    for (int point = 0; point < POINTS; ++point) {
      const int position = point * peak;
      const int step = position / (POINTS - 1);
      const float fraction = (float)(position % (POINTS - 1)) / (POINTS - 1);
      points[point] = effort[step];
      if (step < peak) points[point] += fraction * (effort[step + 1] - effort[step]);
      if (points[point] > peakEffort) peakEffort = points[point];
    }
    if (peakEffort <= 0) return;
    for (int point = 0; point < POINTS; ++point) {
      curve[point] = (uint8_t)(255.0f * (points[point] / peakEffort) + 0.5f);
    }
    curveReady = true;
  }

 private:
  static constexpr int CAPACITY = 300; // at least six seconds at up to 50 samples/s
  struct Sample { uint32_t time; float rpm; };
  Sample history[CAPACITY];
  int head = 0, count = 0;
  uint32_t sessionStart = 0, lastEnd = 0;

  const Sample &at(int i) const { return history[(head + i) % CAPACITY]; }
};
