/*
DS4 controller USB input

  Requires ESP32-S3 with USB host
  ESP32S3 board needs to supply 5V on the USB OTG Vbus\
  Freenove ESP32-S3-WROOM Board does, others don't
  
  Compile with USB Mode: "USB-OTG (TinyUSB)"

  Controller needs to operate in DS4 mode, outputs eg:
    "Sony Computer Entertainment Wireless Controller addr=1 vid=0x054c pid=0x09cc"
  XBox / XInput does not work, outputs eg:
    "2.4G XBOX 360 For Windows addr=2 vid=0x3537 pid=0x100f"

  HID data by byte number per line for DS4 style controller:
  0: 01 (report ID)
  1: left thumbstick X axis 00 - FF default 80
  2: left thumbstick Y axis 00 - FF default 7F
  3: right thumbstick X axis 00 - FF default 80
  4: right thumbstick Y axis 00 - FF default 7F
  5: default value   8 (bit 4 from LSB) = none
     D pad           00 - 07 (8 directions)
     XABY pad        bit 5, 6, 7, 8      [Square/Cross/Circle/Triangle]
  6: LB/RB/LT/RT     bit 1, 2, 3, 4      [L1/R1/L2/R2]
     view/menu       bit 5, 6            [Share/Options]
     L3/R3           bit 7, 8            [L3/R3]
  7: Touchpad
  8: LT analog       00 - FF
  9: RT analog       00 - FF
  10+ gyro, accelerometer, battery, etc

  Polling rate up to 1kHz

  Following buttons do not appear in HID data:
  - M(acro) - halts HID output
  - Home (PS)
  - Screenshot

  s60sc 2026
*/

#include "appGlobals.h"

#if INCLUDE_USBHOST
#if __has_include("../libraries/EspUsbHost/src/EspUsbHost.h")
#include "EspUsbHost.h"

EspUsbHost usb;

static void processDS4Report(const uint8_t* r);
static void releaseAll();
static void showHID(const uint8_t* r);

bool prepUSB() {
  // USB connection to DS4 controller
  LOG_INF("Prep OTG USB interface");
  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &deviceInfo) {
    LOG_INF("%s %s addr=%u vid=0x%04x pid=0x%04x",
            deviceInfo.manufacturer, deviceInfo.product,
            deviceInfo.address, deviceInfo.vid, deviceInfo.pid);
  });

  usb.onHIDInput([](const EspUsbHostHIDInput &input) {
    if (dbgVerbose) showHID(input.data);
    processDS4Report(input.data);
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    LOG_WRN("USB disconnected");
    releaseAll();
  });

  EspUsbHostConfig hostConfig;
  hostConfig.port = ESP_USB_HOST_PORT_FULL_SPEED;
  if (!usb.begin(hostConfig)) {
    LOG_WRN("USB connection failure: %s", usb.lastErrorName());
    return false;
  }
  return true;
}

#else
#error "Need to install EspUsbHost library"
#endif // has_include
#else
  bool prepUSB() {return true;} // dummy
#endif // INCLUDE_USBHOST


#if INCLUDE_DS4

#include <bitset>

#define STICK_DEADBAND       2
#define TRIGGER_THRESHOLD    2
#define ANALOG_SEND_INTERVAL 20    // ms, 50 Hz
#define LAST_BYTE            10

enum Command : uint8_t {

  CMD_NONE = 0,

  CMD_FORWARD,
  CMD_BACK,
  CMD_LEFT,
  CMD_RIGHT,

  CMD_UP,
  CMD_DOWN,

  CMD_ACTION_1,
  CMD_ACTION_2,
  CMD_ACTION_3,
  CMD_ACTION_4,

  CMD_L1,
  CMD_R1,
  CMD_L2,
  CMD_R2,

  CMD_SHARE,
  CMD_OPTIONS,
  CMD_L3,
  CMD_R3,

  CMD_LEFT_STICK,
  CMD_RIGHT_STICK,

  CMD_COUNT  // keep last, not sent
};

// Byte 5 D-pad (low nibble)
#define DPAD_UP         0x00
#define DPAD_UP_RIGHT   0x01
#define DPAD_RIGHT      0x02
#define DPAD_DOWN_RIGHT 0x03
#define DPAD_DOWN       0x04
#define DPAD_DOWN_LEFT  0x05
#define DPAD_LEFT       0x06
#define DPAD_UP_LEFT    0x07
#define DPAD_NONE       0x08

// Byte 5 face buttons
#define SQUARE_X        0x10
#define CROSS_A         0x20
#define CIRCLE_B        0x40
#define TRIANGLE_Y      0x80

// Byte 6 action buttons
#define L1_LB           0x01
#define R1_RB           0x02
#define L2_LT           0x04
#define R2_RT           0x08
#define SHARE_VIEW      0x10
#define OPTIONS_MENU    0x20
#define L3              0x40
#define R3              0x80

// One table row per input: DS4 mask, display name, generic command.
// Used by the sender to decode reports and by the receiver to print names.
struct InputMap {
  uint8_t     mask;
  const char *name;
  Command     command;
};

static const InputMap faceMap[] = {            // report byte 5, upper nibble
  { SQUARE_X,     "Square/X",     CMD_ACTION_1 },
  { CROSS_A,      "Cross/A",      CMD_FORWARD  },
  { CIRCLE_B,     "Circle/B",     CMD_ACTION_2 },
  { TRIANGLE_Y,   "Triangle/Y",   CMD_ACTION_3 },
};

static const InputMap btnMap[] = {             // report byte 6
  { L1_LB,        "L1/LB",        CMD_L1       },
  { R1_RB,        "R1/RB",        CMD_R1       },
  { L2_LT,        "L2/LT",        CMD_L2       },
  { R2_RT,        "R2/RT",        CMD_R2       },
  { SHARE_VIEW,   "Share/View",   CMD_SHARE    },
  { OPTIONS_MENU, "Options/Menu", CMD_OPTIONS  },
  { L3,           "L3",           CMD_L3       },
  { R3,           "R3",           CMD_R3       },
};

static const InputMap dpadMap[] = {            // mask = bit in dpadMask()
  { 0x1,          "D-pad UP",     CMD_UP       },
  { 0x2,          "D-pad RIGHT",  CMD_RIGHT    },
  { 0x4,          "D-pad DOWN",   CMD_DOWN     },
  { 0x8,          "D-pad LEFT",   CMD_LEFT     },
};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static const char* commandName(uint8_t c) {
  for (size_t i = 0; i < COUNT_OF(faceMap); i++) if (faceMap[i].command == c) return faceMap[i].name;
  for (size_t i = 0; i < COUNT_OF(btnMap);  i++) if (btnMap[i].command  == c) return btnMap[i].name;
  for (size_t i = 0; i < COUNT_OF(dpadMap); i++) if (dpadMap[i].command == c) return dpadMap[i].name;
  switch (c) {
    case CMD_LEFT_STICK:  return "Left stick";
    case CMD_RIGHT_STICK: return "Right stick";
    case CMD_NONE:   return "Heartbeat";
    default:              return "?";
  }
}

// DS4 report layout. r[0] = report ID, so data starts at r[1].
#define R_LX      1
#define R_LY      2
#define R_RX      3
#define R_RY      4
#define R_BTN_A   5   // D-pad + face buttons
#define R_BTN_B   6   // shoulders, triggers (digital), share/options, L3/R3
#define R_L2      8
#define R_R2      9

static uint32_t lastAnalogSend   = 0;

// Latest raw stick positions 
static volatile uint8_t curLX = 128;
static volatile uint8_t curLY = 128;
static volatile uint8_t curRX = 128;
static volatile uint8_t curRY = 128;
static volatile uint8_t curL2 = 0;
static volatile uint8_t curR2 = 0;

// Previous button / trigger values (edge detection)
static uint8_t oldBtnA = DPAD_NONE;
static uint8_t oldBtnB = 0;
static uint8_t oldL2   = 0;
static uint8_t oldR2   = 0;

static bool leftStickActive  = false;
static bool rightStickActive = false;

// Convert D-pad value to a bitmask: 1=up 2=right 4=down 8=left
static uint8_t dpadMask(uint8_t value) {
  switch (value & 0x0F) {
    case DPAD_UP:         return 0x1;
    case DPAD_UP_RIGHT:   return 0x1 | 0x2;
    case DPAD_RIGHT:      return 0x2;
    case DPAD_DOWN_RIGHT: return 0x4 | 0x2;
    case DPAD_DOWN:       return 0x4;
    case DPAD_DOWN_LEFT:  return 0x4 | 0x8;
    case DPAD_LEFT:       return 0x8;
    case DPAD_UP_LEFT:    return 0x1 | 0x8;
    default:              return 0;
  }
}

static void processDpad(uint8_t oldValue, uint8_t newValue) {
  uint8_t oldMask = dpadMask(oldValue);
  uint8_t newMask = dpadMask(newValue);
  uint8_t changed = oldMask ^ newMask;
  if (!changed) return;

  AppControllerPacket packet;
  packet.command = 'F';
  packet.state   = 1;
  packet.trigger = 0;
  packet.stickX  = 0;
  packet.stickY  = 0;

  // Releases first, then presses (diagonals are treated as two directions)
  for (size_t i = 0; i < COUNT_OF(dpadMap); i++) {
    uint8_t bit = dpadMap[i].mask;
    packet.button = dpadMap[i].command;
    packet.state = 0;
    if ((changed & bit) && !(newMask & bit)) appControllerCommand(packet);
  }
  for (size_t i = 0; i < COUNT_OF(dpadMap); i++) {
    uint8_t bit = dpadMap[i].mask;
    packet.button = dpadMap[i].command;
    packet.state = 1;
    if ((changed & bit) && (newMask & bit)) appControllerCommand(packet);
  }
}

static void processButton(uint8_t oldValue, uint8_t newValue, uint8_t mask, Command command) {
  if ((oldValue ^ newValue) & mask) {
    bool pressed = newValue & mask;
    AppControllerPacket packet;
    packet.command = 'F';
    packet.button = command;
    packet.state   = pressed ? 1 : 0;
    packet.trigger = 0;
    packet.stickX  = 0;
    packet.stickY  = 0;
    appControllerCommand(packet);
  }
}

static int8_t stickValue(uint8_t value) {
  // Convert stick value to -128 ... +127, with deadband
  int8_t v = value - 128;
  return (abs(v) < STICK_DEADBAND) ? 0 : v;
}

static void sendAnalog() {
  // Send analog stick and trigger value changes at a specified frequency
  uint32_t now = millis();
  if (now - lastAnalogSend < ANALOG_SEND_INTERVAL) return;
  lastAnalogSend = now;
  AppControllerPacket packet;
  packet.command = 'F';

  // Left stick
  int8_t lx = stickValue(curLX);
  int8_t ly = stickValue(curLY);
  bool active = (lx != 0 || ly != 0);
  if (active || leftStickActive) {
    packet.button = CMD_LEFT_STICK;
    packet.state   = active ? 1 : 0;
    packet.trigger = 0;
    packet.stickX  = lx;
    packet.stickY  = ly;
    appControllerCommand(packet);
  }
  leftStickActive = active;

  // Right stick
  int8_t rx = stickValue(curRX);
  int8_t ry = stickValue(curRY);
  active = (rx != 0 || ry != 0);
  if (active || rightStickActive)  {
    packet.button = CMD_RIGHT_STICK;
    packet.state   = active ? 1 : 0;
    packet.trigger = 0;
    packet.stickX  = rx;
    packet.stickY  = ry;
    appControllerCommand(packet);
  }
  rightStickActive = active;
  
  // Left trigger
  uint8_t l2 = curL2;
  if (abs((int)l2 - (int)oldL2) >= TRIGGER_THRESHOLD || (l2 == 0 && oldL2 != 0)) {
    packet.button = CMD_L2;
    packet.state   = l2 ? 1 : 0;
    packet.trigger = l2;
    packet.stickX  = 0;
    packet.stickY  = 0;
    appControllerCommand(packet);
    oldL2 = l2;
  }
  // Right trigger
  uint8_t r2 = curR2;
  if (abs((int)r2 - (int)oldR2) >= TRIGGER_THRESHOLD || (r2 == 0 && oldR2 != 0)) {
    packet.button = CMD_R2;
    packet.state   = r2 ? 1 : 0;
    packet.trigger = r2;
    packet.stickX  = 0;
    packet.stickY  = 0;
    appControllerCommand(packet);
    oldR2 = r2;
  }
}

static void processDS4Report(const uint8_t* r) {
  // process HID input received from game controller
  
  // Save latest stick and trigger positions
  curLX = r[R_LX];
  curLY = r[R_LY];
  curRX = r[R_RX];
  curRY = r[R_RY];
  curL2 = r[R_L2];
  curR2 = r[R_R2];
  sendAnalog(); // send at a fixed rate instead of instantly

  // D-pad and face buttons
  uint8_t a = r[R_BTN_A];
  processDpad(oldBtnA, a);
  for (size_t i = 0; i < COUNT_OF(faceMap); i++) processButton(oldBtnA, a, faceMap[i].mask, faceMap[i].command);
  oldBtnA = a;

  // Other buttons
  uint8_t b = r[R_BTN_B];
  for (size_t i = 0; i < COUNT_OF(btnMap); i++) {
    if (btnMap[i].command == CMD_L2 || btnMap[i].command == CMD_R2) continue;
    processButton(oldBtnB, b, btnMap[i].mask, btnMap[i].command);
  }
  oldBtnB = b;

}

// On disconnect, feed a neutral report so every held input is released
static void releaseAll() {
  uint8_t neutral[LAST_BYTE] = {0};
  neutral[0]       = 0x01;
  neutral[R_LX]    = 128;
  neutral[R_LY]    = 128;
  neutral[R_RX]    = 128;
  neutral[R_RY]    = 128;
  neutral[R_BTN_A] = DPAD_NONE;
  processDS4Report(neutral);
}

static const char* dpadName(uint8_t d) {
  switch (d & 0x0F) {
    case DPAD_UP:         return "UP";
    case DPAD_UP_RIGHT:   return "UP-RIGHT";
    case DPAD_RIGHT:      return "RIGHT";
    case DPAD_DOWN_RIGHT: return "DOWN-RIGHT";
    case DPAD_DOWN:       return "DOWN";
    case DPAD_DOWN_LEFT:  return "DOWN-LEFT";
    case DPAD_LEFT:       return "LEFT";
    case DPAD_UP_LEFT:    return "UP-LEFT";
    default:              return "NONE";
  }
}

// Append the name of each button whose bit is set in value
static void appendButtons(String &out, uint8_t value, const InputMap *map, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (value & map[i].mask) {
      if (out.length()) out += ", ";
      out += map[i].name;
    }
  }
}

static void showHID(const uint8_t* r) {
  // Raw bytes (bytes 5 and 6 in binary)
  if (dbgVerbose) {
    LOG_RAW("HID: ");
    for (size_t i = 1; i < LAST_BYTE; i++) {
      if (i == R_BTN_A || i == R_BTN_B) LOG_RAW("%s ", std::bitset<8>(r[i]).to_string().c_str());
      else LOG_RAW("%02X ", r[i]);
    }
    LOG_RAW(" \n");

    // Decoded names and values
    String face, btns;
    appendButtons(face, r[R_BTN_A], faceMap, COUNT_OF(faceMap));
    appendButtons(btns, r[R_BTN_B], btnMap,  COUNT_OF(btnMap));

    LOG_RAW("  Left stick  X=%3u Y=%3u\n", r[R_LX], r[R_LY]);
    LOG_RAW("  Right stick X=%3u Y=%3u\n", r[R_RX], r[R_RY]);
    LOG_RAW("  D-pad: %s (0x%02X)\n", dpadName(r[R_BTN_A]), r[R_BTN_A] & 0x0F);
    LOG_RAW("  Face buttons: %s\n", face.length() ? face.c_str() : "none");
    LOG_RAW("  Buttons: %s\n", btns.length() ? btns.c_str() : "none");
    LOG_RAW("  L2 analog=%3u  R2 analog=%3u\n", r[R_L2], r[R_R2]);
    delay(2000);
  }
}

void showControllerCommand(const AppControllerPacket &p) {
  // display name of received command and values
  if (p.button > CMD_NONE) {
    bool analog = (p.button == CMD_LEFT_STICK || p.button == CMD_RIGHT_STICK || p.button == CMD_L2 || p.button == CMD_R2);
    if (analog) {
      if (p.button >= CMD_LEFT_STICK) LOG_RAW("%-12s stickX=%d stickY=%d\n", commandName(p.button), p.stickX, p.stickY);
      else LOG_RAW("%-12s state=%u trigger=%u\n", commandName(p.button), p.state, p.trigger);
    } else LOG_RAW("%-12s %s\n", commandName(p.button), p.state ? "PRESSED" : "released");
  }
}

void releaseDS4inputs() {
  // Release everything in case active
  AppControllerPacket p = {'F', 0, 0, 0, 0, 0};
  for (uint8_t c = CMD_FORWARD; c < CMD_COUNT; c++) {
    p.button = c;
    appControllerCommand(p);
  }
}

#endif // INCLUDE_DS4