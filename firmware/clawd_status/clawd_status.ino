/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   CLAWD STATUS — Claude Code Status Indicator
 *   ESP32-C3 Super Mini + ST7789 1.54" 240×240
 *
 *   Wiring:
 *     SDA → GPIO 10  (hardware SPI MOSI)
 *     SCL → GPIO 8   (hardware SPI SCK)
 *     RST → GPIO 2
 *     DC  → GPIO 1
 *     CS  → GPIO 4
 *     BL  → GPIO 3
 *     VCC → 3V3
 *     GND → GND
 *
 *   Serial: 115200 baud over USB CDC
 *
 *   Protocol (newline-terminated):
 *     STATE:working    → animated scanning eyes
 *     STATE:attention  → happy squish eyes (hey, look!)
 *     STATE:inactive   → sleepy half-closed eyes + dim
 *     SET:brightness:N → backlight PWM (0-255)
 *     SET:speed:N      → animation speed (1=slow 2=normal 3=fast)
 *     SET:bgcolor:HEX  → background color (RRGGBB, no #)
 *     PING             → health check (responds PONG)
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>

// ── Pins ──────────────────────────────────────────────────────
#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── Display ───────────────────────────────────────────────────
#define DISP_W 240
#define DISP_H 240

// ── Eye constants ─────────────────────────────────────────────
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_OX  0
#define EYE_OY  40

// ── Backlight PWM ─────────────────────────────────────────────
#define BL_CHANNEL  0
#define BL_FREQ     5000
#define BL_RESOLUTION 8

// ── Colours ───────────────────────────────────────────────────
uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN;
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

// ── State machine ─────────────────────────────────────────────
enum State { STATE_BOOT, STATE_WORKING, STATE_ATTENTION, STATE_INACTIVE };
State     currentState   = STATE_BOOT;
uint8_t   brightness     = 255;
uint8_t   animSpeed      = 2;      // 1=slow 2=normal 3=fast
uint16_t  animBgColor    = 0;

// ── Serial buffer ─────────────────────────────────────────────
String serialBuffer = "";

// ── Animation state (non-blocking) ────────────────────────────
unsigned long lastFrameTime = 0;
uint8_t       animFrame     = 0;

// Working: 8-frame eye wiggle cycle
const int8_t WORK_OFFSETS[] = {0, -8, -16, -8, 0, 8, 16, 8};
#define WORK_FRAMES 8

// Attention: squish eyes with periodic blink
// Phases: 0=open(hold), 1=closing, 2=closed(hold), 3=opening
uint8_t  attnPhase        = 0;
unsigned long attnPhaseStart = 0;

// Inactive: slow drift
const int8_t SLEEP_OFFSETS[] = {0, -2, -4, -6, -4, -2, 0, 2, 4, 6, 4, 2};
#define SLEEP_FRAMES 12

// ── Logo data ─────────────────────────────────────────────────
#define LOGO_CX 120
#define LOGO_CY 105

#define LOGO_TRI_COUNT 162
static const int16_t LOGO_TRIS[][6] PROGMEM = {
  {120,105,65,134,100,114},{120,105,100,114,101,113},{120,105,101,113,100,112},
  {120,105,100,112,99,112},{120,105,99,112,93,111},{120,105,93,111,73,111},
  {120,105,73,111,55,110},{120,105,55,110,38,109},{120,105,38,109,34,108},
  {120,105,34,108,30,103},{120,105,30,103,30,100},{120,105,30,100,34,98},
  {120,105,34,98,39,98},{120,105,39,98,50,99},{120,105,50,99,67,100},
  {120,105,67,100,80,101},{120,105,80,101,98,103},{120,105,98,103,101,103},
  {120,105,101,103,101,102},{120,105,101,102,100,101},{120,105,100,101,100,100},
  {120,105,100,100,82,88},{120,105,82,88,63,76},{120,105,63,76,53,69},
  {120,105,53,69,48,65},{120,105,48,65,45,61},{120,105,45,61,44,54},
  {120,105,44,54,49,49},{120,105,49,49,55,49},{120,105,55,49,57,49},
  {120,105,57,49,64,55},{120,105,64,55,78,66},{120,105,78,66,96,79},
  {120,105,96,79,99,81},{120,105,99,81,100,81},{120,105,100,81,100,80},
  {120,105,100,80,99,78},{120,105,99,78,89,60},{120,105,89,60,78,41},
  {120,105,78,41,73,34},{120,105,73,34,72,29},{120,105,72,29,72,28},
  {120,105,72,28,72,27},{120,105,72,27,71,26},{120,105,71,26,71,25},
  {120,105,71,25,71,24},{120,105,71,24,77,16},{120,105,77,16,80,15},
  {120,105,80,15,87,16},{120,105,87,16,91,19},{120,105,91,19,95,29},
  {120,105,95,29,103,46},{120,105,103,46,114,68},{120,105,114,68,118,75},
  {120,105,118,75,119,81},{120,105,119,81,120,83},{120,105,120,83,121,83},
  {120,105,121,83,121,82},{120,105,121,82,122,69},{120,105,122,69,124,54},
  {120,105,124,54,126,34},{120,105,126,34,126,28},{120,105,126,28,129,21},
  {120,105,129,21,135,18},{120,105,135,18,139,20},{120,105,139,20,143,25},
  {120,105,143,25,142,28},{120,105,142,28,140,42},{120,105,140,42,136,64},
  {120,105,136,64,133,78},{120,105,133,78,135,78},{120,105,135,78,136,76},
  {120,105,136,76,144,67},{120,105,144,67,156,51},{120,105,156,51,162,45},
  {120,105,162,45,168,38},{120,105,168,38,172,35},{120,105,172,35,180,35},
  {120,105,180,35,185,43},{120,105,185,43,183,52},{120,105,183,52,175,62},
  {120,105,175,62,168,71},{120,105,168,71,159,83},{120,105,159,83,153,94},
  {120,105,153,94,154,94},{120,105,154,94,155,94},{120,105,155,94,176,90},
  {120,105,176,90,188,88},{120,105,188,88,201,85},{120,105,201,85,208,88},
  {120,105,208,88,208,91},{120,105,208,91,206,97},{120,105,206,97,191,101},
  {120,105,191,101,174,104},{120,105,174,104,148,110},{120,105,148,110,148,111},
  {120,105,148,111,148,111},{120,105,148,111,160,112},{120,105,160,112,165,112},
  {120,105,165,112,177,112},{120,105,177,112,200,114},{120,105,200,114,205,118},
  {120,105,205,118,209,123},{120,105,209,123,208,126},{120,105,208,126,199,131},
  {120,105,199,131,187,128},{120,105,187,128,159,121},{120,105,159,121,149,119},
  {120,105,149,119,147,119},{120,105,147,119,147,120},{120,105,147,120,156,128},
  {120,105,156,128,170,141},{120,105,170,141,189,158},{120,105,189,158,190,163},
  {120,105,190,163,188,166},{120,105,188,166,185,166},{120,105,185,166,169,153},
  {120,105,169,153,162,148},{120,105,162,148,148,136},{120,105,148,136,147,136},
  {120,105,147,136,147,137},{120,105,147,137,150,142},{120,105,150,142,168,168},
  {120,105,168,168,169,176},{120,105,169,176,168,179},{120,105,168,179,163,180},
  {120,105,163,180,158,179},{120,105,158,179,148,165},{120,105,148,165,137,149},
  {120,105,137,149,129,134},{120,105,129,134,128,135},{120,105,128,135,123,189},
  {120,105,123,189,120,192},{120,105,120,192,115,194},{120,105,115,194,110,191},
  {120,105,110,191,108,185},{120,105,108,185,110,174},{120,105,110,174,113,160},
  {120,105,113,160,116,148},{120,105,116,148,118,134},{120,105,118,134,119,129},
  {120,105,119,129,119,129},{120,105,119,129,118,129},{120,105,118,129,107,144},
  {120,105,107,144,91,166},{120,105,91,166,78,180},{120,105,78,180,75,181},
  {120,105,75,181,70,178},{120,105,70,178,70,173},{120,105,70,173,73,169},
  {120,105,73,169,91,146},{120,105,91,146,102,132},{120,105,102,132,109,124},
  {120,105,109,124,109,123},{120,105,109,123,108,123},{120,105,108,123,61,153},
  {120,105,61,153,52,155},{120,105,52,155,49,151},{120,105,49,151,49,146},
  {120,105,49,146,51,144},{120,105,51,144,65,134},{120,105,65,134,65,134},
};

#define LOGO_SEG_COUNT 162
static const int16_t LOGO_SEGS[][4] PROGMEM = {
  {65,134,100,114},{100,114,101,113},{101,113,100,112},{100,112,99,112},
  {99,112,93,111},{93,111,73,111},{73,111,55,110},{55,110,38,109},
  {38,109,34,108},{34,108,30,103},{30,103,30,100},{30,100,34,98},
  {34,98,39,98},{39,98,50,99},{50,99,67,100},{67,100,80,101},
  {80,101,98,103},{98,103,101,103},{101,103,101,102},{101,102,100,101},
  {100,101,100,100},{100,100,82,88},{82,88,63,76},{63,76,53,69},
  {53,69,48,65},{48,65,45,61},{45,61,44,54},{44,54,49,49},
  {49,49,55,49},{55,49,57,49},{57,49,64,55},{64,55,78,66},
  {78,66,96,79},{96,79,99,81},{99,81,100,81},{100,81,100,80},
  {100,80,99,78},{99,78,89,60},{89,60,78,41},{78,41,73,34},
  {73,34,72,29},{72,29,72,28},{72,28,72,27},{72,27,71,26},
  {71,26,71,25},{71,25,71,24},{71,24,77,16},{77,16,80,15},
  {80,15,87,16},{87,16,91,19},{91,19,95,29},{95,29,103,46},
  {103,46,114,68},{114,68,118,75},{118,75,119,81},{119,81,120,83},
  {120,83,121,83},{121,83,121,82},{121,82,122,69},{122,69,124,54},
  {124,54,126,34},{126,34,126,28},{126,28,129,21},{129,21,135,18},
  {135,18,139,20},{139,20,143,25},{143,25,142,28},{142,28,140,42},
  {140,42,136,64},{136,64,133,78},{133,78,135,78},{135,78,136,76},
  {136,76,144,67},{144,67,156,51},{156,51,162,45},{162,45,168,38},
  {168,38,172,35},{172,35,180,35},{180,35,185,43},{185,43,183,52},
  {183,52,175,62},{175,62,168,71},{168,71,159,83},{159,83,153,94},
  {153,94,154,94},{154,94,155,94},{155,94,176,90},{176,90,188,88},
  {188,88,201,85},{201,85,208,88},{208,88,208,91},{208,91,206,97},
  {206,97,191,101},{191,101,174,104},{174,104,148,110},{148,110,148,111},
  {148,111,148,111},{148,111,160,112},{160,112,165,112},{165,112,177,112},
  {177,112,200,114},{200,114,205,118},{205,118,209,123},{209,123,208,126},
  {208,126,199,131},{199,131,187,128},{187,128,159,121},{159,121,149,119},
  {149,119,147,119},{147,119,147,120},{147,120,156,128},{156,128,170,141},
  {170,141,189,158},{189,158,190,163},{190,163,188,166},{188,166,185,166},
  {185,166,169,153},{169,153,162,148},{162,148,148,136},{148,136,147,136},
  {147,136,147,137},{147,137,150,142},{150,142,168,168},{168,168,169,176},
  {169,176,168,179},{168,179,163,180},{163,180,158,179},{158,179,148,165},
  {148,165,137,149},{137,149,129,134},{129,134,128,135},{128,135,123,189},
  {123,189,120,192},{120,192,115,194},{115,194,110,191},{110,191,108,185},
  {108,185,110,174},{110,174,113,160},{113,160,116,148},{116,148,118,134},
  {118,134,119,129},{119,129,119,129},{119,129,118,129},{118,129,107,144},
  {107,144,91,166},{91,166,78,180},{78,180,75,181},{75,181,70,178},
  {70,178,70,173},{70,173,73,169},{73,169,91,146},{91,146,102,132},
  {102,132,109,124},{109,124,109,123},{109,123,108,123},{108,123,61,153},
  {61,153,52,155},{52,155,49,151},{49,151,49,146},{49,146,51,144},
  {51,144,65,134},{65,134,65,134},
};

// ═════════════════════════════════════════════════════════════
//  HELPERS
// ═════════════════════════════════════════════════════════════

int speedMs(int ms) {
  if (animSpeed == 3) return ms / 2;
  if (animSpeed == 1) return ms * 2;
  return ms;
}

uint16_t hexToRgb565(String hex) {
  hex.replace("#", "");
  if (hex.length() != 6) return C_WHITE;
  long v = strtol(hex.c_str(), nullptr, 16);
  return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void initColours() {
  C_ORANGE = tft.color565(218, 17, 0);
  C_DARKBG = tft.color565(10,  12,  16);
  C_MUTED  = tft.color565(90,  88,  86);
  C_GREEN  = tft.color565(80, 220, 130);
  animBgColor = C_ORANGE;
}

void setBacklightPWM(uint8_t val) {
  ledcWrite(BL_CHANNEL, val);
}

// ═════════════════════════════════════════════════════════════
//  LOGO
// ═════════════════════════════════════════════════════════════

void drawLogoFilled(uint16_t bg, uint16_t fg) {
  tft.fillScreen(bg);
  for (uint16_t i = 0; i < LOGO_TRI_COUNT; i++) {
    tft.fillTriangle(
      pgm_read_word(&LOGO_TRIS[i][0]), pgm_read_word(&LOGO_TRIS[i][1]),
      pgm_read_word(&LOGO_TRIS[i][2]), pgm_read_word(&LOGO_TRIS[i][3]),
      pgm_read_word(&LOGO_TRIS[i][4]), pgm_read_word(&LOGO_TRIS[i][5]),
      fg);
  }
  tft.setTextColor(fg); tft.setTextSize(2);
  tft.setCursor(LOGO_CX - 54, 210); tft.print("Anthropic");
  tft.setCursor(LOGO_CX - 53, 210); tft.print("Anthropic");
}

void animLogoReveal() {
  tft.fillScreen(animBgColor);
  for (uint16_t i = 0; i < LOGO_SEG_COUNT; i++) {
    int16_t x1 = pgm_read_word(&LOGO_SEGS[i][0]);
    int16_t y1 = pgm_read_word(&LOGO_SEGS[i][1]);
    int16_t x2 = pgm_read_word(&LOGO_SEGS[i][2]);
    int16_t y2 = pgm_read_word(&LOGO_SEGS[i][3]);
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, C_WHITE);
    if (i % 4 == 0) delay(speedMs(8));
  }
  drawLogoFilled(animBgColor, C_WHITE);
  delay(1500);
}

// ═════════════════════════════════════════════════════════════
//  DRAWING FUNCTIONS
// ═════════════════════════════════════════════════════════════

inline int16_t eyeLX(int16_t ox) {
  return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
inline int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

// Normal rectangular eyes (used for working state)
void drawNormalEyes(int16_t ox = 0, bool blink = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  if (!blink) {
    tft.fillRect(lx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx, ey, EYE_W, EYE_H, C_BLACK);
  } else {
    tft.fillRect(lx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
    tft.fillRect(rx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
  }
}

// Chevron helper for squish eyes
void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      tft.drawLine(cx - reach/2, cy - arm + t, cx + reach/2, cy + t,      col);
      tft.drawLine(cx + reach/2, cy + t,       cx - reach/2, cy + arm + t, col);
    } else {
      tft.drawLine(cx + reach/2, cy - arm + t, cx - reach/2, cy + t,      col);
      tft.drawLine(cx - reach/2, cy + t,       cx + reach/2, cy + arm + t, col);
    }
  }
}

// Squish/happy eyes (used for attention state)
void drawSquishEyes(bool closed = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm   = EYE_H / 2;
  const int16_t reach = EYE_W / 2;
  const int16_t lcx   = lx + EYE_W / 2;
  const int16_t rcx   = rx + EYE_W / 2;
  if (!closed) {
    drawChevron(lcx, cy, arm, reach, 10, true,  C_BLACK);
    drawChevron(rcx, cy, arm, reach, 10, false, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);
    tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
  }
}

// Sleepy half-closed eyes (used for inactive state)
void drawSleepyEyes(int16_t ox = 0) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox);
  const int16_t halfH = EYE_H / 3;
  const int16_t y = eyeCY() - halfH / 2 + 8;  // shifted down = droopy
  tft.fillRect(lx, y, EYE_W, halfH, C_BLACK);
  tft.fillRect(rx, y, EYE_W, halfH, C_BLACK);
}

// ═════════════════════════════════════════════════════════════
//  NON-BLOCKING ANIMATION TICKS
// ═════════════════════════════════════════════════════════════

// Working: continuous L/R scanning eyes
void tickWorking() {
  unsigned long interval = speedMs(120);
  if (millis() - lastFrameTime >= interval) {
    drawNormalEyes(WORK_OFFSETS[animFrame]);
    animFrame = (animFrame + 1) % WORK_FRAMES;
    lastFrameTime = millis();
  }
}

// Attention: squish eyes with periodic blink
void tickAttention() {
  unsigned long now = millis();
  unsigned long elapsed = now - attnPhaseStart;

  switch (attnPhase) {
    case 0: // open — hold for 2.5s
      if (elapsed >= (unsigned long)speedMs(2500)) {
        attnPhase = 1;
        attnPhaseStart = now;
        drawSquishEyes(true);  // close
      }
      break;
    case 1: // closed — hold for 150ms
      if (elapsed >= (unsigned long)speedMs(150)) {
        attnPhase = 2;
        attnPhaseStart = now;
        drawSquishEyes(false); // open
      }
      break;
    case 2: // open — brief hold 100ms then second blink
      if (elapsed >= (unsigned long)speedMs(100)) {
        attnPhase = 3;
        attnPhaseStart = now;
        drawSquishEyes(true);  // close again
      }
      break;
    case 3: // closed — hold 150ms
      if (elapsed >= (unsigned long)speedMs(150)) {
        attnPhase = 0;
        attnPhaseStart = now;
        drawSquishEyes(false); // open — restart cycle
      }
      break;
  }
}

// Inactive: very slow drifting sleepy eyes
void tickInactive() {
  unsigned long interval = speedMs(350);
  if (millis() - lastFrameTime >= interval) {
    drawSleepyEyes(SLEEP_OFFSETS[animFrame]);
    animFrame = (animFrame + 1) % SLEEP_FRAMES;
    lastFrameTime = millis();
  }
}

// ═════════════════════════════════════════════════════════════
//  STATE TRANSITIONS
// ═════════════════════════════════════════════════════════════

void setState(State newState) {
  if (newState == currentState) return;
  currentState = newState;

  // Reset animation counters
  animFrame = 0;
  lastFrameTime = millis();
  attnPhase = 0;
  attnPhaseStart = millis();

  // Adjust backlight
  switch (newState) {
    case STATE_WORKING:
      setBacklightPWM(brightness);
      drawNormalEyes(0);
      break;
    case STATE_ATTENTION:
      setBacklightPWM(brightness);
      drawSquishEyes(false);
      break;
    case STATE_INACTIVE: {
      uint8_t dimVal = brightness > 20 ? brightness / 8 : 3;
      setBacklightPWM(dimVal);
      drawSleepyEyes(0);
      break;
    }
    default:
      break;
  }
}

// ═════════════════════════════════════════════════════════════
//  SERIAL COMMAND PROCESSING
// ═════════════════════════════════════════════════════════════

void processCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.startsWith("STATE:")) {
    String val = cmd.substring(6);
    if (val == "working") {
      setState(STATE_WORKING);
      Serial.println("OK:STATE:working");
    } else if (val == "attention") {
      setState(STATE_ATTENTION);
      Serial.println("OK:STATE:attention");
    } else if (val == "inactive") {
      setState(STATE_INACTIVE);
      Serial.println("OK:STATE:inactive");
    } else {
      Serial.println("ERR:bad_state");
    }
  }
  else if (cmd.startsWith("SET:")) {
    String rest = cmd.substring(4);
    int colon = rest.indexOf(':');
    if (colon < 0) { Serial.println("ERR:bad_set"); return; }

    String key = rest.substring(0, colon);
    String val = rest.substring(colon + 1);

    if (key == "brightness") {
      brightness = constrain(val.toInt(), 0, 255);
      // Apply immediately unless inactive (which uses dimmed value)
      if (currentState != STATE_INACTIVE) {
        setBacklightPWM(brightness);
      } else {
        uint8_t dimVal = brightness > 20 ? brightness / 8 : 3;
        setBacklightPWM(dimVal);
      }
      Serial.println("OK:SET:brightness:" + String(brightness));
    }
    else if (key == "speed") {
      animSpeed = constrain(val.toInt(), 1, 3);
      Serial.println("OK:SET:speed:" + String(animSpeed));
    }
    else if (key == "bgcolor") {
      animBgColor = hexToRgb565(val);
      // Redraw current state with new bg
      switch (currentState) {
        case STATE_WORKING:   drawNormalEyes(0);      break;
        case STATE_ATTENTION: drawSquishEyes(false);   break;
        case STATE_INACTIVE:  drawSleepyEyes(0);      break;
        default: break;
      }
      Serial.println("OK:SET:bgcolor:" + val);
    }
    else {
      Serial.println("ERR:unknown_key");
    }
  }
  else if (cmd == "PING") {
    Serial.println("PONG");
  }
  else {
    Serial.println("ERR:unknown_cmd");
  }
}

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  // PWM backlight
  ledcSetup(BL_CHANNEL, BL_FREQ, BL_RESOLUTION);
  ledcAttachPin(TFT_BLK, BL_CHANNEL);
  setBacklightPWM(brightness);

  // Display init
  SPI.begin(8, -1, 10, TFT_CS);
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  initColours();

  // ── Boot splash ────────────────────────────────────────────
  tft.fillScreen(animBgColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 - 22); tft.print("Clawd");
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 + 14); tft.print("Mochi");
  delay(1200);

  // ── Logo reveal ────────────────────────────────────────────
  animLogoReveal();

  // ── Ready — start in inactive state ────────────────────────
  currentState = STATE_INACTIVE;
  animFrame = 0;
  lastFrameTime = millis();
  uint8_t dimVal = brightness > 20 ? brightness / 8 : 3;
  setBacklightPWM(dimVal);
  drawSleepyEyes(0);

  Serial.println("BOOT");
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════

void loop() {
  // 1. Read serial commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      processCommand(serialBuffer);
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
    }
  }

  // 2. Tick current state animation
  switch (currentState) {
    case STATE_WORKING:   tickWorking();   break;
    case STATE_ATTENTION: tickAttention(); break;
    case STATE_INACTIVE:  tickInactive();  break;
    default: break;
  }
}
