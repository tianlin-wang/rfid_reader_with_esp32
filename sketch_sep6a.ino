#define TFT_SCK    4
#define TFT_MOSI   5
#define TFT_RST    6
#define TFT_DC     7
#define TFT_CS     10
#define TFT_BLK    13

#define PN532_SCK  17
#define PN532_MISO 18
#define PN532_MOSI 8
#define PN532_SS   3
#define PN532_IRQ  11
#define PN532_RST  12

#define BTN_NEXT  14
#define BTN_UP    15
#define BTN_DOWN  16

#define SD_SCK    42
#define SD_MISO   41
#define SD_MOSI   40
#define SD_CS     39

#include <Adafruit_PN532.h>
#include <LovyanGFX.hpp>
#include <SPI.h>
#include <SD.h>
#include "driver/gpio.h"
#include "esp_timer.h"

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

// 屏幕走 SPI2_HOST(FSPI)，SD 卡必须走 SPI3_HOST(HSPI)。
// 两者都用同一个 SPI 主机时，LovyanGFX 和 Arduino SD 库会各自调用
// spi_bus_initialize() 争抢同一条总线，SD 卡会挂载失败或屏幕花屏。
SPIClass sdSPI(HSPI);
bool sdReady = false;

// 读卡器没到货 / 没接时置 false：setup() 不再卡死，进入"无卡模式"，
// 名单、作业状态、屏幕、SD 存档都能先用串口验证。
bool nfcReady = false;

// ============ 颜色 ============
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define CYAN    0x07FF
#define YELLOW  0xFFE0

// ============ 数据模型 ============
#define MAX_STUDENTS 50
#define MAX_HOMEWORK 30
#define BITS_PER_HW  7

struct Student {
  uint8_t uid[7];
  uint8_t uidLen;
  // 16 字节 = 5 个汉字（UTF-8 3 字节/字）+ 结束符。
  // 原来是 12 字节，只能塞 3 个汉字，四个字的名字会被截断。
  char name[16];
};

enum SystemMode { MODE_CHECKIN, MODE_MISSING, MODE_SETUP, MODE_WRITE, MODE_CONFIRM_NEW };
SystemMode sysMode = MODE_CHECKIN;

Student roster[MAX_STUDENTS];
uint8_t studentCount = 0;
uint8_t currentHW = 0;
uint8_t submissions[MAX_HOMEWORK][BITS_PER_HW];

// ============ 按钮（ISR 置标志，loop 里处理） ============
volatile bool btnNextFlag = false;
volatile bool btnUpFlag = false;
volatile bool btnDownFlag = false;
volatile unsigned long btnNextPressTime = 0;
volatile bool btnNextLongPress = false;

// 中断里不能调 digitalRead() / millis()：它们不在 IRAM 里，
// flash 被占用时会崩。改用 gpio_get_level() 和 esp_timer_get_time()。
static inline unsigned long isrMillis() { return (unsigned long)(esp_timer_get_time() / 1000ULL); }

void IRAM_ATTR isrNext() {
  static unsigned long last = 0;
  unsigned long now = isrMillis();
  if (now - last < 50) return;
  last = now;
  if (gpio_get_level((gpio_num_t)BTN_NEXT) == 0) {
    btnNextPressTime = now;
    btnNextLongPress = false;
  } else if (btnNextPressTime > 0) {
    unsigned long dur = now - btnNextPressTime;
    if (dur > 1000) btnNextLongPress = true;
    else if (dur > 30) btnNextFlag = true;
    btnNextPressTime = 0;
  }
}
void IRAM_ATTR isrUp() {
  static unsigned long last = 0;
  unsigned long now = isrMillis();
  if (now - last < 200) return;
  last = now;
  if (gpio_get_level((gpio_num_t)BTN_UP) == 0) btnUpFlag = true;
}
void IRAM_ATTR isrDown() {
  static unsigned long last = 0;
  unsigned long now = isrMillis();
  if (now - last < 200) return;
  last = now;
  if (gpio_get_level((gpio_num_t)BTN_DOWN) == 0) btnDownFlag = true;
}

// ============ 存储层 (SD Card CSV) ============
bool initSD() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, sdSPI, 4000000);
  if (!sdReady) Serial0.println("SD init FAILED (no card?)");
  else Serial0.println("SD card OK");
  return sdReady;
}

void uidToHex(uint8_t *uid, uint8_t len, char *buf) {
  buf[0] = 0;
  for (uint8_t i = 0; i < len; i++) {
    if (i > 0) strcat(buf, " ");
    char tmp[4];
    snprintf(tmp, sizeof(tmp), "%02X", uid[i]);
    strcat(buf, tmp);
  }
}

void saveRoster() {
  if (!sdReady) return;
  File f = SD.open("/roster.csv", FILE_WRITE);
  if (!f) { Serial0.println("Cannot write roster.csv"); return; }
  f.println("# RFID CheckIn Roster");
  f.println("# UID_HEX,NAME");
  for (uint8_t i = 0; i < studentCount; i++) {
    char uidStr[32];
    uidToHex(roster[i].uid, roster[i].uidLen, uidStr);
    f.print(uidStr); f.print(","); f.println(roster[i].name);
  }
  f.close();
  Serial0.println("roster.csv saved");
}

void loadRoster() {
  if (!sdReady) { studentCount = 0; return; }
  if (!SD.exists("/roster.csv")) { studentCount = 0; return; }

  File f = SD.open("/roster.csv", FILE_READ);
  if (!f) { studentCount = 0; return; }
  studentCount = 0;
  while (f.available() && studentCount < MAX_STUDENTS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;

    int comma = line.indexOf(',');
    if (comma <= 0) continue;
    String uidPart = line.substring(0, comma);
    String namePart = line.substring(comma + 1);

    uint8_t uidBuf[7] = {0};
    uint8_t uidLen = 0;
    int idx = 0;
    String token = "";
    for (int i = 0; i < (int)uidPart.length(); i++) {
      char c = uidPart[i];
      if (c == ' ') {
        if (token.length() == 2 && uidLen < 7) {
          uidBuf[uidLen++] = (uint8_t)strtol(token.c_str(), NULL, 16);
        }
        token = "";
      } else token += c;
    }
    if (token.length() == 2 && uidLen < 7) {
      uidBuf[uidLen++] = (uint8_t)strtol(token.c_str(), NULL, 16);
    }
    if (uidLen == 0) continue;

    memcpy(roster[studentCount].uid, uidBuf, uidLen);
    roster[studentCount].uidLen = uidLen;
    strncpy(roster[studentCount].name, namePart.c_str(), sizeof(roster[studentCount].name) - 1);
    roster[studentCount].name[sizeof(roster[studentCount].name) - 1] = 0;
    studentCount++;
  }
  f.close();
  Serial0.printf("Loaded %d students from SD\n", studentCount);
}

void saveAllHomework() {
  if (!sdReady) return;
  File sf = SD.open("/state.csv", FILE_WRITE);
  if (sf) {
    sf.print("current_hw="); sf.println(currentHW);
    sf.close();
  }
  for (uint8_t h = 0; h <= currentHW; h++) {
    char path[24];
    snprintf(path, sizeof(path), "/hw_%d.csv", h + 1);
    File f = SD.open(path, FILE_WRITE);
    if (!f) continue;
    f.printf("# HW %d Submissions\n", h + 1);
    f.println("# NAME,UID,SUBMITTED");
    for (uint8_t i = 0; i < studentCount; i++) {
      char uidStr[32];
      uidToHex(roster[i].uid, roster[i].uidLen, uidStr);
      f.print(roster[i].name); f.print(","); f.print(uidStr); f.print(",");
      f.println(isSubmitted(h, i) ? "1" : "0");
    }
    f.close();
  }
}

void loadAllHomework() {
  if (!sdReady) { currentHW = 0; return; }
  currentHW = 0;
  memset(submissions, 0, sizeof(submissions));

  if (SD.exists("/state.csv")) {
    File sf = SD.open("/state.csv", FILE_READ);
    if (sf) {
      String line = sf.readStringUntil('\n');
      line.trim();
      if (line.startsWith("current_hw=")) {
        currentHW = (uint8_t)line.substring(11).toInt();
        if (currentHW >= MAX_HOMEWORK) currentHW = 0;
      }
      sf.close();
    }
  }

  for (uint8_t h = 0; h <= currentHW; h++) {
    char path[24];
    snprintf(path, sizeof(path), "/hw_%d.csv", h + 1);
    if (!SD.exists(path)) continue;
    File f = SD.open(path, FILE_READ);
    if (!f) continue;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0 || line[0] == '#') continue;
      int lastComma = line.lastIndexOf(',');
      if (lastComma < 0) continue;
      String submittedStr = line.substring(lastComma + 1);
      int comma2 = line.lastIndexOf(',', lastComma - 1);
      if (comma2 < 0) continue;
      String uidPart = line.substring(comma2 + 1, lastComma);

      uint8_t uidBuf[7] = {0};
      uint8_t uidLen = 0;
      String token = "";
      for (int i = 0; i < (int)uidPart.length(); i++) {
        char c = uidPart[i];
        if (c == ' ') {
          if (token.length() == 2 && uidLen < 7) {
            uidBuf[uidLen++] = (uint8_t)strtol(token.c_str(), NULL, 16);
          }
          token = "";
        } else token += c;
      }
      if (token.length() == 2 && uidLen < 7) {
        uidBuf[uidLen++] = (uint8_t)strtol(token.c_str(), NULL, 16);
      }

      int idx = findStudent(uidBuf, uidLen);
      if (idx >= 0 && submittedStr == "1") {
        uint8_t byteIdx = idx / 8;
        uint8_t bitIdx = idx % 8;
        submissions[h][byteIdx] |= (1 << bitIdx);
      }
    }
    f.close();
  }
  Serial0.printf("Loaded HW state: current=%d\n", currentHW + 1);
}

void saveCurrentHWFile() {
  if (!sdReady) return;
  char path[24];
  snprintf(path, sizeof(path), "/hw_%d.csv", currentHW + 1);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return;
  f.printf("# HW %d Submissions\n", currentHW + 1);
  f.println("# NAME,UID,SUBMITTED");
  for (uint8_t i = 0; i < studentCount; i++) {
    char uidStr[32];
    uidToHex(roster[i].uid, roster[i].uidLen, uidStr);
    f.print(roster[i].name); f.print(","); f.print(uidStr); f.print(",");
    f.println(isSubmitted(currentHW, i) ? "1" : "0");
  }
  f.close();
}

// ============ 辅助函数 ============
int findStudent(uint8_t *uid, uint8_t len) {
  for (uint8_t i = 0; i < studentCount; i++) {
    if (roster[i].uidLen != len) continue;
    if (memcmp(roster[i].uid, uid, len) == 0) return i;
  }
  return -1;
}

bool isSubmitted(uint8_t hw, uint8_t stuIdx) {
  if (hw >= MAX_HOMEWORK) return false;
  uint8_t byteIdx = stuIdx / 8;
  uint8_t bitIdx = stuIdx % 8;
  return (submissions[hw][byteIdx] >> bitIdx) & 1;
}

void markSubmitted(uint8_t hw, uint8_t stuIdx) {
  if (hw >= MAX_HOMEWORK) return;
  uint8_t byteIdx = stuIdx / 8;
  uint8_t bitIdx = stuIdx % 8;
  submissions[hw][byteIdx] |= (1 << bitIdx);
  if (hw == currentHW) saveCurrentHWFile();
}

uint8_t getSubmittedCount(uint8_t hw) {
  uint8_t cnt = 0;
  for (uint8_t i = 0; i < studentCount; i++) {
    if (isSubmitted(hw, i)) cnt++;
  }
  return cnt;
}

void nextHomework() {
  if (currentHW + 1 < MAX_HOMEWORK) {
    currentHW++;
    memset(submissions[currentHW], 0, BITS_PER_HW);
  }
  saveAllHomework();
}

void resetAll() {
  currentHW = 0;
  for (uint8_t h = 0; h < MAX_HOMEWORK; h++) {
    memset(submissions[h], 0, BITS_PER_HW);
  }
  saveAllHomework();
}



class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789   _panel;
  lgfx::Bus_SPI        _bus;
  lgfx::Light_PWM      _light;
public:
  LGFX(void) {
    auto bus = _bus.config();
    bus.spi_host   = SPI2_HOST;
    bus.spi_mode   = 0;
    bus.freq_write = 40000000;
    bus.freq_read  = 16000000;
    bus.pin_sclk   = TFT_SCK;
    bus.pin_mosi   = TFT_MOSI;
    bus.pin_miso   = -1;
    bus.pin_dc     = TFT_DC;
    bus.use_lock   = false;
    _bus.config(bus);
    _panel.setBus(&_bus);

    auto p = _panel.config();
    p.pin_cs       = TFT_CS;
    p.pin_rst      = TFT_RST;
    p.pin_busy     = -1;
    p.memory_width = 240;
    p.memory_height= 320;
    p.panel_width  = 240;
    p.panel_height = 240;
    p.offset_x     = 0;
    p.offset_y     = 0;
    p.invert       = false;
    p.rgb_order    = false;
    p.readable     = false;
    _panel.config(p);

    auto light = _light.config();
    light.pin_bl = TFT_BLK;
    light.invert = false;
    light.freq   = 44100;
    light.pwm_channel = 7;
    _light.config(light);
    _panel.setLight(&_light);

    setPanel(&_panel);
  }
};

static LGFX lcd;

// ============ 内置 16x16 点阵字库 ============
// 不再走 LovyanGFX 的 loadFont()：它的第二个参数是字体类型枚举
// （默认 ft_vlw），传 TTF 路径或长度进去根本加载不出来，而且还要依赖 SD 卡。
// 现在字形直接编译进 flash，SD 卡拔了中文照样显示。
#include "font_cn.h"

// 在升序码点表里二分查找汉字，返回字形下标；找不到返回 -1
static int cjkIndex(uint16_t code) {
  int lo = 0, hi = (int)FONT_CN_CJK_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t c = font_cjk_codes[mid];
    if (c == code) return mid;
    if (c < code) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

// 画一个字符；返回它占的宽度（ASCII 8px，汉字 16px）
static int drawGlyph(int16_t x, int16_t y, uint16_t code, uint16_t fg, uint16_t bg) {
  uint16_t buf[FONT_CN_CJK_W * FONT_CN_HEIGHT];

  if (code >= 0x20 && code <= 0x7E) {
    const uint8_t *bmp = font_ascii[code - 0x20];
    for (int r = 0; r < FONT_CN_HEIGHT; r++) {
      uint8_t bits = bmp[r];
      for (int c = 0; c < FONT_CN_ASCII_W; c++)
        buf[r * FONT_CN_ASCII_W + c] = (bits & (0x80 >> c)) ? fg : bg;
    }
    lcd.pushImage(x, y, FONT_CN_ASCII_W, FONT_CN_HEIGHT, buf);
    return FONT_CN_ASCII_W;
  }

  int idx = cjkIndex(code);
  if (idx < 0) {
    // 字库没这个字，画个方框提示，别静默留空白
    lcd.fillRect(x, y, FONT_CN_CJK_W, FONT_CN_HEIGHT, bg);
    lcd.drawRect(x, y, FONT_CN_CJK_W, FONT_CN_HEIGHT, fg);
    return FONT_CN_CJK_W;
  }

  const uint8_t *bmp = font_cjk_bitmap[idx];
  for (int r = 0; r < FONT_CN_HEIGHT; r++) {
    uint16_t bits = ((uint16_t)bmp[r * 2] << 8) | bmp[r * 2 + 1];
    for (int c = 0; c < FONT_CN_CJK_W; c++)
      buf[r * FONT_CN_CJK_W + c] = (bits & (0x8000 >> c)) ? fg : bg;
  }
  lcd.pushImage(x, y, FONT_CN_CJK_W, FONT_CN_HEIGHT, buf);
  return FONT_CN_CJK_W;
}

// 画 UTF-8 字符串，返回结束时的 x 坐标（便于接着画）
static int drawText(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  int cx = x;
  while (*s) {
    unsigned char c = (unsigned char)*s;
    uint32_t code;
    int n;
    if (c < 0x80)       { code = c & 0x7F; n = 1; }
    else if ((c & 0xE0) == 0xC0) { code = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { code = c & 0x07; n = 4; }
    else                 { code = '?'; n = 1; }
    for (int i = 1; i < n; i++) {
      if (!s[i]) break;
      code = (code << 6) | (s[i] & 0x3F);
    }
    if (cx + FONT_CN_CJK_W > lcd.width()) break;
    cx += drawGlyph(cx, y, (uint16_t)code, fg, bg);
    s += n;
  }
  return cx;
}

// 量一段文字的像素宽度，用来做居中 / 右对齐
static int textWidth(const char *s) {
  int w = 0;
  while (*s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80)       { w += FONT_CN_ASCII_W; s += 1; }
    else if ((c & 0xE0) == 0xC0) { w += FONT_CN_CJK_W; s += 2; }
    else if ((c & 0xF0) == 0xE0) { w += FONT_CN_CJK_W; s += 3; }
    else if ((c & 0xF8) == 0xF0) { w += FONT_CN_CJK_W; s += 4; }
    else                 { w += FONT_CN_ASCII_W; s += 1; }
  }
  return w;
}

void st7789Init() {
  lcd.init();
  lcd.setRotation(0);
  lcd.fillScreen(BLACK);
  Serial0.printf("Built-in bitmap font: %d ASCII + %d CJK glyphs\n",
                 FONT_CN_ASCII_COUNT, FONT_CN_CJK_COUNT);
}

void fillColor(uint16_t c) { lcd.fillScreen(c); }
void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  if (x + w <= 0 || y + h <= 0 || x >= lcd.width() || y >= lcd.height()) return;
  lcd.fillRect(x, y, w, h, c);
}
void drawPixel(int16_t x, int16_t y, uint16_t c) { lcd.drawPixel(x, y, c); }
void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) { lcd.drawFastHLine(x, y, w, c); }

uint16_t _fgColor = WHITE, _bgColor = BLACK;
int16_t _cx = 0, _cy = 0;

void setTextColor(uint16_t c) { _fgColor = c; }
void setTextBackground(uint16_t c) { _bgColor = c; }
void setCursor(int16_t x, int16_t y) { _cx = x; _cy = y; }
void printStr(const char *s) { _cx = drawText(_cx, _cy, s, _fgColor, _bgColor); }
void printNum(uint32_t n, int base = 10) {
  char b[12];
  if (base == 16) snprintf(b, sizeof(b), "%X", n);
  else snprintf(b, sizeof(b), "%u", n);
  printStr(b);
}
// 居中画一行（y 为行顶），返回占用的宽度
int printCentered(int16_t y, const char *s) {
  int w = textWidth(s);
  int x = (lcd.width() - w) / 2;
  if (x < 0) x = 0;
  drawText(x, y, s, _fgColor, _bgColor);
  return w;
}
// 右对齐，右边距 pad
int printRight(int16_t y, const char *s, int16_t pad) {
  int w = textWidth(s);
  int x = lcd.width() - w - pad;
  if (x < 0) x = 0;
  drawText(x, y, s, _fgColor, _bgColor);
  return w;
}

// ============ UI ============
enum ViewMode { VIEW_CHECKIN, VIEW_MISSING };
ViewMode currentView = VIEW_CHECKIN;
uint8_t missingPage = 0;

#define HEADER_H 22   // 顶栏高度，16px 字高 + 上下各 3px 留白

void drawHeader() {
  fillRect(0, 0, 240, HEADER_H, BLUE);
  setTextBackground(BLUE);          // 点阵要填背景色，顶栏上必须跟蓝底一致
  setTextColor(WHITE);
  setCursor(4, 3);
  printStr("作业");
  printNum(currentHW + 1);

  char right[16];
  snprintf(right, sizeof(right), "%d/%d", getSubmittedCount(currentHW), studentCount);
  printRight(3, right, 6);

  setTextBackground(BLACK);
}

void showCheckInView() {
  fillColor(BLACK);
  setTextBackground(BLACK);
  drawHeader();
  drawFastHLine(0, HEADER_H, 240, CYAN);

  setTextColor(CYAN);
  printCentered(66, "请刷卡登记");

  char line[24];
  snprintf(line, sizeof(line), "第 %d 次作业", currentHW + 1);
  setTextColor(WHITE);
  printCentered(96, line);

  if (nfcReady) {
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "已交 %d / %d", getSubmittedCount(currentHW), studentCount);
    setTextColor(GREEN);
    printCentered(138, cnt);
  } else {
    setTextColor(RED);
    printCentered(138, "读卡器未连接");
  }

  setTextColor(YELLOW);
  printCentered(186, "n 开新   u 上翻");
  printCentered(208, "m 名单   h 帮助");
}

void showCheckInResult(int stuIdx, uint8_t *uid, uint8_t uidLen) {
  fillColor(BLACK);
  setTextBackground(BLACK);
  drawHeader();
  drawFastHLine(0, HEADER_H, 240, CYAN);

  if (stuIdx < 0) {
    setTextColor(RED);
    printCentered(50, "未登记的卡");

    char uidStr[32];
    if (uid && uidLen) uidToHex(uid, uidLen, uidStr);
    else snprintf(uidStr, sizeof(uidStr), "--------");
    setTextColor(WHITE);
    printCentered(85, uidStr);

    setTextColor(YELLOW);
    printCentered(145, "串口输入 s");
    printCentered(168, "进入登记模式");
    return;
  }

  setTextColor(WHITE);
  printCentered(46, roster[stuIdx].name);

  if (isSubmitted(currentHW, stuIdx)) {
    setTextColor(YELLOW);
    printCentered(80, "这次已经交过了");
  } else {
    markSubmitted(currentHW, stuIdx);
    setTextColor(GREEN);
    printCentered(80, "提交成功");
  }

  uint8_t submitted = getSubmittedCount(currentHW);
  uint8_t pct = studentCount ? (submitted * 100 / studentCount) : 0;

  // 进度条：外框 + 填充
  lcd.drawRect(20, 116, 200, 12, WHITE);
  uint16_t barW = (uint16_t)submitted * 196 / (studentCount ? studentCount : 1);
  fillRect(22, 118, barW, 8, GREEN);

  char prog[32];
  snprintf(prog, sizeof(prog), "已交 %d/%d  %d%%", submitted, studentCount, pct);
  setTextColor(WHITE);
  printCentered(145, prog);

  setTextColor(CYAN);
  printCentered(180, "继续刷下一本");
}

void showMissingView() {
  fillColor(BLACK);
  setTextBackground(BLACK);

  uint8_t missing[50];
  uint8_t missingIdx = 0;
  for (uint8_t i = 0; i < studentCount && missingIdx < 50; i++) {
    if (!isSubmitted(currentHW, i)) missing[missingIdx++] = i;
  }

  fillRect(0, 0, 240, HEADER_H, RED);
  setTextBackground(RED);
  setTextColor(WHITE);
  setCursor(4, 3); printStr("未交名单");
  char right[16];
  snprintf(right, sizeof(right), "%d人", missingIdx);
  printRight(3, right, 6);
  setTextBackground(BLACK);
  drawFastHLine(0, HEADER_H, 240, RED);

  if (missingIdx == 0) {
    setTextColor(GREEN);
    printCentered(100, "全部交齐！");
    setTextColor(CYAN);
    printCentered(140, "按 A 开新作业");
    return;
  }

  const uint8_t perPage = 10;      // 16px 字高，一屏最多 10 行
  uint8_t totalPages = (missingIdx + perPage - 1) / perPage;
  if (missingPage >= totalPages) missingPage = totalPages - 1;

  uint8_t start = missingPage * perPage;
  uint8_t end = start + perPage;
  if (end > missingIdx) end = missingIdx;

  int row = 28;
  for (uint8_t i = start; i < end; i++) {
    char num[8];
    snprintf(num, sizeof(num), "%d.", i + 1);
    setTextColor(CYAN);
    setCursor(8, row); printStr(num);
    setTextColor(WHITE);
    printStr(" ");
    printStr(roster[missing[i]].name);
    row += 19;
  }

  char page[16];
  snprintf(page, sizeof(page), "%d/%d", missingPage + 1, totalPages);
  setTextColor(YELLOW);
  printRight(219, page, 8);
}

void showNewHWConfirm() {
  fillColor(BLACK);
  setTextBackground(BLACK);
  setTextColor(YELLOW);
  printCentered(38, "开始新作业？");
  drawFastHLine(24, 62, 192, YELLOW);

  char cur[24], nxt[24];
  snprintf(cur, sizeof(cur), "当前：第 %d 次", currentHW + 1);
  snprintf(nxt, sizeof(nxt), "新的：第 %d 次", currentHW + 2);
  setTextColor(WHITE);
  printCentered(82, cur);
  setTextColor(CYAN);
  printCentered(108, nxt);

  setTextColor(GREEN);
  printCentered(168, "A 键确认");
  setTextColor(RED);
  printCentered(196, "B 或 C 取消");
}

void showAllDone() {
  fillColor(BLACK);
  setTextBackground(BLACK);
  setTextColor(GREEN);
  printCentered(88, "全部交齐！");
  char line[24];
  snprintf(line, sizeof(line), "第 %d 次作业完成", currentHW + 1);
  setTextColor(CYAN);
  printCentered(130, line);
}

void showSetupMode() {
  fillColor(BLACK);
  setTextBackground(BLACK);

  fillRect(0, 0, 240, HEADER_H, YELLOW);
  setTextBackground(YELLOW);
  setTextColor(BLACK);
  setCursor(4, 3); printStr("登记模式");
  char right[16];
  snprintf(right, sizeof(right), "%d人", studentCount);
  printRight(3, right, 6);
  setTextBackground(BLACK);
  drawFastHLine(0, HEADER_H, 240, YELLOW);

  setTextColor(WHITE);
  printCentered(38, "刷一张新贴纸");
  printCentered(60, "再串口输入姓名");

  drawFastHLine(24, 88, 192, CYAN);
  setTextColor(CYAN);
  printCentered(98, "串口命令");

  setTextColor(GREEN);
  setCursor(24, 124); printStr("name=张三");
  setCursor(24, 146); printStr("d 完成登记");
  setCursor(24, 168); printStr("list 名单");
  setTextColor(RED);
  setCursor(24, 190); printStr("clear 清空");

  setTextColor(YELLOW);
  printCentered(216, "长按 A 会清空记录");
}

// ============ 写卡 UI + 逻辑（保留） ============
#define NTAG_START_PAGE  8
#define NTAG_PAGE_COUNT  16
const uint8_t WRITE_DATA[] = "HELLO-RFID-TEST-!";
bool writeMode = false;
uint8_t writeLastUID[7] = {0};
uint8_t writeLastLen = 0;

void showWriteMode() {
  fillColor(BLACK);
  setTextBackground(BLACK);

  fillRect(0, 0, 240, HEADER_H, YELLOW);
  setTextBackground(YELLOW);
  setTextColor(BLACK);
  setCursor(4, 3); printStr("写卡模式");
  setTextBackground(BLACK);
  drawFastHLine(0, HEADER_H, 240, YELLOW);

  setTextColor(CYAN);
  printCentered(46, "放上 NTAG 标签");

  setTextColor(WHITE);
  printCentered(90, "写入页  8");
  printCentered(112, "页数 16");
  printCentered(134, "每页 4 字节");

  setTextColor(GREEN);
  printCentered(196, "r 退出写卡");
}

void showWriteOK(uint8_t *uid, uint8_t len) {
  fillColor(BLACK);
  setTextBackground(BLACK);

  setTextColor(GREEN);
  printCentered(38, "写卡成功");

  char uidStr[32];
  uidToHex(uid, len, uidStr);
  setTextColor(WHITE);
  printCentered(80, uidStr);

  setTextColor(CYAN);
  printCentered(130, "读回校验通过");

  setTextColor(YELLOW);
  printCentered(196, "r 退出    w 继续");
}

void showWriteFail(uint8_t reason) {
  fillColor(BLACK);
  setTextBackground(BLACK);

  setTextColor(RED);
  printCentered(38, "写卡失败");

  setTextColor(WHITE);
  if (reason == 0)      printCentered(84, "写命令无响应");
  else if (reason == 1) printCentered(84, "写入后无应答");
  else                  printCentered(84, "读回数据不一致");

  setTextColor(YELLOW);
  printCentered(140, "标签拿开一点");
  printCentered(162, "再重新放上去");

  setTextColor(GREEN);
  printCentered(200, "r 退出写卡");
}

bool performWrite(uint8_t *uid, uint8_t uidLen) {
  uint16_t dataLen = strlen((char*)WRITE_DATA);
  Serial0.println("==== NTAG WRITE START ====");
  Serial0.printf("dataLen=%d pages=%d\n", dataLen, NTAG_PAGE_COUNT);

  for (uint8_t i = 0; i < NTAG_PAGE_COUNT; i++) {
    uint8_t page = NTAG_START_PAGE + i;
    uint16_t offset = i * 4;
    uint8_t data[4] = {0, 0, 0, 0};
    for (uint8_t b = 0; b < 4; b++) {
      if (offset + b < dataLen) data[b] = WRITE_DATA[offset + b];
    }
    Serial0.printf("  Page %d -> ", page);
    for (uint8_t k = 0; k < 4; k++) { if (data[k] < 0x10) Serial0.print("0"); Serial0.print(data[k], HEX); Serial0.print(" "); }

    uint8_t w = nfc.ntag2xx_WritePage(page, data);
    Serial0.printf(" -> %s\n", w ? "OK" : "FAIL");
    if (!w) { showWriteFail(1); return false; }
    delay(100);
  }
  Serial0.println("==== NTAG WRITE DONE ====");
  return true;
}

// ============ 主程序 ============

void printSerialHelp() {
  Serial0.println("---- 串口命令 ----");
  Serial0.println("  n    = 按键A：开新作业 / 确认");
  Serial0.println("  u    = 按键B：上翻页 / 取消");
  Serial0.println("  m    = 按键C：未交名单 / 下翻页");
  Serial0.println("  s    = 进入登记模式");
  Serial0.println("  w    = 进入 NTAG 写卡模式");
  Serial0.println("  r    = 回到刷卡界面");
  Serial0.println("  sim=<姓名>   模拟刷某人的卡");
  Serial0.println("  sim=<UID>    模拟刷指定 UID");
  Serial0.println("  sim?         模拟刷一张陌生卡");
  Serial0.println("  h    = 显示本帮助");
}

void enterMode(SystemMode m) {
  sysMode = m;
  switch (m) {
    case MODE_CHECKIN: showCheckInView(); break;
    case MODE_MISSING: showMissingView(); break;
    case MODE_SETUP:   showSetupMode();   break;
    case MODE_WRITE:   showWriteMode();   break;
    case MODE_CONFIRM_NEW: showNewHWConfirm(); break;
  }
}

void handleSerialSetup() {
  static uint8_t setupUID[7] = {0};
  static uint8_t setupUIDLen = 0;
  static bool waitingForName = false;

  while (Serial0.available()) {
    char line[64];
    uint8_t n = 0;
    while (Serial0.available() && n < sizeof(line) - 1) {
      char c = Serial0.read();
      if (c == '\n' || c == '\r') break;
      line[n++] = c;
    }
    line[n] = 0;
    if (n == 0) continue;

    if (strcmp(line, "d") == 0 || strcmp(line, "DONE") == 0) {
      saveRoster();
      saveAllHomework();
      Serial0.printf("Setup done! %d students saved.\n", studentCount);
      enterMode(MODE_CHECKIN);
      return;
    }
    if (strcmp(line, "list") == 0) {
      Serial0.println("Current roster:");
      for (uint8_t i = 0; i < studentCount; i++) {
        Serial0.printf("  %d. %s (UID:", i + 1, roster[i].name);
        for (uint8_t k = 0; k < roster[i].uidLen; k++) {
          Serial0.printf(" %02X", roster[i].uid[k]);
        }
        Serial0.println(")");
      }
    }
    if (strncmp(line, "name=", 5) == 0) {
      if (!waitingForName) {
        Serial0.println("No card scanned yet! Scan a sticker first.");
        continue;
      }
      if (studentCount >= MAX_STUDENTS) {
        Serial0.println("Roster full!");
        continue;
      }
      strncpy(roster[studentCount].name, line + 5, sizeof(roster[studentCount].name) - 1);
      roster[studentCount].name[sizeof(roster[studentCount].name) - 1] = 0;
      memcpy(roster[studentCount].uid, setupUID, setupUIDLen);
      roster[studentCount].uidLen = setupUIDLen;
      studentCount++;
      saveRoster();
      Serial0.printf("Saved: %d. %s\n", studentCount, roster[studentCount - 1].name);
      waitingForName = false;
      showSetupMode();
    }
    if (strcmp(line, "clear") == 0) {
      studentCount = 0;
      saveRoster();
      Serial0.println("Roster cleared!");
      showSetupMode();
    }
  }

  if (!nfcReady) return;

  uint8_t uid[7];
  uint8_t uidLength;
  uint8_t ok = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
  if (ok) {
    static uint8_t lastUID[7] = {0};
    static uint8_t lastLen = 0;
    bool changed = (uidLength != lastLen);
    if (!changed) for (uint8_t i = 0; i < uidLength; i++)
      if (uid[i] != lastUID[i]) { changed = true; break; }

    if (changed) {
      int existing = findStudent(uid, uidLength);
      if (existing >= 0) {
        Serial0.printf("Already enrolled: %d. %s\n", existing + 1, roster[existing].name);
      } else {
        memcpy(setupUID, uid, uidLength);
        setupUIDLen = uidLength;
        waitingForName = true;
        Serial0.print("Scanned new sticker! UID:");
        for (uint8_t i = 0; i < uidLength; i++) Serial0.printf(" %02X", uid[i]);
        Serial0.println();
        Serial0.println("Type: name=YOUR_NAME");
      }
      memcpy(lastUID, uid, uidLength);
      lastLen = uidLength;
      delay(1500);
    }
  }
}

void setup(void) {
  Serial0.begin(115200);
  Serial0.setTimeout(50);        // 串口命令按行读，别让 loop 卡在等换行上
  while (!Serial0) delay(10);
  delay(500);
  Serial0.println("=== RFID CheckIn ===");

  Serial0.println("Init ST7789...");
  st7789Init();
  Serial0.println("ST7789 OK");

  Serial0.println("Init PN532...");
  pinMode(PN532_RST, OUTPUT);
  digitalWrite(PN532_RST, LOW);
  delay(100);
  digitalWrite(PN532_RST, HIGH);
  delay(200);
  nfc.begin();

  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) {
    Serial0.println("WARN: PN532 not found - running WITHOUT reader");
    Serial0.println("      Use 'sim=<name>' to simulate a scan.");
    nfcReady = false;
  } else {
    Serial0.printf("PN5%d F/W %d.%d\n", (ver>>24)&0xFF, (ver>>16)&0xFF, (ver>>8)&0xFF);
    nfc.SAMConfig();
    nfcReady = true;
  }

  Serial0.println("Init buttons...");
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_NEXT), isrNext, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BTN_UP), isrUp, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_DOWN), isrDown, FALLING);

  Serial0.println("Init SD...");
  initSD();

  Serial0.println("Loading storage...");
  loadRoster();
  loadAllHomework();
  Serial0.printf("Students: %d, Current HW: %d\n", studentCount, currentHW + 1);

  if (studentCount == 0) {
    Serial0.println("No students enrolled. Starting setup...");
    Serial0.println("Scan each sticker, then type: name=YOUR_NAME");
    Serial0.println("Type 'd' when done, 'list' to see roster, 'clear' to reset.");
    enterMode(MODE_SETUP);
    return;
  }

  uint8_t submitted = getSubmittedCount(currentHW);
  if (submitted == studentCount && studentCount > 0 && currentHW + 1 < MAX_HOMEWORK) {
    enterMode(MODE_CONFIRM_NEW);
  } else {
    enterMode(MODE_CHECKIN);
  }

  printSerialHelp();
}

void loop(void) {
  // 长按 A 只在刷卡 / 未交名单 / 开新作业 这三个界面生效。
  // 登记模式和写卡模式下必须忽略，否则登记学生时长按会直接清空整份名单。
  if (btnNextLongPress) {
    btnNextLongPress = false;
    if (sysMode == MODE_CHECKIN || sysMode == MODE_MISSING || sysMode == MODE_CONFIRM_NEW) {
      Serial0.println("Long press detected - reset all");
      resetAll();
      enterMode(MODE_CHECKIN);
    } else {
      Serial0.println("Long press ignored (not in checkin view)");
    }
  }

  if (sysMode == MODE_SETUP) {
    handleSerialSetup();
    return;
  }

  if (sysMode == MODE_WRITE) {
    while (Serial0.available()) {
      char c = Serial0.read();
      if (c == 'r' || c == 'R') {
        writeMode = false;
        enterMode(MODE_CHECKIN);
        Serial0.println("-> CHECKIN MODE");
      }
    }
    if (!nfcReady) return;

    uint8_t uid[7];
    uint8_t uidLength;
    uint8_t ok = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
    if (ok) {
      bool changed = (uidLength != writeLastLen);
      if (!changed) for (uint8_t i = 0; i < uidLength; i++)
        if (uid[i] != writeLastUID[i]) { changed = true; break; }
      if (changed) {
        Serial0.print("Write UID: ");
        for (uint8_t i = 0; i < uidLength; i++) {
          if (uid[i] < 0x10) Serial0.print("0");
          Serial0.print(uid[i], HEX);
          Serial0.print(" ");
        }
        Serial0.println();
        bool res = performWrite(uid, uidLength);
        if (res) { showWriteOK(uid, uidLength); Serial0.println("WRITE OK"); }
        else { Serial0.println("WRITE FAIL"); }
        memcpy(writeLastUID, uid, uidLength);
        writeLastLen = uidLength;
        delay(2000);
        writeLastLen = 0;
      }
    }
    return;
  }

  if (sysMode == MODE_CONFIRM_NEW) {
    if (btnNextFlag) {
      btnNextFlag = false;
      nextHomework();
      Serial0.printf("Started HW %d\n", currentHW + 1);
      enterMode(MODE_CHECKIN);
    }
    if (btnUpFlag || btnDownFlag) {
      btnUpFlag = false; btnDownFlag = false;
      enterMode(MODE_CHECKIN);
    }
    return;
  }

  // 串口命令：n/u/m 等价于按键 A/B/C，没有按键时靠这些操作
  while (Serial0.available()) {
    String cmd = Serial0.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) continue;
    char c = cmd[0];

    if (c == 'n' || c == 'N') { btnNextFlag = true; Serial0.println("[A] next / confirm"); }
    else if (c == 'u' || c == 'U') { btnUpFlag = true; Serial0.println("[B] up / cancel"); }
    else if (c == 'm' || c == 'M') { btnDownFlag = true; Serial0.println("[C] missing / down"); }
    else if (c == 's' || c == 'S') { enterMode(MODE_SETUP); Serial0.println("-> SETUP MODE"); }
    else if (c == 'w' || c == 'W') {
      if (!nfcReady) { Serial0.println("Write mode needs PN532, not connected"); }
      else { writeMode = true; writeLastLen = 0; enterMode(MODE_WRITE); Serial0.println("-> WRITE MODE"); }
    }
    else if (c == 'r' || c == 'R') { enterMode(MODE_CHECKIN); Serial0.println("-> CHECKIN MODE"); }
    else if (cmd.startsWith("sim=")) {
      String arg = cmd.substring(4);
      arg.trim();
      if (arg.length() == 0) { Serial0.println("用法: sim=<姓名> 或 sim=<UID十六进制> 或 sim?"); continue; }
      if (arg == "?") { showCheckInResult(-1, nullptr, 0); Serial0.println("sim: unknown card"); continue; }

      // 先按姓名找，找不到再按 UID 十六进制找
      int idx = -1;
      for (uint8_t i = 0; i < studentCount; i++) {
        if (strcmp(roster[i].name, arg.c_str()) == 0) { idx = i; break; }
      }
      if (idx >= 0) {
        Serial0.printf("sim: %s\n", roster[idx].name);
        showCheckInResult(idx, roster[idx].uid, roster[idx].uidLen);
      } else {
        uint8_t uid[7] = {0};
        uint8_t len = 0;
        String tok = "";
        for (int i = 0; i < (int)arg.length(); i++) {
          char ch = arg[i];
          if (ch == ' ' || ch == ':') {
            if (tok.length() == 2 && len < 7) uid[len++] = (uint8_t)strtol(tok.c_str(), NULL, 16);
            tok = "";
          } else tok += ch;
        }
        if (tok.length() == 2 && len < 7) uid[len++] = (uint8_t)strtol(tok.c_str(), NULL, 16);
        if (len == 0) { Serial0.println("sim: 找不到这个学生"); continue; }
        idx = findStudent(uid, len);
        Serial0.printf("sim: UID -> %s\n", idx >= 0 ? roster[idx].name : "UNKNOWN");
        showCheckInResult(idx, uid, len);
      }
      delay(1500);
      enterMode(MODE_CHECKIN);
    }
    else if (c == 'h' || c == 'H') printSerialHelp();
    else Serial0.println("未知命令，输入 h 看帮助");
  }

  if (btnNextFlag) {
    btnNextFlag = false;
    uint8_t submitted = getSubmittedCount(currentHW);
    if (submitted == studentCount && studentCount > 0) {
      if (currentHW + 1 < MAX_HOMEWORK) {
        nextHomework();
        Serial0.printf("Auto-advanced to HW %d (all submitted)\n", currentHW + 1);
        showAllDone();
        delay(1500);
        enterMode(MODE_CHECKIN);
      }
    } else if (sysMode == MODE_CHECKIN) {
      enterMode(MODE_CONFIRM_NEW);
    } else if (sysMode == MODE_MISSING) {
      missingPage = 0;
      enterMode(MODE_CHECKIN);
    }
  }

  if (btnUpFlag) {
    btnUpFlag = false;
    if (sysMode == MODE_MISSING && missingPage > 0) {
      missingPage--;
      showMissingView();
    }
  }

  if (btnDownFlag) {
    btnDownFlag = false;
    if (sysMode == MODE_CHECKIN) {
      missingPage = 0;
      enterMode(MODE_MISSING);
    } else if (sysMode == MODE_MISSING) {
      missingPage++;
      showMissingView();
    }
  }

  if (sysMode != MODE_CHECKIN) return;
  if (!nfcReady) return;

  uint8_t uid[7];
  uint8_t uidLength;
  uint8_t ok = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);

  static uint8_t lastUID[7] = {0};
  static uint8_t lastLen = 0;
  if (ok) {
    bool changed = (uidLength != lastLen);
    if (!changed) for (uint8_t i = 0; i < uidLength; i++)
      if (uid[i] != lastUID[i]) { changed = true; break; }

    if (changed) {
      int idx = findStudent(uid, uidLength);
      Serial0.printf("UID: ");
      for (uint8_t i = 0; i < uidLength; i++) Serial0.printf("%02X ", uid[i]);
      if (idx >= 0) Serial0.printf("-> %s", roster[idx].name);
      else Serial0.print("-> UNKNOWN");
      Serial0.println();

      showCheckInResult(idx, uid, uidLength);
      memcpy(lastUID, uid, uidLength);
      lastLen = uidLength;
      delay(1500);

      uint8_t submitted = getSubmittedCount(currentHW);
      if (submitted == studentCount && studentCount > 0 && currentHW + 1 < MAX_HOMEWORK) {
        showAllDone();
        delay(1500);
        nextHomework();
        Serial0.printf("Auto HW %d started\n", currentHW + 1);
      }
      enterMode(MODE_CHECKIN);
    }
  } else {
    lastLen = 0;
  }
}