// ESP-NOW Sender or Receiver
// 
// Sender device must support USB OTG eg ESP32S3
//
// Start Receiver device.
// On Sender device:
// - connect power / serial monitor to USB UART socket
// - connect game controller to ESP32S3 USB OTG socket
// - start controller in DS4 mode and wait for steady Home light
// - press device reset button
//
// s60sc 2026

#include "appGlobals.h"

void setup() {
  Serial.begin(115200);
  appSetup();
}

void loop() {
  vTaskDelete(NULL); // if loop task not required
}

