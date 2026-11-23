# Freenove ESP32-S3 force display

1. Open `perg_freenove_force_display.ino` in Arduino IDE. Keep
   `build_opt.h` and `BleLineFramer.h` in the same folder and install
   TFT_eSPI by Bodmer. The sketch uses the Freenove FNK0104B display and touch
   configuration.
2. Select **ESP32S3 Dev Module**, **Flash Size → 16MB** (check your board),
   **Partition Scheme → Huge APP (3MB No OTA)** and **USB CDC On Boot → Enabled**.
   Upload to the display.
3. Upload the updated [combined sensor logger](../final_sensor_and_display_devboard/README.md)
   to the classic ESP32 Dev Module. Power both boards. The logger automatically
   connects to the display. Set `DISPLAY_BLE_MAC` in the logger if multiple
   displays are nearby.

Tap **START** to begin a session. After two detected catches, the graph shows
the latest drive, from the catch to the flywheel speed peak. The logger estimates
relative drive force from the Hall sensor's flywheel acceleration and the
drag inferred from that stroke's recovery. Each curve scales to its own peak
and stays visible until the next completed stroke or a new session.
The rectangles show live left/right foot load. Only the heavier side fills, with the fill width and color
proportional to the difference divided by the total force. Both stay empty
when the load is equal or zero.

The sketch draws the entire screen into a 320×240, 8-bit frame buffer, then
pushes the frame to the LCD. The logger still supports the
[original LCD sketch](<../Old Firmware/perg_freenove_esp32_s3_display/README.md>)
and the existing USB recordings.
