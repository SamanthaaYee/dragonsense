/*
 * Combined Perg Hall and four-channel FlexiForce logger for a classic ESP32.
 * USB carries raw Hall samples, Perg telemetry, and tagged foot-force samples.
 * BLE carries the existing stats and Start/Stop protocol to the Freenove display.
 * See README.md in this folder for wiring and recording instructions.
 */
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include "PergMetrics.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Select ESP32 Dev Module for the combined sensor board."
#endif

// ---------------- Hall sensor ----------------
const int HALL_PIN = 27;                     // Perg signal (via 10 kΩ) -> ADC2_CH7
const unsigned long SAMPLE_PERIOD_US = 500; // 0.5 ms -> 2000 samples/s
const unsigned long BAUD = 460800;

// Calibration and streaming pulse/stroke analysis live in PergMetrics.h.
PergMetrics metrics;

// ---------------- FlexiForce ----------------
// Each sensor goes from 3.3 V to the ADC node, with 1 kΩ from the node to GND.
// Channels are L_bot, L_top, R_top, R_bot (physical sensors 4, 1, 3, 2).
struct FootSensor {
  uint8_t pin;
  float slope;
  float intercept;
};
constexpr FootSensor FOOT_SENSORS[] = {
  {14, 2.557f, 6.34f},
  {32, 2.706f, 4.01f},
  {33, 2.733f, 2.36f},
  {34, 2.577f, 6.82f},
};
constexpr unsigned FOOT_CHANNELS = sizeof(FOOT_SENSORS) / sizeof(FOOT_SENSORS[0]);
constexpr float FOOT_RESISTANCE_OHMS = 1000.0f;
constexpr float FOOT_SUPPLY_MV = 3300.0f;
constexpr float FOOT_MIN_CONDUCTANCE = 0.3f; // µS
constexpr unsigned FOOT_TARE_READS = 16;
constexpr unsigned FOOT_READS_PER_CHANNEL = 4;
constexpr unsigned FOOT_READS_PER_FRAME = FOOT_CHANNELS * FOOT_READS_PER_CHANNEL;
constexpr uint32_t FOOT_PERIOD_US = 10000; // target 100 frames/s
constexpr int32_t FOOT_READ_MARGIN_US = 200;

float footZeroMv[FOOT_CHANNELS];
uint32_t footSumMv[FOOT_CHANNELS];
unsigned footReadIndex = FOOT_READS_PER_FRAME;
uint32_t footFrameStartUs = 0;
uint32_t nextFootFrameUs = 0;

float footForceLb(unsigned channel) {
  const FootSensor &sensor = FOOT_SENSORS[channel];
  const float mv = (float)footSumMv[channel] / FOOT_READS_PER_CHANNEL - footZeroMv[channel];
  if (mv <= 0.0f) return 0.0f;
  const float conductance = 1e6f * mv / (FOOT_RESISTANCE_OHMS * (FOOT_SUPPLY_MV - mv));
  if (conductance < FOOT_MIN_CONDUCTANCE) return 0.0f;
  return sensor.slope * conductance + sensor.intercept;
}

void sampleFootSensors(uint32_t nowUs) {
  if (footReadIndex == FOOT_READS_PER_FRAME) {
    if ((int32_t)(nowUs - nextFootFrameUs) < 0) return;
    nextFootFrameUs += FOOT_PERIOD_US;
    if ((int32_t)(nowUs - nextFootFrameUs) >= 0) {
      nextFootFrameUs = nowUs + FOOT_PERIOD_US;
    }
    footFrameStartUs = nowUs;
    footReadIndex = 0;
    memset(footSumMv, 0, sizeof(footSumMv));
  }

  // Read one channel per Hall interval. Round-robin sampling puts each
  // channel's average near the same frame midpoint.
  const unsigned channel = footReadIndex % FOOT_CHANNELS;
  footSumMv[channel] += analogReadMilliVolts(FOOT_SENSORS[channel].pin);
  if (++footReadIndex != FOOT_READS_PER_FRAME) return;

  const uint32_t frameUs = footFrameStartUs + (nowUs - footFrameStartUs) / 2;
  const float leftBottom = footForceLb(0);
  const float leftTop = footForceLb(1);
  const float rightTop = footForceLb(2);
  const float rightBottom = footForceLb(3);
  const float balance = rightTop + rightBottom - leftTop - leftBottom;
  Serial.printf("foot,%lu,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                (unsigned long)frameUs, leftBottom, leftTop, rightTop, rightBottom, balance);
}

// ---------------- BLE display discovery ----------------
// Optional: enter the ACTUAL S3 BLE MAC printed by the display at startup.
// Empty selects the first display advertising our custom Perg service.
// A4:F0:0F:8D:32:7E belongs to the old Bluetooth Classic CYD, not this S3.
const char *DISPLAY_BLE_MAC = "";
const char *SERVICE_UUID = "be7a1000-7c2d-4e58-a608-dbea31094d61";
const char *TELEMETRY_UUID = "be7a1001-7c2d-4e58-a608-dbea31094d61";
const char *COMMAND_UUID = "be7a1002-7c2d-4e58-a608-dbea31094d61";

const unsigned long TELEMETRY_PERIOD_MS = 100; // 10 telemetry updates/s

// ---------------- State ----------------

// A one-item queue copies complete lines between tasks. The detector never
// waits for BLE; a slow/disconnected display gets the newest update.
struct TelemetryLine {
  char text[144];
};
QueueHandle_t telemetryQueue = nullptr;
QueueHandle_t commandQueue = nullptr;

// START/STOP notifications are short complete messages. Only queue commands;
// callbacks never write USB or mutate session state from another task.
void receiveCommand(BLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
  char action = length == 6 && memcmp(data, "START\n", 6) == 0 ? 'S' :
                length == 5 && memcmp(data, "STOP\n", 5) == 0 ? 'T' : 0;
  if (action && commandQueue) xQueueSend(commandQueue, &action, 0);
}

void bluetoothTask(void *) {
  BLEDevice::init("perg-final-sensor");
  BLEDevice::setMTU(185);
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  BLEClient *client = BLEDevice::createClient(); // reused across reconnects
  BLERemoteCharacteristic *telemetry = nullptr;
  for (;;) {
    if (!client->isConnected()) {
      telemetry = nullptr; // remote characteristics expire on reconnect
      BLEScanResults *results = scan->start(3, false);
      bool found = false;
      if (results) {
        for (int i = 0; i < results->getCount(); ++i) {
          BLEAdvertisedDevice device = results->getDevice(i);
          if (!device.haveServiceUUID() ||
              !device.isAdvertisingService(BLEUUID(SERVICE_UUID))) continue;
          if (DISPLAY_BLE_MAC[0] &&
              !device.getAddress().toString().equalsIgnoreCase(DISPLAY_BLE_MAC)) continue;
          // Preserve the advertised address type (public or random).
          found = client->connect(device.getAddress(), device.getAddressType(), 5000);
          if (found) break;
        }
      }
      scan->clearResults();
      if (!found) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      client->setMTU(185);
      BLERemoteService *service = client->getService(BLEUUID(SERVICE_UUID));
      BLERemoteCharacteristic *commands = service ? service->getCharacteristic(BLEUUID(COMMAND_UUID)) : nullptr;
      telemetry = service ? service->getCharacteristic(BLEUUID(TELEMETRY_UUID)) : nullptr;
      if (!telemetry || !telemetry->canWrite() || !commands || !commands->canNotify()) {
        client->disconnect();
        telemetry = nullptr;
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      commands->registerForNotify(receiveCommand);
    }
    TelemetryLine line;
    if (telemetry && xQueueReceive(telemetryQueue, &line, pdMS_TO_TICKS(10)) == pdTRUE) {
      size_t length = strlen(line.text);
      // Use the negotiated payload size; fall back to 20-byte packets at MTU 23.
      // Acknowledged writes keep chunks ordered and detect a broken transfer.
      size_t payload = client->getMTU() > 23 ? client->getMTU() - 3 : 20;
      for (size_t offset = 0; offset < length; offset += payload) {
        size_t count = length - offset;
        if (count > payload) count = payload;
        if (!client->isConnected() ||
            !telemetry->writeValue((uint8_t *)line.text + offset, count, true)) {
          client->disconnect(); // discard any partial line on the receiver
          break;
        }
      }
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
    footReadIndex = FOOT_READS_PER_FRAME;
    nextFootFrameUs = nowUs; // the first saved frame starts after the session marker
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
  Serial.setTxBufferSize(1024);
  Serial.begin(BAUD);

  // No pinMode(INPUT_PULLUP/PULLDOWN): pull resistors would distort the
  // analog signal. analogRead() configures the pin for the ADC itself.
  analogReadResolution(12);                   // 0-4095
  analogSetPinAttenuation(HALL_PIN, ADC_11db); // full range ~0-3.1 V
  for (const FootSensor &sensor : FOOT_SENSORS) {
    analogSetPinAttenuation(sensor.pin, ADC_11db);
  }
  delay(500);
  for (unsigned channel = 0; channel < FOOT_CHANNELS; ++channel) {
    uint32_t sumMv = 0;
    for (unsigned i = 0; i < FOOT_TARE_READS; ++i) {
      sumMv += analogReadMilliVolts(FOOT_SENSORS[channel].pin);
    }
    footZeroMv[channel] = (float)sumMv / FOOT_TARE_READS;
  }

  telemetryQueue = xQueueCreate(1, sizeof(TelemetryLine));
  commandQueue = xQueueCreate(8, sizeof(char));
  if (telemetryQueue != nullptr) {
    // Keep potentially blocking BLE calls out of the sampling loop.
    if (xTaskCreate(bluetoothTask, "perg-ble", 8192, nullptr, 1,
                    nullptr) != pdPASS) {
      vQueueDelete(telemetryQueue);
      telemetryQueue = nullptr;
    }
  }
  // USB logging remains available even if the BLE task cannot start.
  // USB carries raw sample CSV plus explicitly tagged telemetry CSV.

  nextSampleUs = micros();
  nextFootFrameUs = nextSampleUs;
  nextTelemetryMs = millis() + TELEMETRY_PERIOD_MS;
}

void loop() {
  // Session state belongs to this task; the BLE task only queues commands.
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

  const uint32_t footSampleUs = micros();
  if ((int32_t)(nextSampleUs - footSampleUs) >= FOOT_READ_MARGIN_US) {
    sampleFootSensors(footSampleUs);
  }
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
  // Mirror exactly the formatted BLE fields, with the update timestamp
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
