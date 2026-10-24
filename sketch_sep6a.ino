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

// ★ 顺序不能改：LovyanGFX 靠 `defined(_SD_H_)` 来决定是否为 fs::SDFS 生成
//   DataWrapperT 特化。SD.h 必须在 LovyanGFX.hpp 之前包含，否则
//   lcd.loadFont(SD, path) 会报 “invalid new-expression of abstract class type”。
#include <SPI.h>
#include <SD.h>
#include <Adafruit_PN532.h>
#include <LovyanGFX.hpp>
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

// PN532 固件版本（setup 时读到）。0 = 没读到。
uint32_t nfcFwVer = 0;

// 实际加载的 VLW 文件名（状态页要显示）
char fontName[48] = "";

// ============ 状态页（顶替不存在的指示灯） ============
// 板子上没有 LED 时，"系统现在跑到哪一步了"只能靠屏幕和串口来判断。
// 开机先弹这个状态页，串口敲 t 随时可以再调出来。
bool statusViewActive = false;
uint32_t statusViewUntil = 0;   // 0 = 一直停着，直到再按一次 t 切回

// 可选状态灯：接一颗 LED（串 220~330Ω 电阻）到任意一个空闲 GPIO 和 GND 之间，
// 然后把下面的数字改成那个 GPIO 号即可。-1 = 关闭（默认）。
// 常亮=正常；慢闪=读卡器没接上；快闪=SD 卡挂载失败；双闪=登记模式。
#define LED_PIN -1

// ============ 颜色 ============
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define CYAN    0x07FF
#define YELLOW  0xFFE0
#define GRAY    0x8410   // 次要信息（UID、页码），别跟正文抢注意力

// ============ 数据模型 ============
#define MAX_STUDENTS 50
#define MAX_HOMEWORK 30
#define BITS_PER_HW  7

struct Student {
  uint8_t uid[7];
  uint8_t uidLen;
  // 专属编号，1~MAX_STUDENTS，登记时按顺序自动分配。
  // 同时写进卡片用户页，刷卡时读回来和这里比对，能抓出拿错卡/换卡。
  uint16_t sid;
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
// 下面几个函数定义在后面的写卡区，这里先声明
int findStudentBySid(uint16_t sid);
void saveRoster();
uint16_t nextFreeSid();
bool writeCardSid(uint16_t sid);
bool readCardSid(uint16_t *sid);

// 状态页相关（定义在 UI 区，setup/loop 在更后面）
void showStatusView();
void printStatus();
void bootShowStatus();
void ledTick();

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
  f.println("# UID_HEX,SID,NAME");
  for (uint8_t i = 0; i < studentCount; i++) {
    char uidStr[32];
    uidToHex(roster[i].uid, roster[i].uidLen, uidStr);
    f.print(uidStr); f.print(",");
    f.print(roster[i].sid); f.print(",");
    f.println(roster[i].name);
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
    String rest = line.substring(comma + 1);

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

    // 新格式 UID,SID,NAME；旧格式 UID,NAME 只有两列，
    // 此时第二个逗号找不到，就当没有编号、按顺序补一个。
    uint16_t sid = 0;
    String namePart = rest;
    int comma2 = rest.indexOf(',');
    if (comma2 >= 0) {
      sid = (uint16_t)strtol(rest.substring(0, comma2).c_str(), NULL, 10);
      namePart = rest.substring(comma2 + 1);
    }

    memcpy(roster[studentCount].uid, uidBuf, uidLen);
    roster[studentCount].uidLen = uidLen;
    roster[studentCount].sid = sid;
    strncpy(roster[studentCount].name, namePart.c_str(), sizeof(roster[studentCount].name) - 1);
    roster[studentCount].name[sizeof(roster[studentCount].name) - 1] = 0;
    studentCount++;
  }
  f.close();

  // 编号为 0 说明是老格式，按顺序补齐，顺便排掉重复编号
  bool needFix = false;
  for (uint8_t i = 0; i < studentCount; i++) {
    if (roster[i].sid == 0) needFix = true;
  }
  if (needFix) {
    Serial0.println("Old roster format, assigning SIDs...");
    uint16_t s = 1;
    for (uint8_t i = 0; i < studentCount; i++) {
      if (roster[i].sid != 0) continue;
      while (s <= MAX_STUDENTS && findStudentBySid(s) >= 0) s++;
      if (s > MAX_STUDENTS) break;
      roster[i].sid = s++;
    }
    saveRoster();
  }
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
  f.println("# SID,NAME,UID,SUBMITTED");
  for (uint8_t i = 0; i < studentCount; i++) {
    char uidStr[32];
    uidToHex(roster[i].uid, roster[i].uidLen, uidStr);
    f.print(roster[i].sid); f.print(",");
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

// ============ 字库 ============
// 两套方案，SD 卡上的字库优先，找不到就回退内置点阵：
//   1. SD 卡 /font.vlw —— 8bit 抗锯齿灰度，观感最好
//   2. 内置 16x16 点阵（font_cn.h）—— 编译进 flash，不插卡也能显示中文
//
// ⚠️ 为什么 SD 卡上放的是 .vlw 而不是 .ttf：
//    LovyanGFX 的 TTF 解码器在官方仓库的可选文件 lgfx_ttffont.hpp 里，
//    库管理器装的发行版没有它。没有它时 load_font() 把所有文件都当
//    VLW 解析（看 lgfx_fonts.cpp 的 #ifdef LGFX_TTFFONT_HPP_），
//    塞个 .ttf 进去只会解析失败或显示乱码。
//    .vlw 本来就是"用任意 TTF 预渲染出来的字体文件"，
//    由 tools/gen_font_cn.py 从 SimHei 等字体生成（15px / 行高 16）。
//
// ⚠️ 关键：loadFont() 的第二个参数是"字体格式枚举"，不是字号！
//    旧代码写成 loadFont("/font.ttf", 16) 是把 16 当格式传，编译都过不了。
#include "font_cn.h"

// true  = 走 lcd.print()，用 SD 卡上的 vlw 字库
// false = 走自绘 drawText()，用内置点阵字库
bool useTTF = false;

// 目标行高。vlw 按 15px 生成（ascent 13 + descent 3 = 16），
// 内置点阵也是 16；万一卡里放了别的字号的 vlw，initFont() 会等比缩放。
#define FONT_SIZE 16

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
  if (useTTF) return lcd.textWidth(s);
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
  lcd.setTextColor(WHITE, BLACK);
  lcd.setFont(&fonts::Font0);
}

// 列出 SD 卡根目录的文件（判断"卡上到底有什么"时很有用）
void listSdFiles(const char *tag) {
  Serial0.printf("%s\n", tag);
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    Serial0.println("  (打不开根目录)");
    if (root) root.close();
    return;
  }
  int n = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    Serial0.printf("  %-24s %8lu B%s\n", f.name(), (unsigned long)f.size(),
                   f.isDirectory() ? "  [目录]" : "");
    n++;
    f.close();
  }
  root.close();
  if (n == 0) Serial0.println("  (空)");
}

// 判断一个文件是不是 VLW（不是的话多半是 .ttf 改名来的）
bool looksLikeVlw(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint8_t h[4] = {0, 0, 0, 0};
  f.read(h, 4);
  f.close();
  if (h[0] == 0x00 && h[1] == 0x01) return false;                 // TTF (sfnt)
  if (h[0] == 'O' && h[1] == 'T' && h[2] == 'T' && h[3] == 'O') return false;  // OTF
  if (h[0] == 't' && h[1] == 'r' && h[2] == 'u' && h[3] == 'e') return false;  // TrueType
  if (h[0] == 'w' && h[1] == 'O' && h[2] == 'F' && h[3] == 'F') return false;  // WOFF
  if (h[0] == 'w' && h[1] == 'O' && h[2] == 'F' && h[3] == '2') return false;  // WOFF2
  return true;
}

// 必须在 initSD() 之后调用：SD 卡上有没有字库到这时才知道
void initFont() {
  useTTF = false;
  fontName[0] = 0;

  if (!sdReady) {
    Serial0.println("Font: no SD card, fall back to built-in 16x16 bitmap");
    return;
  }

  // 只认 vlw。loadFont() 没装 TTF 解码器时会把一切当 VLW 解析，
  // 别放 .ttf/.otf 进卡，会乱码。
  // 优先 /font.vlw；没有就扫根目录，把第一个 .vlw 拿来用（文件名不用改）。
  char path[48] = "";
  if (SD.exists("/font.vlw")) {
    strcpy(path, "/font.vlw");
  } else {
    File root = SD.open("/");
    if (root && root.isDirectory()) {
      for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (!f.isDirectory()) {
          const char *n = f.name();
          size_t len = strlen(n);
          if (len > 4 && len < 40) {
            bool isV = (n[len-4] == '.') &&
                       (n[len-3] == 'v' || n[len-3] == 'V') &&
                       (n[len-2] == 'l' || n[len-2] == 'L') &&
                       (n[len-1] == 'w' || n[len-1] == 'W');
            if (isV) { snprintf(path, sizeof(path), "/%s", n); f.close(); break; }
          }
        }
        f.close();
      }
      root.close();
    }
  }

  if (path[0] == 0) {
    Serial0.println("Font: no .vlw on card, using built-in bitmap");
    listSdFiles("Font: 卡上现在的文件:");
    Serial0.printf("Font: built-in bitmap (%d ASCII + %d CJK)\n",
                   FONT_CN_ASCII_COUNT, FONT_CN_CJK_COUNT);
    return;
  }

  Serial0.printf("Font: try %s...\n", path);
  if (!looksLikeVlw(path)) {
    Serial0.println("Font: 这个文件不是 VLW 格式（可能是 .ttf 改了后缀）");
    Serial0.println("Font: 请用 tools/gen_font_cn.py 生成真正的 font.vlw");
    listSdFiles("Font: 卡上现在的文件:");
    return;
  }

  if (lcd.loadFont(SD, path)) {
    // setTextSize 是"倍率"不是像素！VLW 自带行高，按行高等比缩放到 FONT_SIZE，
    // 这样就算以后重新生成了别的字号的 vlw，UI 的 16px 行距也不会乱。
    float h = (float)lcd.fontHeight();
    if (h > 1.0f) lcd.setTextSize((float)FONT_SIZE / h);
    useTTF = true;
    snprintf(fontName, sizeof(fontName), "%s", path);
    Serial0.printf("Font: vlw loaded (%s), native line height=%.0f, scale=%.2f\n",
                   path, h, (h > 1.0f) ? (float)FONT_SIZE / h : 1.0f);
    return;
  }
  Serial0.println("Font: load failed (文件损坏或字形表为空?)");
  listSdFiles("Font: 卡上现在的文件:");
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

void setTextColor(uint16_t c) {
  _fgColor = c;
  if (useTTF) lcd.setTextColor(c, _bgColor);
}
void setTextBackground(uint16_t c) {
  _bgColor = c;
  if (useTTF) lcd.setTextColor(_fgColor, c);
}
void setCursor(int16_t x, int16_t y) {
  _cx = x; _cy = y;
  if (useTTF) lcd.setCursor(x, y);
}
void printStr(const char *s) {
  if (useTTF) { lcd.print(s); return; }   // TTF：交给 LovyanGFX 渲染
  _cx = drawText(_cx, _cy, s, _fgColor, _bgColor);
}
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

// ============ 状态页 ============
// 一屏看清：屏幕 / 读卡器 / SD 卡 / 字库 / 名单 / 作业 各到哪一步了。
void statusRow(int16_t y, const char *label, const char *value, uint16_t vc) {
  setTextColor(GRAY);
  setCursor(4, y);
  printStr(label);
  setTextColor(vc);
  setCursor(58, y);
  printStr(value);
}

void showStatusView() {
  statusViewActive = true;
  fillColor(BLACK);
  setTextBackground(BLACK);
  setTextColor(YELLOW);
  printCentered(2, "系统状态");
  drawFastHLine(0, 20, 240, CYAN);

  char buf[32];

  statusRow(26, "屏幕", "OK  ST7789", GREEN);

  if (nfcReady) {
    snprintf(buf, sizeof(buf), "OK  PN532 F/W %d.%d",
             (nfcFwVer >> 16) & 0xFF, (nfcFwVer >> 8) & 0xFF);
    statusRow(52, "读卡器", buf, GREEN);
  } else {
    statusRow(52, "读卡器", "未找到！", RED);
  }

  if (sdReady) {
    statusRow(78, "SD卡", "OK", GREEN);
  } else {
    statusRow(78, "SD卡", "挂载失败！", RED);
  }

  if (useTTF) {
    // 文件名可能很长，截断一下免得撑破 240 宽
    char fn[24];
    snprintf(fn, sizeof(fn), "%s", fontName);
    statusRow(104, "字库", fn, GREEN);
  } else {
    statusRow(104, "字库", "内置点阵", YELLOW);
  }

  snprintf(buf, sizeof(buf), "%d 人", studentCount);
  statusRow(130, "学生", buf, WHITE);

  snprintf(buf, sizeof(buf), "第 %d 次", currentHW + 1);
  statusRow(156, "作业", buf, WHITE);

  snprintf(buf, sizeof(buf), "%d / %d", getSubmittedCount(currentHW), studentCount);
  statusRow(182, "已交", buf, WHITE);

  setTextColor(GRAY);
  printCentered(210, "t 返回  h 帮助");
}

// 串口版状态报告，比屏幕上更详细
void printStatus() {
  Serial0.println("---- 系统状态 ----");
  Serial0.printf("  屏幕      : OK  ST7789 240x240\n");
  if (nfcReady) {
    Serial0.printf("  读卡器    : OK  PN532 F/W %d.%d\n",
                   (nfcFwVer >> 16) & 0xFF, (nfcFwVer >> 8) & 0xFF);
  } else {
    Serial0.println("  读卡器    : 未找到 —— 检查接线 + 拨码开关(SPI 模式) + 断电重上电");
  }
  Serial0.printf("  SD 卡     : %s\n", sdReady ? "OK" : "挂载失败 —— 检查接线/卡是否插好/是不是 FAT32");
  if (useTTF) Serial0.printf("  字库      : %s（卡上）\n", fontName);
  else Serial0.println("  字库      : 内置 16x16 点阵（卡上没有 .vlw）");
  Serial0.printf("  学生      : %d 人\n", studentCount);
  Serial0.printf("  当前作业  : 第 %d 次，已交 %d/%d\n",
                 currentHW + 1, getSubmittedCount(currentHW), studentCount);
  const char *m = "刷卡登记";
  switch (sysMode) {
    case MODE_CHECKIN:    m = "刷卡登记"; break;
    case MODE_MISSING:    m = "未交名单"; break;
    case MODE_SETUP:      m = "登记模式（刷卡 + name=姓名）"; break;
    case MODE_WRITE:      m = "写卡模式（批量发编号）"; break;
    case MODE_CONFIRM_NEW:m = "确认开新作业"; break;
  }
  Serial0.printf("  界面模式  : %s\n", m);
  Serial0.printf("  运行      : 已启动 %lu 秒，空闲内存 %lu KB\n",
                 millis() / 1000, ESP.getFreeHeap() / 1024);
  Serial0.println("  提示      : t 状态页  h 帮助  probe 读卡器自检  list 名单");
  Serial0.println("-------------------");
}

// 可选状态灯。LED_PIN 配成实际 GPIO 号才生效（-1 = 关闭）。
// 常亮=一切正常；慢闪=读卡器没接上；快闪=SD 卡挂了；双闪=登记模式。
void ledTick() {
#if LED_PIN >= 0
  static uint32_t t0 = 0;
  static uint8_t  phase = 255;
  uint32_t now = millis();
  if (now - t0 < 60) return;

  uint8_t want;
  if (!nfcReady)      want = 1;   // 慢闪
  else if (!sdReady)  want = 2;   // 快闪
  else if (sysMode == MODE_SETUP || sysMode == MODE_WRITE) want = 3;  // 双闪
  else                want = 0;   // 常亮

  if (want == 0) {
    if (phase != 0) { digitalWrite(LED_PIN, HIGH); phase = 0; }
    t0 = now;
    return;
  }
  if (want == 1) {            // 1 Hz
    if ((now - t0) >= 500) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); t0 = now; }
    return;
  }
  if (want == 2) {            // 5 Hz
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    t0 = now;
    return;
  }
  // 双闪：亮 60ms 灭 120ms 亮 60ms 灭 720ms
  if (phase == 255) { phase = 0; t0 = now; return; }
  if (now - t0 >= 120) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    t0 = now;
    phase++;
    if (phase > 3) phase = 0;
  }
#endif
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

  char idBuf[32];
  snprintf(idBuf, sizeof(idBuf), "[%04u]", roster[stuIdx].sid);
  setTextColor(GRAY);
  printCentered(30, idBuf);

  setTextColor(WHITE);
  printCentered(56, roster[stuIdx].name);

  if (isSubmitted(currentHW, stuIdx)) {
    setTextColor(YELLOW);
    printCentered(88, "这次已经交过了");
  } else {
    markSubmitted(currentHW, stuIdx);
    setTextColor(GREEN);
    printCentered(88, "提交成功");
  }

  uint8_t submitted = getSubmittedCount(currentHW);
  uint8_t pct = studentCount ? (submitted * 100 / studentCount) : 0;

  // 进度条：外框 + 填充
  lcd.drawRect(20, 120, 200, 12, WHITE);
  uint16_t barW = (uint16_t)submitted * 196 / (studentCount ? studentCount : 1);
  fillRect(22, 122, barW, 8, GREEN);

  char prog[32];
  snprintf(prog, sizeof(prog), "已交 %d/%d  %d%%", submitted, studentCount, pct);
  setTextColor(WHITE);
  printCentered(148, prog);

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
    // 左列编号（4 位数字 + 两个空格 = 48px），右列姓名
    char num[12];
    snprintf(num, sizeof(num), "%04u", roster[missing[i]].sid);
    setTextColor(CYAN);
    setCursor(6, row); printStr(num);
    setTextColor(WHITE);
    setCursor(60, row);
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

// ============ 写卡：给每个学生发专属编号并写进卡片 ============
//
// 卡片用户页布局（只占 2 页，写起来快，刷卡校验也快）：
//   第 8 页: 'R' 'F' 'I' 'D'   魔数，一眼认出是我们的卡
//   第 9 页: 版本号 | 编号高字节 | 编号低字节 | 校验和
//   校验和 = 前 7 个字节按位异或，防止卡被写坏或读到脏数据
//
// 为什么不再写那个固定字符串 "HELLO-RFID-TEST-!"：所有卡内容一样，
// 刷卡时没法核对身份，只能靠 UID 猜。

#define NTAG_START_PAGE  8
#define NTAG_MAGIC_P0    'R'
#define NTAG_MAGIC_P1    'F'
#define NTAG_MAGIC_P2    'I'
#define NTAG_MAGIC_P3    'D'
#define CARD_VERSION     1

bool writeMode = false;
uint8_t writeLastUID[7] = {0};
uint8_t writeLastLen = 0;

// 当前待写入的编号；写卡模式下刷卡时分配
uint16_t writePendingSid = 0;

static uint8_t xorSum(const uint8_t *d, uint8_t n) {
  uint8_t s = 0;
  for (uint8_t i = 0; i < n; i++) s ^= d[i];
  return s;
}

// 读回卡内编号。返回 true 表示卡里确实是本系统的卡且校验通过
bool readCardSid(uint16_t *sid) {
  if (!nfcReady) return false;

  uint8_t p0[4] = {0}, p1[4] = {0};
  if (!nfc.ntag2xx_ReadPage(NTAG_START_PAGE, p0)) return false;
  delay(20);
  if (!nfc.ntag2xx_ReadPage(NTAG_START_PAGE + 1, p1)) return false;
  delay(20);

  Serial0.print("Card raw: ");
  for (int i = 0; i < 4; i++) Serial0.printf("%02X ", p0[i]);
  for (int i = 0; i < 4; i++) Serial0.printf("%02X ", p1[i]);
  Serial0.println();

  if (p0[0] != NTAG_MAGIC_P0 || p0[1] != NTAG_MAGIC_P1 ||
      p0[2] != NTAG_MAGIC_P2 || p0[3] != NTAG_MAGIC_P3) {
    Serial0.println("  -> not our card (magic mismatch)");
    return false;
  }
  if (p1[0] != CARD_VERSION) {
    Serial0.printf("  -> version %u, expect %u\n", p1[0], CARD_VERSION);
    return false;
  }
  if (xorSum(p0, 4) != 0 || p1[0] ^ p1[1] ^ p1[2] != p1[3]) {
    Serial0.println("  -> CRC failed, card data corrupted");
    return false;
  }

  *sid = ((uint16_t)p1[1] << 8) | p1[2];
  Serial0.printf("  -> card sid=%u\n", *sid);
  return true;
}

// 把编号写进卡片，写完立刻读回核对
bool writeCardSid(uint16_t sid) {
  if (!nfcReady) return false;

  uint8_t p0[4] = {NTAG_MAGIC_P0, NTAG_MAGIC_P1, NTAG_MAGIC_P2, NTAG_MAGIC_P3};
  uint8_t p1[4] = {CARD_VERSION, (uint8_t)(sid >> 8), (uint8_t)(sid & 0xFF), 0};
  p1[3] = xorSum(p1, 3);

  Serial0.printf("Write sid=%u (hi=%02X lo=%02X crc=%02X)\n",
                 sid, p1[1], p1[2], p1[3]);

  if (!nfc.ntag2xx_WritePage(NTAG_START_PAGE, p0)) return false;
  delay(60);
  if (!nfc.ntag2xx_WritePage(NTAG_START_PAGE + 1, p1)) return false;
  delay(60);

  uint16_t back = 0;
  if (!readCardSid(&back)) {
    Serial0.println("  verify read failed");
    return false;
  }
  if (back != sid) {
    Serial0.printf("  verify MISMATCH: wrote %u, read %u\n", sid, back);
    return false;
  }
  Serial0.println("  verify OK");
  return true;
}

// 下一个可用的顺序编号：跳过已占用的
uint16_t nextFreeSid() {
  for (uint16_t s = 1; s <= MAX_STUDENTS; s++) {
    bool used = false;
    for (uint8_t i = 0; i < studentCount; i++) {
      if (roster[i].sid == s) { used = true; break; }
    }
    if (!used) return s;
  }
  return 0;   // 编号用完了
}

// 按编号反查学生，-1 表示没人
int findStudentBySid(uint16_t sid) {
  for (uint8_t i = 0; i < studentCount; i++) {
    if (roster[i].sid == sid) return i;
  }
  return -1;
}

// 编号核对失败：卡里存的编号和名单不一致，或者卡没写过编号
void showSidMismatch(int stuIdx, uint16_t cardSid, uint8_t *uid, uint8_t len) {
  fillColor(BLACK);
  setTextBackground(BLACK);

  setTextColor(RED);
  printCentered(30, "编号不符");

  char buf[32];
  if (stuIdx >= 0) {
    snprintf(buf, sizeof(buf), "名单编号 %04u", roster[stuIdx].sid);
    setTextColor(WHITE);
    printCentered(70, buf);

    if (cardSid == 0) {
      setTextColor(YELLOW);
      printCentered(92, "卡里没有编号");
    } else {
      snprintf(buf, sizeof(buf), "卡里编号 %04u", cardSid);
      setTextColor(YELLOW);
      printCentered(92, buf);
      // 如果卡里那个编号属于别人，直接告诉你是谁
      int other = findStudentBySid(cardSid);
      if (other >= 0) {
        setTextColor(CYAN);
        snprintf(buf, sizeof(buf), "那是 %s 的卡", roster[other].name);
        printCentered(114, buf);
      }
    }

    setTextColor(WHITE);
    printCentered(146, roster[stuIdx].name);

    setTextColor(GRAY);
    char uidStr[32];
    uidToHex(uid, len, uidStr);
    printCentered(168, uidStr);
  }

  setTextColor(YELLOW);
  printCentered(200, "别代交  拿开重刷");
}

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
  printCentered(44, "放上一张空白标签");

  char buf[32];
  snprintf(buf, sizeof(buf), "将分配编号 %04u", writePendingSid);
  setTextColor(WHITE);
  printCentered(84, buf);
  setTextColor(GRAY);
  printCentered(106, "写完刷另一张继续");

  setTextColor(YELLOW);
  printCentered(196, "r 退出写卡");
}

// 写卡结果页。kind: 0=成功新卡 1=重写已有卡 2=失败
void showWriteDone(uint8_t kind, uint16_t sid, uint8_t *uid, uint8_t len) {
  fillColor(BLACK);
  setTextBackground(BLACK);

  setTextColor(kind == 2 ? RED : GREEN);
  printCentered(36, kind == 2 ? "写卡失败" : "写卡成功");

  char buf[32];
  snprintf(buf, sizeof(buf), "编号 %04u", sid);
  setTextColor(WHITE);
  printCentered(76, buf);

  char uidStr[32];
  uidToHex(uid, len, uidStr);
  setTextColor(GRAY);
  printCentered(100, uidStr);

  if (kind == 1) {
    int stu = findStudentBySid(sid);
    if (stu >= 0) {
      setTextColor(CYAN);
      snprintf(buf, sizeof(buf), "%s", roster[stu].name);
      printCentered(128, buf);
    }
  }
  setTextColor(YELLOW);
  printCentered(200, kind == 2 ? "换一张重试" : "换下一张继续");
}

// ============ 主程序 ============

void printSerialHelp() {
  Serial0.println("---- 串口命令 ----");
  Serial0.println("  n    = 按键A：开新作业 / 确认");
  Serial0.println("  u    = 按键B：上翻页 / 取消");
  Serial0.println("  m    = 按键C：未交名单 / 下翻页");
  Serial0.println("  s    = 进入登记模式（刷卡 + name=姓名，自动发编号并写卡）");
  Serial0.println("  w    = 进入写卡模式（批量发编号 / 补写卡）");
  Serial0.println("  r    = 回到刷卡界面");
  Serial0.println("  list = 查看名单（含编号）");
  Serial0.println("  t    = 状态页：屏幕+串口看各模块运行状况（没有指示灯就看这个）");
  Serial0.println("  ls   = 列出 SD 卡根目录的文件");
  Serial0.println("  probe    = 读卡器自检：连续扫 10 秒，打印每张卡的 UID 和卡内编号");
  Serial0.println("  sim=<姓名>   模拟刷某人的卡");
  Serial0.println("  sim=<UID>    模拟刷指定 UID");
  Serial0.println("  sim?         模拟刷一张陌生卡");
  Serial0.println("  h    = 显示本帮助");
}

void enterMode(SystemMode m) {
  sysMode = m;
  // 切界面时把状态页收掉，免得状态页和业务界面互相盖
  statusViewActive = false;
  statusViewUntil = 0;
  switch (m) {
    case MODE_CHECKIN: showCheckInView(); break;
    case MODE_MISSING: showMissingView(); break;
    case MODE_SETUP:   showSetupMode();   break;
    case MODE_WRITE:
      // 进来就把"下一个要发的编号"算好，写卡页要显示它
      writePendingSid = nextFreeSid();
      if (writePendingSid == 0) writePendingSid = MAX_STUDENTS;
      Serial0.printf("Write mode, next free sid=%u\n", writePendingSid);
      showWriteMode();
      break;
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
        Serial0.printf("  [%04u] %s (UID:", roster[i].sid, roster[i].name);
        for (uint8_t k = 0; k < roster[i].uidLen; k++) {
          Serial0.printf(" %02X", roster[i].uid[k]);
        }
        Serial0.println(")");
      }
    }
    if (strcmp(line, "t") == 0 || strcmp(line, "T") == 0) {
      showStatusView();
      statusViewUntil = 0;
      printStatus();
    }
    if (strcmp(line, "ls") == 0 || strcmp(line, "LS") == 0) {
      if (!sdReady) Serial0.println("ls: SD 卡没挂上");
      else listSdFiles("---- SD 卡根目录 ----");
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
      // 自动分配下一个空闲编号
      uint16_t sid = nextFreeSid();
      if (sid == 0) {
        Serial0.println("No free SID left!");
        continue;
      }
      strncpy(roster[studentCount].name, line + 5, sizeof(roster[studentCount].name) - 1);
      roster[studentCount].name[sizeof(roster[studentCount].name) - 1] = 0;
      memcpy(roster[studentCount].uid, setupUID, setupUIDLen);
      roster[studentCount].uidLen = setupUIDLen;
      roster[studentCount].sid = sid;
      studentCount++;
      saveRoster();
      Serial0.printf("Saved: [%04u] %s\n", roster[studentCount - 1].sid,
                     roster[studentCount - 1].name);

      // 顺手把这张卡的编号也写进去，省得再去写卡模式走一遍
      if (nfcReady) {
        if (writeCardSid(sid)) {
          Serial0.printf("Card sid written and verified: %u\n", sid);
        } else {
          Serial0.printf("WARN: could not write sid to card, run 'w' mode to retry\n");
        }
      }
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
    nfcFwVer = ver;
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

#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
#endif

  Serial0.println("Init SD...");
  initSD();
  initFont();          // 必须在 initSD() 之后

  Serial0.println("Loading storage...");
  loadRoster();
  loadAllHomework();
  Serial0.printf("Students: %d, Current HW: %d\n", studentCount, currentHW + 1);

  if (studentCount == 0) {
    Serial0.println("No students enrolled. Starting setup...");
    Serial0.println("Scan each sticker, then type: name=YOUR_NAME");
    Serial0.println("Type 'd' when done, 'list' to see roster, 'clear' to reset.");
    enterMode(MODE_SETUP);
    printSerialHelp();
    bootShowStatus();
    return;
  }

  uint8_t submitted = getSubmittedCount(currentHW);
  if (submitted == studentCount && studentCount > 0 && currentHW + 1 < MAX_HOMEWORK) {
    enterMode(MODE_CONFIRM_NEW);
  } else {
    enterMode(MODE_CHECKIN);
  }

  printSerialHelp();
  bootShowStatus();
}

// 开机先弹状态页：全正常 4 秒后自动回主界面；有故障就停在状态页别动，
// 让人一眼看出是哪一环挂了（板子上没指示灯，只能靠这个）。
void bootShowStatus() {
  showStatusView();
  if (nfcReady && sdReady) statusViewUntil = millis() + 4000;
  else statusViewUntil = 0;
  Serial0.println("(屏幕上是状态页，串口敲 t 随时再看；h 看命令列表)");
}

void loop(void) {
  // 状态页超时自动退回当前界面
  if (statusViewActive && statusViewUntil != 0 && (int32_t)(millis() - statusViewUntil) >= 0) {
    statusViewUntil = 0;
    statusViewActive = false;
    enterMode(sysMode);
  }
  ledTick();

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

        // 已经登记过的卡 -> 把它的编号重写回去（补写/修卡）
        // 没登记过的卡 -> 分配一个新编号
        int known = findStudent(uid, uidLength);
        uint16_t sid;
        uint8_t kind;
        if (known >= 0) {
          sid = roster[known].sid;
          kind = 1;
          Serial0.printf("Known card #%d, re-write sid=%u\n", known + 1, sid);
        } else {
          sid = nextFreeSid();
          kind = 0;
          Serial0.printf("New card, assign sid=%u\n", sid);
        }

        if (sid == 0) {
          Serial0.println("No free SID left!");
          showWriteDone(2, 0, uid, uidLength);
        } else if (writeCardSid(sid)) {
          showWriteDone(kind, sid, uid, uidLength);
          Serial0.println("WRITE OK");
          // 只有真发了新编号才推进待分配号
          if (kind == 0) {
            writePendingSid = nextFreeSid();
            if (writePendingSid == 0) writePendingSid = MAX_STUDENTS;
            showWriteMode();
          }
        } else {
          showWriteDone(2, sid, uid, uidLength);
          Serial0.println("WRITE FAIL");
        }

        memcpy(writeLastUID, uid, uidLength);
        writeLastLen = uidLength;
        delay(2500);
        writeLastLen = 0;      // 清零，下一张卡才算"换卡"
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
    else if (c == 'l' || c == 'L') {
      if (!sdReady) { Serial0.println("ls: SD 卡没挂上"); }
      else listSdFiles("---- SD 卡根目录 ----");
    }
    else if (c == 't' || c == 'T') {
      if (statusViewActive) {
        statusViewActive = false;
        statusViewUntil = 0;
        enterMode(sysMode);
        Serial0.println("-> 状态页已关闭");
      } else {
        showStatusView();
        statusViewUntil = 0;      // 手动打开就一直停着
        Serial0.println("-> 状态页（再敲 t 关闭）");
      }
      printStatus();
    }
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
    else if (cmd.startsWith("probe")) {
      // 读卡器自检：只走读卡链路，不碰名单、不写卡，
      // 用来确认接线和拨码开关对不对。
      if (!nfcReady) {
        Serial0.println("probe: PN532 没连上，先查接线和拨码开关");
        continue;
      }
      Serial0.println("probe: 连续扫描 10 秒，把卡片放上去...");
      unsigned long t0 = isrMillis();
      uint8_t lastUID[7] = {0};
      uint8_t lastLen = 0;
      int seen = 0;
      while (isrMillis() - t0 < 10000) {
        uint8_t uid[7];
        uint8_t len = 0;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len)) {
          bool same = (len == lastLen);
          if (same) for (uint8_t i = 0; i < len; i++)
            if (uid[i] != lastUID[i]) { same = false; break; }
          if (!same) {
            Serial0.printf("  卡 #%d UID:", ++seen);
            for (uint8_t i = 0; i < len; i++) Serial0.printf(" %02X", uid[i]);
            Serial0.printf("  (%d 字节)", len);

            uint16_t sid = 0;
            if (readCardSid(&sid)) {
              int owner = findStudentBySid(sid);
              Serial0.printf("  编号=%04u", sid);
              if (owner >= 0) Serial0.printf("  属于 %s", roster[owner].name);
              else Serial0.print("  名单里没人");
            } else {
              Serial0.print("  卡内无编号（空白卡 / 非本系统）");
            }
            int known = findStudent(uid, len);
            Serial0.printf("  名单:%s\n", known >= 0 ? roster[known].name : "未登记");
            memcpy(lastUID, uid, len);
            lastLen = len;
          }
          delay(300);
        } else {
          lastLen = 0;
          delay(100);
        }
      }
      Serial0.printf("probe 结束，共识别 %d 张卡\n", seen);
      if (seen == 0) {
        Serial0.println("一张都没读到，依次检查：");
        Serial0.println("  1. 拨码开关是不是 SPI 模式");
        Serial0.println("  2. SCK/MISO/MOSI/SS 四根线有没有接错或松动");
        Serial0.println("  3. 卡片是不是 ISO14443A（NTAG/Mifare）—— 身份证和门禁卡不行");
      }
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

      // 核对卡内编号：UID 对上名单还不够，卡里存的编号也得一致。
      // 不一致说明这张卡是别人的（张冠李戴）或被改写过，必须拦下来。
      if (idx >= 0) {
        uint16_t cardSid = 0;
        if (readCardSid(&cardSid)) {
          if (cardSid == roster[idx].sid) {
            Serial0.printf("  [sid %04u OK]", cardSid);
          } else {
            Serial0.printf("  [SID MISMATCH! card=%u roster=%u]",
                           cardSid, roster[idx].sid);
            Serial0.println("  !! 卡里编号和名单对不上，疑似拿错卡/换卡");
            showSidMismatch(idx, cardSid, uid, uidLength);
            memcpy(lastUID, uid, uidLength);
            lastLen = uidLength;
            delay(2500);
            enterMode(MODE_CHECKIN);
            return;
          }
        } else {
          Serial0.println("  [card has no valid sid - was it written?]");
          showSidMismatch(idx, 0, uid, uidLength);
          memcpy(lastUID, uid, uidLength);
          lastLen = uidLength;
          delay(2500);
          enterMode(MODE_CHECKIN);
          return;
        }
      }
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