/*
 * Perg Stat Display (ESP32 CYD)
 *
 * Receives telemetry lines over Bluetooth SPP from the ESP32 running
 * perg_bt_logger.ino or perg_full_logger.ino every 100 ms (10 Hz):
 *
 *   rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts
 *
 * and shows:
 *   - RPM and pace (mm:ss per 500 m) as big values
 *   - distance (m), pulse period (ms), peak watts (max over the last
 *     1.25 s), elapsed time (mm:ss), watts, and the instantaneous Hall
 *     voltage (bottom right, 3 decimals)
 *
 * This board is the Bluetooth slave: it advertises as "ESP32-BT-Slave" and
 * waits for perg_bt_logger.ino (the master) to connect to its MAC
 * A4:F0:0F:8D:32:7E. The CYD must be running this sketch BEFORE the logger
 * boots, because the logger retries connecting until it succeeds.
 *
 * Display pin definitions are the working ones from the previous WiFi/HTTP
 * version of this sketch. Touch is not needed here and was removed to save
 * memory alongside Bluetooth Classic.
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

BluetoothSerial SerialBT = BluetoothSerial();
TFT_eSPI tft = TFT_eSPI();

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
// row 1: DISTANCE, PERIOD, PEAK      row 2: TIME, WATTS, VOLT (right-aligned)
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

char buf[72]; // matches the logger's telemetry line buffer

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

void drawStatic() {
  tft.fillScreen(COL_BG);

  tft.setTextColor(COL_DIM);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("PERG MONITOR", tft.width() / 2, TITLE_Y, 4);

  tft.setTextColor(COL_TEXT);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("RPM",  RPM_LABEL_X,  RPM_LABEL_Y, 2);
  tft.drawString("PACE", PACE_LABEL_X, PACE_LABEL_Y, 2);
  tft.drawString("DISTANCE", DIST_LABEL_X, DIST_LABEL_Y, 2);
  tft.drawString("PERIOD", PERIOD_LABEL_X, PERIOD_LABEL_Y, 2);
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

  drawStatic();
  tft.setTextColor(COL_TEXT);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Waiting for logger...", tft.width() / 2,
                 tft.height() / 2, 2);

  SerialBT.begin("ESP32-BT-Slave"); // slave: the logger connects to us
  Serial.println("Slave started. Waiting for master to connect...");
  Serial.println(SerialBT.getBtAddressString()); // A4:F0:0F:8D:32:7E
}

void loop() {
  if (!SerialBT.hasClient()) {
    // Show the waiting banner whenever the link is down.
    if (wasConnected) { // just lost the link -> redraw once
      wasConnected = false;
      drawStatic();
      tft.setTextColor(COL_TEXT);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Waiting for logger...", tft.width() / 2,
                     tft.height() / 2, 2);
    }
    return;
  }
  wasConnected = true;

  // Redraw for each incoming update: the reader sends every 100 ms.
  // Read one telemetry line:
  //   rpm,pace_s,distance_m,hall_v,period_ms,elapsed_s,watts
  if (!SerialBT.available()) return;
  int n = SerialBT.readBytesUntil('\n', buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';

  float rpm, paceS, distM, volt, periodMs, elapsedS, watts;
  if (sscanf(buf, "%f,%f,%f,%f,%f,%f,%f",
             &rpm, &paceS, &distM, &volt, &periodMs, &elapsedS,
             &watts) != 7) {
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
    int minutes = (int)(paceS / 60.0f);
    int seconds = (int)(paceS - 60.0f * minutes) + 0.5f;
    if (seconds >= 60) { // rounding pushed it over a minute
      minutes++;
      seconds = 0;
    }
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

  // Pulse period (median interval between detected magnet pulses)
  if (periodMs > 0) {
    snprintf(text, sizeof(text), "%.1f ms", periodMs);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  drawValueAt(PERIOD_X, PERIOD_Y, PERIOD_W, text);

  // Elapsed time since the logger booted (mm:ss)
  int emin = (int)(elapsedS / 60.0f);
  int esec = (int)(elapsedS - 60.0f * emin);
  snprintf(text, sizeof(text), "%d:%02d", emin, esec);
  drawValueAt(ELAPSED_X, ELAPSED_Y, ELAPSED_W, text);

  // Watts (drag-model power behind the pace)
  snprintf(text, sizeof(text), "%.0f W", watts);
  drawValueAt(WATTS_X, WATTS_Y, WATTS_W, text);

  // Peak watts over the last 1.25 seconds
  snprintf(text, sizeof(text), "%.0f W", peakWatts);
  drawValueAt(PEAK_X, PEAK_Y, PEAK_W, text);

  // Hall voltage, bottom right, 3 decimals
  snprintf(text, sizeof(text), "%.3f V", volt);
  drawValueRight(VOLT_ALIGN_X, VOLT_ALIGN_Y, VOLT_ERASE_W, text);
}
