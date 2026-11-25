# FlexiForce sensors on an ESP32

Open `flexiforce_esp32.ino` in Arduino IDE with the Espressif ESP32 board package installed. Select **ESP32 Dev Module** for a classic ESP32 board and upload the sketch. Open Serial Monitor at **115200 baud**. The sketch prints the 12-bit ADC count and measured voltage for GPIO 33 and GPIO 32 every 200 ms.

## Wiring

| Physical sensor | ESP32 ADC1 pin | Reported 5 V calibration factor (likely lbf/V) |
| --- | --- | --- |
| 1 | GPIO 32 | 68.8 |
| 3 | GPIO 33 | 67.1 |
| 4 | Disconnected | 74.5 |

For each sensor, connect `3V3 → FlexiForce → signal junction → 10 kΩ → GND` and wire the signal junction to its assigned GPIO. Each sensor has its own resistor and signal junction. Keep the sensor dividers on the ESP32's **3V3** rail. A 5 V divider can exceed the ESP32 GPIO voltage limit. At this ADC setting, the documented measurement range is roughly 0.15–3.1 V. [Espressif ADC documentation](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html)

## Calibration

The factors in the wiring table are historical 5 V calibration values with provisional units. The current sketch reports ADC counts and voltage for checking the sensor circuits. Apply a known load to each sensor to verify its response.
