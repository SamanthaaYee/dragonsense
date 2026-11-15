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
 *   telemetry,time_us,rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts,stroke_rate_spm
 *
 * Telemetry line (CSV, one every 100 ms):
 *   rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts,stroke_rate_spm
 * where hall_v is the instantaneous Hall voltage, period_ms is the magnet
 * pulse interval (the flywheel-revolution period is 8x that), elapsed_s is
 * the time since the LCD Start button, frozen by Stop.
 * USB session_start,time_us and session_stop,time_us markers control capture.
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
 * Calibration: perg_rpm_stroke_rate_v2.xlsx (drag 61). Pulse crossings
 * are interpolated at ADC 1820, armed below 1670. Centered RPM/stroke
 * windows run with 0.45 s latency. Distance counts turns at 0.2481 m/rev;
 * power cubes the trailing 1.5 s mean angular speed. Pace measures distance
 * gained / time over whole turns spanning about 1.5 s, in seconds per 500 m.
 */

#include "BluetoothSerial.h"
#include "PergMetrics.h"
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

// Calibration and streaming pulse/stroke analysis live in PergMetrics.h.
PergMetrics metrics;

// ---------------- Bluetooth ----------------
const char *BT_LOCAL_NAME = "perg-logger";
// Bluetooth MAC of the CYD running perg_stat_display.ino.
uint8_t displayMac[6] = {0xA4, 0xF0, 0x0F, 0x8D, 0x32, 0x7E};

const unsigned long TELEMETRY_PERIOD_MS = 100; // 10 telemetry updates/s

// ---------------- State ----------------
BluetoothSerial SerialBT;

// A one-item queue copies complete lines between tasks. The detector never
// waits for Bluetooth; a slow/disconnected display gets the newest update.
struct TelemetryLine {
  char text[144];
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

// Hall voltage, same calibration as perg_logger.ino: deviation in volts
// from the signal's ~1.64 V bias point (3.6 V across the 12-bit range,
// bias at count 1870).
const float VOLT_FULL_SCALE = 3.6f;
const float VOLT_BIAS_COUNTS = 1870.0f;
float lastVolt = 0.0f;

float distanceM = 0.0f;           // accepted magnet intervals x metres per pulse
uint32_t sessionPulseOrigin = 0;
bool sessionStarted = false;
unsigned long sessionStartMs = 0;
unsigned long sessionElapsedMs = 0;

void sendTelemetry(unsigned long nowUs);

void controlSession(char action) {
  unsigned long nowUs = micros();
  if (action == 'S' && !sessionStarted) {
    sessionStarted = true;
    sessionStartMs = millis();
    sessionElapsedMs = 0;
    distanceM = 0;
    sessionPulseOrigin = metrics.pulses;
    Serial.print("session_start,");
    Serial.println(nowUs);
    sendTelemetry(nowUs);
  } else if (action == 'T' && sessionStarted) {
    // Report the final pulse-count distance before freezing the session.
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
    sessionPulseOrigin = metrics.pulses;
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

  metrics.sample((uint32_t)now, adc);
}

void sendTelemetry(unsigned long nowUs) {
  metrics.tick((uint32_t)nowUs);
  if (sessionStarted) {
    distanceM = (uint32_t)(metrics.pulses - sessionPulseOrigin) *
                (PergMetrics::METRES_PER_REV / 8.0f);
  }
  float paceS = metrics.pace((uint32_t)nowUs);
  float elapsedS = (sessionStarted ? millis() - sessionStartMs : sessionElapsedMs) / 1000.0f;
  float watts = metrics.watts((uint32_t)nowUs);
  // Append stroke rate; keep the original seven fields in their existing order.
  TelemetryLine line = {};
  char values[112];
  snprintf(values, sizeof(values), "%.1f,%.1f,%.1f,%.3f,%.1f,%.1f,%.0f,%.1f\n",
           metrics.rpm, paceS, distanceM, lastVolt, metrics.periodMs, elapsedS,
           watts, metrics.strokeRate);
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
