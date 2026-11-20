// FlexiForce sensor test: prints the raw ADC value (0-4095) of each sensor.
// 0 = no pressure, higher = more pressure. Open Serial Plotter at 115200 baud.

// Each sensor is labelled. Here are their numbers in the same order as the pins and labels: [4, 1, 3, 2]
const int pins[4] = {14, 32, 33, 34};
const char *labels[4] = {"L_bot", "L_top", "R_top", "R_bot"};

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // full 0-3.3 V range
}

void loop() {
  int balance = 0;  // right minus left: positive = right, negative = left
  for (int i = 0; i < 4; i++) {
    int v = analogRead(pins[i]);
    balance += (i < 2) ? -v : v;  // indices 0,1 are left; 2,3 are right
    Serial.print(labels[i]);
    Serial.print(':');
    Serial.print(v);
    Serial.print(',');
  }
  Serial.print("balance:");
  Serial.println(balance);
  delay(100);
}