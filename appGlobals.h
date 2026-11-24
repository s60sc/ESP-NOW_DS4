// App configuration 
//
// s60sc 2026

#include "Arduino.h"
#include <WiFi.h>

#pragma once

// Sender is standalone and hosts the USB connection to DS4, sending the input over ESP-NOW o the receiver
// Receiver hosts the user app, eg drone, that receives the controller input over ESP-NOW

//#define ESPNOW_SENDER // comment out for Receiver

#define USE_SECURE true // set to true to add encryption, false for plaintext

/*
 If using USE_SECURE true
 In Windows PowerShell administrator mode, run following command to generate own private 32 byte key 
 to clipboard and overwrite the #define below with the result 
 
$b = New-Object byte[] 32
$rng = [System.Security.Cryptography.RNGCryptoServiceProvider]::new()
$rng.GetBytes($b)
$rng.Dispose()
$h = $b | ForEach-Object { '0x{0:X2}' -f $_ }
"#define ESPNOW_PSK_BYTES { \`n  $($h[0..15] -join ', '), \`n  $($h[16..31] -join ', ') }" | Set-Clipboard

*/

#define ESPNOW_PSK_BYTES { \
  0xA9, 0x69, 0xA5, 0x22, 0x8B, 0xA4, 0xF6, 0xEA, 0x01, 0x67, 0xCE, 0x03, 0xD0, 0xFE, 0x87, 0x13, \
  0x3C, 0x23, 0x59, 0x22, 0xCC, 0x36, 0x5A, 0x8D, 0xAC, 0x97, 0x14, 0x01, 0x0A, 0x5D, 0xB0, 0xB9 }

#define AP_CHANNEL 1 // channels 1, 6, 11 are preferred
// Note that ESP-NOW does not need the AP_SSID, just the WiFi channel
#define AP_PASS ""   // enter an AP password if required for extra Receiver security, 
#define AP_HIDE 1    // AP SSID not broadcast, change to 0 if need to connect to Receiver AP

#ifdef ESPNOW_SENDER
#define LAMP_PIN 33  // usually 2 for onboard mono led, 48 for onboard WS2812
#define LAMP_TYPE 1 // type of lamp LED: 0 = mono led, 1 = WS2812 RGB
#else // receiver
#define LAMP_PIN 48
#define LAMP_TYPE 1 // type of lamp LED: 0 = mono led, 1 = WS2812 RGB
#endif
  
/*********************** leave below as is ***********************/

#ifdef ESPNOW_SENDER
 #define APP_MODE "Sender"
 #ifndef CONFIG_IDF_TARGET_ESP32S3
  #error "Must select ESP32S3 board for USB OTG"
 #endif
 #define INCLUDE_USBHOST true
#else
 #define APP_MODE "Receiver"
 #define ESPNOW_RECEIVER
 #define INCLUDE_USBHOST false
#endif
#define INCLUDE_DS4 true

#define APP_NAME "ESP-NOW_DS4"
#define APP_VER "1.0"

#define ESPNOW_STACK_SIZE (1024 * 3)
#define ESPNOW_PRI 4
#define LED_STACK_SIZE (1024 * 2)
#define LED_PRI 1
#define STACK_MEM MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT

#define HB_PING 0xF0
#define HB_PONG 0xF1

#define STANDALONE

// DS4 controller data used for ESP_NOW
struct AppControllerPacket {
  uint8_t command;   
  uint8_t button;    // button pressed identifier
  uint8_t state;     // 1 = pressed / active, 0 = released / inactive
  uint8_t trigger;   // trigger 0..255
  int8_t stickX;     // stick X, -128..0..127
  int8_t stickY;     // stick Y -128..0..127
};
using AppDataPacket = AppControllerPacket;

extern bool dbgVerbose;
extern bool appSetupDone;
extern int lampType;
extern int lampPin;

enum class DevStatus : uint8_t { NA1, STARTING, FAILURE, NA2, READY, NA3, DISCONNECTED, Count };  // device status shown by LED 
enum class Brightness : uint8_t  { Off = 0, Low, Medium, High, Full, Count }; //  Perceived brightness 25/50/75/100%
enum class LedType : uint8_t { Mono = 0, WS2812, RGB3Pin }; // Status LED type

void appControllerCommand(const AppControllerPacket &p);
bool appSetup();
void espNowCommand(const AppDataPacket &p);
bool espNowConnect();
void espNowLinkLost();
void espNowLinkRestored();
bool espNowPrep();
bool prepUSB();
void releaseDS4inputs();
void setLamp(DevStatus s);
void setLamp(DevStatus s, Brightness b);
void setLamp(uint8_t lampLevel);
void showControllerCommand(const AppControllerPacket &p);

#define LOG_SUBMIT(fmtStr, ...) printf(fmtStr, ##__VA_ARGS__)
  
#define LOG_RAW(fmt, ...) LOG_SUBMIT(fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) LOG_SUBMIT("[%s ERROR @ %s:%u] " fmt "\n", esp_log_system_timestamp(), pathToFileName(__FILE__), __LINE__, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) LOG_SUBMIT("[%s WARN %s] " fmt "\n", esp_log_system_timestamp(), __FUNCTION__, ##__VA_ARGS__)
#define LOG_INF(fmt, ...) LOG_SUBMIT("[%s %s] " fmt "\n", esp_log_system_timestamp(), __FUNCTION__, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) LOG_SUBMIT("[%s ### %s] " fmt "\n", esp_log_system_timestamp(), __FUNCTION__, ##__VA_ARGS__)
#define LOG_VRB(fmt, ...) do {if (dbgVerbose) LOG_SUBMIT("[%s VERBOSE %s] " fmt "\n", esp_log_system_timestamp(), __FUNCTION__, ##__VA_ARGS__);} while(0)
