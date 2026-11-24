/********************* LED Lamp Driver **********************/

// Single-colour (analogWrite), WS2812 / SK6812 (rgbLedWrite), 3-pin RGB (ledcWrite)
// DevStatus -> colour + blink, global perceptual brightness,
// Use the widest-separated colors (Red, Yellow, Green, Cyan, Blue, Magenta) for important states
// For single color LED the averages are Red/Green/Blue 85, Orange 98, Yellow 131, Lime 122, Cyan 170, Magenta 138, White 255.
//
// s60sc 2026

#include "appGlobals.h"

int lampPinG = -1; // RGB3Pin only (green)
int lampPinB = -1; // RGB3Pin only (blue)
int lampPin; // pin for single color LED, WS2812, 3 color RGB red
uint8_t lampLevel; // brightness of on board lamp led 
bool lampAuto = false; // if true in conjunction with pirUse and accelUse, switch on lamp when PIR or accelerometer activated at night
bool lampNight; // if true, lamp comes on at night (not used)
int lampMode; // how lamp is used
int lampType = 0; // type of lamp LED 
int lampActiveHi; // whether lamp lit when pulled Hi (true) or Lo (false)

// Multiples of 2 from Rapid blink duration
enum class Blink : uint8_t { Solid, Slow, Medium, Fast, Rapid, Count };
enum class Color : uint8_t {
  Red, Orange, Yellow, Lime, Green, Teal, Cyan, 
  Azure, Blue, Violet, Magenta, White, Count
};

// Data types
struct Rgb { uint8_t r, g, b; };

struct LedStyle {
  Color color;
  Blink blink;
};

TaskHandle_t ledHandle = NULL;
static Brightness thisBright = (Brightness)lampLevel;
static LedType thisLedType = (LedType)lampType;
static DevStatus thisStatus = DevStatus::STARTING; // initial state  DevStatus enum must start with {READY, STARTING, FAILURE, ...
static const uint32_t pwmFreqHz = 5000; // used by RGB3Pin and Mono
static constexpr uint32_t kTickMs = 125; // duration of rapid blink in ms
static bool lampInit = false;

// Predeclared tables 
constexpr Rgb kPalette[] = {
  // must be in same order as enum Color
    /* Red     */ {255,   0,   0},
    /* Orange  */ {255,  40,   0},
    /* Yellow  */ {255, 140,   0},
    /* Lime    */ {110, 255,   0},
    /* Green   */ {  0, 255,   0},
    /* Teal    */ {  0, 255,  80},
    /* Cyan    */ {  0, 255, 255},
    /* Azure   */ {  0, 100, 255},
    /* Blue    */ {  0,   0, 255},
    /* Violet  */ {120,   0, 255},
    /* Magenta */ {255,   0, 160},
    /* White   */ {255, 255, 255},
};
static_assert(std::size(kPalette) == static_cast<size_t>(Color::Count), "kPalette must have one entry per Color");

constexpr LedStyle kStyles[] = {
  // DevStatus values applied in this order
  // first entry should always be solid white if different states not used
  {Color::White,  Blink::Solid},
  {Color::Cyan,   Blink::Medium},
  {Color::Red,    Blink::Rapid},
  {Color::Blue,   Blink::Medium}, 
  {Color::Green,  Blink::Solid},
  {Color::Yellow, Blink::Fast},
  {Color::Magenta, Blink::Slow},
};
static_assert(std::size(kStyles) >= static_cast<size_t>(DevStatus::Count), "LedStyle must have an entry in order of each devStatus");

// Perceived % -> scale, gamma 2.2: round(255 * p^2.2)
constexpr uint8_t kBrightnessScale[] = { 0, 12, 55, 135, 255 };   // Off/Low/Medium/High/Full
static_assert(std::size(kBrightnessScale) == static_cast<size_t>(Brightness::Count),
    "kBrightnessScale must have one entry per Brightness");

constexpr uint8_t kBlinkToggleTicks[] = {
  0,   // Solid
  8,   // Slow   (toggle every 1000 ms)
  4,   // Medium (toggle every 500 ms)
  2,   // Fast   (toggle every 250 ms)
  1    // Rapid  (toggle every 125 ms)
};
static_assert(std::size(kBlinkToggleTicks) == static_cast<size_t>(Blink::Count), "kBlinkToggleTicks must have one entry per Blink");

// Lookup helpers 
constexpr const LedStyle& styleFor(DevStatus s) { return kStyles[static_cast<size_t>(s)]; }
constexpr Rgb rgbOf(Color c)                 { return kPalette[static_cast<size_t>(c)]; }

constexpr uint8_t scaleChannel(uint8_t ch, uint8_t s) {
  return static_cast<uint8_t>((ch * s + 127) / 255);
}

constexpr Rgb render(Color c, Brightness b) {
  const Rgb     p = rgbOf(c);
  const uint8_t s = kBrightnessScale[static_cast<size_t>(b)];
  return { scaleChannel(p.r, s), scaleChannel(p.g, s), scaleChannel(p.b, s) };
}

// Single-colour LED: average of the (already brightness-scaled) channels
constexpr uint8_t monoLevel(const Rgb& c) {
  return static_cast<uint8_t>((c.r + c.g + c.b) / 3);
}

static_assert(monoLevel({255, 255, 255}) == 255, "White must map to full level");

// Shared between API callers and ledTask (guarded by m_mux)
portMUX_TYPE m_mux  = portMUX_INITIALIZER_UNLOCKED;
Rgb          m_onColor     = {0, 0, 0};
uint8_t      m_toggleTicks = 0;  // 0 = solid
uint8_t      m_tickCount   = 0;
bool         m_phaseOn     = true;
bool         m_change      = false;   // change, redraw on next tick

// Output stage: called from ledTask (or ledInit before the task starts)
void writeLed(const Rgb& c) {
  switch (thisLedType) {
    case LedType::WS2812:
      rgbLedWrite(lampPin, c.r, c.g, c.b);
      break;
    case LedType::RGB3Pin: {
      const uint8_t r = lampActiveHi ? 255 - c.r : c.r;
      const uint8_t g = lampActiveHi ? 255 - c.g : c.g;
      const uint8_t b = lampActiveHi ? 255 - c.b : c.b;
      ledcWrite(lampPin, r);
      ledcWrite(lampPinG, g);
      ledcWrite(lampPinB, b);
      break;
    }
    case LedType::Mono: 
    default: {
      const uint8_t v = monoLevel(c);
      analogWrite(lampPin, lampActiveHi ? 255 - v : v);
      break;
    }
  }
}

void ledTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(kTickMs);

  while (true) {
    vTaskDelayUntil(&lastWake, period);
    bool doWrite = false;
    Rgb  out;

    portENTER_CRITICAL(&m_mux);
    if (m_change) {  // new status or brightness
      m_change = false;
      doWrite = true;
    } else if (m_toggleTicks != 0 && ++m_tickCount >= m_toggleTicks) {
      m_tickCount = 0;
      m_phaseOn   = !m_phaseOn;
      doWrite     = true;
    }
    out = m_phaseOn ? m_onColor : Rgb{0, 0, 0};
    portEXIT_CRITICAL(&m_mux);

    if (doWrite) writeLed(out);
  }
}

// Recompute task data from status + brightness; restart blink phase "on"
void applyState() {
  const LedStyle& st = styleFor(thisStatus);
  const Rgb     on = render(st.color, thisBright);
  const uint8_t tk = kBlinkToggleTicks[static_cast<size_t>(st.blink)];

  portENTER_CRITICAL(&m_mux);
  m_onColor     = on;
  m_toggleTicks = tk;
  m_tickCount   = 0;
  m_phaseOn     = true;
  m_change       = true;
  portEXIT_CRITICAL(&m_mux);
}

static void setupLamp() {
  // setup lamp LED according to type
  if (!lampInit) {
    char val[10];
#ifdef LED_GPIO_NUM
    lampPin = LED_GPIO_NUM;
    itoa(lampPin, val, 10);
#if !defined(STANDALONE)
    updateConfigVect("lampPin", val);
#endif
#endif // LED_GPIO_NUM
#ifdef USE_WS2812
    lampType = (int)LedType::WS2812;
#else
    if (lampType == 0) lampType = (int)LedType::Mono;
#endif
    itoa(lampType, val, 10);
#if !defined(STANDALONE)
    updateConfigVect("lampType", val);
#endif
    thisLedType = (LedType)lampType;
    char lampStr[10];
    if (lampPin) {
      // hardware setup
      switch (thisLedType) {
        case LedType::RGB3Pin:
          ledcAttach(lampPin, pwmFreqHz, 8);
          ledcAttach(lampPinG, pwmFreqHz, 8);
          ledcAttach(lampPinB, pwmFreqHz, 8);
          strcpy(lampStr, "RGB3Pin");
          break;
        case LedType::WS2812:
          // RMT set up on first call of rgbLedWrite()
          strcpy(lampStr, "WS2812");
          break;
        case LedType::Mono:
        default:
          analogWriteResolution(lampPin, 8);  // 8 bit
          analogWriteFrequency(lampPin, pwmFreqHz);
          strcpy(lampStr, "Mono");
          break;
      }
      applyState();
      m_change = false;
      writeLed(m_onColor);  // initial state
      if (ledHandle == NULL) xTaskCreateWithCaps(&ledTask, "ledTask", LED_STACK_SIZE, NULL, LED_PRI, &ledHandle, STACK_MEM);
      lampInit = true;
      lampLevel > 0 ? setLamp(lampLevel) : setLamp((uint8_t)Brightness::Medium);
 #ifdef LED_GPIO_NUM
      updateStatus("save", "0", true);
 #endif
      LOG_INF("%s LED available on pin %d", lampStr, lampPin);
    }
  } 
}

// Set device status: colour and blink rate come from the predeclared table.
static void updateStatusLed(DevStatus s) {
  thisStatus = s;
  applyState();
}

// Set overall brightness (perceptually scaled)
static void setLedBrightness(Brightness b) {
  thisBright = b;
  applyState();
}

void setLamp(uint8_t lampLevel) {
  setLamp(DevStatus::READY, (Brightness)lampLevel);
}

void setLamp(DevStatus s, uint8_t lampLevel) {
  setLamp(s, (Brightness)lampLevel);
}

void setLamp(DevStatus s) {
  if (lampType == (int)LedType::WS2812) setLamp(s, Brightness::Low);
  else setLamp(s, Brightness::High);
}

void setLamp(DevStatus s, Brightness b) {
  // control lamp status
  if (lampPin) {
    if (!lampInit) setupLamp();
    if (lampInit) {
      if (b == Brightness::Off && !appSetupDone) b = Brightness::Medium; // show lamp status during startup if off      
      updateStatusLed(s);
      setLedBrightness(b);
    }
  }
}