// FlexiForce x4 on ESP32 — calibrated output in lb.
// Circuit per sensor: 3V3 -> FlexiForce -> ADC pin -> R_FIXED -> GND.
// Open Serial Plotter at 115200 baud.

// Each sensor is labelled. Here are their numbers in the same order as the pins and labels: [4, 1, 3, 2]
const int pins[4] = {14, 32, 33, 34};
const char *labels[4] = {"L_bot", "L_top", "R_top", "R_bot"};

// Calibration (force = A * conductance + B), ordered to match sensors [4, 1, 3, 2].
const float A[4] = {2.557, 2.706, 2.733, 2.577};  // lb per µS
const float B[4] = {6.34,  4.01,  2.36,  6.82};   // lb

const float R_FIXED = 1000.0;  // ohms, pin-to-GND resistor
const float VCC_MV  = 3300.0;   // measured 3V3 pin voltage
const float G_MIN   = 0.3;      // µS; below this, report 0
const int   SAMPLES = 16;

float zeroMv[4];  // no-load reading, captured at startup

float readMv(int i) {
  uint32_t sum = 0;
  for (int k = 0; k < SAMPLES; k++) sum += analogReadMilliVolts(pins[i]);
  return (float)sum / SAMPLES;
}

float readLbs(int i) {
  float v = readMv(i) - zeroMv[i];               // remove the ESP32's ~0.1 V floor
  if (v <= 0) return 0.0;
  float G = 1e6 * v / (R_FIXED * (VCC_MV - v));  // conductance, µS
  if (G < G_MIN) return 0.0;
  return A[i] * G + B[i];
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  delay(500);
  for (int i = 0; i < 4; i++) zeroMv[i] = readMv(i);  // keep sensors unloaded during boot
}

void loop() {
  float balance = 0;  // right minus left: positive = right, negative = left
  for (int i = 0; i < 4; i++) {
    float lb = readLbs(i);
    balance += (i < 2) ? -lb : lb;  // indices 0,1 are left; 2,3 are right
    Serial.print(labels[i]);
    Serial.print(':');
    Serial.print(lb, 1);
    Serial.print(',');
  }
  Serial.print("balance:");
  Serial.println(balance, 1);
  delay(100);
}
