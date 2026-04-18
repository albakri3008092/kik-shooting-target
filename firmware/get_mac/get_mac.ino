// Utility sketch: prints the ESP32 MAC address over Serial.
// Flash to every board once, note the MAC, label the board.
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(100);
  Serial.println();
  Serial.println("=================================");
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("=================================");
  Serial.println("Label this board (Target1..15 or Central).");
}

void loop() {}
