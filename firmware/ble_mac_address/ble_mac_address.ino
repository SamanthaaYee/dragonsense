#include <Arduino.h>
#include <esp_mac.h>

// For the ESP32-S3 native USB port, select Hardware CDC and JTAG under
// Tools > USB Mode and enable Tools > USB CDC On Boot.

void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.println("Start of loop");
  // Read the local Bluetooth interface's public MAC address.
  uint8_t mac[6];
  const esp_err_t result = esp_read_mac(mac, ESP_MAC_BT);

  if (result == ESP_OK) {
    Serial.printf("BLE MAC address: " MACSTR "\n", MAC2STR(mac));
  } else {
    Serial.printf("Could not read Bluetooth MAC address: %s\n", esp_err_to_name(result));
  }

  delay(2000);
}
