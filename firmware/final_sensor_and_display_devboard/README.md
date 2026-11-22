# Final Sensor and Display Devboard

The new **classic ESP32 Dev Module** reads the Perg Hall signal and four
FlexiForce foot sensors. It sends the existing stats to the Freenove ESP32-S3
over BLE and streams Hall, telemetry, and foot-force data over USB on one clock.
The Freenove uses its [existing display firmware](../perg_freenove_esp32_s3_display/README.md).

## Wire the new sensor board

With power disconnected, transfer the circuits to these pins.

| Signal | Pin | Circuit |
| --- | --- | --- |
| Perg Hall signal | GPIO 27 | Perg 3.5 mm left terminal through the existing 10 kΩ series resistor |
| Left bottom, sensor 4 | GPIO 14 | FlexiForce divider |
| Left top, sensor 1 | GPIO 32 | FlexiForce divider |
| Right top, sensor 3 | GPIO 33 | FlexiForce divider |
| Right bottom, sensor 2 | GPIO 34 | FlexiForce divider |

Connect the Perg sleeve and all four dividers to the ESP32's GND. Use the new
board's **3.3 V** supply for the dividers. For each foot sensor, wire
**3.3 V → FlexiForce → GPIO junction → 1 kΩ → GND**. Use one 1 kΩ resistor per
sensor, matching the FlexiForce calibration in the current sketch.

Leave the Perg jack's right terminal disconnected. Keep Wi-Fi disabled because
GPIO 14 and GPIO 27 use ADC2. Boot with all four sensors unloaded so the sketch
can measure each channel's zero offset. Then check the readings with a known
load. The calibration coefficients are carried over from the previous board.

## Upload and connect

1. Plug the **new sensor board** in by USB and open
   `final_sensor_and_display_devboard.ino` in Arduino IDE. Keep `PergMetrics.h`
   beside it.
2. Select **ESP32 Dev Module**, **Partition Scheme → Huge APP (3MB No OTA)**,
   **Upload Speed → 115200**, and the new sensor board's USB port. Upload.
3. Power the Freenove display and turn off the previous BLE sensor logger so
   it releases the display. The new logger automatically finds the Perg BLE
   service. To target your display specifically, set `DISPLAY_BLE_MAC` to
   `"14:c1:9f:d0:d9:7d"` before uploading.
4. Confirm that the display shows the Perg stats and the **START** button.
   The display uses the existing stats layout. Foot-force samples are
   available in the USB recording.

## Record a test

From the repository root, install `requirements.txt` if needed and close
Arduino Serial Monitor. Use `python3 -m serial.tools.list_ports -v` to find the
new **sensor** board's USB port. Substitute that port for `/dev/cu.usbserial-XXX`.

```bash
python3 firmware/final_sensor_and_display_devboard/record_combined.py /dev/cu.usbserial-XXX
```

Enter a fresh test name. Wait for **Ready: tap START on the LCD**, tap
**Start**, and tap **Stop** to save and exit. Each test produces three files
in `data/live/<test-name>/`.

| File | Contents |
| --- | --- |
| `perg_data.csv` | Raw Hall ADC samples |
| `perg_telemetry.csv` | RPM, pace, distance, Hall voltage, pulse interval, elapsed time, watts, and stroke rate |
| `foot_force.csv` | Four foot-force channels and right-minus-left balance, in pounds-force |

All three use the same microsecond clock relative to LCD Start. You can set
a different parent folder using `--output-dir`. For a bench capture without
the LCD, use `--start-immediately` and stop with Ctrl+C.

The sketch targets **2 kHz Hall**, **100 Hz foot force**, and **10 Hz display
telemetry**. Before using it for a trial, check that the four force channels
respond in the expected order, and inspect the CSV timestamp gaps with the
display connected. The foot sampler yields to the Hall timing, so its
actual rate may be lower under load.
