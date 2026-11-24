//
// Application handlers to be tailored for user's receiver application
//
// s60sc 2026

#include "appGlobals.h"

bool dbgVerbose = false; // true for verbose log output (slow)
bool appSetupDone = false;

bool appSetup() {
  LOG_INF("Starting %s %s v%s", APP_NAME, APP_VER, APP_MODE);
  lampType = LAMP_TYPE;
  lampPin = LAMP_PIN;
  setLamp(DevStatus::STARTING);
#ifdef ESPNOW_SENDER
  prepUSB();
  delay(1000);
#endif
  // start wifi to open channel
  WiFi.AP.begin();
  WiFi.AP.create(APP_MODE, AP_PASS, AP_CHANNEL, AP_HIDE);
  // start ESP-NOW service
  if (espNowPrep()) {
    // wait until connection has started or a failure occurs
    LOG_INF("Wait for ESP-NOW connection");
    setLamp(DevStatus::DISCONNECTED);
    if (espNowConnect()) {
#ifdef ESPNOW_SENDER
      setLamp(DevStatus::READY);
      LOG_INF("%s %s connected", APP_NAME, APP_MODE);
#endif
      appSetupDone = true;
      return true;
    }
  }
  setLamp(DevStatus::FAILURE);
  return false;
}

void espNowLinkLost() {
  // Called when no packet (including heartbeat) arrived before HEARTBEAT_TIMEOUT.
  setLamp(DevStatus::DISCONNECTED);
  LOG_WRN("ESP-NOW link down");
  releaseDS4inputs();
}

void espNowLinkRestored() {
  setLamp(DevStatus::READY);
  LOG_INF("ESP-NOW link up");
}

void appControllerCommand(const AppControllerPacket &p) {
  // receive button command for processing
#ifdef ESPNOW_SENDER
  showControllerCommand(p); // optional
  espNowCommand(p);
#else
  // add Receiver specific command handler for inputs received from Sender
  showControllerCommand(p); // optional
#endif
}

