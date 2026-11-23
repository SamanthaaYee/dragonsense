# Freenove ESP32-S3 CYD 2.8-inch Perg display

For the Freenove **FNK0104B**, 240×320 ILI9341 screen with FT6336U capacitive
touch. This is a separate BLE implementation. The existing CYD display and
Bluetooth Classic logger sketches remain unchanged.

## Upload the screen

1. Open `perg_freenove_esp32_s3_display.ino` in Arduino IDE. Keep `build_opt.h`
   and `BleLineFramer.h` in this same folder.
2. Install **TFT_eSPI by Bodmer** through Library Manager. BLE and Wire come
   with the Espressif ESP32 board package. No separate touch library is needed.
3. Select **ESP32S3 Dev Module**. The build was checked using ESP32 core
   **3.3.12**, **Flash Size: 16MB**, **Partition Scheme: Huge APP (3MB No OTA)**,
   and **USB CDC On Boot: Enabled**. Confirm the flash size against your board's
   markings. This sketch does not require PSRAM.
4. Select the screen's USB port and Upload. Use the programming USB connector
   and a data cable. If no port appears, hold BOOT, press and release RESET,
   release BOOT, then check the port list again.

The local `build_opt.h` applies Freenove's TFT pins, driver and fonts to both
the sketch and the TFT_eSPI library. **Keep your global TFT_eSPI configuration
as it is** so the old CYD continues to build with its existing settings.

After uploading, the waiting screen displays this board's **actual BLE MAC**.
It is also printed in Serial Monitor at **115200 baud**. Open Serial Monitor
and press RESET if the startup message was missed. The previously saved
`A4:F0:0F:8D:32:7E` belongs to the old CYD; it is only recorded as a reference,
and is not assigned to this board.

## Upload the matching logger

1. Open `../perg_full_logger_ble/perg_full_logger_ble.ino` for the **original
   sensor ESP32**. Keep its supplied `PergMetrics.h` alongside it.
2. Select **ESP32 Dev Module** and **Partition Scheme: Huge APP (3MB No OTA)**,
   then select the sensor board's port and Upload. USB recording remains at
   **460800 baud**, with the same CSV format and Start/Stop markers.
3. Power both boards. The logger scans for the custom Perg BLE service and
   connects automatically. It retries if the screen starts later or disconnects.

There is no need to enter a MAC when using one display. To select a particular
screen when several are nearby, put the address printed by the new screen in
`DISPLAY_BLE_MAC` in the **BLE logger** and upload that logger again. The address
is a peer selector, not a security or pairing mechanism.

The original `perg_full_logger` uses Bluetooth Classic and cannot communicate
directly with the S3. Upload the separate BLE logger for this screen, or use
the original logger and original CYD together.

## Behavior and checks

- Same RPM, recent pace per 500 m, distance, SPM, peak watts, elapsed time,
  watts and Hall voltage as the old display.
- The touch Start/Stop button sends BLE commands and waits for the logger's
  session-state acknowledgment. Unacknowledged commands retry every second.
- Receiving and reconnecting never draw from BLE callbacks. Complete lines
  are queued for the display loop, including when BLE splits a line across packets.
- Start/Stop is disabled while disconnected or after two seconds without telemetry.
- FT6336U uses native capacitive coordinates, with rotation 3 mapped to
  landscape. No resistive-touch calibration is used. Verify the button position
  and touch orientation on the physical board before starting a recording.
- The copied `PergMetrics.h` is identical to the original at creation; future
  calibration changes must be copied to this variant as well.

Builds and native framing tests can verify software structure, but the new
screen, touch orientation, radio connection and sustained sampling need a
physical check. No firmware was uploaded as part of creating these files.

Hardware references: [Freenove board documentation](https://docs.freenove.com/projects/fnk0104/en/latest/fnk0104/codes/MAIN/Freenove_ESP32S3_Display.html),
[Freenove TFT configurations](https://github.com/Freenove/Freenove_ESP32_S3_Display/tree/main/Libraries/FNK0104AB),
and [Freenove touch example](https://github.com/Freenove/Freenove_ESP32_S3_Display/blob/main/Tutorial_With_Touch/Sketches/Sketch_11.1_Touch/Sketch_11.1_Touch.ino).
