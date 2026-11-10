/*
 * Perg USB Serial + Bluetooth Logger (ESP32)
 *
 * Reads the Perg's Hall-effect sensor, detects the flywheel magnet pulses in
 * real time, and broadcasts RPM, pace (seconds per 500 m) and total distance
 * over Bluetooth SPP to the ESP32 CYD running perg_stat_display.ino.
 *
 * Samples every 500 microseconds (2 kHz) and sends each raw reading over USB
 * serial as CSV: time_us,adc_value (460800 baud, for record_perg.py).
 * The same samples feed the pulse detector and Bluetooth telemetry below.
 * Bluetooth connection attempts and writes run in a separate task so an
 * unavailable display does not stop USB logging. Only the latest telemetry
 * is kept while disconnected; raw samples are sent only over USB.
 * Each telemetry update is also mirrored over USB as:
 *   telemetry,time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts
 *
 * Telemetry line (CSV, one every 100 ms):
 *   rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts
 * where hall_v is the instantaneous Hall voltage, period_ms is the magnet
 * pulse interval (the flywheel-revolution period is 8x that), elapsed_s is
 * the time since the LCD Start button, frozen by Stop.
 * USB session_start,time_us and session_stop,time_us markers control capture.
 *
 * Idle detection: when the flywheel is idle the signal is flat, and ADC
 * noise alone makes the adaptive envelope span collapse to ~±0.005 V
 * (spikes to ~0.05 V). The detector is therefore gated on signal amplitude:
 * pulses only count when the envelope span exceeds GATE_ON_V (and the gate
 * closes again below GATE_OFF_V), so idle noise cannot register as RPM.
 *
 * Wiring (same as perg_logger.ino):
 *   Perg 3.5 mm L (signal) --[10 kΩ]-- GPIO 27
 *   Perg 3.5 mm GND        ----------- ESP32 GND
 *   (R terminal is not used)
 *
 * GPIO 27 is ADC2, which cannot be read while Wi-Fi is on. Classic Bluetooth
 * (SPP) does not block ADC2, so this sketch can use both at once. Do NOT add
 * Wi-Fi here without moving the sensor to an ADC1 pin (GPIO 32-39).
 *
 * RPM detection - and why the voltage is NOT smoothed:
 *   Each of the 8 flywheel magnets produces one pulse. The raw signal is a
 *   ~0.6 VAC sine-ish wave centered near mid-scale, so each pulse is one full
 *   cycle and RPM = 60 / (pulse_interval * 8).
 *   A moving average "over a few samples" would shift every crossing by
 *   ~(N-1)/2 samples in time. At 2 kHz sampling and ~60 Hz pulse rate a
 *   3-sample smoothing already moves the crossing ~0.5 ms, which is ~3% of the
 *   interval - a direct RPM error. So the voltage is used raw. Noise is
 *   handled where it costs no timing accuracy:
 *     - A slow envelope follower (envHi/envLo) tracks the signal's rails and
 *       sets an adaptive midpoint threshold.
 *     - A Schmitt trigger (separate rising 65% / falling 35% thresholds)
 *       rejects noise around the threshold instead of smoothing it away.
 *     - The pulse INTERVALS, not the voltage, are then smoothed: the median
 *       of the last 5 intervals rejects double-triggered or missed pulses,
 *       and an EMA on RPM gives a stable display value.
 */

#include "BluetoothSerial.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// ---------------- Hall sensor ----------------
const int HALL_PIN = 27;                     // Perg signal (via 10 kΩ) -> ADC2_CH7
const unsigned long SAMPLE_PERIOD_US = 500; // 0.5 ms -> 2000 samples/s
// Worst-case raw CSV: 17 bytes/sample * 2000 = 34 kB/s. At 460800
// baud, 8N1 carries 46.08 kB/s, leaving room for telemetry. 10 kHz CSV
// would require up to 170 kB/s and cannot fit at this baud rate.
const unsigned long BAUD = 460800;

// ---------------- Flywheel geometry ----------------
const int MAGNETS_PER_REV = 8;

// Distance calibration for the drag-based speed model: with the flywheel
// spinning at `rpm`, the drag model gives absorbed power, the erg-standard
// power-to-speed relation gives m/s, and distance is the integral of speed.
// (The old metersPerRev() approach is gone: everything now derives from the
// drag model, so pace and distance can never disagree.)

// ---------------- Pace (drag model) ----------------
// The Perg reports a drag coefficient of 61 for its flywheel, in unknown
// units. Flywheel power absorbed by drag grows with the cube of angular
// speed; the erg-standard power-to-speed relation is P = 2.8 * v³
// (Concept2 calibration), so speed v = cbrt(P / 2.8) m/s and
// pace = 500 m / v.
//
// The 61 is NOT watts per (rad/s)³ - used directly it made the pace read
// ~8 s/500m, i.e. power was ~3000x too high (pace scales with P^(-1/3)).
// DRAG_WATT_SCALE converts the machine's drag units into watts per
// (rad/s)³. Default chosen so a typical working flywheel speed (~450 rpm,
// ω ≈ 47 rad/s) lands near a realistic ~1:50/500m pace:
//   P = 61 * 4.0e-5 * ω³ ≈ 250 W  ->  v ≈ 4.5 m/s  ->  ≈ 111 s
// Calibrate: row at a speed whose pace the machine shows, then adjust
// DRAG_WATT_SCALE until the display matches (pace scales with the cube root
// of the scale, so it is forgiving).
const float DRAG_COEFF = 61.0;
const float DRAG_WATT_SCALE = 4.0e-5f;
const float POWER_TO_SPEED_K = 2.8f;
const float PACE_DISTANCE_M = 500.0; // rowing standard: pace per 500 m

// Flywheel power absorbed by drag at a given rpm (watts).
float flywheelPower(float rpm) {
  float omega = rpm * 6.2831853f / 60.0f; // rad/s
  return DRAG_COEFF * DRAG_WATT_SCALE * omega * omega * omega;
}

// ---------------- Bluetooth ----------------
const char *BT_LOCAL_NAME = "perg-logger";
// Bluetooth MAC of the CYD running perg_stat_display.ino.
uint8_t displayMac[6] = {0xA4, 0xF0, 0x0F, 0x8D, 0x32, 0x7E};

const unsigned long TELEMETRY_PERIOD_MS = 100; // 10 telemetry updates/s

// ---------------- Pulse detection ----------------
// Envelope follower: how fast each rail relaxes toward the opposite rail when
// the signal does not reach it. Scale decay with the sample period to keep
// approximately the same 0.5 s rail relaxation time when changing the rate,
// slow enough to ride out one pulse cycle.
const float ENV_DECAY_PER_SAMPLE = SAMPLE_PERIOD_US / 500000.0f;
// Schmitt thresholds as fractions of the envelope span (envLo..envHi):
// pulse starts on a rising crossing at 65%, ends on a falling crossing at
// 35%. The gap between them is the hysteresis that swallows noise.
const float THR_UP_F = 0.65f;
const float THR_DN_F = 0.35f;
// Valid magnet-to-magnet interval window. Outside it the interval is an
// artifact (noise, missed pulse, flywheel stopped) and gets discarded.
const float MIN_INTERVAL_MS = 4.0f;   // faster than ~1900 rpm
const float MAX_INTERVAL_MS = 300.0f; // slower than ~25 rpm (coasting)
// Noise-floor gate (in volts, peak-to-peak envelope span). At idle the Hall
// signal is flat and only noise remains - observed ~±0.004 V with spikes to
// ~0.048 V (≈0.1 V p-p) - while real magnet pulses ride at ~0.6 VAC
// (≈1.2 V p-p). Detection only runs when the span is above GATE_ON_V, and
// switches off again below GATE_OFF_V, so the gate itself has hysteresis and
// cannot chatter around one boundary.
const float GATE_ON_V = 0.1f;  // ~1/4 of a real pulse's p-p swing
const float GATE_OFF_V = 0.05f; // safely above the 0.048 V idle spikes
// No valid pulse for this long -> the flywheel has stopped, RPM goes to 0.
const unsigned long SPIN_TIMEOUT_US = 700000UL;

// Interval smoothing: median of the last N valid intervals, then an EMA on
// the resulting RPM.
const int MEDIAN_N = 5;
const float RPM_EMA_ALPHA = 0.25f;

// ---------------- State ----------------
BluetoothSerial SerialBT;

// A one-item queue copies complete lines between tasks. The detector never
// waits for Bluetooth; a slow/disconnected display gets the newest update.
struct TelemetryLine {
  char text[104];
};
QueueHandle_t telemetryQueue = nullptr;
QueueHandle_t commandQueue = nullptr;

void bluetoothTask(void *parameter) {
  if (!SerialBT.begin(BT_LOCAL_NAME, true)) { // master: initiate connection
    vTaskDelete(nullptr);
    return;
  }

  char command[12];
  size_t used = 0;
  bool overflow = false;
  for (;;) {
    if (!SerialBT.connected()) {
      used = 0;
      overflow = false;
      if (!SerialBT.connect(displayMac)) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }

    // Receive short newline-delimited commands without blocking sampling.
    while (SerialBT.available()) {
      char c = SerialBT.read();
      if (c == '\n') {
        command[used] = '\0';
        char action = !overflow && strcmp(command, "START") == 0 ? 'S' :
                      !overflow && strcmp(command, "STOP") == 0 ? 'T' : 0;
        if (action && commandQueue) xQueueSend(commandQueue, &action, 0);
        used = 0;
        overflow = false;
      } else if (c != '\r') {
        if (used < sizeof(command) - 1) command[used++] = c;
        else overflow = true;
      }
    }
    TelemetryLine line;
    if (xQueueReceive(telemetryQueue, &line, pdMS_TO_TICKS(10)) == pdTRUE) {
      SerialBT.print(line.text); // newline-delimited CSV for the CYD
    }
  }
}

float envHi, envLo;      // adaptive signal rails (ADC counts)
bool stateHigh = false;  // Schmitt trigger state
bool gateOpen = false;   // noise-floor gate: signal amplitude is real

unsigned long lastPulseUs = 0;    // 0 = no pulse seen yet
unsigned long pulseCount = 0;     // total magnet pulses since boot
float recentIntervals[MEDIAN_N];  // circular buffer of valid intervals (ms)
int intervalCount = 0;
int intervalIdx = 0;

float rpmEma = 0.0f;              // smoothed RPM
bool rpmValid = false;
float periodMs = 0.0f;            // median pulse interval the detector sees

// Hall voltage, same calibration as perg_logger.ino: deviation in volts
// from the signal's ~1.64 V bias point (3.6 V across the 12-bit range,
// bias at count 1870).
const float VOLT_FULL_SCALE = 3.6f;
const float VOLT_BIAS_COUNTS = 1870.0f;
float lastVolt = 0.0f;

float distanceM = 0.0f;           // integral of drag-model speed
bool sessionStarted = false;
unsigned long sessionStartMs = 0;
unsigned long sessionElapsedMs = 0;
unsigned long lastDistanceUs = 0;

void sendTelemetry(unsigned long nowUs);

void controlSession(char action) {
  unsigned long nowUs = micros();
  if (action == 'S' && !sessionStarted) {
    sessionStarted = true;
    sessionStartMs = millis();
    sessionElapsedMs = 0;
    distanceM = 0;
    lastDistanceUs = nowUs;
    Serial.print("session_start,");
    Serial.println(nowUs);
    sendTelemetry(nowUs);
  } else if (action == 'T' && sessionStarted) {
    // Integrate and report the final distance before freezing the session.
    sendTelemetry(nowUs);
    sessionElapsedMs = millis() - sessionStartMs;
    sessionStarted = false;
    sendTelemetry(nowUs);
    Serial.print("session_stop,");
    Serial.println(nowUs);
  } else if (action == 'A') {
    // The USB recorder arms a new session without any reset-button timing.
    sessionStarted = false;
    sessionElapsedMs = 0;
    distanceM = 0;
    lastDistanceUs = nowUs;
    Serial.print("session_ready,");
    Serial.println(nowUs);
    sendTelemetry(nowUs);
  }
}

unsigned long nextSampleUs;
unsigned long nextTelemetryMs;

void setup() {
  Serial.begin(BAUD);

  // No pinMode(INPUT_PULLUP/PULLDOWN): pull resistors would distort the
  // analog signal. analogRead() configures the pin for the ADC itself.
  analogReadResolution(12);                   // 0-4095
  analogSetPinAttenuation(HALL_PIN, ADC_11db); // full range ~0-3.1 V

  telemetryQueue = xQueueCreate(1, sizeof(TelemetryLine));
  commandQueue = xQueueCreate(8, sizeof(char));
  if (telemetryQueue != nullptr) {
    // Keep potentially blocking Bluetooth calls out of the sampling loop.
    if (xTaskCreate(bluetoothTask, "perg-bluetooth", 4096, nullptr, 1,
                    nullptr) != pdPASS) {
      vQueueDelete(telemetryQueue);
      telemetryQueue = nullptr;
    }
  }
  // USB logging remains available even if the Bluetooth task cannot start.
  // USB carries raw sample CSV plus explicitly tagged telemetry CSV.

  nextSampleUs = micros();
  nextTelemetryMs = millis() + TELEMETRY_PERIOD_MS;
}

// Record one detected magnet pulse: validate the interval, smooth it, and
// update RPM / revolution counters.
void onPulse(unsigned long nowUs) {
  if (lastPulseUs != 0) {
    // Interval between this pulse and the previous one, in ms.
    float intervalMs = (nowUs - lastPulseUs) / 1000.0f;

    if (intervalMs > MIN_INTERVAL_MS && intervalMs < MAX_INTERVAL_MS) {
      recentIntervals[intervalIdx] = intervalMs;
      intervalIdx = (intervalIdx + 1) % MEDIAN_N;
      if (intervalCount < MEDIAN_N) intervalCount++;

      // Median of the kept intervals: one outlier (missed/double pulse)
      // cannot shift it, unlike a plain average.
      float sorted[MEDIAN_N];
      for (int i = 0; i < intervalCount; i++) sorted[i] = recentIntervals[i];
      for (int i = 1; i < intervalCount; i++) { // insertion sort
        float key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
          sorted[j + 1] = sorted[j];
          j--;
        }
        sorted[j + 1] = key;
      }
      float medianMs = sorted[intervalCount / 2];
      periodMs = medianMs; // reported as telemetry "period"

      // f_pulses = 1/interval; RPM = f_pulses * 60 / MAGNETS_PER_REV
      float rpm = (1000.0f / medianMs) * 60.0f / MAGNETS_PER_REV;

      if (rpmValid) {
        rpmEma += RPM_EMA_ALPHA * (rpm - rpmEma);
      } else {
        rpmEma = rpm;
        rpmValid = true;
      }
    }
  }

  lastPulseUs = nowUs;
  pulseCount++;
}

void loop() {
  // Session state belongs to this task; the Bluetooth task only queues commands.
  char action;
  if (commandQueue && xQueueReceive(commandQueue, &action, 0) == pdTRUE) {
    controlSession(action);
  }
  // A single-byte USB arm command is used only by --start-on-button.
  if (Serial.available() && Serial.read() == 'A') controlSession('A');
  // ---- Telemetry first, unconditional and on its own timeline ----
  // Emitted every TELEMETRY_PERIOD_MS no matter what the detector is doing:
  // the voltage field always reflects the last ADC reading, RPM is 0 when
  // the flywheel is stopped. The Start/Stop button controls elapsed time.
  if ((long)(millis() - nextTelemetryMs) >= 0) {
    nextTelemetryMs = millis() + TELEMETRY_PERIOD_MS;
    sendTelemetry(micros());
  }

  // ---- Sample on a fixed 0.5 ms timeline ----
  unsigned long now = micros();
  if ((long)(now - nextSampleUs) < 0) {
    return;
  }
  nextSampleUs += SAMPLE_PERIOD_US;

  // If we fell more than one period behind (e.g. USB serial stalled), skip
  // ahead instead of firing a burst of back-to-back catch-up samples.
  if ((long)(now - nextSampleUs) >= 0) {
    nextSampleUs = now + SAMPLE_PERIOD_US;
  }

  int adc = analogRead(HALL_PIN);

  // Raw USB capture, identical to perg_logger.ino and record_perg.py's input.
  Serial.print(now);
  Serial.print(',');
  Serial.println(adc);

  float v = (float)adc;

  // Instantaneous Hall voltage for the display (same calibration as
  // perg_logger.ino's CSV column: volts relative to the signal bias point).
  lastVolt = VOLT_FULL_SCALE * (v - VOLT_BIAS_COUNTS) / 4096.0f;

  // ---- Envelope follower (raw signal, no smoothing) ----
  if (envHi == 0 && envLo == 0) {
    envHi = v; // first sample seeds the rails
    envLo = v;
  }
  if (v > envHi) {
    envHi = v; // attack: rail jumps to the signal instantly
  } else {
    envHi += (envLo - envHi) * ENV_DECAY_PER_SAMPLE; // slow release
  }
  if (v < envLo) {
    envLo = v;
  } else {
    envLo += (envHi - envLo) * ENV_DECAY_PER_SAMPLE;
  }

  // ---- Schmitt trigger on the adaptive midpoint ----
  float span = envHi - envLo;
  float thrUp = envLo + span * THR_UP_F;
  float thrDn = envLo + span * THR_DN_F;

  // Noise-floor gate: convert the envelope span to volts (peak-to-peak) and
  // compare against GATE_ON_V / GATE_OFF_V. This is what keeps the idle
  // flywheel from reading as RPM: its noise (~±0.004 V, spikes ~0.05 V) is
  // far below the gate, while real pulses (~0.6 VAC) open it easily.
  float spanV = span * VOLT_FULL_SCALE / 4096.0f;
  if (!gateOpen && spanV >= GATE_ON_V) {
    gateOpen = true;
  } else if (gateOpen && spanV < GATE_OFF_V) {
    // Signal dropped back into noise: treat as stopped and reset everything
    // so stale intervals can't survive the next real row.
    gateOpen = false;
    stateHigh = false;
    rpmEma = 0.0f;
    rpmValid = false;
    periodMs = 0.0f;
    intervalCount = 0;
    lastPulseUs = 0;
  }

  if (!stateHigh && gateOpen && v >= thrUp) {
    stateHigh = true;
    onPulse(now); // rising edge = one magnet passed
  } else if (stateHigh && v <= thrDn) {
    stateHigh = false;
  }

  // ---- Telemetry is emitted at the top of loop(), unconditionally ----
}

void sendTelemetry(unsigned long nowUs) {
  // RPM is 0 whenever the flywheel is stopped. Two paths enforce this:
  //   1. The noise gate closing in loop() (signal amplitude dropped into
  //      noise), which resets everything immediately.
  //   2. This spin timeout: the gate may still be open while the flywheel
  //      decelerates, but with no pulse for SPIN_TIMEOUT_US it is stopped.
  // Either way, the telemetry line below is still emitted so the display
  // keeps showing voltage (and 0 rpm / --:-- pace). Start/Stop controls time.
  if (lastPulseUs != 0 && (nowUs - lastPulseUs) > SPIN_TIMEOUT_US) {
    rpmEma = 0.0f;
    rpmValid = false;
    periodMs = 0.0f;
    intervalCount = 0; // stale intervals must not survive the restart
  }

  // Drag model: absorbed power -> erg-standard speed -> integrated distance.
  // P = k*ω³, v = cbrt(P/2.8)
  float speedMs = powf(flywheelPower(rpmEma) / POWER_TO_SPEED_K,
                       1.0f / 3.0f);
  // Pace in seconds per 500 m. 0 means "not moving" for the display.
  float paceS = (speedMs > 0.05f) ? PACE_DISTANCE_M / speedMs : 0.0f;

  // Distance integrates the drag-model speed, so it always agrees with the
  // pace display.
  if (sessionStarted && lastDistanceUs != 0) {
    float dtS = (nowUs - lastDistanceUs) / 1000000.0f;
    distanceM += speedMs * dtS;
  }
  lastDistanceUs = nowUs;

  // Telemetry line (CSV, easy to parse on the CYD):
  //   rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts
  // period_ms is the magnet pulse interval; the full revolution period is
  // 8x that. elapsed_s is controlled by Start/Stop. watts is the drag-model
  // power behind the pace (see flywheelPower()).
  float elapsedS = (sessionStarted ? millis() - sessionStartMs : sessionElapsedMs) / 1000.0f;
  float watts = flywheelPower(rpmEma);
  TelemetryLine line = {};
  char values[72];
  snprintf(values, sizeof(values), "%.1f,%.1f,%.1f,%.3f,%.1f,%.1f,%.0f\n",
           rpmEma, paceS, distanceM, lastVolt, periodMs, elapsedS, watts);
  snprintf(line.text, sizeof(line.text), "session_state,%d\n%s", sessionStarted ? 1 : 0, values);
  // Mirror exactly the formatted Bluetooth fields, with the update timestamp
  // so the recorder can align telemetry with raw Hall samples. USB writes
  // stay in this task to prevent telemetry and sample lines interleaving.
  Serial.print("telemetry,");
  Serial.print(nowUs);
  Serial.print(',');
  Serial.print(values);
  if (telemetryQueue != nullptr) {
    xQueueOverwrite(telemetryQueue, &line);
  }
}
