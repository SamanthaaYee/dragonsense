// Freenove FNK0104B force display for final_sensor_and_display_devboard.
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

// The combined sensor logger selects this display's force characteristic.
const char *SERVICE_UUID = "be7a1000-7c2d-4e58-a608-dbea31094d61";
const char *FORCE_TELEMETRY_UUID = "be7a1003-7c2d-4e58-a608-dbea31094d61";
const char *COMMAND_UUID = "be7a1002-7c2d-4e58-a608-dbea31094d61";
TFT_eSPI tft;
TFT_eSprite frame(&tft);
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

constexpr int BUTTON_X = 214, BUTTON_Y = 5, BUTTON_W = 96, BUTTON_H = 28;
constexpr int CURVE_POINTS = 32; // Matches StrokeForce::POINTS in the logger.
constexpr uint16_t BG = 0x10A3;
constexpr uint16_t PANEL = 0x21A6;
constexpr uint16_t MUTED = 0xB5D7;
constexpr uint16_t ACCENT = TFT_CYAN;

struct Stats {
  float rpm, pace, distance, elapsed, watts, spm, balance;
};
Stats stats = {};
uint8_t curve[CURVE_POINTS];
bool curveReady = false;
bool sessionKnown = false, sessionRunning = false;
bool commandPending = false, requestedRunning = false;
bool redraw = true;
uint32_t commandSentMs = 0;
String displayAddress;

void drawText(const char *text, int x, int y, int font, uint16_t color = TFT_WHITE) {
  frame.setTextColor(color);
  frame.drawString(text, x, y, font);
}

void drawCurve() {
  constexpr int X = 10, Y = 58, W = 180, H = 74;
  constexpr int LEFT = X + 6, RIGHT = X + W - 7, BASE = Y + H - 8;
  constexpr int HEIGHT = H - 27;
  drawText("STROKE FORCE (EST.)", X, 39, 2, MUTED);
  frame.fillRect(X, Y, W, H, PANEL);
  if (!curveReady) {
    drawText("Waiting for a full stroke", X + 12, Y + 29, 2, MUTED);
    return;
  }

  frame.drawFastHLine(LEFT, BASE, RIGHT - LEFT + 1, MUTED);
  frame.drawFastHLine(LEFT, BASE - HEIGHT / 2, RIGHT - LEFT + 1, TFT_DARKGREY);
  frame.setTextDatum(TR_DATUM);
  drawText("Relative force", RIGHT, Y + 2, 2, MUTED);
  frame.setTextDatum(TL_DATUM);
  for (int i = 1; i < CURVE_POINTS; ++i) {
    const int x0 = LEFT + (RIGHT - LEFT) * (i - 1) / (CURVE_POINTS - 1);
    const int x1 = LEFT + (RIGHT - LEFT) * i / (CURVE_POINTS - 1);
    const int y0 = BASE - HEIGHT * curve[i - 1] / 255;
    const int y1 = BASE - HEIGHT * curve[i] / 255;
    frame.drawLine(x0, y0, x1, y1, ACCENT);
  }
}

void drawBalanceBar(int x, float excess, bool left) {
  constexpr int Y = 155, W = 87, H = 30;
  const int fill = (int)((W - 2) * excess / 100.0f + 0.5f);
  const uint16_t color = tft.color565(80 + 175 * excess / 100.0f,
                                     44 - 44 * excess / 100.0f,
                                     48 - 48 * excess / 100.0f);
  frame.fillRect(x, Y, W, H, PANEL);
  frame.drawRect(x, Y, W, H, MUTED);
  if (fill > 0) frame.fillRect(left ? x + W - 1 - fill : x + 1, Y + 1, fill, H - 2, color);
  frame.setTextDatum(MC_DATUM);
  drawText(left ? "L" : "R", x + W / 2, Y + H / 2, 2);
  frame.setTextDatum(TL_DATUM);
}

void drawMetric(const char *label, const char *value, int x, int y) {
  drawText(label, x, y, 2, MUTED);
  drawText(value, x, y + 16, 4);
}

void formatTime(char *text, size_t size, uint32_t seconds) {
  snprintf(text, size, "%lu:%02lu", (unsigned long)(seconds / 60),
           (unsigned long)(seconds % 60));
}

void drawDashboard() {
  frame.fillSprite(BG);
  frame.setTextDatum(TL_DATUM);
  drawText("PERG", 10, 7, 4);

  const uint16_t buttonColor = !sessionKnown || commandPending ? PANEL :
                               sessionRunning ? TFT_MAROON : TFT_DARKGREEN;
  frame.fillRoundRect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, 5, buttonColor);
  frame.setTextDatum(MC_DATUM);
  drawText(!sessionKnown ? "CONNECTING" : commandPending ? "WAIT..." :
           sessionRunning ? "STOP" : "START",
           BUTTON_X + BUTTON_W / 2, BUTTON_Y + BUTTON_H / 2, 2);
  frame.setTextDatum(TL_DATUM);
  frame.drawFastHLine(10, 35, 300, MUTED);

  if (!sessionKnown) {
    frame.setTextDatum(MC_DATUM);
    drawText("Waiting for sensor logger", 160, 105, 4);
    drawText(displayAddress.c_str(), 160, 136, 2, MUTED);
    frame.pushSprite(0, 0);
    return;
  }

  drawCurve();
  drawText("LIVE FOOT BALANCE", 10, 137, 2, MUTED);
  drawBalanceBar(10, stats.balance < 0 ? -stats.balance : 0, true);
  drawBalanceBar(103, stats.balance > 0 ? stats.balance : 0, false);

  frame.drawFastVLine(200, 41, 145, MUTED);
  char text[24];
  formatTime(text, sizeof(text), (uint32_t)stats.elapsed);
  drawMetric("TIME", text, 212, 41);
  if (stats.pace > 0) formatTime(text, sizeof(text), (uint32_t)(stats.pace + 0.5f));
  drawMetric("SPLIT / 500 m", stats.pace > 0 ? text : "--:--", 212, 89);
  snprintf(text, sizeof(text), "%.0f", stats.spm);
  drawMetric("STROKE RATE / SPM", stats.spm > 0 ? text : "--", 212, 137);

  frame.drawFastHLine(10, 191, 300, MUTED);
  snprintf(text, sizeof(text), "%.0f", stats.rpm);
  drawMetric("RPM", text, 10, 197);
  snprintf(text, sizeof(text), "%.0f m", stats.distance);
  drawMetric("DISTANCE", text, 104, 197);
  snprintf(text, sizeof(text), "%.0f W", stats.watts);
  drawMetric("POWER", text, 212, 197);
  frame.pushSprite(0, 0);
}

// A valid stats line also acknowledges the current Start/Stop state.
bool readStats(const char *line) {
  int running;
  Stats incoming;
  if (sscanf(line, "stats,%d,%f,%f,%f,%f,%f,%f,%f",
             &running, &incoming.rpm, &incoming.pace, &incoming.distance,
             &incoming.elapsed, &incoming.watts, &incoming.spm, &incoming.balance) != 8 ||
      (running != 0 && running != 1)) return false;

  const float values[] = {incoming.rpm, incoming.pace, incoming.distance,
                          incoming.elapsed, incoming.watts, incoming.spm};
  for (float value : values) if (!isfinite(value) || value < 0) return false;
  if (incoming.pace > 5999 || incoming.elapsed > 4294967 ||
      !isfinite(incoming.balance) || fabsf(incoming.balance) > 100) return false;
  if (!sessionRunning && running) curveReady = false;
  stats = incoming;
  sessionRunning = running == 1;
  sessionKnown = true;
  if (commandPending && sessionRunning == requestedRunning) commandPending = false;
  redraw = true;
  return true;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

void readCurve(const char *line) {
  if (strncmp(line, "drive,", 6) != 0) return;
  const char *points = line + 6;
  const size_t length = strlen(points);
  if (length == 0) {
    if (curveReady) redraw = true;
    curveReady = false;
    return;
  }
  if (length != 2 * CURVE_POINTS) return;

  uint8_t incoming[CURVE_POINTS];
  for (int i = 0; i < CURVE_POINTS; ++i) {
    const int high = hexDigit(points[2 * i]);
    const int low = hexDigit(points[2 * i + 1]);
    if (high < 0 || low < 0) return;
    incoming[i] = (high << 4) | low;
  }
  memcpy(curve, incoming, sizeof(curve));
  curveReady = redraw = true;
}

void pollTouch() {
  static bool held = false;
  static uint32_t pressedMs = 0;
  int x, y;
  if (!readTouch(x, y)) { held = false; pressedMs = 0; return; }
  if (held) return;
  if (!pressedMs) pressedMs = millis();
  if (millis() - pressedMs < 60) return;
  held = true;
  if (!sessionKnown || commandPending || x < BUTTON_X || x >= BUTTON_X + BUTTON_W ||
      y < BUTTON_Y || y >= BUTTON_Y + BUTTON_H) return;
  requestedRunning = !sessionRunning;
  commandPending = redraw = true;
  commandSentMs = millis();
  sendCommand(requestedRunning);
}

void setup() {
  Serial.begin(115200); // USB CDC if enabled; no wait for a serial monitor
  tft.init();
  tft.setRotation(3);
  // 320 x 240 x 8 bits fits without relying on PSRAM.
  frame.setColorDepth(8);
  if (!frame.createSprite(tft.width(), tft.height())) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.drawString("Frame buffer allocation failed", 10, 100, 2);
    for (;;) delay(1000);
  }
  initTouch();
  receivedLines = xQueueCreate(12, sizeof(ReceivedLine));
  if (!receivedLines) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.drawString("Receive queue failed; reset", 10, 100, 2);
    for (;;) delay(1000);
  }
  BLEDevice::init("perg-force-display");
  BLEDevice::setMTU(185);
  Serial.print("Freenove display BLE MAC: ");
  Serial.println(BLEDevice::getAddress().toString().c_str());
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);
  BLEService *service = server->createService(SERVICE_UUID);
  BLECharacteristic *telemetry = service->createCharacteristic(
      FORCE_TELEMETRY_UUID, BLECharacteristic::PROPERTY_WRITE);
  telemetry->setCallbacks(&telemetryCallbacks);
  commandCharacteristic = service->createCharacteristic(
      COMMAND_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  commandCharacteristic->addDescriptor(new BLE2902());
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
  displayAddress = BLEDevice::getAddress().toString();
}

void loop() {
  static uint32_t observedGeneration = 0, lastStatsMs = 0, lastTouchMs = 0, lastFrameMs = 0;
  const uint32_t now = millis();
  const uint32_t generation = linkGeneration.load();
  if (generation != observedGeneration) {
    observedGeneration = generation;
    sessionKnown = sessionRunning = commandPending = curveReady = false;
    redraw = true;
  }
  if (advertisePending.exchange(false)) BLEDevice::startAdvertising();

  if (linkConnected.load()) {
    ReceivedLine line;
    for (int n = 0; n < 6 && xQueueReceive(receivedLines, &line, 0) == pdTRUE; ++n) {
      if (line.generation != observedGeneration) continue;
      if (readStats(line.text)) lastStatsMs = now;
      else if (sessionKnown) readCurve(line.text);
    }
    if (sessionKnown && now - lastStatsMs > 2000) {
      sessionKnown = commandPending = curveReady = false;
      redraw = true;
    }
    if (now - lastTouchMs >= 15) {
      lastTouchMs = now;
      pollTouch();
    }
    if (commandPending && millis() - commandSentMs > 1000) {
      sendCommand(requestedRunning);
      commandSentMs = millis();
    }
  }

  if (redraw && now - lastFrameMs >= 50) {
    drawDashboard();
    redraw = false;
    lastFrameMs = now;
  }
  delay(1);
}
