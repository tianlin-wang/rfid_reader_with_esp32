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

#include <Adafruit_PN532.h>
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

// ============ 颜色 ============
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define CYAN    0x07FF
#define YELLOW  0xFFE0

// ============ 写卡配置 ============
// NTAG213: pages 0x04~0x28,  NTAG215: 0x04~0x7F,  NTAG216: 0x04~0xE1
// 前 4 页是 UID/厂商数据，只读
#define NTAG_START_PAGE  8
#define NTAG_PAGE_COUNT  16

const uint8_t WRITE_DATA[] = "HELLO-RFID-TEST-!";

bool writeMode = false;
uint8_t writeLastUID[7] = {0};
uint8_t writeLastLen = 0;

// ============ 极简 5x7 字库 ============
const uint8_t FONT5x7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, // space 0
  {0x3E,0x51,0x49,0x45,0x3E}, // 0      1
  {0x00,0x42,0x7F,0x40,0x00}, // 1      2
  {0x42,0x61,0x51,0x49,0x46}, // 2      3
  {0x21,0x41,0x45,0x4B,0x31}, // 3      4
  {0x18,0x14,0x12,0x7F,0x10}, // 4      5
  {0x27,0x45,0x45,0x45,0x39}, // 5      6
  {0x3C,0x4A,0x49,0x49,0x30}, // 6      7
  {0x01,0x71,0x09,0x05,0x03}, // 7      8
  {0x36,0x49,0x49,0x49,0x36}, // 8      9
  {0x06,0x49,0x49,0x29,0x1E}, // 9      10
  {0x3E,0x09,0x09,0x09,0x3E}, // A      11
  {0x7F,0x49,0x49,0x49,0x36}, // B      12
  {0x3E,0x41,0x41,0x41,0x22}, // C      13
  {0x7F,0x41,0x41,0x22,0x1C}, // D      14
  {0x7F,0x49,0x49,0x49,0x41}, // E      15
  {0x7F,0x09,0x09,0x09,0x01}, // F      16
  {0x3E,0x41,0x49,0x49,0x3A}, // G      17
  {0x7F,0x08,0x08,0x08,0x7F}, // H      18
  {0x00,0x41,0x7F,0x41,0x00}, // I      19
  {0x20,0x40,0x41,0x3F,0x01}, // J      20
  {0x7F,0x08,0x14,0x22,0x41}, // K      21
  {0x7F,0x40,0x40,0x40,0x40}, // L      22
  {0x7F,0x02,0x0C,0x02,0x7F}, // M      23
  {0x7F,0x04,0x08,0x10,0x7F}, // N      24
  {0x3E,0x41,0x41,0x41,0x3E}, // O      25
  {0x7F,0x09,0x09,0x09,0x06}, // P      26
  {0x3E,0x41,0x51,0x21,0x5E}, // Q      27
  {0x7F,0x09,0x19,0x29,0x46}, // R      28
  {0x46,0x49,0x49,0x49,0x31}, // S      29
  {0x01,0x01,0x7F,0x01,0x01}, // T      30
  {0x3F,0x40,0x40,0x40,0x3F}, // U      31
  {0x1F,0x20,0x40,0x20,0x1F}, // V      32
  {0x3F,0x40,0x38,0x40,0x3F}, // W      33
  {0x63,0x14,0x08,0x14,0x63}, // X      34
  {0x07,0x08,0x70,0x08,0x07}, // Y      35
  {0x61,0x51,0x49,0x45,0x43}, // Z      36
  {0x00,0x36,0x36,0x00,0x00}, // :      37
  {0x00,0x00,0x00,0x30,0x30}, // .      38
  {0x00,0x00,0x00,0x40,0x00}, // -      39
  {0x40,0x30,0x08,0x06,0x01}, // /      40
  {0x00,0x00,0x7F,0x00,0x00}, // |      41
};

uint8_t charIndex(char c) {
  if (c == ' ') return 0;
  if (c >= '0' && c <= '9') return 1 + (c - '0');
  if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
  if (c >= 'a' && c <= 'z') return 11 + (c - 'a');
  if (c == ':') return 37;
  if (c == '.') return 38;
  if (c == '-') return 39;
  if (c == '/') return 40;
  if (c == '|') return 41;
  return 0;
}

// ============ 纯 GPIO SPI (ST7789) ============
void spiBit(uint8_t bit) {
  digitalWrite(TFT_MOSI, bit);
  digitalWrite(TFT_SCK, HIGH);
  digitalWrite(TFT_SCK, LOW);
}
void spiByte(uint8_t data) {
  for (int i = 7; i >= 0; i--) spiBit((data >> i) & 1);
}
void tftCmd(uint8_t cmd) {
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_CS, LOW);
  spiByte(cmd);
  digitalWrite(TFT_CS, HIGH);
}
void tftDat(uint8_t dat) {
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  spiByte(dat);
  digitalWrite(TFT_CS, HIGH);
}

// ============ ST7789 底层 ============
void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  tftCmd(0x2A);
  tftDat(x0 >> 8); tftDat(x0 & 0xFF);
  tftDat(x1 >> 8); tftDat(x1 & 0xFF);
  tftCmd(0x2B);
  tftDat(y0 >> 8); tftDat(y0 & 0xFF);
  tftDat(y1 >> 8); tftDat(y1 & 0xFF);
  tftCmd(0x2C);
}

void fillColor(uint16_t color) {
  setWindow(0, 0, 239, 239);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (uint32_t i = 0; i < 240UL * 240UL; i++) {
    spiByte(color >> 8);
    spiByte(color & 0xFF);
  }
  digitalWrite(TFT_CS, HIGH);
}

void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (x + w < 0 || y + h < 0 || x >= 240 || y >= 240) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
    spiByte(color >> 8);
    spiByte(color & 0xFF);
  }
  digitalWrite(TFT_CS, HIGH);
}

void drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= 240 || y >= 240) return;
  setWindow(x, y, x, y);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  spiByte(color >> 8);
  spiByte(color & 0xFF);
  digitalWrite(TFT_CS, HIGH);
}

void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  fillRect(x, y, w, 1, color);
}

// ============ 文字绘制 ============
int16_t cursorX = 0, cursorY = 0;
uint16_t fgColor = WHITE, bgColor = BLACK;

void setTextColor(uint16_t c) { fgColor = c; }
void setTextBackground(uint16_t c) { bgColor = c; }
void setCursor(int16_t x, int16_t y) { cursorX = x; cursorY = y; }

void drawChar(char c) {
  uint8_t idx = charIndex(c);
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t b = pgm_read_byte(&FONT5x7[idx][col]);
    for (uint8_t row = 0; row < 7; row++) {
      if (b & (1 << row)) drawPixel(cursorX + col, cursorY + row, fgColor);
      else drawPixel(cursorX + col, cursorY + row, bgColor);
    }
  }
  cursorX += 6;
  if (cursorX > 234) { cursorX = 0; cursorY += 8; }
}

void printStr(const char *s) {
  while (*s) drawChar(*s++);
}

void printNum(uint32_t n, int base = 10) {
  char buf[12];
  if (n == 0) drawChar('0');
  else {
    int i = 0;
    while (n > 0) {
      char c = (n % base < 10) ? ('0' + n % base) : ('A' + n % base - 10);
      buf[i++] = c;
      n /= base;
    }
    while (i > 0) drawChar(buf[--i]);
  }
}

// ============ ST7789 初始化 ============
void st7789Init() {
  pinMode(TFT_SCK,  OUTPUT);
  pinMode(TFT_MOSI, OUTPUT);
  pinMode(TFT_CS,   OUTPUT);
  pinMode(TFT_DC,   OUTPUT);
  pinMode(TFT_RST,  OUTPUT);
  pinMode(TFT_BLK,  OUTPUT);
  digitalWrite(TFT_SCK, LOW);
  digitalWrite(TFT_MOSI, LOW);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TFT_DC, LOW);

  digitalWrite(TFT_RST, LOW);
  delay(20);
  digitalWrite(TFT_RST, HIGH);
  delay(150);

  tftCmd(0x01); delay(150);
  tftCmd(0x11); delay(150);
  tftCmd(0x36); tftDat(0x00);
  tftCmd(0x3A); tftDat(0x55);
  tftCmd(0x21); delay(10);
  tftCmd(0x29); delay(50);

  digitalWrite(TFT_BLK, HIGH);
  fillColor(BLACK);
}

// ============ UI ============
void showReady() {
  fillColor(BLACK);
  setTextColor(WHITE);
  setCursor(80, 30);  printStr("RFID");
  setCursor(70, 40);  printStr("READER");
  drawFastHLine(20, 55, 200, CYAN);
  setTextColor(GREEN);
  setCursor(90, 100); printStr("READY");
  setTextColor(CYAN);
  setCursor(40, 160); printStr("Place card");
  setCursor(30, 170); printStr("near reader");
}

void showCard(uint8_t *uid, uint8_t len) {
  fillColor(BLACK);
  setTextColor(WHITE);
  setCursor(80, 20);  printStr("RFID");
  setCursor(70, 30);  printStr("READER");
  drawFastHLine(20, 45, 200, CYAN);

  setTextColor(GREEN);
  setCursor(30, 60); printStr("CARD DETECTED!");

  setTextColor(RED);
  setCursor(20, 85); printStr("UID("); printNum(len); printStr("):");

  setTextColor(WHITE);
  int row = 100;
  int col = 20;
  for (uint8_t i = 0; i < len; i++) {
    if (i == 3 || i == 5) {
      row += 10;
      col = 20;
    }
    setCursor(col, row);
    uint8_t hi = (uid[i] >> 4) & 0xF;
    uint8_t lo = uid[i] & 0xF;
    drawChar(hi < 10 ? '0' + hi : 'A' + hi - 10);
    drawChar(lo < 10 ? '0' + lo : 'A' + lo - 10);
    drawChar(' ');
    col += 14;
  }
}

// ============ 写卡 UI + 逻辑 ============
void showWriteMode() {
  fillColor(BLACK);
  setTextColor(YELLOW);
  setCursor(70, 20);  printStr("WRITE MODE");
  drawFastHLine(20, 35, 200, YELLOW);
  setTextColor(CYAN);
  setCursor(20, 55);  printStr("NTAG CARD");
  setTextColor(WHITE);
  setCursor(10, 100); printStr("Page:");
  setCursor(110, 100); printNum(NTAG_START_PAGE);
  setCursor(10, 115); printStr("Count:");
  setCursor(110, 115); printNum(NTAG_PAGE_COUNT);
  setCursor(10, 130); printStr("4B/page");
  setTextColor(GREEN);
  setCursor(10, 170); printStr("'r' exit");
  setCursor(10, 182); printStr("write mode");
}

void showWriteOK(uint8_t *uid, uint8_t len) {
  fillColor(BLACK);
  setTextColor(GREEN);
  setCursor(60, 40);  printStr("WRITE");
  setCursor(70, 50);  printStr("SUCCESS!");
  setTextColor(WHITE);
  setCursor(20, 80);  printStr("UID:");
  for (uint8_t i = 0; i < len && i < 4; i++) {
    setCursor(20 + i*40, 95);
    uint8_t hi = (uid[i] >> 4) & 0xF, lo = uid[i] & 0xF;
    drawChar(hi < 10 ? '0'+hi : 'A'+hi-10);
    drawChar(lo < 10 ? '0'+lo : 'A'+lo-10);
  }
  setTextColor(CYAN);
  setCursor(20, 140); printStr("Verified OK");
  setTextColor(YELLOW);
  setCursor(10, 190); printStr("'r' exit / 'w' next");
}

void showWriteFail(uint8_t reason) {
  fillColor(BLACK);
  setTextColor(RED);
  setCursor(65, 40);  printStr("WRITE");
  setCursor(70, 50);  printStr("FAILED!");
  setTextColor(WHITE);
  if (reason == 0) {
    setCursor(10, 80); printStr("Write cmd");
    setCursor(10, 95); printStr("no response");
  } else if (reason == 1) {
    setCursor(10, 80); printStr("ACK missing");
    setCursor(10, 95); printStr("after write");
  } else {
    setCursor(10, 80); printStr("Readback");
    setCursor(10, 95); printStr("mismatch");
  }
  setTextColor(GREEN);
  setCursor(10, 190); printStr("'r' exit");
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
void setup(void) {
  Serial0.begin(115200);
  while (!Serial0) delay(10);
  delay(500);
  Serial0.println("=== RFID Reader ===");

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
    Serial0.println("ERROR: PN532 not found!");
    fillColor(RED);
    setTextColor(WHITE);
    setCursor(30, 100); printStr("ERROR");
    setCursor(20, 120); printStr("PN532 not");
    setCursor(20, 130); printStr("found");
    while (1) delay(1000);
  }
  Serial0.printf("PN5%d F/W %d.%d\n", (ver>>24)&0xFF, (ver>>16)&0xFF, (ver>>8)&0xFF);
  nfc.SAMConfig();

  showReady();
  Serial0.println("All ready! Waiting for card...");
  Serial0.println("Commands:");
  Serial0.println("  'w' -> enter WRITE MODE");
  Serial0.println("  'r' -> return to READ MODE");
}

void loop(void) {
  while (Serial0.available()) {
    char c = Serial0.read();
    if (c == 'w' || c == 'W') {
      writeMode = true;
      writeLastLen = 0;
      showWriteMode();
      Serial0.println("-> WRITE MODE (place card)");
    }
    if (c == 'r' || c == 'R') {
      writeMode = false;
      writeLastLen = 0;
      showReady();
      Serial0.println("-> READ MODE");
    }
  }

  uint8_t uid[7];
  uint8_t uidLength;
  uint8_t ok = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);

  if (writeMode) {
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
        if (res) {
          showWriteOK(uid, uidLength);
          Serial0.println("WRITE OK");
        } else {
          showWriteFail(1);
          Serial0.println("WRITE FAIL (place card again to retry, or 'r' to exit)");
        }
        memcpy(writeLastUID, uid, uidLength);
        writeLastLen = uidLength;
        delay(2000);
        writeLastLen = 0;
      }
    }
    return;
  }

  static uint8_t lastUID[7] = {0};
  static uint8_t lastLen = 0;
  if (ok) {
    bool changed = (uidLength != lastLen);
    if (!changed) for (uint8_t i = 0; i < uidLength; i++)
      if (uid[i] != lastUID[i]) { changed = true; break; }
    if (changed) {
      showCard(uid, uidLength);
      Serial0.print("UID: ");
      for (uint8_t i = 0; i < uidLength; i++) {
        if (uid[i] < 0x10) Serial0.print("0");
        Serial0.print(uid[i], HEX);
        Serial0.print(" ");
      }
      Serial0.println();
      memcpy(lastUID, uid, uidLength);
      lastLen = uidLength;
      delay(1500);
    }
  } else {
    if (lastLen > 0) {
      delay(3000);
      lastLen = 0;
      showReady();
    }
  }
}