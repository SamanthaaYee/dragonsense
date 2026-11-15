/*
 * Perg Stat Display (ESP32 CYD)
 *
 * Receives telemetry lines over Bluetooth SPP from the ESP32 running
 * perg_bt_logger.ino or perg_full_logger.ino every 100 ms (10 Hz):
 *
 *   rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts,stroke_rate_spm
 *
 * and shows:
 *   - RPM and pace (mm:ss per 500 m) as big values
 *   - distance (m), stroke rate (SPM), peak watts (max over the last
 *     1.25 s), elapsed time (mm:ss), watts, and the instantaneous Hall
 *     voltage (bottom right, 3 decimals)
 *
 * This board is the Bluetooth slave: it advertises as "ESP32-BT-Slave" and
 * waits for perg_bt_logger.ino (the master) to connect to its MAC
 * A4:F0:0F:8D:32:7E. The CYD must be running this sketch BEFORE the logger
 * boots, because the logger retries connecting until it succeeds.
 *
 * Display pin definitions are the working ones from the previous WiFi/HTTP
 * version of this sketch. XPT2046 touch uses the CYD's separate SPI bus.
 * One Start/Stop button controls the updated full logger over Bluetooth.
 */

// ---------------- TFT_eSPI user setup ----------------
// #define USER_SETUP_LOADED 1
#define ILI9341_DRIVER
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

#define SPI_FREQUENCY 40000000
#include <TFT_eSPI.h>

#include "BluetoothSerial.h"
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>

BluetoothSerial SerialBT = BluetoothSerial();
TFT_eSPI tft = TFT_eSPI();
// TFT_eSPI defaults to VSPI even with the CYD's GPIO 12/13/14 pins.
// Always give touch the OTHER hardware bus so it cannot reroute the TFT pins.
#ifdef USE_HSPI_PORT
SPIClass touchSPI(VSPI);
#else
SPIClass touchSPI(HSPI);
#endif
XPT2046_Touchscreen touch(33, 36); // CS, IRQ; CLK=25, MISO=39, MOSI=32
int16_t touchCal[4]; // raw coordinates at screen (20,20) and (300,220)

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

char buf[112]; // accommodates stroke rate appended by the full logger

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

// Two targets also handle reversed axes after the display is rotated.
void calibrateTouch() {
  for (;;) {
    for (int i = 0; i < 2; i++) {
      tft.fillScreen(COL_BG);
      tft.setTextColor(COL_TEXT);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Touch calibration", 160, 95, 4);
      tft.drawString("Press and release the cross", 160, 125, 2);
      int x = i == 0 ? 20 : 300, y = i == 0 ? 20 : 220;
      tft.drawLine(x - 10, y, x + 10, y, TFT_YELLOW);
      tft.drawLine(x, y - 10, x, y + 10, TFT_YELLOW);
      while (touch.touched()) delay(20);
      long sumX = 0, sumY = 0;
      int count = 0;
      while (count < 3) {
        sumX = sumY = count = 0;
        while (!touch.touched()) delay(20);
        while (touch.touched()) {
          TS_Point p = touch.getPoint();
          sumX += p.x; sumY += p.y; count++;
          delay(20);
        }
      }
      touchCal[i * 2] = sumX / count;
      touchCal[i * 2 + 1] = sumY / count;
      delay(200);
    }
    if (abs(touchCal[2] - touchCal[0]) > 500 &&
        abs(touchCal[3] - touchCal[1]) > 500) break;
  }
  Preferences prefs;
  prefs.begin("perg-touch", false);
  prefs.putBytes("corners", touchCal, sizeof(touchCal));
  prefs.end();
}

void pollTouch() {
  static bool held = false;
  static unsigned long pressedMs = 0;
  bool down = touch.touched();
  if (!down) { held = false; pressedMs = 0; return; }
  if (held) return;
  if (pressedMs == 0) pressedMs = millis();
  if (millis() - pressedMs < 60) return;
  held = true; // one command per physical press, including a long hold
  TS_Point p = touch.getPoint();
  int x = map(p.x, touchCal[0], touchCal[2], 20, 300);
  int y = map(p.y, touchCal[1], touchCal[3], 20, 220);
  if (!sessionKnown || commandPending || x < BUTTON_X || x >= BUTTON_X + BUTTON_W ||
      y < BUTTON_Y || y >= BUTTON_Y + BUTTON_H) return;
  requestedRunning = !sessionRunning;
  commandPending = true;
  commandSentMs = millis();
  SerialBT.print(requestedRunning ? "START\n" : "STOP\n");
  drawButton(); // show WAIT until the logger confirms the authoritative state
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

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(3); // landscape, 180° from rotation 1 (upside down)
  touchSPI.begin(25, 39, 32, 33);
  touch.begin(touchSPI);
  touch.setRotation(3);
  Preferences prefs;
  prefs.begin("perg-touch", true);
  bool calibrated = prefs.getBytes("corners", touchCal, sizeof(touchCal)) == sizeof(touchCal);
  prefs.end();
  // Hold the touchscreen during reset to repeat calibration.
  delay(300);
  if (!calibrated || touch.touched()) calibrateTouch();

  drawStatic();
  tft.setTextColor(COL_TEXT);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Waiting for logger...", tft.width() / 2,
                 tft.height() / 2, 2);

  SerialBT.begin("ESP32-BT-Slave"); // slave: the logger connects to us
  Serial.println("Slave started. Waiting for master to connect...");
  Serial.println(SerialBT.getBtAddressString()); // A4:F0:0F:8D:32:7E
}

void handleLine() {
  int running;
  if (sscanf(buf, "session_state,%d", &running) == 1) {
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

void loop() {
  static size_t used = 0;
  static bool overflow = false;
  if (!SerialBT.hasClient()) {
    if (wasConnected) {
      wasConnected = sessionKnown = commandPending = false;
      used = 0; overflow = false;
      drawStatic();
      tft.setTextColor(COL_TEXT);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Waiting for logger...", 160, 120, 2);
    }
    return;
  }
  if (!wasConnected) drawStatic(); // clear the waiting banner on reconnect
  wasConnected = true;
  pollTouch();
  // Bound each read pass so partial Bluetooth lines cannot block touch input.
  for (int n = 0; n < 128 && SerialBT.available(); n++) {
    char c = SerialBT.read();
    if (c == '\n') {
      buf[used] = '\0';
      if (!overflow && used) handleLine();
      used = 0; overflow = false;
    } else if (c != '\r') {
      if (used < sizeof(buf) - 1) buf[used++] = c;
      else overflow = true;
    }
  }
  if (commandPending && millis() - commandSentMs > 1000) {
    SerialBT.print(requestedRunning ? "START\n" : "STOP\n");
    commandSentMs = millis();
  }
}
