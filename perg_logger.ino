/*
 * Perg Hall Sensor Logger (ESP32)
 *
 * Samples the Perg's Hall-effect sensor as a raw analog waveform and streams
 * it over USB serial as CSV: time_us,adc_value
 *
 * Wiring:
 *   Perg 3.5 mm L (signal) --[10 kΩ]-- GPIO 27
 *   Perg 3.5 mm GND        ----------- ESP32 GND
 *   (R terminal is not used)
 *
 * GPIO 27:
 *   Used as an analog input (ADC2, channel 7). The ESP32 reads the voltage on
 *   the pin and converts it to a 12-bit number: 0 = 0 V, 4095 = ~3.1 V at 11 dB
 *   attenuation. The 10 kΩ series resistor limits current into the pin.
 *   Note: ADC2 pins cannot be read while Wi-Fi is on. This sketch never
 *   enables Wi-Fi, so that's fine here. If you add Wi-Fi later, move the signal
 *   to an ADC1 pin (GPIO 32-39).
 *
 * Why ADC instead of digitalRead():
 *   The multimeter shows the Hall output as a small AC signal (~0.6 VAC,
 *   50-70 Hz), not a clean 0/3.3 V square wave. A digital input only reports
 *   HIGH/LOW around a ~1.6 V threshold, so a signal this small might never
 *   register, or it could toggle unpredictably. Reading the ADC captures the
 *   real shape (amplitude, peaks, noise), and that shape tells us how to design
 *   the signal-conditioning circuit later.
 *
 * How the data represents the waveform:
 *   Each magnet on the flywheel (8 total) passing the Hall sensor produces one
 *   pulse/cycle in the voltage. Sampling every 1 ms (1000 Hz) gives ~15-20
 *   points per cycle at 50-70 Hz, which is enough to find peak times. The
 *   time between peaks (Δt) gives f = 1/Δt, and RPM = f / 8 * 60.
 *
 * Limitation: the ESP32 ADC cannot measure negative voltages. If the signal
 * swings below 0 V, the negative half reads as 0 (clipped). Peaks are still
 * detectable from the positive half. A DC-bias circuit can fix this later.
 */

const int HALL_PIN = 27;                  // Perg signal (via 10 kΩ) -> ADC2_CH7
const unsigned long SAMPLE_PERIOD_US = 1000;  // 1 ms -> 1000 samples/s

// 115200 baud is too slow for this stream: each line is ~14 bytes x 1000/s ≈
// 14 kB/s, but 115200 baud carries only ~11.5 kB/s. At 115200 the serial
// buffer fills and samples arrive in bursts with ~11 ms gaps. The Python
// script must use the same value. (This is separate from Tools > Upload Speed,
// which only affects flashing and can stay at 115200.)
const unsigned long BAUD = 921600;

unsigned long nextSampleUs;

void setup() {
  Serial.begin(BAUD);

  // No pinMode(INPUT_PULLUP/PULLDOWN): pull resistors would distort the analog
  // signal. analogRead() configures the pin for the ADC itself.
  analogReadResolution(12);                  // 0-4095
  analogSetPinAttenuation(HALL_PIN, ADC_11db);  // full range ~0-3.1 V

  nextSampleUs = micros();
}

void loop() {
  // Schedule off a fixed timeline instead of delay(1), so the per-sample
  // overhead (ADC read + serial print) doesn't add drift.
  unsigned long now = micros();
  if ((long)(now - nextSampleUs) < 0) {
    return;
  }
  nextSampleUs += SAMPLE_PERIOD_US;

  // If we fell more than one period behind (e.g. serial stalled), skip ahead
  // instead of firing a burst of back-to-back catch-up samples.
  if ((long)(now - nextSampleUs) >= 0) {
    nextSampleUs = now + SAMPLE_PERIOD_US;
  }

  int adc = analogRead(HALL_PIN);

  // CSV line: time_us,adc_value
  // time_us is taken when the sample is read, so it stays accurate even if
  // timing jitters slightly.
  Serial.print(now);
  Serial.print(',');
  Serial.println(adc);
}
