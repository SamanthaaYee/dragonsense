// FlexiForce sensor test: prints the raw ADC value (0-4095) of each sensor.
// 0 = no pressure, higher = more pressure. Open Serial Plotter at 115200 baud.

const int pins[4] = {14, 32, 33, 34};
const char *labels[4] = {"L_top", "L_bot", "R_top", "R_bot"};

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // full 0-3.3 V range
}

void loop() {
  for (int i = 0; i < 4; i++) {
    Serial.print(labels[i]);
    Serial.print(':');
    Serial.print(analogRead(pins[i]));
    Serial.print(i < 3 ? ',' : '\n');
  }
  delay(100);
}