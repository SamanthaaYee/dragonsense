#pragma once
// Interval workouts pushed from web/index.html. The format and runner rules are
// specified in docs/workout_push.md. Plain C++ only: no Arduino, BLE or TFT
// code, so the same logic serves any transport and compiles natively in tests.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace workout {

constexpr uint8_t MAX_PIECES = 20;
constexpr uint32_t MIN_TIME_S = 10;
constexpr uint32_t MAX_TIME_S = 3600;
constexpr uint32_t MIN_DISTANCE_M = 50;
constexpr uint32_t MAX_DISTANCE_M = 10000;
constexpr uint32_t MAX_REST_S = 1800;
// "W1" plus MAX_PIECES of the longest piece, ";D10000,1800".
constexpr size_t MAX_LINE = 2 + MAX_PIECES * 12;
// A larger drop in elapsed time means the logger started a new session.
constexpr float RESTART_TOLERANCE_S = 1.0f;
// Absorbs float noise so an exact remaining value never rounds up a unit.
constexpr float ROUNDING_SLACK = 1e-3f;

enum class Kind : uint8_t { Time, Distance };

struct Piece {
  Kind kind;
  uint16_t target;  // seconds (Time) or metres (Distance)
  uint16_t restS;   // rest after this piece; ignored after the last piece
};

struct Plan {
  uint8_t count = 0;  // 0 = no workout
  Piece pieces[MAX_PIECES] = {};
};

// Busy is never returned by parse(); the transport reports it while a
// workout is in progress.
enum class Error : uint8_t { None, Length, Version, Syntax, Range, Count, Busy };

inline const char *errorCode(Error error) {
  switch (error) {
    case Error::None: return "none";
    case Error::Length: return "length";
    case Error::Version: return "version";
    case Error::Syntax: return "syntax";
    case Error::Range: return "range";
    case Error::Count: return "count";
    case Error::Busy: return "busy";
  }
  return "unknown";
}

struct ParseResult {
  Error error;
  Plan plan;  // empty unless error == Error::None
};

namespace detail {

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Canonical decimal: 1-5 digits, no leading zeros. Advances p on success.
inline bool readNumber(const char *&p, uint32_t &value) {
  if (!isDigit(*p) || (*p == '0' && isDigit(p[1]))) return false;
  uint32_t result = 0;
  int digits = 0;
  while (isDigit(*p)) {
    if (++digits > 5) return false;
    result = result * 10 + (uint32_t)(*p - '0');
    ++p;
  }
  value = result;
  return true;
}

inline bool inRange(Kind kind, uint32_t target, uint32_t restS) {
  const bool targetOk = kind == Kind::Time
      ? target >= MIN_TIME_S && target <= MAX_TIME_S
      : target >= MIN_DISTANCE_M && target <= MAX_DISTANCE_M;
  return targetOk && restS <= MAX_REST_S;
}

inline ParseResult failure(Error error) { return ParseResult{error, Plan{}}; }

}  // namespace detail

// Parses one line without its newline. Never partially accepts a workout.
inline ParseResult parse(const char *line) {
  using detail::failure;
  if (strnlen(line, MAX_LINE + 1) > MAX_LINE) return failure(Error::Length);
  if (line[0] != 'W' || line[1] != '1') return failure(Error::Version);
  Plan plan;
  const char *p = line + 2;
  while (*p) {
    if (*p != ';') return failure(Error::Syntax);
    ++p;
    if (*p != 'T' && *p != 'D') return failure(Error::Syntax);
    const Kind kind = *p == 'T' ? Kind::Time : Kind::Distance;
    ++p;
    uint32_t target = 0, restS = 0;
    if (!detail::readNumber(p, target) || *p != ',') return failure(Error::Syntax);
    ++p;
    if (!detail::readNumber(p, restS)) return failure(Error::Syntax);
    if (!detail::inRange(kind, target, restS)) return failure(Error::Range);
    if (plan.count == MAX_PIECES) return failure(Error::Count);
    plan.pieces[plan.count++] = Piece{kind, (uint16_t)target, (uint16_t)restS};
  }
  return ParseResult{Error::None, plan};
}

// Writes the canonical line. Returns its length, or 0 if it does not fit.
inline size_t format(const Plan &plan, char *out, size_t size) {
  size_t used = 0;
  int written = snprintf(out, size, "W1");
  if (written < 0 || (size_t)written >= size) return 0;
  used = (size_t)written;
  for (uint8_t i = 0; i < plan.count; ++i) {
    const Piece &piece = plan.pieces[i];
    written = snprintf(out + used, size - used, ";%c%u,%u",
                       piece.kind == Kind::Time ? 'T' : 'D',
                       (unsigned)piece.target, (unsigned)piece.restS);
    if (written < 0 || (size_t)written >= size - used) return 0;
    used += (size_t)written;
  }
  return used;
}

// FNV-1a 32-bit. The display acknowledges a push with this hash of the line.
inline uint32_t checksum(const char *text) {
  uint32_t hash = 0x811c9dc5u;
  for (; *text; ++text) {
    hash ^= (uint8_t)*text;
    hash *= 0x01000193u;
  }
  return hash;
}

// ---------------- Runner ----------------

enum class Phase : uint8_t { Idle, Work, Rest, Finishing, Finished };

struct RunState {
  Phase phase = Phase::Idle;
  uint8_t index = 0;        // current piece (Work) or the piece just finished (Rest)
  float startS = 0;         // elapsed time when this phase began
  float startM = 0;         // distance baseline for the current piece
  float lastElapsedS = 0;   // previous sample, to detect a restarted session
};

struct Sample {
  bool running;     // logger session_state
  float elapsedS;   // logger elapsed_s
  float distanceM;  // logger distance_m
};

inline bool inProgress(Phase phase) {
  return phase == Phase::Work || phase == Phase::Rest || phase == Phase::Finishing;
}

namespace detail {

inline RunState phase(Phase p, uint8_t index, float startS, float startM, float lastS) {
  RunState state;
  state.phase = p;
  state.index = index;
  state.startS = startS;
  state.startM = startM;
  state.lastElapsedS = lastS;
  return state;
}

inline float progress(const Piece &piece, const RunState &state, const Sample &sample) {
  return piece.kind == Kind::Time ? sample.elapsedS - state.startS
                                  : sample.distanceM - state.startM;
}

// Applies every piece/rest boundary already reached by this sample.
inline RunState settle(const Plan &plan, RunState state, const Sample &sample) {
  for (int guard = 0; guard <= 2 * MAX_PIECES; ++guard) {
    const Piece &piece = plan.pieces[state.index];
    if (state.phase == Phase::Work) {
      if (progress(piece, state, sample) < piece.target) break;
      const bool timed = piece.kind == Kind::Time;
      // Time pieces end exactly on target; distance pieces carry overshoot.
      const float endS = timed ? state.startS + piece.target : sample.elapsedS;
      const float endM = timed ? sample.distanceM : state.startM + piece.target;
      if (state.index + 1 >= plan.count) {
        state = phase(Phase::Finishing, state.index, endS, endM, state.lastElapsedS);
        break;
      }
      state = piece.restS > 0
          ? phase(Phase::Rest, state.index, endS, endM, state.lastElapsedS)
          : phase(Phase::Work, state.index + 1, endS, endM, state.lastElapsedS);
    } else if (state.phase == Phase::Rest) {
      if (sample.elapsedS - state.startS < piece.restS) break;
      // Distance paddled during the rest never counts toward the next piece.
      state = phase(Phase::Work, state.index + 1, state.startS + piece.restS,
                    sample.distanceM, state.lastElapsedS);
    } else {
      break;
    }
  }
  return state;
}

}  // namespace detail

// Pure state transition: returns the runner state after one telemetry sample.
inline RunState advance(const Plan &plan, const RunState &previous, const Sample &sample) {
  RunState next = previous;
  if (plan.count == 0) {
    next = RunState{};
  } else if (!sample.running) {
    if (previous.phase == Phase::Finishing || previous.phase == Phase::Finished) {
      next.phase = Phase::Finished;
    } else {
      next.phase = Phase::Idle;  // never started, or stopped mid-workout
    }
  } else {
    const bool restarted = inProgress(previous.phase) &&
        sample.elapsedS + RESTART_TOLERANCE_S < previous.lastElapsedS;
    if (previous.phase == Phase::Idle || previous.phase == Phase::Finished || restarted) {
      next = detail::phase(Phase::Work, 0, sample.elapsedS, sample.distanceM, 0);
    }
    next = detail::settle(plan, next, sample);
  }
  next.lastElapsedS = sample.elapsedS;
  return next;
}

// ---------------- Status text ----------------

struct Banner {
  char text[24];  // longest real text is 12 chars; sized so snprintf provably fits
  Phase phase;
  float fraction;  // 0..1 progress through the current piece or rest
};

namespace detail {

// Remaining amount rounded up, never more than the full target (clock jitter
// can briefly put a sample before the phase start).
inline uint32_t roundUp(float value, uint32_t limit) {
  const float slack = value - ROUNDING_SLACK;
  if (slack <= 0) return 0;
  const uint32_t rounded = (uint32_t)ceilf(slack);
  return rounded < limit ? rounded : limit;
}

inline float clamp01(float value) { return value < 0 ? 0 : value > 1 ? 1 : value; }

}  // namespace detail

inline Banner describe(const Plan &plan, const RunState &state, const Sample &sample) {
  Banner banner = {};
  banner.phase = state.phase;
  const unsigned count = plan.count;
  if (count == 0) {
    snprintf(banner.text, sizeof(banner.text), "PERG");
    return banner;
  }
  const Piece &piece = plan.pieces[state.index];
  switch (state.phase) {
    case Phase::Idle:
      snprintf(banner.text, sizeof(banner.text), "READY %u %s", count,
               count == 1 ? "PC" : "PCS");
      break;
    case Phase::Work: {
      const float done = detail::progress(piece, state, sample);
      const uint32_t left = detail::roundUp(piece.target - done, piece.target);
      if (piece.kind == Kind::Time) {
        snprintf(banner.text, sizeof(banner.text), "P%u/%u %u:%02u",
                 state.index + 1u, count, (unsigned)(left / 60), (unsigned)(left % 60));
      } else {
        snprintf(banner.text, sizeof(banner.text), "P%u/%u %um",
                 state.index + 1u, count, (unsigned)left);
      }
      banner.fraction = detail::clamp01(done / piece.target);
      break;
    }
    case Phase::Rest: {
      const float done = sample.elapsedS - state.startS;
      const uint32_t left = detail::roundUp(piece.restS - done, piece.restS);
      snprintf(banner.text, sizeof(banner.text), "REST %u:%02u",
               (unsigned)(left / 60), (unsigned)(left % 60));
      banner.fraction = detail::clamp01(done / piece.restS);
      break;
    }
    case Phase::Finishing:
    case Phase::Finished:
      snprintf(banner.text, sizeof(banner.text), "DONE");
      banner.fraction = 1;
      break;
  }
  return banner;
}

}  // namespace workout
