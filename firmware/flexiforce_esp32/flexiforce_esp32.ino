
const int sensor1Pin = 33;
const int sensor2Pin = 32;

void setup() {
  Serial.begin(115200);

  pinMode(sensor1Pin, INPUT);
  pinMode(sensor2Pin, INPUT);

  analogReadResolution(12);

  analogSetPinAttenuation(sensor1Pin, ADC_11db);
  analogSetPinAttenuation(sensor2Pin, ADC_11db);
}

void loop() {
  // Read raw ADC values
  int raw1 = analogRead(sensor1Pin);
  int raw2 = analogRead(sensor2Pin);

  // Read voltages
  float voltage1 =
      analogReadMilliVolts(sensor1Pin) / 1000.0;

  float voltage2 =
      analogReadMilliVolts(sensor2Pin) / 1000.0;

  // Print sensor 1
  Serial.print("GPIO 33: ADC=");
  Serial.print(raw1);

  Serial.print(" Voltage=");
  Serial.print(voltage1, 3);

  // Print sensor 2
  Serial.print(" V | GPIO 32: ADC=");
  Serial.print(raw2);

  Serial.print(" Voltage=");
  Serial.print(voltage2, 3);
  Serial.println(" V");

  delay(200);
}
