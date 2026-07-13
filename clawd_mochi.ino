/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   CLAWD MOCHI — ESP32-C3 Super Mini + ST7789 1.54" 240×240
 *   公版 - 支持WiFi配网 + PC系统监控
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
 *   重置配网引脚: GPIO 5 (启动时拉低可清除WiFi配置重新配网)
 *
 *   WiFi配网流程:
 *     首次启动 → AP模式(Clawd-Mochi-Setup, 192.168.4.1)
 *     用户访问配网页面 → 输入WiFi密码 + PC监控IP → 保存配置
 *     重启 → Station模式连接WiFi → 正常运行
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <PubSubClient.h>

// ── Pins ──────────────────────────────────────────────────────
#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── WiFi配网系统 ──────────────────────────────────────────────────────────────
#define RESET_PIN        5    // 重置配网引脚（启动时拉低清除配置）
#define AP_NAME          "Clawd-Mochi-Setup"
#define CONTROL_AP_NAME  "Clawd-Mochi"
#define AP_IP            "192.168.4.1"
#define CONNECT_TIMEOUT  15000   // WiFi连接超时15秒
#define FORCE_EXPRESSION_AP 1     // 表情优先：固定开热点和控制页

Preferences preferences;

// 配置存储字段
String cfgSSID;
String cfgPass;
String cfgPcIp;      // PC监控IP地址

// WiFi扫描缓存（AP配网时使用）
#define MAX_SCAN_RESULTS 20
String scanSSID[MAX_SCAN_RESULTS];
int scanRSSI[MAX_SCAN_RESULTS];
uint8_t scanCount = 0;

// 配网模式标志
bool inSetupMode = false;    // true = AP配网模式, false = Station运行模式
bool wifiConnected = false;  // WiFi连接成功标志

WebServer server(80);
WebServer* setupServer = nullptr;  // 配网专用服务器
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

// ── Display ───────────────────────────────────────────────────
#define DISP_W 240
#define DISP_H 240

// ── Eye constants (shared by both eye views) ──────────────────
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_OX  0     // horizontal offset
#define EYE_OY  40    // vertical offset upward (subtracted from centre)

// ── Colours ───────────────────────────────────────────────────
uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN;
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

// ── State ─────────────────────────────────────────────────────
#define VIEW_EYES_NORMAL 0
#define VIEW_EYES_SQUISH 1
#define VIEW_CODE        2
#define VIEW_DRAW        3
#define VIEW_MONITOR     4
#define VIEW_REMINDER    5
#define VIEW_MOCHI_EXPR  6

enum MochiExpression : uint8_t {
  MOCHI_IDLE,
  MOCHI_HAPPY,
  MOCHI_SLEEP,
  MOCHI_THINKING,
  MOCHI_PROUD,
  MOCHI_SAD,
  MOCHI_ANGRY,
  MOCHI_WINK
};

uint8_t  currentView     = VIEW_EYES_NORMAL;
uint8_t  prevView        = VIEW_EYES_NORMAL;  // 用于提醒结束后返回
bool     busy            = false;
bool     backlightOn     = true;
uint8_t  animSpeed       = 1;   // 1=slow(default) 2=normal 3=fast
uint8_t  brightnessLevel = 90;

uint16_t animBgColor  = 0;   // background for eye/logo animations
uint16_t drawBgColor  = 0;   // background for canvas

// ── Orange expression system ───────────────────────────────────
MochiExpression mochiExpr = MOCHI_IDLE;
MochiExpression idleExpr = MOCHI_IDLE;
uint8_t mochiFrame = 0;
bool mochiFullRedraw = true;
unsigned long lastMochiFrameMs = 0;
uint16_t mochiFrameMs = 250;

// ── MQTT control ───────────────────────────────────────────────
bool mqttEnabled = false;
String mqttHost = "broker.hivemq.com";
uint16_t mqttPort = 1883;
String mqttDeviceId = "clawd-001";
unsigned long lastMqttTryMs = 0;

// ── Monitor ───────────────────────────────────────────────────
unsigned long lastMonitorUpdate = 0;
const unsigned long MONITOR_INTERVAL = 50000;  // 50 seconds

// ── Stats from NanoPi (for Monitor and Reminder) ────────────────
String statsLoad = "--";
String statsMem = "--";
String statsTemp = "--";
String statsUptime = "--";
uint8_t ntpHour = 0;
uint8_t ntpMinute = 0;
uint8_t ntpDay = 0;
uint8_t currentDay = 0;
bool statsValid = false;

// ── Reminder ───────────────────────────────────────────────────
#define MAX_REMINDERS 5
#define REMINDER_MSG_LEN 20

struct Reminder {
  uint8_t hour;
  uint8_t minute;
  char message[REMINDER_MSG_LEN + 1];
  bool enabled;
  bool triggeredToday;  // 当天已触发标记
};

Reminder reminders[MAX_REMINDERS];
uint8_t reminderCount = 0;
unsigned long reminderStartTime = 0;      // 提醒显示开始时间
const unsigned long REMINDER_DURATION = 30000;  // 30秒
unsigned long lastMinuteCheck = 0;         // 上次分钟检查时间

// ── Terminal ──────────────────────────────────────────────────
#define TERM_COLS      15
#define TERM_ROWS       8
#define TERM_CHAR_W    12
#define TERM_CHAR_H    20
#define TERM_PAD_X      8
#define TERM_PAD_Y     18

bool    termMode    = false;
String  termLines[TERM_ROWS];
uint8_t termRow     = 0;
uint8_t termCol     = 0;

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

// ── 配置读写接口 ──────────────────────────────────────────────
bool loadConfig() {
  preferences.begin("clawd", true);  // readonly
  cfgSSID = preferences.getString("ssid", "");
  cfgPass = preferences.getString("pass", "");
  cfgPcIp = preferences.getString("pcip", "");
  preferences.end();
  return (cfgSSID.length() > 0);  // 有WiFi配置返回true
}

void saveConfig(const String& ssid, const String& pass, const String& pcip) {
  preferences.begin("clawd", false);  // write mode
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("pcip", pcip);
  preferences.end();
}

void clearConfig() {
  preferences.begin("clawd", false);
  preferences.clear();
  preferences.end();
  cfgSSID = "";
  cfgPass = "";
  cfgPcIp = "";
}

// ── WiFi预扫描（Station模式下扫描并缓存结果）──────────────────
void scanWiFi() {
  scanCount = 0;
  Serial.println("Scanning WiFi...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("No networks found");
    return;
  }
  Serial.printf("Found %d networks\n", n);
  // 按信号强度排序，取前MAX_SCAN_RESULTS个
  for (int i = 0; i < n && scanCount < MAX_SCAN_RESULTS; i++) {
    scanSSID[scanCount] = WiFi.SSID(i);
    scanRSSI[scanCount] = WiFi.RSSI(i);
    // 去重
    bool dup = false;
    for (int j = 0; j < scanCount; j++) {
      if (scanSSID[j] == scanSSID[scanCount]) { dup = true; break; }
    }
    if (!dup) scanCount++;
  }
  WiFi.scanDelete();
}

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

void setBacklight(bool on) {
  backlightOn = on;
  analogWrite(TFT_BLK, on ? brightnessLevel : 0);
}

// ── 表情函数（依赖 tft, animBgColor, C_BLACK, C_WHITE, busy, speedMs）─────────
#include "faces_code.h"

void initColours() {
  C_ORANGE = tft.color565(255, 128, 0);
  C_DARKBG = tft.color565(10,  12,  16);
  C_MUTED  = tft.color565(90,  88,  86);
  C_GREEN  = tft.color565(80, 220, 130);
  animBgColor = C_ORANGE;
  drawBgColor = C_ORANGE;
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

// ═════════════════════════════════════════════════════════════
//  VIEWS
// ═════════════════════════════════════════════════════════════

// Eye helpers — shared constants via #define EYE_*
inline int16_t eyeLX(int16_t ox) {
  return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
inline int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

void mochiBase(uint16_t bg = C_ORANGE) {
  if (mochiFullRedraw) {
    tft.fillScreen(bg);
    mochiFullRedraw = false;
    return;
  }
  tft.fillRect(38, 36, 166, 112, bg);
}

void mochiBlockEye(int16_t x, int16_t y, int16_t w = 22, int16_t h = 34) {
  tft.fillRect(x - w / 2, y - h / 2, w, h, C_BLACK);
}

void mochiLineEye(int16_t x, int16_t y, int16_t w = 34) {
  tft.fillRect(x - w / 2, y - 4, w, 8, C_BLACK);
}

void mochiBrow(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
  for (int i = -2; i <= 2; i++) tft.drawLine(x1, y1 + i, x2, y2 + i, C_BLACK);
}

void mochiSoftBrow(int16_t x, int16_t y, int8_t tilt) {
  mochiBrow(x - 18, y + tilt, x + 18, y - tilt);
}

bool mochiBlinkNow(uint8_t every = 10) {
  return (mochiFrame % every) == (every - 1);
}

int8_t mochiBob(uint8_t speed = 2) {
  return ((mochiFrame / speed) % 2) ? 2 : -1;
}

void drawMochiIdle() {
  mochiBase();
  const int8_t glance = (mochiFrame % 12 < 6) ? -2 : 2;
  if (mochiBlinkNow()) {
    mochiLineEye(88, 104, 30);
    mochiLineEye(152, 104, 30);
    return;
  }
  mochiSoftBrow(88, 76, 5);
  mochiSoftBrow(152, 76, -5);
  mochiBlockEye(88 + glance, 106, 18, 34);
  mochiBlockEye(152 + glance, 106, 18, 34);
}

void drawMochiHappy() {
  mochiBase();
  const int8_t y = 100 + mochiBob();
  mochiSoftBrow(88, 78, -3);
  mochiSoftBrow(152, 78, 3);
  mochiLineEye(88, y, 36);
  mochiLineEye(152, y, 36);
  tft.fillRect(74, y - 8, 28, 6, C_ORANGE);
  tft.fillRect(138, y - 8, 28, 6, C_ORANGE);
}

void drawMochiSleep() {
  mochiBase();
  const int8_t y = 110 + mochiBob(3);
  mochiLineEye(88, y, 32);
  mochiLineEye(152, y, 32);
  mochiBrow(72, y - 24, 104, y - 24);
  mochiBrow(136, y - 24, 168, y - 24);
}

void drawMochiThinking() {
  mochiBase();
  const int8_t glance = (mochiFrame % 8 < 4) ? 4 : -1;
  mochiSoftBrow(88, 74, -6);
  mochiSoftBrow(152, 74, -2);
  mochiBlockEye(88 + glance, 108, 16, 32);
  mochiBlockEye(152 + glance, 108, 16, 32);
  tft.fillCircle(174, 62 + mochiBob(), 5, C_BLACK);
  tft.fillCircle(190, 48 - mochiBob(), 7, C_BLACK);
}

void drawMochiProud() {
  mochiBase();
  const int8_t lift = (mochiFrame % 6 == 0) ? -3 : 0;
  mochiBrow(68, 76 + lift, 104, 66 + lift);
  mochiBrow(136, 66 + lift, 172, 76 + lift);
  mochiBlockEye(88, 110 + lift, 20, 34);
  mochiBlockEye(152, 110 + lift, 20, 34);
}

void drawMochiSad() {
  mochiBase();
  const int8_t droop = (mochiFrame % 8 == 0) ? 2 : 0;
  mochiBrow(68, 68, 104, 80);
  mochiBrow(136, 80, 172, 68);
  mochiBlockEye(88, 114 + droop, 18, 30);
  mochiBlockEye(152, 114 + droop, 18, 30);
}

void drawMochiAngry() {
  mochiBase();
  const int8_t shake = (mochiFrame % 2) ? 2 : -2;
  mochiBrow(66 + shake, 68, 104 + shake, 84);
  mochiBrow(136 + shake, 84, 174 + shake, 68);
  mochiBlockEye(88 + shake, 112, 22, 34);
  mochiBlockEye(152 + shake, 112, 22, 34);
}

void drawMochiWink() {
  mochiBase();
  mochiSoftBrow(88, 78, 5);
  mochiSoftBrow(152, 76, -6);
  mochiLineEye(88, 106, 34);
  mochiBlockEye(152, 106 + mochiBob(), 20, 34);
}

void drawMochiExpression() {
  switch (mochiExpr) {
    case MOCHI_HAPPY:    drawMochiHappy(); break;
    case MOCHI_SLEEP:    drawMochiSleep(); break;
    case MOCHI_THINKING: drawMochiThinking(); break;
    case MOCHI_PROUD:    drawMochiProud(); break;
    case MOCHI_SAD:      drawMochiSad(); break;
    case MOCHI_ANGRY:    drawMochiAngry(); break;
    case MOCHI_WINK:     drawMochiWink(); break;
    default:             drawMochiIdle(); break;
  }
}

void drawNormalEyes(int16_t ox = 0, bool blink = false) {
  currentView = VIEW_MOCHI_EXPR;
  mochiExpr = blink ? MOCHI_WINK : MOCHI_IDLE;
  mochiFullRedraw = true;
  mochiFrame++;
  drawMochiExpression();
}

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

void drawSquishEyes(bool closed = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm   = EYE_H / 2;
  const int16_t reach = EYE_W / 2;
  const int16_t lcx   = lx + EYE_W / 2;
  const int16_t rcx   = rx + EYE_W / 2;
  if (!closed) {
    // 左眼：) 形弧形眯眯眼 - 向右弯
    // 用多个三角形叠加形成弧形效果
    tft.fillTriangle(lcx - reach, cy - arm, lcx + reach, cy, lcx - reach, cy + arm, C_BLACK);
    tft.fillTriangle(lcx - reach + 4, cy - arm, lcx + reach - 2, cy, lcx - reach + 4, cy + arm, C_BLACK);
    tft.fillTriangle(lcx - reach + 8, cy - arm, lcx + reach - 4, cy, lcx - reach + 8, cy + arm, C_BLACK);
    // 右眼：( 形弧形眯眯眼 - 向左弯（镜像）
    tft.fillTriangle(rcx + reach, cy - arm, rcx - reach, cy, rcx + reach, cy + arm, C_BLACK);
    tft.fillTriangle(rcx + reach - 4, cy - arm, rcx - reach + 2, cy, rcx + reach - 4, cy + arm, C_BLACK);
    tft.fillTriangle(rcx + reach - 8, cy - arm, rcx - reach + 4, cy, rcx + reach - 8, cy + arm, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);
    tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
  }
}

void drawCodeView() {
  termMode = false;
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,          DISP_W, 4, C_ORANGE);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_ORANGE); tft.setTextSize(4);
  tft.setCursor((DISP_W - 144) / 2, DISP_H / 2 - 52); tft.print("Claude");
  tft.setTextColor(C_WHITE);  tft.setTextSize(4);
  tft.setCursor((DISP_W - 96) / 2,  DISP_H / 2 + 8);  tft.print("Code");
  tft.fillRect((DISP_W - 96) / 2, DISP_H / 2 + 52, 96, 3, C_ORANGE);
}

// ═════════════════════════════════════════════════════════════
//  MONITOR
// ═════════════════════════════════════════════════════════════

void drawMonitorPanel(const String& load, const String& mem, const String& temp, const String& uptime) {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,          DISP_W, 4, C_ORANGE);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_ORANGE);

  // Title
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(12, 16); tft.print("Monitor PC");

  // Data rows
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(12, 50);  tft.print("Load: "); tft.setTextColor(C_GREEN); tft.print(load);
  tft.setTextColor(C_WHITE); tft.setCursor(12, 80);  tft.print("Mem:  "); tft.setTextColor(C_GREEN); tft.print(mem);
  tft.setTextColor(C_WHITE); tft.setCursor(12, 110); tft.print("Temp: "); tft.setTextColor(C_GREEN); tft.print(temp);
  tft.setTextColor(C_WHITE); tft.setCursor(12, 140); tft.print("Up:   "); tft.setTextColor(C_GREEN); tft.print(uptime);

  // Separator
  tft.drawFastHLine(12, 170, DISP_W - 24, C_ORANGE);

  // Status
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(12, 180); tft.print("Auto-refresh: 50s");
}

void fetchAndDrawMonitor() {
  if (WiFi.status() != WL_CONNECTED) {
    statsValid = false;
    drawMonitorPanel("--", "--", "--", "No WiFi");
    return;
  }

  // 检查PC IP是否配置
  if (cfgPcIp.length() == 0) {
    statsValid = false;
    drawMonitorPanel("--", "--", "--", "No PC IP");
    return;
  }

  HTTPClient http;
  WiFiClient client;  // 使用HTTP客户端
  String url = "http://" + cfgPcIp + "/stats.json";
  http.begin(client, url);
  http.setTimeout(5000);
  int httpCode = http.GET();

  Serial.print("HTTP code: ");
  Serial.println(httpCode);
  Serial.print("PC IP: ");
  Serial.println(cfgPcIp);

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      statsValid = false;
      drawMonitorPanel("err", "err", "err", "JSON err");
    } else {
      statsLoad   = doc["load"]   | "--";
      statsMem    = String(doc["mem"].as<int>()) + "%";
      statsTemp   = doc["temp"]   | "--";
      statsUptime = doc["uptime"] | "--";
      ntpHour     = doc["hour"]   | 0;
      ntpMinute   = doc["minute"] | 0;
      ntpDay      = doc["day"]    | 0;
      statsValid  = true;
      drawMonitorPanel(statsLoad, statsMem, statsTemp, statsUptime);

      Serial.print("Time from PC: ");
      Serial.print(ntpHour < 10 ? "0" : "");
      Serial.print(ntpHour);
      Serial.print(":");
      Serial.print(ntpMinute < 10 ? "0" : "");
      Serial.println(ntpMinute);
    }
  } else {
    statsValid = false;
    drawMonitorPanel("err", "err", "err", "HTTP err");
  }
  http.end();
}

// ═════════════════════════════════════════════════════════════
//  REMINDER
// ═════════════════════════════════════════════════════════════

String getNtpTimeStr() {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", ntpHour, ntpMinute);
  return String(buf);
}

void drawReminder(const char* timeStr, const char* message) {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,          DISP_W, 4, C_ORANGE);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_ORANGE);

  // 提醒图标和标题
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(DISP_W / 2 - 36, 20); tft.print("Reminder");

  // 时间显示
  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(DISP_W / 2 - 36, DISP_H / 2 - 40); tft.print(timeStr);

  // 提醒内容
  tft.setTextColor(C_GREEN); tft.setTextSize(2);
  int16_t msgX = (DISP_W - strlen(message) * 12) / 2;
  if (msgX < 12) msgX = 12;
  tft.setCursor(msgX, DISP_H / 2 + 10); tft.print(message);

  // 底部提示
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(DISP_W / 2 - 60, DISP_H - 20); tft.print("Auto-close in 30s");
}

void checkReminders() {
  if (reminderCount == 0) return;

  // 检查PC IP是否配置
  if (cfgPcIp.length() == 0) {
    Serial.println("No PC IP, skip reminder check");
    return;
  }

  // 先从PC获取时间
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    WiFiClient client;  // 使用HTTP客户端
    String url = "http://" + cfgPcIp + "/stats.json";
    http.begin(client, url);
    http.setTimeout(5000);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      JsonDocument doc;
      if (!deserializeJson(doc, payload)) {
        ntpHour     = doc["hour"]   | ntpHour;
        ntpMinute   = doc["minute"] | ntpMinute;
        ntpDay      = doc["day"]    | ntpDay;
        statsValid  = true;
        statsLoad   = doc["load"]   | statsLoad;
        statsMem    = String(doc["mem"].as<int>()) + "%";
        statsTemp   = doc["temp"]   | statsTemp;
        statsUptime = doc["uptime"] | statsUptime;
      }
    }
    http.end();
  }

  if (!statsValid) {
    Serial.println("Stats not valid, skip reminder check");
    return;
  }

  Serial.print("Check: ");
  Serial.print(ntpHour < 10 ? "0" : "");
  Serial.print(ntpHour);
  Serial.print(":");
  Serial.print(ntpMinute < 10 ? "0" : "");
  Serial.println(ntpMinute);

  // 新的一天，重置所有triggeredToday
  if (ntpDay != currentDay) {
    currentDay = ntpDay;
    Serial.println("New day, reset triggers");
    for (uint8_t i = 0; i < reminderCount; i++) {
      reminders[i].triggeredToday = false;
    }
  }

  // 检查是否有匹配的提醒
  for (uint8_t i = 0; i < reminderCount; i++) {
    Serial.print("Reminder ");
    Serial.print(i);
    Serial.print(": ");
    Serial.print(reminders[i].hour);
    Serial.print(":");
    Serial.print(reminders[i].minute);
    Serial.print(" enabled=");
    Serial.print(reminders[i].enabled);
    Serial.print(" triggered=");
    Serial.println(reminders[i].triggeredToday);

    if (reminders[i].enabled &&
        !reminders[i].triggeredToday &&
        reminders[i].hour == ntpHour &&
        reminders[i].minute == ntpMinute) {
      Serial.println("MATCH! Triggering reminder");
      reminders[i].triggeredToday = true;
      prevView = currentView;
      currentView = VIEW_REMINDER;
      reminderStartTime = millis();
      drawReminder(getNtpTimeStr().c_str(), reminders[i].message);
      return;
    }
  }
  Serial.println("No match");
}

void endReminder() {
  currentView = prevView;
  reminderStartTime = 0;
  // 根据prevView恢复显示
  switch (prevView) {
    case VIEW_EYES_NORMAL: drawNormalEyes(); break;
    case VIEW_EYES_SQUISH: drawSquishEyes(); break;
    case VIEW_CODE:        drawCodeView();   break;
    case VIEW_DRAW:        tft.fillScreen(drawBgColor); break;
    case VIEW_MONITOR:     fetchAndDrawMonitor(); break;
    default:               drawNormalEyes(); break;
  }
}

// ═════════════════════════════════════════════════════════════
//  TERMINAL
// ═════════════════════════════════════════════════════════════

void termClear() {
  for (uint8_t i = 0; i < TERM_ROWS; i++) termLines[i] = "";
  termRow = 0; termCol = 0;
}

void termDrawHeader() {
  tft.fillRect(0, 0, DISP_W, TERM_PAD_Y + 1, C_DARKBG);
  tft.setTextColor(C_ORANGE); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, 4); tft.print("clawd@mochi terminal");
  tft.drawFastHLine(0, TERM_PAD_Y, DISP_W, C_ORANGE);
}

// Prefix "clawd:~$ " in green, drawn only when the row has content
void termDrawPrefix(int16_t yy) {
  tft.setTextColor(C_GREEN); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, yy + 6);
  tft.print("clawd:~$ ");
}

#define PREFIX_PX 54   // 9 chars × 6px = 54px at textSize 1

void termDrawLine(uint8_t r) {
  const int16_t yy = TERM_PAD_Y + 4 + r * TERM_CHAR_H;
  tft.fillRect(0, yy, DISP_W, TERM_CHAR_H, C_DARKBG);
  // show prefix only on the currently active (cursor) line
  if (r == termRow) termDrawPrefix(yy);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(TERM_PAD_X + PREFIX_PX, yy + 1);
  tft.print(termLines[r]);
  if (r == termRow) {
    const int16_t cx = TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W;
    tft.fillRect(cx, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  }
}

void termDrawLastChar() {
  if (termCol == 0) return;
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  const uint8_t prev  = termCol - 1;
  // erase prev cell (had cursor block)
  tft.fillRect(baseX + prev * TERM_CHAR_W, yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(baseX + prev * TERM_CHAR_W, yy + 1);
  tft.print(termLines[termRow][prev]);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
}

void termDrawBackspace() {
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  // erase deleted char + old cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W * 2, TERM_CHAR_H - 1, C_DARKBG);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  // if line now empty, erase the prefix too
  if (termLines[termRow].length() == 0) {
    tft.fillRect(0, yy, TERM_PAD_X + PREFIX_PX, TERM_CHAR_H, C_DARKBG);
  }
}

void termFullRedraw() {
  tft.fillScreen(C_DARKBG);
  termDrawHeader();
  for (uint8_t r = 0; r < TERM_ROWS; r++) termDrawLine(r);
}

void termScroll() {
  for (uint8_t i = 0; i < TERM_ROWS - 1; i++) termLines[i] = termLines[i + 1];
  termLines[TERM_ROWS - 1] = "";
  termRow = TERM_ROWS - 1;
  termFullRedraw();
}

void termAddChar(char c) {
  if (c == '\n' || c == '\r') {
    const int16_t yy = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
    // erase cursor on current row
    tft.fillRect(TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W,
                 yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
    termRow++; termCol = 0;
    if (termRow >= TERM_ROWS) { termScroll(); return; }
    termDrawLine(termRow);  // draws prefix on new line
  } else if (c == '\b' || c == 127) {
    if (termCol > 0) {
      termCol--;
      termLines[termRow].remove(termLines[termRow].length() - 1);
      termDrawBackspace();
    }
  } else if (c >= 32 && c < 127) {
    if (termCol >= TERM_COLS) {
      termRow++; termCol = 0;
      if (termRow >= TERM_ROWS) { termScroll(); return; }
    }
    // draw prefix on first char of this line
    if (termCol == 0) termDrawPrefix(TERM_PAD_Y + 4 + termRow * TERM_CHAR_H);
    termLines[termRow] += c;
    termCol++;
    termDrawLastChar();
  }
}

// ═════════════════════════════════════════════════════════════
//  ANIMATIONS
// ═════════════════════════════════════════════════════════════

void animNormalEyes() {
  busy = true;
  const int16_t offs[] = {-16, 16, -16, 16, 0};
  for (uint8_t i = 0; i < 5; i++) { drawNormalEyes(offs[i]); delay(speedMs(80)); }
  drawNormalEyes(0, true);  delay(speedMs(100));
  drawNormalEyes(0, false); delay(speedMs(70));
  drawNormalEyes(0, true);  delay(speedMs(70));
  drawNormalEyes(0, false);
  busy = false;
}

void animSquishEyes() {
  busy = true;
  for (uint8_t i = 0; i < 3; i++) {
    drawSquishEyes(false); delay(speedMs(160));
    drawSquishEyes(true);  delay(speedMs(100));
  }
  drawSquishEyes(false);
  busy = false;
}

void animLogoReveal() {
  busy = true;
  tft.fillScreen(animBgColor);
  for (uint16_t i = 0; i < LOGO_SEG_COUNT; i++) {
    int16_t x1 = pgm_read_word(&LOGO_SEGS[i][0]);
    int16_t y1 = pgm_read_word(&LOGO_SEGS[i][1]);
    int16_t x2 = pgm_read_word(&LOGO_SEGS[i][2]);
    int16_t y2 = pgm_read_word(&LOGO_SEGS[i][3]);
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, C_WHITE);
    if (i % 4 == 0) { server.handleClient(); delay(speedMs(8)); }
  }
  drawLogoFilled(animBgColor, C_WHITE);
  delay(1500);
  busy = false;
}

// ═════════════════════════════════════════════════════════════
//  配网系统 (WiFi Setup - AP Mode)
// ═════════════════════════════════════════════════════════════

const char SETUP_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px;gap:14px;min-height:100vh}

.hdr{text-align:center;padding:10px 0}
.mascot{font-size:15px;color:#c96a3e;line-height:1.3;font-weight:bold}
.sitename{font-size:10px;color:#5a5048;margin-top:8px;letter-spacing:3px}

.sec{width:100%;max-width:390px;font-size:10px;color:#8a8278;
  letter-spacing:2px;font-weight:bold;padding:0 2px;margin-top:8px}

.form{width:100%;max-width:390px;display:flex;flex-direction:column;gap:10px}
.field{display:flex;flex-direction:column;gap:4px}
.lbl{font-size:11px;color:#c96a3e;font-weight:bold}
.in{background:#0c1018;border:1.5px solid #38343a;border-radius:9px;
  color:#d8d4dc;font-family:'Courier New',monospace;font-size:14px;
  padding:12px;outline:none}
.in:focus{border-color:#c96a3e}
select.in{cursor:pointer}

.btn{background:#c96a3e;border:none;border-radius:9px;color:#fff;
  font-family:'Courier New',monospace;font-size:14px;font-weight:bold;
  padding:14px;cursor:pointer;transition:all .12s}
.btn:active{transform:scale(.95);background:#a05530}
.btn:disabled{opacity:.5;cursor:default}

.wlist{display:flex;flex-direction:column;gap:6px}
.witem{background:#252428;border:1.5px solid #38343a;border-radius:8px;
  color:#d8d4dc;font-family:'Courier New',monospace;font-size:12px;
  padding:10px 12px;cursor:pointer;display:flex;justify-content:space-between;
  align-items:center}
.witem:active{background:#201408;border-color:#c96a3e}
.witem.selected{border-color:#c96a3e;background:#201408}
.rssi{font-size:10px;color:#6a6058}
.rssi.good{color:#28b878}
.rssi.ok{color:#c96a3e}
.rssi.bad{color:#c06040}

.tip{font-size:10px;color:#6a6058;text-align:center;padding:10px}
.err{color:#c06040;font-size:12px;text-align:center;padding:8px}

.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);
  background:#252428;border:1.5px solid #38343a;border-radius:9px;
  font-size:12px;color:#d8d4dc;padding:7px 16px;opacity:0;
  transition:opacity .18s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
.toast.err{border-color:#c06040;color:#c06040}
</style>
</head>
<body>

<div class="hdr">
  <span class="mascot">&#x2590;&#x259B;&#x2588;&#x2588;&#x2588;&#x259C;&#x258C;<br>&#x259C;&#x2588;&#x2588;&#x2588;&#x2588;&#x2588;&#x259B;<br>&#x2598;&#x2598;&nbsp;&#x259D;&#x259D;</span>
  <div class="sitename">WIFI SETUP</div>
</div>

<div class="sec">// Available Networks</div>
<div class="wlist" id="wlist"></div>

<div class="sec">// Manual Input (if not listed)</div>
<div class="form">
  <div class="field">
    <span class="lbl">SSID (Network Name)</span>
    <input class="in" id="ssid" type="text" placeholder="Enter WiFi name" autocomplete="off">
  </div>
  <div class="field">
    <span class="lbl">Password</span>
    <input class="in" id="pass" type="password" placeholder="Enter WiFi password" autocomplete="off">
  </div>
</div>

<div class="sec">// PC Monitor Settings</div>
<div class="form">
  <div class="field">
    <span class="lbl">PC IP Address (for system monitoring)</span>
    <input class="in" id="pcip" type="text" placeholder="e.g. 192.168.1.100" autocomplete="off">
  </div>
</div>

<div class="form" style="margin-top:10px">
  <button class="btn" id="saveBtn" onclick="saveConfig()">Save & Restart</button>
</div>

<div class="tip">Connect your phone/PC to WiFi: <b>Clawd-Mochi-Setup</b></div>
<div class="err" id="err"></div>
<div class="toast" id="toast"></div>

<script>
let selectedSSID = '';
let scanned = [];

// Load scanned networks
async function loadScan() {
  try {
    const r = await fetch('/scan');
    scanned = await r.json();
    renderWlist();
  } catch(e) {
    document.getElementById('err').textContent = 'Failed to load networks';
  }
}

function renderWlist() {
  const el = document.getElementById('wlist');
  el.innerHTML = '';
  if (scanned.length === 0) {
    el.innerHTML = '<div class="tip">No networks found. Enter manually.</div>';
    return;
  }
  scanned.forEach(w => {
    const div = document.createElement('div');
    div.className = 'witem';
    const rssiClass = w.rssi > -50 ? 'good' : (w.rssi > -70 ? 'ok' : 'bad');
    div.innerHTML = `<span>${w.ssid}</span><span class="rssi ${rssiClass}">${w.rssi}dBm</span>`;
    div.onclick = () => selectWifi(w.ssid, div);
    el.appendChild(div);
  });
}

function selectWifi(ssid, el) {
  selectedSSID = ssid;
  document.getElementById('ssid').value = ssid;
  document.querySelectorAll('.witem').forEach(e => e.classList.remove('selected'));
  el.classList.add('selected');
}

function toast(msg, isError=false) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast show' + (isError ? ' err' : '');
  setTimeout(() => el.classList.remove('show'), 2000);
}

async function saveConfig() {
  const ssid = document.getElementById('ssid').value.trim();
  const pass = document.getElementById('pass').value;
  const pcip = document.getElementById('pcip').value.trim();

  if (!ssid) {
    toast('Please enter SSID', true);
    return;
  }

  const btn = document.getElementById('saveBtn');
  btn.disabled = true;
  btn.textContent = 'Saving...';

  try {
    const r = await fetch('/save?ssid=' + encodeURIComponent(ssid) +
      '&pass=' + encodeURIComponent(pass) +
      '&pcip=' + encodeURIComponent(pcip));
    const j = await r.json();
    if (j.ok) {
      toast('Saved! Restarting...');
      btn.textContent = 'Restarting...';
    } else {
      toast(j.error || 'Failed', true);
      btn.disabled = false;
      btn.textContent = 'Save & Restart';
    }
  } catch(e) {
    toast('Connection lost', true);
    btn.disabled = false;
    btn.textContent = 'Save & Restart';
  }
}

loadScan();
</script>
</body>
</html>
)rawhtml";

// 配网服务器路由
void setupRouteRoot() {
  if (setupServer) {
    setupServer->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    setupServer->send_P(200, "text/html", SETUP_HTML);
  }
}

void setupRouteScan() {
  String json = "[";
  for (uint8_t i = 0; i < scanCount; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"";
    json += scanSSID[i];
    json += "\",\"rssi\":";
    json += scanRSSI[i];
    json += "}";
  }
  json += "]";
  if (setupServer) setupServer->send(200, "application/json", json);
}

void setupRouteSave() {
  if (!setupServer) return;

  String ssid = setupServer->arg("ssid");
  String pass = setupServer->arg("pass");
  String pcip = setupServer->arg("pcip");

  if (ssid.length() == 0) {
    setupServer->send(400, "application/json", "{\"error\":\"SSID required\"}");
    return;
  }

  // 验证PC IP格式（简单验证）
  if (pcip.length() > 0 && pcip.indexOf('.') < 0) {
    setupServer->send(400, "application/json", "{\"error\":\"Invalid PC IP\"}");
    return;
  }

  Serial.println("Saving config:");
  Serial.println(String("  SSID: ") + ssid);
  Serial.println(String("  PC IP: ") + pcip);

  saveConfig(ssid, pass, pcip);

  setupServer->send(200, "application/json", "{\"ok\":true}");

  // 延迟重启
  delay(500);
  ESP.restart();
}

// 启动配网模式
void startSetupMode() {
  inSetupMode = true;
  Serial.println("Entering setup mode (AP)");

  // 显示配网界面
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(DISP_W/2 - 60, 16); tft.print("Setup Mode");
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(12, 50); tft.print("Connect to WiFi:");
  tft.setTextColor(C_GREEN); tft.setTextSize(2);
  tft.setCursor(12, 78); tft.print("Clawd-Mochi-Setup");
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(12, 106); tft.print("Open browser:");
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(12, 134); tft.print("192.168.4.1");
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(12, 164); tft.print("Enter WiFi password & PC IP");

  // 启动AP模式
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_NAME);  // 无密码开放热点

  Serial.println(String("AP started: ") + AP_NAME);
  Serial.println(String("AP IP: ") + WiFi.softAPIP().toString());

  // 创建配网服务器
  setupServer = new WebServer(80);
  setupServer->on("/",        HTTP_GET, setupRouteRoot);
  setupServer->on("/scan",    HTTP_GET, setupRouteScan);
  setupServer->on("/save",    HTTP_GET, setupRouteSave);
  setupServer->begin();

  Serial.println("Setup server started");
}

// 处理配网模式循环
void handleSetupLoop() {
  if (setupServer) setupServer->handleClient();
}

// 尝试连接WiFi（Station模式）
bool tryConnectWiFi() {
  if (cfgSSID.length() == 0) return false;

  Serial.println(String("Connecting to WiFi: ") + cfgSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSSID.c_str(), cfgPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > CONNECT_TIMEOUT) {
      Serial.println("WiFi connection timeout");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.println(String("IP: ") + WiFi.localIP().toString());
  return true;
}

// 显示WiFi连接状态
void showConnectingScreen() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(DISP_W/2 - 60, DISP_H/2 - 12);
  tft.print("Connecting...");
}

void showConnectFailedScreen() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(DISP_W/2 - 70, 16); tft.print("Connection Failed");
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(12, 50); tft.print("SSID: ");
  tft.print(cfgSSID);
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(12, 80); tft.print("Check password or reset");
  tft.setCursor(12, 96); tft.print("Pull GPIO5 LOW at startup");
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(DISP_W/2 - 50, 120); tft.print("Retrying...");
}

void showConnectedScreen() {
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_GREEN); tft.setTextSize(2);
  tft.setCursor(DISP_W/2 - 70, 16); tft.print("WiFi Connected");
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(12, 50); tft.print("SSID: ");
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(60, 50); tft.print(cfgSSID);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(12, 74); tft.print("IP: ");
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(60, 74); tft.print(WiFi.localIP());
  if (cfgPcIp.length() > 0) {
    tft.setTextColor(C_WHITE); tft.setTextSize(1);
    tft.setCursor(12, 102); tft.print("PC Monitor: ");
    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(90, 102); tft.print(cfgPcIp);
  }
  tft.setTextColor(C_MUTED); tft.setTextSize(1);
  tft.setCursor(DISP_W/2 - 80, 140); tft.print("Starting web server...");
}

// ═════════════════════════════════════════════════════════════
//  WEB PAGE (Normal Operation)
// ═════════════════════════════════════════════════════════════

// 设置页面HTML
const char SETTINGS_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi Settings</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px;gap:14px;min-height:100vh}

.hdr{text-align:center;padding:10px 0}
.mascot{font-size:15px;color:#c96a3e;line-height:1.3;font-weight:bold}
.sitename{font-size:10px;color:#5a5048;margin-top:8px;letter-spacing:3px}

.sec{width:100%;max-width:390px;font-size:10px;color:#8a8278;
  letter-spacing:2px;font-weight:bold;padding:0 2px;margin-top:8px}

.form{width:100%;max-width:390px;display:flex;flex-direction:column;gap:10px}
.field{display:flex;flex-direction:column;gap:4px}
.lbl{font-size:11px;color:#c96a3e;font-weight:bold}
.in{background:#0c1018;border:1.5px solid #38343a;border-radius:9px;
  color:#d8d4dc;font-family:'Courier New',monospace;font-size:14px;
  padding:12px;outline:none}
.in:focus{border-color:#c96a3e}

.btn{background:#c96a3e;border:none;border-radius:9px;color:#fff;
  font-family:'Courier New',monospace;font-size:14px;font-weight:bold;
  padding:14px;cursor:pointer;transition:all .12s}
.btn:active{transform:scale(.95);background:#a05530}
.btn:disabled{opacity:.5;cursor:default}
.btn.back{background:#38343a}

.tip{font-size:10px;color:#6a6058;text-align:center;padding:10px}
.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);
  background:#252428;border:1.5px solid #38343a;border-radius:9px;
  font-size:12px;color:#d8d4dc;padding:7px 16px;opacity:0;
  transition:opacity .18s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
.toast.err{border-color:#c06040;color:#c06040}
</style>
</head>
<body>

<div class="hdr">
  <span class="mascot">&#x2590;&#x259B;&#x2588;&#x2588;&#x2588;&#x259C;&#x258C;<br>&#x259C;&#x2588;&#x2588;&#x2588;&#x2588;&#x2588;&#x259B;<br>&#x2598;&#x2598;&nbsp;&#x259D;&#x259D;</span>
  <div class="sitename">SETTINGS</div>
</div>

<div class="sec">// PC Monitor Configuration</div>
<div class="form">
  <div class="field">
    <span class="lbl">PC IP Address</span>
    <input class="in" id="pcip" type="text" placeholder="e.g. 192.168.1.100" autocomplete="off">
  </div>
  <button class="btn" onclick="savePcIp()">Save PC IP</button>
</div>

<div class="sec">// WiFi Reset</div>
<div class="form">
  <button class="btn" style="background:#c06040" onclick="confirmReset()">Reset WiFi Config</button>
  <div class="tip">Reset will clear WiFi settings and restart into setup mode</div>
</div>

<div class="form" style="margin-top:20px">
  <button class="btn back" onclick="goBack()">&#8592; Back to Controller</button>
</div>

<div class="toast" id="toast"></div>

<script>
let currentPcIp = '';

async function loadSettings() {
  try {
    const r = await fetch('/settings');
    const j = await r.json();
    currentPcIp = j.pcip || '';
    document.getElementById('pcip').value = currentPcIp;
  } catch(e) {
    toast('Failed to load settings', true);
  }
}

function toast(msg, isError=false) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast show' + (isError ? ' err' : '');
  setTimeout(() => el.classList.remove('show'), 2000);
}

async function savePcIp() {
  const pcip = document.getElementById('pcip').value.trim();
  if (pcip && pcip.indexOf('.') < 0) {
    toast('Invalid IP format', true);
    return;
  }
  try {
    const r = await fetch('/settings?pcip=' + encodeURIComponent(pcip), {method:'POST'});
    const j = await r.json();
    if (j.ok) {
      toast('PC IP saved!');
      currentPcIp = pcip;
    } else {
      toast(j.error || 'Failed', true);
    }
  } catch(e) {
    toast('Connection error', true);
  }
}

function confirmReset() {
  if (confirm('Are you sure you want to reset WiFi configuration?\nDevice will restart into setup mode.')) {
    doReset();
  }
}

async function doReset() {
  try {
    const r = await fetch('/reset', {method:'POST'});
    const j = await r.json();
    if (j.ok) {
      toast('Resetting...');
      setTimeout(() => location.reload(), 2000);
    }
  } catch(e) {
    toast('Device is restarting...');
  }
}

function goBack() {
  window.location.href = '/';
}

loadSettings();
</script>
</body>
</html>
)rawhtml";

const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px 52px;gap:14px;min-height:100vh}

.hdr{text-align:center;padding:2px 0 4px}
.mascot{font-size:15px;color:#c96a3e;line-height:1.3;font-weight:bold;
  font-family:'Courier New',monospace;display:block;letter-spacing:1px}
.sitename{font-size:10px;color:#5a5048;margin-top:8px;letter-spacing:3px}

.sec{width:100%;max-width:390px;font-size:10px;color:#8a8278;
  letter-spacing:2px;font-weight:bold;padding:0 2px}

/* Busy bar */
.busy{width:100%;max-width:390px;height:2px;background:#2e2a28;
  border-radius:1px;overflow:hidden;opacity:0;transition:opacity .2s}
.busy.show{opacity:1}
.busy-i{height:100%;width:30%;background:#c96a3e;border-radius:1px;
  animation:sl 1s linear infinite}
@keyframes sl{0%{margin-left:-30%}100%{margin-left:100%}}

/* Controls */
.ctrl{display:flex;gap:8px;width:100%;max-width:390px}
.cbtn{flex:1;background:#252428;border:1.5px solid #38343a;border-radius:10px;
  color:#b8b4ac;font-family:'Courier New',monospace;font-size:11px;font-weight:bold;
  padding:12px 4px;cursor:pointer;text-align:center;transition:all .12s}
.cbtn:active:not(:disabled){transform:scale(.94)}
.cbtn:disabled{opacity:.3;cursor:default}
.cbtn.on{border-color:#c96a3e;color:#c96a3e;background:#201408}
.cbtn.dim{border-color:#2e2a28;color:#4a4540}

/* View grid */
.vgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;width:100%;max-width:390px}
.vbtn{background:#252428;border:1.5px solid #38343a;border-radius:12px;
  color:#d8d4cc;font-family:'Courier New',monospace;
  padding:14px 6px 10px;cursor:pointer;text-align:center;
  transition:all .12s;user-select:none}
.vbtn:active:not(:disabled){transform:scale(.94)}
.vbtn:disabled{opacity:.3;cursor:default}
.vbtn .ic{font-size:20px;display:block;margin-bottom:4px;line-height:1;color:#c96a3e}
.vbtn .nm{font-size:12px;font-weight:bold;color:#e8e4dc}
.vbtn .ht{font-size:9px;color:#8a8278;margin-top:3px}
.vbtn.active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="2"].active{border-color:#4a8acd;background:#0c1628}
.vbtn[data-v="3"].active{border-color:#38343a;background:#201c18}
.vbtn[data-v="4"].active{border-color:#28b878;background:#0c1e12}

/* Speed slider */
.speed-row{width:100%;max-width:390px;display:flex;align-items:center;gap:10px}
.sl{font-size:10px;color:#6a6058;white-space:nowrap;min-width:36px}
input[type=range]{flex:1;accent-color:#c96a3e;cursor:pointer;height:20px}
.sv{font-size:11px;color:#c96a3e;min-width:44px;text-align:right;font-weight:bold}

/* Terminal */
.twrap{width:100%;max-width:390px;display:none;flex-direction:column;gap:8px}
.twrap.open{display:flex}
.thdr{display:flex;justify-content:space-between;align-items:center}
.tttl{font-size:11px;color:#28b878;letter-spacing:1px;font-weight:bold}
.tx{background:#0c1e12;border:2px solid #1a4828;border-radius:9px;
  color:#28b878;font-family:'Courier New',monospace;font-size:13px;
  font-weight:bold;padding:10px 18px;cursor:pointer}
.tx:active{background:#081410}
.trow{display:flex;gap:6px}
.tin{flex:1;background:#0c1018;border:1.5px solid #1a2820;border-radius:9px;
  color:#40d880;font-family:'Courier New',monospace;font-size:15px;
  padding:11px;outline:none}
.tin::placeholder{color:#2a3828}
.tgo{background:#1a9060;border:none;border-radius:9px;color:#fff;
  font-family:'Courier New',monospace;font-size:22px;font-weight:bold;
  padding:11px 16px;cursor:pointer;min-width:52px}
.tgo:active{background:#0f6040}

/* Canvas */
.cwrap{width:100%;max-width:390px;background:#222028;border:1.5px solid #38343a;
  border-radius:12px;padding:12px;flex-direction:column;gap:10px;display:none}
.cwrap.open{display:flex}
.crow{display:flex;gap:8px}
.ci{display:flex;flex-direction:column;align-items:center;gap:4px;flex:1}
.cl{font-size:10px;color:#7a7068;letter-spacing:1px;font-weight:bold}
.cs{width:100%;height:38px;border-radius:7px;border:1.5px solid #38343a;cursor:pointer;padding:0}
.dacts{display:flex;gap:7px}
.db{flex:1;background:#1c1820;border:1.5px solid #38343a;border-radius:9px;
  color:#c0bab8;font-family:'Courier New',monospace;font-size:11px;
  font-weight:bold;padding:11px 4px;cursor:pointer;transition:all .12s}
.db:active{transform:scale(.95);background:#281838}
.db.hi{border-color:#c96a3e;color:#c96a3e}
canvas{width:100%;border-radius:8px;border:1.5px solid #38343a;
  touch-action:none;cursor:crosshair;display:block}

/* Reminder */
.rwrap{width:100%;max-width:390px;background:#222028;border:1.5px solid #38343a;
  border-radius:12px;padding:12px;display:flex;flex-direction:column;gap:10px}
.rhdr{display:flex;justify-content:space-between;align-items:center}
.rttl{font-size:11px;color:#c96a3e;letter-spacing:1px;font-weight:bold}
.rlist{display:flex;flex-direction:column;gap:8px}
.ritem{display:flex;align-items:center;gap:8px;padding:8px 10px;
  background:#1c1820;border-radius:8px;border:1.5px solid #38343a}
.rtime{font-size:14px;color:#28b878;font-weight:bold;font-family:'Courier New',monospace}
.rmsg{font-size:12px;color:#d8d4cc;flex:1}
.rsw{width:36px;height:18px;background:#38343a;border-radius:9px;
  position:relative;cursor:pointer;transition:background .2s}
.rsw.on{background:#28b878}
.rsw::after{content:'';position:absolute;width:14px;height:14px;background:#fff;
  border-radius:7px;top:2px;left:2px;transition:left .2s}
.rsw.on::after{left:20px}
.rdel{font-size:12px;color:#c96a3e;cursor:pointer;padding:2px 6px;
  border-radius:4px;border:1.5px solid #c96a3e;background:none}
.rdel:hover{background:#201408}
.radd{display:flex;gap:8px;align-items:center}
.rin{flex:1;background:#0c1018;border:1.5px solid #38343a;border-radius:8px;
  color:#d8d4cc;font-family:'Courier New',monospace;font-size:12px;
  padding:10px;outline:none}
.rin:focus{border-color:#c96a3e}
.rb{background:#1c1820;border:1.5px solid #38343a;border-radius:8px;
  color:#d8d4cc;font-size:11px;padding:10px 12px;cursor:pointer}
.rb:hover{background:#281838}
.rb.add{border-color:#c96a3e;color:#c96a3e}

/* View select dropdown */
.vsel{background:#252428;border:1.5px solid #38343a;border-radius:12px;
  color:#d8d4cc;font-family:'Courier New',monospace;
  padding:14px 6px 10px;cursor:pointer;text-align:center;
  transition:all .12s;user-select:none;appearance:none;
  text-align-last:center;font-size:12px;font-weight:bold}
.vsel:hover{border-color:#c96a3e;background:#201408}
.vsel:focus{outline:none;border-color:#c96a3e}

/* Toast */
.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);
  background:#252428;border:1.5px solid #38343a;border-radius:9px;
  font-size:12px;color:#d8d4cc;padding:7px 16px;opacity:0;
  transition:opacity .18s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
</style>
</head>
<body>

<div class="hdr">
  <span class="mascot">&#x2590;&#x259B;&#x2588;&#x2588;&#x2588;&#x259C;&#x258C;<br>&#x259C;&#x2588;&#x2588;&#x2588;&#x2588;&#x2588;&#x259B;<br>&#x2598;&#x2598;&nbsp;&#x259D;&#x259D;</span>
  <div class="sitename">CLAWD &middot; MOCHI &middot; CONTROLLER</div>
</div>

<div class="busy" id="busy"><div class="busy-i"></div></div>

<div class="sec">// controls</div>
<div class="ctrl">
  <button class="cbtn on" id="blBtn" onclick="toggleBL()">&#9728; display on</button>
  <button class="cbtn" onclick="openSettings()">&#9881; settings</button>
</div>

<div class="sec">// views</div>
<div class="vgrid">
  <button class="vbtn active" data-v="0" onclick="setView(0)">
    <span class="ic">&#9632; &#9632;</span>
    <span class="nm">Normal eyes</span>
    <span class="ht">wiggle + blink</span>
  </button>
  <button class="vbtn" data-v="2" onclick="setView(2)">
    <span class="ic">{ }</span>
    <span class="nm">Claude Code</span>
    <span class="ht">opens terminal</span>
  </button>
  <button class="vbtn" data-v="3" onclick="toggleCanvas()">
    <span class="ic">&#11035;</span>
    <span class="nm">Canvas</span>
    <span class="ht">draw on display</span>
  </button>
  <button class="vbtn" data-v="4" onclick="setView(4)">
    <span class="ic">&#9881;</span>
    <span class="nm">Monitor</span>
    <span class="ht">system stats</span>
  </button>
  <select class="vsel" onchange="sendCmd(this.value); this.value=''">
    <option value="">— Static Face —</option>
    <option value="face_wuyu">无语</option>
    <option value="face_wenhao">问号</option>
    <option value="face_gantanhao">感叹号</option>
    <option value="face_angry">生气</option>
    <option value="face_yes">对 √</option>
    <option value="face_X">错 ×</option>
    <option value="face_glass">墨镜</option>
  </select>
  <select class="vsel" onchange="sendCmd(this.value); this.value=''">
    <option value="">— Animated Face —</option>
    <option value="anim_jiyanjing">挤眼睛</option>
    <option value="anim_yun">晕晕</option>
    <option value="anim_close">闭眼睛</option>
    <option value="anim_dead">死掉了</option>
    <option value="anim_dian">等等</option>
    <option value="anim_smile">笑笑</option>
    <option value="anim_look">看你</option>
    <option value="anim_hart">心跳</option>
    <option value="anim_zzz">睡着了</option>
    <option value="anim_ganga">尴尬</option>
  </select>
</div>

<div class="sec">// mochi expression</div>
<div class="vgrid">
  <button class="vbtn" onclick="setExpr('idle')"><span class="ic">&#9632; &#9632;</span><span class="nm">Idle</span><span class="ht">orange standby</span></button>
  <button class="vbtn" onclick="setExpr('thinking')"><span class="ic">..</span><span class="nm">Thinking</span><span class="ht">Codex working</span></button>
  <button class="vbtn" onclick="setExpr('proud')"><span class="ic">^^</span><span class="nm">Done</span><span class="ht">task complete</span></button>
  <button class="vbtn" onclick="setExpr('angry')"><span class="ic">&gt;&lt;</span><span class="nm">Error</span><span class="ht">needs attention</span></button>
  <button class="vbtn" onclick="setExpr('happy')"><span class="ic">--</span><span class="nm">Happy</span><span class="ht">smooth blink</span></button>
  <button class="vbtn" onclick="setExpr('sleep')"><span class="ic">zz</span><span class="nm">Sleep</span><span class="ht">quiet mode</span></button>
</div>
<div class="ctrl">
  <select class="vsel" id="idleExpr" onchange="setIdleExpr(this.value)">
    <option value="idle">Idle standby</option>
    <option value="sleep">Sleep standby</option>
    <option value="thinking">Thinking standby</option>
    <option value="happy">Happy standby</option>
  </select>
  <select class="vsel" id="refreshSel" onchange="setRefresh(this.value)">
    <option value="700">refresh slow</option>
    <option value="400">refresh normal</option>
    <option value="250" selected>refresh smooth</option>
    <option value="150">refresh fast</option>
  </select>
</div>
<div class="speed-row">
  <span class="sl">dim</span>
  <input type="range" id="bright" min="20" max="255" value="255" step="5" oninput="setBrightness(this.value)">
  <span class="sv" id="brightV">255</span>
</div>

<div class="sec">// speed</div>
<div class="speed-row">
  <span class="sl">slow</span>
  <input type="range" id="spd" min="1" max="3" value="1" step="1" oninput="setSpeed(this.value)">
  <span class="sv" id="spdV">slow</span>
</div>

<div class="ctrl">
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">BACKGROUND</span>
    <input type="color" class="cs" id="bgCol" value="#aa4818" oninput="onBgChange(this.value)">
  </div>
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">PEN COLOR</span>
    <input type="color" class="cs" id="penCol" value="#000000">
  </div>
</div>

<div class="sec">// terminal</div>
<div class="twrap" id="twrap">
  <div class="thdr">
    <span class="tttl">&#9658; clawd:~$</span>
    <button class="tx" onclick="closeTerm()">&#x2715; exit terminal</button>
  </div>
  <div class="trow">
    <input class="tin" id="tin" type="text" placeholder="type here..."
           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
    <button class="tgo" onclick="termEnter()">&#8629;</button>
  </div>
</div>

<div class="cwrap" id="cwrap">
  <div class="dacts">
    <button class="db hi" onclick="clearAll()">&#11035; clear</button>
    <button class="db" style="border-color:#28b878;color:#28b878" onclick="toggleCanvas()">&#10003; done</button>
  </div>
  <canvas id="cvs" width="240" height="240"></canvas>
</div>

<div class="sec">// reminders</div>
<div class="rwrap" id="rwrap">
  <div class="rhdr">
    <span class="rttl">&#128276; Reminders</span>
    <button class="rb" onclick="loadReminders()">Refresh</button>
  </div>
  <div class="rlist" id="rlist"></div>
  <div class="radd">
    <input class="rin" id="rtime" type="time" value="08:00">
    <input class="rin" id="rmsg" type="text" placeholder="message (max 20)" maxlength="20">
    <button class="rb add" onclick="addReminder()">+ Add</button>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
let activeView  = 0;
let termOpen    = false;
let canvasOpen  = false;
let blOn        = true;
let isBusy      = false;
let drawing     = false;
let lastX = 0, lastY = 0;
let tt;

const spdLabels = ['','slow','normal','fast'];

// ── Toast ──────────────────────────────────────────────────────
function toast(msg, ok=true) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.borderColor = ok ? '#28b878' : '#c96a3e';
  el.classList.add('show');
  clearTimeout(tt);
  tt = setTimeout(() => el.classList.remove('show'), 1300);
}

// ── Busy ────────────────────────────────────────────────────────
function setBusy(b) {
  isBusy = b;
  document.getElementById('busy').classList.toggle('show', b);
  const locked = b || termOpen;
  document.querySelectorAll('.vbtn').forEach(el => {
    // when canvas open, keep canvas btn (data-v=3) active so user can exit
    el.disabled = canvasOpen ? parseInt(el.dataset.v) !== 3 : locked;
  });
  document.querySelectorAll('.lbtn').forEach(el => el.disabled = locked || canvasOpen);
  document.querySelectorAll('.cbtn').forEach(el => {
    if (el.id !== 'blBtn') el.disabled = locked;
  });
}

// ── HTTP ────────────────────────────────────────────────────────
async function req(path) {
  try { const r = await fetch(path); return r.ok; }
  catch(e) { toast('no connection', false); return false; }
}

async function waitNotBusy() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch('/state');
      const j = await r.json();
      if (!j.busy) return;
    } catch(e) {}
    await new Promise(r => setTimeout(r, 150));
  }
}

// ── Background colour ───────────────────────────────────────────
async function onBgChange(hex) {
  if (canvasOpen) {
    await req('/draw/clear?bg=' + encodeURIComponent(hex));
  } else {
    await req('/redraw?bg=' + encodeURIComponent(hex));
  }
  redrawCanvas(hex);
}

// ── Speed ───────────────────────────────────────────────────────
async function setSpeed(v) {
  document.getElementById('spdV').textContent = spdLabels[v];
  await req('/speed?v=' + v);
}

// ── Mochi expression controls ─────────────────────────────────
async function setExpr(name) {
  if (!await req('/api/expression?name=' + encodeURIComponent(name))) return;
  activeView = 6;
  termOpen = false;
  canvasOpen = false;
  document.getElementById('twrap').classList.remove('open');
  document.getElementById('cwrap').classList.remove('open');
  document.querySelectorAll('.vbtn').forEach(b => b.classList.remove('active'));
  toast('expression: ' + name);
}

async function setRefresh(ms) {
  await req('/refresh?ms=' + encodeURIComponent(ms));
  toast('refresh ' + ms + 'ms');
}

async function setBrightness(v) {
  document.getElementById('brightV').textContent = v;
  await req('/brightness?v=' + encodeURIComponent(v));
}

async function setIdleExpr(name) {
  await req('/idle?name=' + encodeURIComponent(name));
  toast('idle: ' + name);
}

// ── Views ───────────────────────────────────────────────────────
async function setView(v) {
  if (isBusy || termOpen || canvasOpen) return;
  if (v === 3) { toggleCanvas(); return; }  // canvas button in grid
  // keys map: 0->w, 2->d, 4->m (no squish eyes anymore)
  const keyMap = {0:'w', 2:'d', 4:'m'};
  if (!keyMap[v]) return;
  if (!await req('/cmd?k=' + keyMap[v])) return;
  activeView = v;
  document.querySelectorAll('.vbtn').forEach(b =>
    b.classList.toggle('active', parseInt(b.dataset.v) === v));
  if (v === 2) {
    termOpen = true;
    document.getElementById('twrap').classList.add('open');
    setBusy(false);   // re-run to apply termOpen lock
    setBusy(false);
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    const cvb = document.getElementById('cvBtn'); if (cvb) cvb.disabled = true;
    document.getElementById('tin').focus();
    toast('terminal open');
    return;
  }
  setBusy(true);
  await waitNotBusy();
  setBusy(false);
}

// ── Send command (for dropdown selects) ──────────────────────────
async function sendCmd(cmd) {
  if (!cmd || isBusy) return;
  setBusy(true);
  await req('/cmd?k=' + cmd);
  await waitNotBusy();
  setBusy(false);
}

// ── Logo animations (kept for startup, not exposed in UI) ──────

// ── Settings ───────────────────────────────────────────────────
function openSettings() {
  window.location.href = '/settings';
}

// ── Backlight ───────────────────────────────────────────────────
async function toggleBL() {
  blOn = !blOn;
  await req('/backlight?on=' + (blOn ? 1 : 0));
  const b = document.getElementById('blBtn');
  b.textContent = blOn ? '\u2600 display on' : '\u25cb display off';
  b.classList.toggle('on', blOn);
  b.classList.toggle('dim', !blOn);
}

// ── Canvas toggle ───────────────────────────────────────────────
async function toggleCanvas() {
  canvasOpen = !canvasOpen;
  document.getElementById('cwrap').classList.toggle('open', canvasOpen);
  const b = document.getElementById('cvBtn');
  if (b) { b.classList.toggle('on', canvasOpen); b.textContent = canvasOpen ? '\u2b1b canvas on' : '\u2b1b canvas'; }
  // highlight the canvas vbtn (data-v=3) in the grid
  document.querySelectorAll('.vbtn').forEach(btn =>
    btn.classList.toggle('active', canvasOpen && parseInt(btn.dataset.v) === 3));
  await req('/canvas?on=' + (canvasOpen ? 1 : 0));
  if (canvasOpen) {
    const bg = document.getElementById('bgCol').value;
    redrawCanvas(bg);
    await req('/draw/clear?bg=' + encodeURIComponent(bg));
    // lock all other buttons
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    toast('canvas active');
  } else {
    setBusy(false);   // re-evaluate locks
    toast('canvas off');
  }
}

// ── Terminal ────────────────────────────────────────────────────
const tin = document.getElementById('tin');
let lastVal = '';
tin.addEventListener('input', async () => {
  const cur = tin.value, prev = lastVal;
  if (cur.length > prev.length) {
    await req('/char?c=' + encodeURIComponent(cur[cur.length - 1]));
  } else if (cur.length < prev.length) {
    await req('/char?c=%08');
  }
  lastVal = cur;
});
async function termEnter() {
  await req('/char?c=%0A');
  tin.value = ''; lastVal = ''; tin.focus();
}
tin.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); termEnter(); }
});
async function closeTerm() {
  await req('/cmd?k=q');
  termOpen = false;
  document.getElementById('twrap').classList.remove('open');
  setBusy(false);
  toast('terminal closed');
}

// ── Canvas drawing — send full stroke on finger lift ────────────
const cvs = document.getElementById('cvs');
const ctx = cvs.getContext('2d');
let strokePts = [];

function getPos(e) {
  const r = cvs.getBoundingClientRect();
  const sx = cvs.width / r.width, sy = cvs.height / r.height;
  const s = e.touches ? e.touches[0] : e;
  return { x: (s.clientX - r.left) * sx, y: (s.clientY - r.top) * sy };
}

function redrawCanvas(hex) {
  ctx.fillStyle = hex;
  ctx.fillRect(0, 0, cvs.width, cvs.height);
}

function startDraw(e) {
  e.preventDefault();
  drawing = true;
  strokePts = [];
  const p = getPos(e); lastX = p.x; lastY = p.y;
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  // draw dot on canvas preview only — no display send yet
  ctx.beginPath(); ctx.arc(p.x, p.y, 2, 0, Math.PI * 2);
  ctx.fillStyle = document.getElementById('penCol').value; ctx.fill();
}
function moveDraw(e) {
  if (!drawing) return; e.preventDefault();
  const p = getPos(e);
  ctx.beginPath(); ctx.moveTo(lastX, lastY); ctx.lineTo(p.x, p.y);
  ctx.strokeStyle = document.getElementById('penCol').value;
  ctx.lineWidth = 4; ctx.lineCap = 'round'; ctx.stroke();
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  lastX = p.x; lastY = p.y;
}
async function endDraw(e) {
  if (!drawing) return; drawing = false;
  if (!canvasOpen || strokePts.length < 1) return;
  const pen = document.getElementById('penCol').value.replace('#', '');
  const pts = strokePts.map(p => p.x + ',' + p.y).join(';');
  await req('/draw/stroke?pen=' + pen + '&pts=' + encodeURIComponent(pts));
  strokePts = [];
}

cvs.addEventListener('mousedown',  startDraw);
cvs.addEventListener('mousemove',  moveDraw);
cvs.addEventListener('mouseup',    endDraw);
cvs.addEventListener('mouseleave', endDraw);
cvs.addEventListener('touchstart', startDraw, {passive:false});
cvs.addEventListener('touchmove',  moveDraw,  {passive:false});
cvs.addEventListener('touchend',   endDraw);

// Clear = clear both web canvas and display
async function clearAll() {
  const bg = document.getElementById('bgCol').value;
  redrawCanvas(bg);
  await req('/draw/clear?bg=' + encodeURIComponent(bg));
  toast('cleared');
}

// ── Reminder functions ──────────────────────────────────────────
let reminders = [];

async function loadReminders() {
  try {
    const r = await fetch('/reminder');
    reminders = await r.json();
    renderReminders();
  } catch(e) {
    toast('Failed to load reminders', false);
  }
}

function renderReminders() {
  const list = document.getElementById('rlist');
  list.innerHTML = '';
  reminders.forEach((rm, i) => {
    const div = document.createElement('div');
    div.className = 'ritem';
    div.innerHTML = `
      <span class="rtime">${String(rm.hour).padStart(2,'0')}:${String(rm.minute).padStart(2,'0')}</span>
      <span class="rmsg">${rm.message}</span>
      <div class="rsw ${rm.enabled ? 'on' : ''}" onclick="toggleReminder(${i})"></div>
      <button class="rdel" onclick="deleteReminder(${i})">X</button>
    `;
    list.appendChild(div);
  });
}

async function addReminder() {
  const timeInput = document.getElementById('rtime').value;
  const msgInput = document.getElementById('rmsg').value;
  if (!timeInput || !msgInput) {
    toast('Please enter time and message', false);
    return;
  }
  const [h, m] = timeInput.split(':').map(Number);
  try {
    const r = await fetch(`/reminder?hour=${h}&minute=${m}&message=${encodeURIComponent(msgInput)}`, {method: 'POST'});
    const j = await r.json();
    if (j.ok) {
      toast('Reminder added');
      loadReminders();
      document.getElementById('rmsg').value = '';
    } else {
      toast(j.error || 'Failed', false);
    }
  } catch(e) {
    toast('Failed to add', false);
  }
}

async function toggleReminder(id) {
  const enabled = !reminders[id].enabled;
  try {
    await fetch(`/reminder?id=${id}&enabled=${enabled}`, {method: 'PUT'});
    reminders[id].enabled = enabled;
    renderReminders();
    toast(enabled ? 'Enabled' : 'Disabled');
  } catch(e) {
    toast('Failed', false);
  }
}

async function deleteReminder(id) {
  try {
    await fetch(`/reminder?id=${id}`, {method: 'DELETE'});
    reminders.splice(id, 1);
    renderReminders();
    toast('Deleted');
  } catch(e) {
    toast('Failed to delete', false);
  }
}

// Init: sync speed and backlight from ESP32, reset bg to default
(async () => {
  try {
    const r = await fetch('/state');
    const j = await r.json();
    // Sync speed
    const spd = j.speed || 1;
    document.getElementById('spd').value = spd;
    document.getElementById('spdV').textContent = spdLabels[spd];
    // Sync backlight
    if (j.bl === false) {
      blOn = false;
      const b = document.getElementById('blBtn');
      b.textContent = '\u25cb display off';
      b.classList.remove('on'); b.classList.add('dim');
    }
    if (j.refreshMs) document.getElementById('refreshSel').value = String(j.refreshMs);
    if (j.brightness) {
      document.getElementById('bright').value = j.brightness;
      document.getElementById('brightV').textContent = j.brightness;
    }
    if (j.idle) document.getElementById('idleExpr').value = j.idle;
  } catch(e) {}
  // Always reset bg picker to default orange on page load
  document.getElementById('bgCol').value = '#aa4818';
  redrawCanvas('#aa4818');
  // Load reminders on init
  loadReminders();
})();
</script>
</body>
</html>
)rawhtml";

// ═════════════════════════════════════════════════════════════
//  WEB ROUTES
// ═════════════════════════════════════════════════════════════

void routeRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

const char* mochiExprName(MochiExpression expr) {
  switch (expr) {
    case MOCHI_HAPPY:    return "happy";
    case MOCHI_SLEEP:    return "sleep";
    case MOCHI_THINKING: return "thinking";
    case MOCHI_PROUD:    return "proud";
    case MOCHI_SAD:      return "sad";
    case MOCHI_ANGRY:    return "angry";
    case MOCHI_WINK:     return "wink";
    default:             return "idle";
  }
}

MochiExpression parseMochiExpr(String name) {
  name.toLowerCase();
  if (name == "happy") return MOCHI_HAPPY;
  if (name == "sleep") return MOCHI_SLEEP;
  if (name == "thinking" || name == "work" || name == "working") return MOCHI_THINKING;
  if (name == "proud" || name == "done" || name == "complete") return MOCHI_PROUD;
  if (name == "sad") return MOCHI_SAD;
  if (name == "angry" || name == "error" || name == "failed") return MOCHI_ANGRY;
  if (name == "wink") return MOCHI_WINK;
  return MOCHI_IDLE;
}

MochiExpression exprForStatus(String status) {
  status.toLowerCase();
  if (status == "working" || status == "work" || status == "busy" || status == "thinking") return MOCHI_THINKING;
  if (status == "done" || status == "complete" || status == "success" || status == "ok") return MOCHI_PROUD;
  if (status == "error" || status == "failed" || status == "fail") return MOCHI_ANGRY;
  if (status == "sleep") return MOCHI_SLEEP;
  if (status == "happy") return MOCHI_HAPPY;
  return MOCHI_IDLE;
}

String requestValue(const char* key) {
  if (server.hasArg(key)) return server.arg(key);
  String body = server.arg("plain");
  if (body.length() == 0) return "";
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return "";
  const char* v = doc[key];
  return v ? String(v) : "";
}

void setMochiExpression(MochiExpression expr, bool makeIdle = false) {
  mochiExpr = expr;
  if (makeIdle) idleExpr = expr;
  currentView = VIEW_MOCHI_EXPR;
  termMode = false;
  busy = false;
  mochiFrame = 0;
  mochiFullRedraw = true;
  drawMochiExpression();
}

void routeExpressionApi() {
  String name = requestValue("name");
  if (name.length() == 0) name = requestValue("expr");
  if (name.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing expression\"}");
    return;
  }
  setMochiExpression(parseMochiExpr(name));
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeStatusApi() {
  String status = requestValue("status");
  if (status.length() == 0) status = requestValue("name");
  if (status.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing status\"}");
    return;
  }
  setMochiExpression(exprForStatus(status));
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeRefreshApi() {
  if (server.hasArg("ms")) {
    mochiFrameMs = constrain(server.arg("ms").toInt(), 100, 2000);
  } else if (server.hasArg("v")) {
    uint8_t v = constrain(server.arg("v").toInt(), 1, 4);
    const uint16_t mapMs[] = {0, 700, 400, 250, 150};
    mochiFrameMs = mapMs[v];
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeIdleApi() {
  String name = requestValue("name");
  if (name.length() == 0) name = requestValue("expr");
  if (name.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing idle expression\"}");
    return;
  }
  idleExpr = parseMochiExpr(name);
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeBrightnessApi() {
  if (server.hasArg("v")) brightnessLevel = constrain(server.arg("v").toInt(), 1, 255);
  if (server.hasArg("on")) backlightOn = server.arg("on") == "1";
  setBacklight(backlightOn);
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeMqttApi() {
  if (server.hasArg("host")) mqttHost = server.arg("host");
  if (server.hasArg("port")) mqttPort = constrain(server.arg("port").toInt(), 1, 65535);
  if (server.hasArg("device")) mqttDeviceId = server.arg("device");
  if (server.hasArg("enabled")) mqttEnabled = server.arg("enabled") == "1" || server.arg("enabled") == "true";
  mqttClient.disconnect();
  server.send(200, "application/json", "{\"ok\":true}");
}

void routeConfigApi() {
  String j = "{\"expr\":\""; j += mochiExprName(mochiExpr);
  j += "\",\"idle\":\""; j += mochiExprName(idleExpr);
  j += "\",\"refreshMs\":"; j += mochiFrameMs;
  j += ",\"brightness\":"; j += brightnessLevel;
  j += ",\"mqtt\":"; j += mqttEnabled ? "true" : "false";
  j += ",\"mqttHost\":\""; j += mqttHost;
  j += "\",\"mqttDevice\":\""; j += mqttDeviceId;
  j += "\"}";
  server.send(200, "application/json", j);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];
  String topicStr(topic);
  StaticJsonDocument<192> doc;
  bool parsed = deserializeJson(doc, body) == DeserializationError::Ok;
  if (topicStr.endsWith("/status")) {
    String status = parsed && doc["status"] ? String((const char*)doc["status"]) : body;
    setMochiExpression(exprForStatus(status));
  } else {
    String expr = parsed && doc["expr"] ? String((const char*)doc["expr"]) : body;
    setMochiExpression(parseMochiExpr(expr));
  }
}

void ensureMqtt() {
  if (!mqttEnabled || !wifiConnected) return;
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }
  unsigned long now = millis();
  if (now - lastMqttTryMs < 5000) return;
  lastMqttTryMs = now;
  mqttClient.setServer(mqttHost.c_str(), mqttPort);
  mqttClient.setCallback(mqttCallback);
  String clientId = "clawd-mochi-" + mqttDeviceId;
  if (mqttClient.connect(clientId.c_str())) {
    String base = "clawd/" + mqttDeviceId;
    mqttClient.subscribe((base + "/expression").c_str());
    mqttClient.subscribe((base + "/status").c_str());
  }
}

void routeCmd() {
  if (!server.hasArg("k") || server.arg("k").isEmpty()) {
    server.send(400, "application/json", "{\"e\":1}"); return;
  }
  String cmd = server.arg("k");

  if (termMode) {
    if (cmd == "q") { termMode = false; drawCodeView(); }
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }

  server.send(200, "application/json", "{\"ok\":1}");

  // View commands (single char)
  if (cmd.length() == 1) {
    char c = cmd[0];
    switch (c) {
      case 'w': currentView = VIEW_EYES_NORMAL; animNormalEyes(); break;
      case 's': currentView = VIEW_EYES_SQUISH; animSquishEyes(); break;
      case 'd':
        currentView = VIEW_CODE; drawCodeView();
        termMode = true; termClear(); termFullRedraw(); break;
      case 'a':
        currentView = VIEW_EYES_NORMAL;
        animLogoReveal();
        break;
      case 'm':
        currentView = VIEW_MONITOR;
        lastMonitorUpdate = 0;
        fetchAndDrawMonitor();
        break;
    }
    return;
  }

  // Static face commands (保持在 normal view)
  if (cmd == "face_wuyu") { busy = true; drawFace_wuyu(); currentView = VIEW_EYES_NORMAL; busy = false; }
  else if (cmd == "face_wenhao") { busy = true; drawFace_wenhao(); currentView = VIEW_EYES_NORMAL; busy = false; }
  else if (cmd == "face_gantanhao") { busy = true; drawFace_gantanhao(); currentView = VIEW_EYES_NORMAL; busy = false; }
  else if (cmd == "face_angry") { busy = true; drawFace_angry(); currentView = VIEW_EYES_NORMAL; busy = false; }
  else if (cmd == "face_yes") { busy = true; drawFace_yes(); currentView = VIEW_EYES_NORMAL; busy = false; }
  else if (cmd == "face_X") { busy = true; drawFace_X(); currentView = VIEW_EYES_NORMAL; busy = false; }
  else if (cmd == "face_glass") { busy = true; drawFace_glass(); currentView = VIEW_EYES_NORMAL; busy = false; }

  // Animated face commands (已有 busy 机制)
  else if (cmd == "anim_jiyanjing") anim_jiyanjing();
  else if (cmd == "anim_yun") anim_yun();
  else if (cmd == "anim_close") anim_close();
  else if (cmd == "anim_dead") anim_dead();
  else if (cmd == "anim_dian") anim_dian();
  else if (cmd == "anim_smile") anim_smile();
  else if (cmd == "anim_look") anim_look();
  else if (cmd == "anim_hart") anim_hart();
  else if (cmd == "anim_zzz") anim_zzz();
  else if (cmd == "anim_ganga") anim_ganga();
}

void routeChar() {
  if (!termMode) { server.send(200, "application/json", "{\"ok\":1}"); return; }
  const String val = server.arg("c");
  if (val.length() > 0) termAddChar(val[0]);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeSpeed() {
  if (server.hasArg("v")) animSpeed = constrain(server.arg("v").toInt(), 1, 3);
  server.send(200, "application/json", "{\"ok\":1}");
}

// /redraw?bg=hex — set animBg and immediately redraw current view
void routeRedraw() {
  if (server.hasArg("bg")) {
    animBgColor = hexToRgb565(server.arg("bg"));
    drawBgColor = animBgColor;
  }
  switch (currentView) {
    case VIEW_EYES_NORMAL: drawNormalEyes(); break;
    case VIEW_EYES_SQUISH: drawSquishEyes(); break;
    case VIEW_CODE:        drawCodeView();   break;
    case VIEW_DRAW:        tft.fillScreen(drawBgColor); break;
    case VIEW_MONITOR:     fetchAndDrawMonitor(); break;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeCanvas() {
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  if (on) { currentView = VIEW_DRAW; tft.fillScreen(drawBgColor); }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawClear() {
  const String bg = server.hasArg("bg") ? server.arg("bg") : "#aa4818";
  drawBgColor = hexToRgb565(bg);
  animBgColor = drawBgColor;  // keep in sync
  currentView = VIEW_DRAW; termMode = false;
  tft.fillScreen(drawBgColor);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawStroke() {
  if (!server.hasArg("pts") || !server.hasArg("pen")) {
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }
  const uint16_t color = hexToRgb565(server.arg("pen"));
  const String   data  = server.arg("pts");
  currentView = VIEW_DRAW;

  struct Pt { int16_t x, y; };
  Pt prev = {-1, -1};
  int start = 0;
  while (start < (int)data.length()) {
    int semi = data.indexOf(';', start);
    if (semi == -1) semi = data.length();
    String entry = data.substring(start, semi);
    const int comma = entry.indexOf(',');
    if (comma > 0) {
      const int16_t x = entry.substring(0, comma).toInt();
      const int16_t y = entry.substring(comma + 1).toInt();
      if (prev.x >= 0) {
        tft.drawLine(prev.x, prev.y, x, y, color);
        tft.drawLine(prev.x + 1, prev.y, x + 1, y, color);
        tft.drawLine(prev.x, prev.y + 1, x, y + 1, color);
      } else {
        tft.fillCircle(x, y, 2, color);
      }
      prev = {x, y};
    }
    start = semi + 1;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeBacklight() {
  setBacklight(server.hasArg("on") && server.arg("on") == "1");
  server.send(200, "application/json", "{\"ok\":1}");
}

// Convert RGB565 back to #RRGGBB for state endpoint
String rgb565ToHex(uint16_t c) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5)  & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

// /settings - 设置页面
void routeSettingsPage() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/html", SETTINGS_HTML);
}

// /settings API - 获取/设置PC IP
void routeSettingsApi() {
  // GET: 返回当前设置
  if (server.method() == HTTP_GET) {
    String j = "{\"ssid\":\"";
    j += cfgSSID;
    j += "\",\"pcip\":\"";
    j += cfgPcIp;
    j += "\"}";
    server.send(200, "application/json", j);
    return;
  }

  // POST: 更新PC IP
  if (server.method() == HTTP_POST) {
    if (server.hasArg("pcip")) {
      String newPcIp = server.arg("pcip");
      // 简单验证IP格式
      if (newPcIp.length() > 0 && newPcIp.indexOf('.') < 0) {
        server.send(400, "application/json", "{\"error\":\"invalid IP\"}");
        return;
      }
      cfgPcIp = newPcIp;
      // 保存到Preferences
      preferences.begin("clawd", false);
      preferences.putString("pcip", cfgPcIp);
      preferences.end();
      Serial.println(String("PC IP updated: ") + cfgPcIp);
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"missing pcip\"}");
    }
    return;
  }

  server.send(405, "application/json", "{\"error\":\"method not allowed\"}");
}

// /reset - 清除WiFi配置并重启
void routeReset() {
  if (server.method() == HTTP_POST) {
    Serial.println("Reset requested via web");
    server.send(200, "application/json", "{\"ok\":true}");
    delay(500);
    clearConfig();
    ESP.restart();
  } else {
    server.send(405, "application/json", "{\"error\":\"use POST\"}");
  }
}

void routeState() {
  String j = "{\"view\":"; j += currentView;
  j += ",\"busy\":";   j += busy        ? "true" : "false";
  j += ",\"term\":";   j += termMode    ? "true" : "false";
  j += ",\"bl\":";     j += backlightOn ? "true" : "false";
  j += ",\"speed\":";  j += animSpeed;
  j += ",\"expr\":\""; j += mochiExprName(mochiExpr);
  j += "\",\"idle\":\""; j += mochiExprName(idleExpr);
  j += "\",\"refreshMs\":"; j += mochiFrameMs;
  j += ",\"brightness\":"; j += brightnessLevel;
  j += "}";
  server.send(200, "application/json", j);
}

// /reminder - CRUD for reminders
void routeReminder() {
  // GET: 返回所有提醒
  if (server.method() == HTTP_GET) {
    String j = "[";
    for (uint8_t i = 0; i < reminderCount; i++) {
      if (i > 0) j += ",";
      j += "{\"id\":"; j += i;
      j += ",\"hour\":"; j += reminders[i].hour;
      j += ",\"minute\":"; j += reminders[i].minute;
      j += ",\"message\":\""; j += reminders[i].message;
      j += "\",\"enabled\":"; j += reminders[i].enabled ? "true" : "false";
      j += "}";
    }
    j += "]";
    server.send(200, "application/json", j);
    return;
  }

  // POST: 添加新提醒
  if (server.method() == HTTP_POST) {
    if (reminderCount >= MAX_REMINDERS) {
      server.send(400, "application/json", "{\"error\":\"max reminders\"}");
      return;
    }
    if (!server.hasArg("hour") || !server.hasArg("minute") || !server.hasArg("message")) {
      server.send(400, "application/json", "{\"error\":\"missing params\"}");
      return;
    }
    uint8_t h = constrain(server.arg("hour").toInt(), 0, 23);
    uint8_t m = constrain(server.arg("minute").toInt(), 0, 59);
    String msg = server.arg("message");
    msg = msg.substring(0, REMINDER_MSG_LEN);

    reminders[reminderCount].hour = h;
    reminders[reminderCount].minute = m;
    strncpy(reminders[reminderCount].message, msg.c_str(), REMINDER_MSG_LEN);
    reminders[reminderCount].message[REMINDER_MSG_LEN] = '\0';
    reminders[reminderCount].enabled = true;
    reminders[reminderCount].triggeredToday = false;
    reminderCount++;

    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  // DELETE: 删除提醒
  if (server.method() == HTTP_DELETE) {
    if (!server.hasArg("id")) {
      server.send(400, "application/json", "{\"error\":\"missing id\"}");
      return;
    }
    uint8_t id = server.arg("id").toInt();
    if (id >= reminderCount) {
      server.send(400, "application/json", "{\"error\":\"invalid id\"}");
      return;
    }
    // 移动后面的提醒填补空位
    for (uint8_t i = id; i < reminderCount - 1; i++) {
      reminders[i] = reminders[i + 1];
    }
    reminderCount--;
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  // PUT: 更新提醒（启用/禁用或修改内容）
  if (server.method() == HTTP_PUT) {
    if (!server.hasArg("id")) {
      server.send(400, "application/json", "{\"error\":\"missing id\"}");
      return;
    }
    uint8_t id = server.arg("id").toInt();
    if (id >= reminderCount) {
      server.send(400, "application/json", "{\"error\":\"invalid id\"}");
      return;
    }
    if (server.hasArg("enabled")) {
      reminders[id].enabled = (server.arg("enabled") == "1" || server.arg("enabled") == "true");
    }
    if (server.hasArg("hour")) {
      reminders[id].hour = constrain(server.arg("hour").toInt(), 0, 23);
    }
    if (server.hasArg("minute")) {
      reminders[id].minute = constrain(server.arg("minute").toInt(), 0, 59);
    }
    if (server.hasArg("message")) {
      String msg = server.arg("message");
      msg = msg.substring(0, REMINDER_MSG_LEN);
      strncpy(reminders[id].message, msg.c_str(), REMINDER_MSG_LEN);
      reminders[id].message[REMINDER_MSG_LEN] = '\0';
    }
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  server.send(405, "application/json", "{\"error\":\"method not allowed\"}");
}

void routeNotFound() { server.send(404, "text/plain", "not found"); }

void registerMainRoutes() {
  server.on("/",            HTTP_GET, routeRoot);
  server.on("/cmd",         HTTP_GET, routeCmd);
  server.on("/char",        HTTP_GET, routeChar);
  server.on("/speed",       HTTP_GET, routeSpeed);
  server.on("/redraw",      HTTP_GET, routeRedraw);
  server.on("/canvas",      HTTP_GET, routeCanvas);
  server.on("/draw/clear",  HTTP_GET, routeDrawClear);
  server.on("/draw/stroke", HTTP_GET, routeDrawStroke);
  server.on("/backlight",   HTTP_GET, routeBacklight);
  server.on("/brightness",  HTTP_GET, routeBrightnessApi);
  server.on("/refresh",     HTTP_GET, routeRefreshApi);
  server.on("/idle",        HTTP_ANY, routeIdleApi);
  server.on("/mqtt",        HTTP_GET, routeMqttApi);
  server.on("/api/config",      HTTP_GET, routeConfigApi);
  server.on("/api/expression", HTTP_ANY, routeExpressionApi);
  server.on("/api/status",     HTTP_ANY, routeStatusApi);
  server.on("/state",       HTTP_GET, routeState);
  server.on("/reminder",    HTTP_ANY,  routeReminder);
  server.on("/settings",    HTTP_GET, routeSettingsPage);
  server.on("/settings",    HTTP_ANY,  routeSettingsApi);
  server.on("/reset",       HTTP_POST, routeReset);
  server.onNotFound(routeNotFound);
}

void startExpressionApMode() {
  inSetupMode = false;
  wifiConnected = false;
  Serial.println("Starting expression AP mode");
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(500);
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_2dBm);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  bool apOk = WiFi.softAP(CONTROL_AP_NAME, "12345678", 1, false, 4);
  Serial.print("softAP result: ");
  Serial.println(apOk ? "OK" : "FAIL");
  Serial.print("softAP IP: ");
  Serial.println(WiFi.softAPIP());
  registerMainRoutes();
  server.begin();

  currentView = VIEW_MOCHI_EXPR;
  mochiExpr = MOCHI_IDLE;
  mochiFullRedraw = true;
  drawMochiExpression();
  tft.setTextColor(C_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 204);
  tft.print(apOk ? "WiFi: Clawd-Mochi  pass:12345678" : "WiFi AP FAILED");
  tft.setCursor(8, 220);
  tft.print("Open: http://192.168.4.1");
}

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);

  // ── 硬件初始化 ────────────────────────────────────────────
  pinMode(TFT_BLK, OUTPUT);
  pinMode(RESET_PIN, INPUT_PULLUP);  // 重置配网引脚
  setBacklight(true);

  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  initColours();

#if FORCE_EXPRESSION_AP
  startExpressionApMode();
  return;
#endif

  // ── Boot splash ────────────────────────────────────────────
  tft.fillScreen(animBgColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 - 22); tft.print("Clawd");
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 + 14); tft.print("Mochi");
  delay(800);

  // ── Logo shown once at startup ─────────────────────────────
  animLogoReveal();

  // ── 检测重置引脚（GPIO5拉低清除配置）──────────────────────
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("Reset pin LOW - clearing config");
    clearConfig();
    tft.fillScreen(C_DARKBG);
    tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
    tft.setTextColor(C_ORANGE); tft.setTextSize(2);
    tft.setCursor(DISP_W/2 - 70, DISP_H/2 - 20);
    tft.print("Config Cleared");
    tft.setTextColor(C_MUTED); tft.setTextSize(1);
    tft.setCursor(DISP_W/2 - 70, DISP_H/2 + 10);
    tft.print("Release reset pin...");
    delay(2000);
    // 等待引脚释放
    while (digitalRead(RESET_PIN) == LOW) delay(100);
  }

  // ── 读取配置 ──────────────────────────────────────────────
  bool hasConfig = loadConfig();
  Serial.println("Config loaded:");
  Serial.println(String("  SSID: ") + (cfgSSID.length() > 0 ? cfgSSID : String("(empty)")));
  Serial.println(String("  PC IP: ") + (cfgPcIp.length() > 0 ? cfgPcIp : String("(empty)")));

  // ── WiFi预扫描（在Station模式下扫描避免AP模式断连问题）──────
  if (hasConfig) {
    // 有配置，先扫描可用网络（用于后续显示）
    WiFi.mode(WIFI_STA);
    scanWiFi();
    WiFi.mode(WIFI_OFF);  // 暂时关闭WiFi
  } else {
    // 无配置，先扫描（此时在Station模式扫描）
    WiFi.mode(WIFI_STA);
    scanWiFi();
    WiFi.mode(WIFI_OFF);
  }

  // ── 根据配置决定启动模式 ─────────────────────────────────
  if (!hasConfig) {
    // 无WiFi配置 → 进入AP配网模式
    startSetupMode();
    return;  // setup结束，loop中处理配网
  }

  // ── 有WiFi配置 → Station模式连接 ───────────────────────────
  showConnectingScreen();

  if (!tryConnectWiFi()) {
    // 连接失败
    showConnectFailedScreen();
    delay(3000);
    // 重试一次
    if (!tryConnectWiFi()) {
      // 仍然失败，进入配网模式让用户重新配置
      Serial.println("Connection failed, entering setup mode");
      startSetupMode();
      return;
    }
  }

  // ── WiFi连接成功 ──────────────────────────────────────────
  wifiConnected = true;
  inSetupMode = false;
  showConnectedScreen();
  delay(1000);

  // ── 启动Web服务器（正常运行模式）───────────────────────────
  server.on("/",            HTTP_GET, routeRoot);
  server.on("/cmd",         HTTP_GET, routeCmd);
  server.on("/char",        HTTP_GET, routeChar);
  server.on("/speed",       HTTP_GET, routeSpeed);
  server.on("/redraw",      HTTP_GET, routeRedraw);
  server.on("/canvas",      HTTP_GET, routeCanvas);
  server.on("/draw/clear",  HTTP_GET, routeDrawClear);
  server.on("/draw/stroke", HTTP_GET, routeDrawStroke);
  server.on("/backlight",   HTTP_GET, routeBacklight);
  server.on("/brightness",  HTTP_GET, routeBrightnessApi);
  server.on("/refresh",     HTTP_GET, routeRefreshApi);
  server.on("/idle",        HTTP_ANY, routeIdleApi);
  server.on("/mqtt",        HTTP_GET, routeMqttApi);
  server.on("/api/config",      HTTP_GET, routeConfigApi);
  server.on("/api/expression", HTTP_ANY, routeExpressionApi);
  server.on("/api/status",     HTTP_ANY, routeStatusApi);
  server.on("/state",       HTTP_GET, routeState);
  server.on("/reminder",    HTTP_ANY,  routeReminder);
  server.on("/settings",    HTTP_GET, routeSettingsPage);   // 设置页面
  server.on("/settings",    HTTP_ANY,  routeSettingsApi);    // 设置API
  server.on("/reset",       HTTP_POST, routeReset);          // 重置配网
  server.onNotFound(routeNotFound);
  server.begin();

  Serial.println("Web server started");
  Serial.println(String("Ready! Open: ") + WiFi.localIP().toString());

  // 显示eyes动画作为默认视图
  drawNormalEyes();
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════

void loop() {
  // ── 配网模式处理 ──────────────────────────────────────────
  if (inSetupMode) {
    handleSetupLoop();
    return;  // 配网模式下不执行其他逻辑
  }

  // ── 正常运行模式 ──────────────────────────────────────────
  server.handleClient();
  ensureMqtt();

  unsigned long now = millis();

  if (currentView == VIEW_MOCHI_EXPR && !busy) {
    if (now - lastMochiFrameMs >= mochiFrameMs || lastMochiFrameMs == 0) {
      lastMochiFrameMs = now;
      mochiFrame++;
      drawMochiExpression();
    }
  }

  // Auto-refresh monitor every 50 seconds
  if (currentView == VIEW_MONITOR) {
    if (now - lastMonitorUpdate >= MONITOR_INTERVAL || lastMonitorUpdate == 0) {
      lastMonitorUpdate = now;
      fetchAndDrawMonitor();
    }
  }

  // Reminder handling
  if (currentView == VIEW_REMINDER) {
    // 30秒后自动关闭提醒
    if (now - reminderStartTime >= REMINDER_DURATION) {
      endReminder();
    }
  } else {
    // 每分钟检查一次提醒（60000ms）
    if (now - lastMinuteCheck >= 60000 || lastMinuteCheck == 0) {
      lastMinuteCheck = now;
      checkReminders();
    }
  }
}
