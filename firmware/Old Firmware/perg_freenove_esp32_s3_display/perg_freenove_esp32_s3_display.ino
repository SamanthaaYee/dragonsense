/*
 * Freenove ESP32-S3 CYD 2.8-inch display (FNK0104B, ILI9341 + FT6336U).
 * Standalone BLE variant: use with perg_full_logger_ble on the sensor ESP32.
 * The existing perg_stat_display and perg_full_logger sketches are untouched.
 * Select ESP32S3 Dev Module; keep build_opt.h alongside this file so TFT_eSPI
 * receives the Freenove settings without editing its global User_Setup.h.
 * The board's ACTUAL BLE MAC is printed on USB at 115200 baud after startup.
 * The old CYD address A4:F0:0F:8D:32:7E is not this board's address.
 * Hardware settings: Freenove/Freenove_ESP32_S3_Display,
 * Libraries/FNK0104AB/TFT_eSPI_Setups_v1.3.zip and Sketch_11.1_Touch.
 */
#include <Arduino.h>
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select ESP32S3 Dev Module for the Freenove screen."
#endif
#if !defined(USER_SETUP_LOADED) || TFT_MOSI != 11 || TFT_DC != 46
#error "Keep the supplied build_opt.h in this sketch folder."
#endif
#include <TFT_eSPI.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <math.h>
#include "BleLineFramer.h"

// Custom Perg service; matching UUIDs are in perg_full_logger_ble.
const char *SERVICE_UUID = "be7a1000-7c2d-4e58-a608-dbea31094d61";
const char *TELEMETRY_UUID = "be7a1001-7c2d-4e58-a608-dbea31094d61";
const char *COMMAND_UUID = "be7a1002-7c2d-4e58-a608-dbea31094d61";
TFT_eSPI tft;
BLECharacteristic *commandCharacteristic = nullptr;
std::atomic<bool> linkConnected{false};
std::atomic<bool> advertisePending{false};
std::atomic<uint32_t> linkGeneration{0};
struct ReceivedLine { uint32_t generation; char text[112]; };
QueueHandle_t receivedLines = nullptr;
BleLineFramer framer; // only the BLE callback task accesses this instance

class DisplayServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    framer.reset();
    linkGeneration.fetch_add(1);
    linkConnected.store(true);
  }
  void onDisconnect(BLEServer *) override {
    framer.reset();
    linkConnected.store(false);
    linkGeneration.fetch_add(1);
    advertisePending.store(true);
  }
};
class TelemetryCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String packet = characteristic->getValue();
    framer.feed((const uint8_t *)packet.c_str(), packet.length(), [](const char *line) {
      ReceivedLine item = {};
      item.generation = linkGeneration.load();
      strlcpy(item.text, line, sizeof(item.text));
      // Never render or wait in the BLE callback. Repeated telemetry recovers
      // a dropped update if the main loop was busy drawing the screen.
      if (receivedLines) xQueueSend(receivedLines, &item, 0);
    });
  }
};
DisplayServerCallbacks serverCallbacks;
TelemetryCallbacks telemetryCallbacks;

// Native FT6336U capacitive-touch coordinates; no resistive calibration.
const uint8_t TOUCH_ADDR = 0x38;
bool touchReady = false;
void initTouch() {
  Wire.begin(16, 15, 400000); // Freenove SDA, SCL
  Wire.setTimeOut(20);
  pinMode(17, INPUT); // interrupt
  pinMode(18, OUTPUT); // touch reset
  digitalWrite(18, LOW); delay(10);
  digitalWrite(18, HIGH); delay(200);
  Wire.beginTransmission(TOUCH_ADDR);
  touchReady = Wire.endTransmission() == 0;
  Serial.println(touchReady ? "FT6336U detected" : "FT6336U not detected; check board model");
}
bool readTouch(int &x, int &y) {
  if (!touchReady) return false;
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02); // TD_STATUS, P1_XH, P1_XL, P1_YH, P1_YL
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(TOUCH_ADDR, (uint8_t)5) != 5)
    return false;
  uint8_t count = Wire.read() & 0x0f;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (!count || count > 2 || (xh >> 6) == 1) return false; // release event
  int nativeX = ((xh & 0x0f) << 8) | xl;
  int nativeY = ((yh & 0x0f) << 8) | yl;
  if (nativeX >= 240 || nativeY >= 320) return false;
  // ILI9341 rotation 3: portrait (x,y) -> landscape (319-y,x).
  x = 319 - nativeY;
  y = nativeX;
  return true;
}
void sendCommand(bool running) {
  if (!linkConnected.load() || !commandCharacteristic) return;
  commandCharacteristic->setValue(running ? "START\n" : "STOP\n");
  commandCharacteristic->notify();
}

// ---------------- Colors ----------------
#define COL_BG    TFT_BLACK
#define COL_TEXT  TFT_WHITE
#define COL_VALUE TFT_GREENYELLOW
#define COL_DIM   TFT_DARKGREY

// ---------------- Layout (320x240 landscape) ----------------
// Title
const int TITLE_Y = 8;
// Big values: RPM left, PACE right (font 7: 7-segment digits + ':')
const int RPM_LABEL_X = 20,  RPM_LABEL_Y = 40;
const int RPM_X = 20,  RPM_Y = 58, RPM_W = 130, RPM_H = 60;
const int PACE_LABEL_X = 180, PACE_LABEL_Y = 40;
const int PACE_X = 180, PACE_Y = 58, PACE_W = 120, PACE_H = 60;

// Bottom strip, two rows of (label font 2, value font 4):
// row 1: DISTANCE, SPM, PEAK      row 2: TIME, WATTS, VOLT (right-aligned)
const int DIST_LABEL_X = 20,   DIST_LABEL_Y = 128;
const int DIST_X = 20,         DIST_Y = 144, DIST_W = 110;
const int PERIOD_LABEL_X = 130, PERIOD_LABEL_Y = 128;
const int PERIOD_X = 130,      PERIOD_Y = 144, PERIOD_W = 105;
const int PEAK_LABEL_X = 240,  PEAK_LABEL_Y = 128;
const int PEAK_X = 240,        PEAK_Y = 144, PEAK_W = 80;
const int ELAPSED_LABEL_X = 20, ELAPSED_LABEL_Y = 176;
const int ELAPSED_X = 20,      ELAPSED_Y = 192, ELAPSED_W = 75;
const int WATTS_LABEL_X = 130, WATTS_LABEL_Y = 176;
const int WATTS_X = 130,       WATTS_Y = 192, WATTS_W = 75;
// Voltage hugs the bottom-right corner: region 210..310, text at x=310 TR.
const int VOLT_LABEL_X = 310,  VOLT_LABEL_Y = 176;
const int VOLT_ERASE_X = 210,  VOLT_ERASE_Y = 192, VOLT_ERASE_W = 100;
const int VOLT_ALIGN_X = 310,  VOLT_ALIGN_Y = 192;

char buf[112];

// Keep enough readings for the 1.25 s peak window at 100 ms updates.
// Use timestamps so slower/delayed packets do not stretch the window.
const unsigned long TELEMETRY_PERIOD_MS = 100; // matches the reader
const unsigned long WATTS_WINDOW_MS = 1250;
const int WATTS_WINDOW_N = WATTS_WINDOW_MS / TELEMETRY_PERIOD_MS + 2;
float wattsWindow[WATTS_WINDOW_N]; // zero-initialized
unsigned long wattsTimes[WATTS_WINDOW_N];
int wattsWinCount = 0;
int wattsWinIdx = 0;

// Link-state tracking: true once the logger has connected (so a lost link can
// trigger the "Waiting..." banner redraw exactly once).
bool wasConnected = false;
bool sessionRunning = false;
bool sessionKnown = false;
bool commandPending = false;
bool requestedRunning = false;
unsigned long commandSentMs = 0;
const int BUTTON_X = 208, BUTTON_Y = 2, BUTTON_W = 108, BUTTON_H = 34;

void drawButton() {
  uint16_t color = !sessionKnown ? COL_DIM : sessionRunning ? TFT_RED : TFT_DARKGREEN;
  tft.fillRoundRect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, 6, color);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, color);
  tft.drawString(!sessionKnown ? "CONNECTING" : commandPending ? "WAIT..." :
                 sessionRunning ? "STOP" : "START",
                 BUTTON_X + BUTTON_W / 2, BUTTON_Y + BUTTON_H / 2, 2);
}

void drawStatic() {
  tft.fillScreen(COL_BG);

  tft.setTextColor(COL_DIM);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("PERG", 20, TITLE_Y, 4);
  drawButton();

  tft.setTextColor(COL_TEXT);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("RPM",  RPM_LABEL_X,  RPM_LABEL_Y, 2);
  tft.drawString("PACE /500m", PACE_LABEL_X, PACE_LABEL_Y, 2);
  tft.drawString("DISTANCE", DIST_LABEL_X, DIST_LABEL_Y, 2);
  tft.drawString("SPM", PERIOD_LABEL_X, PERIOD_LABEL_Y, 2);
  tft.drawString("PEAK", PEAK_LABEL_X, PEAK_LABEL_Y, 2);
  tft.drawString("TIME", ELAPSED_LABEL_X, ELAPSED_LABEL_Y, 2);
  tft.drawString("WATTS", WATTS_LABEL_X, WATTS_LABEL_Y, 2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("VOLT", VOLT_LABEL_X, VOLT_LABEL_Y, 2);
}

// Big 7-segment value (digits + ':' only), erased with its fillRect box.
void drawBig(int x, int y, int w, int h, const char *text) {
  tft.setTextColor(COL_VALUE, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.fillRect(x, y, w, h, COL_BG);
  tft.drawString(text, x, y + h / 2 - 16, 7);
}

// Small font-4 value, erased with a fixed-width box before drawing.
void drawValueAt(int x, int y, int w, const char *text) {
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.fillRect(x, y, w, 26, COL_BG);
  tft.drawString(text, x, y, 4);
}

// Right-aligned variant for the voltage in the bottom-right corner.
void drawValueRight(int rightX, int y, int eraseW, const char *text) {
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(TR_DATUM);
  tft.fillRect(rightX - eraseW, y, eraseW, 26, COL_BG);
  tft.drawString(text, rightX, y, 4);
}

void handleLine() {
  int running;
  if (sscanf(buf, "session_state,%d", &running) == 1) {
    if (running != 0 && running != 1) return;
    bool changed = !sessionKnown || sessionRunning != (running == 1);
    sessionRunning = running == 1;
    sessionKnown = true;
    if (changed && sessionRunning) wattsWinCount = wattsWinIdx = 0;
    if (commandPending && sessionRunning == requestedRunning) commandPending = false;
    drawButton();
    return;
  }

  float rpm, paceS, distM, volt, periodMs, elapsedS, watts, strokeRate = 0;
  int fields = sscanf(buf, "%f,%f,%f,%f,%f,%f,%f,%f",
             &rpm, &paceS, &distM, &volt, &periodMs, &elapsedS,
             &watts, &strokeRate);
  if (fields < 7) {
    Serial.print("Bad telemetry: ");
    Serial.println(buf);
    return;
  }

  float numbers[] = {rpm, paceS, distM, volt, periodMs, elapsedS, watts, strokeRate};
  for (float value : numbers) if (!isfinite(value)) return;
  if (rpm < 0 || rpm > 2000 || paceS < 0 || paceS > 5999 || distM < 0 ||
      elapsedS < 0 || watts < 0 || strokeRate < 0) return;

  // Peak watts over the last 1.25 s, independent of packet timing.
  unsigned long nowMs = millis();
  wattsWindow[wattsWinIdx] = watts;
  wattsTimes[wattsWinIdx] = nowMs;
  wattsWinIdx = (wattsWinIdx + 1) % WATTS_WINDOW_N;
  if (wattsWinCount < WATTS_WINDOW_N) wattsWinCount++;
  float peakWatts = 0.0f;
  for (int i = 0; i < wattsWinCount; i++) {
    if ((nowMs - wattsTimes[i]) <= WATTS_WINDOW_MS &&
        wattsWindow[i] > peakWatts) {
      peakWatts = wattsWindow[i];
    }
  }

  // RPM (integer, font 7)
  char text[16];
  snprintf(text, sizeof(text), "%d", (int)(rpm + 0.5f));
  drawBig(RPM_X, RPM_Y, RPM_W, RPM_H, text);

  // Pace as mm:ss per 500 m. paceS == 0 means "not moving" -> "--:--"
  // (basic font, because the 7-segment font has no '-' glyph).
  if (paceS > 0) {
    int rounded = (int)(paceS + 0.5f);
    int minutes = rounded / 60;
    int seconds = rounded % 60;
    snprintf(text, sizeof(text), "%d:%02d", minutes, seconds);
    drawBig(PACE_X, PACE_Y, PACE_W, PACE_H, text);
  } else {
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.fillRect(PACE_X, PACE_Y, PACE_W, PACE_H, COL_BG);
    tft.drawString("--:--", PACE_X, PACE_Y + PACE_H / 2 - 8, 4);
  }

  // Distance
  snprintf(text, sizeof(text), "%.1f m", distM);
  drawValueAt(DIST_X, DIST_Y, DIST_W, text);

  // Calibrated catch-to-catch rate; older seven-field loggers have no SPM.
  if (fields == 8 && strokeRate > 0) {
    snprintf(text, sizeof(text), "%.0f", strokeRate);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  drawValueAt(PERIOD_X, PERIOD_Y, PERIOD_W, text);

  // Session elapsed time supplied by the logger (mm:ss)
  int emin = (int)(elapsedS / 60.0f);
  int esec = (int)(elapsedS - 60.0f * emin);
  snprintf(text, sizeof(text), "%d:%02d", emin, esec);
  drawValueAt(ELAPSED_X, ELAPSED_Y, ELAPSED_W, text);

  // Watts (cube of the trailing average angular speed)
  snprintf(text, sizeof(text), "%.0f W", watts);
  drawValueAt(WATTS_X, WATTS_Y, WATTS_W, text);

  // Peak watts over the last 1.25 seconds
  snprintf(text, sizeof(text), "%.0f W", peakWatts);
  drawValueAt(PEAK_X, PEAK_Y, PEAK_W, text);

  // Hall voltage, bottom right, 3 decimals
  snprintf(text, sizeof(text), "%.3f V", volt);
  drawValueRight(VOLT_ALIGN_X, VOLT_ALIGN_Y, VOLT_ERASE_W, text);
}


void pollTouch() {
  static bool held = false;
  static uint32_t pressedMs = 0;
  int x, y;
  bool down = readTouch(x, y);
  if (!down) { held = false; pressedMs = 0; return; }
  if (held) return;
  if (!pressedMs) pressedMs = millis();
  if (millis() - pressedMs < 60) return;
  held = true;
  if (!sessionKnown || commandPending || x < BUTTON_X || x >= BUTTON_X + BUTTON_W ||
      y < BUTTON_Y || y >= BUTTON_Y + BUTTON_H) return;
  requestedRunning = !sessionRunning;
  commandPending = true;
  commandSentMs = millis();
  sendCommand(requestedRunning);
  drawButton();
}

void showWaiting() {
  drawStatic();
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Waiting for BLE logger...", 160, 115, 2);
  tft.drawString(BLEDevice::getAddress().toString().c_str(), 160, 139, 2);
}

void setup() {
  Serial.begin(115200); // USB CDC if enabled; no wait for a serial monitor
  tft.init();
  tft.setRotation(3);
  initTouch();
  receivedLines = xQueueCreate(12, sizeof(ReceivedLine));
  if (!receivedLines) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.drawString("Receive queue failed; reset", 10, 100, 2);
    for (;;) delay(1000);
  }
  BLEDevice::init("perg-freenove-s3");
  BLEDevice::setMTU(185);
  Serial.print("Freenove display BLE MAC: ");
  Serial.println(BLEDevice::getAddress().toString().c_str());
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);
  BLEService *service = server->createService(SERVICE_UUID);
  BLECharacteristic *telemetry = service->createCharacteristic(
      TELEMETRY_UUID, BLECharacteristic::PROPERTY_WRITE);
  telemetry->setCallbacks(&telemetryCallbacks);
  commandCharacteristic = service->createCharacteristic(
      COMMAND_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  commandCharacteristic->addDescriptor(new BLE2902());
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
  showWaiting(); // includes the address, even without Serial Monitor
}

void loop() {
  static uint32_t observedGeneration = 0;
  static uint32_t lastReceivedMs = 0;
  static uint32_t lastTouchMs = 0;
  uint32_t generation = linkGeneration.load();
  if (generation != observedGeneration) {
    observedGeneration = generation;
    sessionKnown = commandPending = false;
    wattsWinCount = wattsWinIdx = 0;
    lastReceivedMs = millis();
    showWaiting();
  }
  if (advertisePending.exchange(false)) BLEDevice::startAdvertising();
  if (linkConnected.load()) {
    if (millis() - lastTouchMs >= 15) {
      lastTouchMs = millis();
      pollTouch();
    }
    ReceivedLine line;
    for (int n = 0; n < 6 && xQueueReceive(receivedLines, &line, 0) == pdTRUE; ++n) {
      if (line.generation != observedGeneration) continue;
      if (!sessionKnown) drawStatic(); // clear waiting banner before the first state
      strlcpy(buf, line.text, sizeof(buf));
      handleLine();
      lastReceivedMs = millis();
    }
    if (sessionKnown && millis() - lastReceivedMs > 2000) {
      sessionKnown = commandPending = false;
      showWaiting(); // disable Start/Stop if telemetry stops without disconnecting
    }
    if (sessionKnown && commandPending && millis() - commandSentMs > 1000) {
      sendCommand(requestedRunning);
      commandSentMs = millis();
    }
  }
  delay(1);
}
