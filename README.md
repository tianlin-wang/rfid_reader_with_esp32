# ESP32 RFID Reader

基于 **ESP32-S3** 的 RFID 读卡 / 写卡器，驱动一块 ST7789 TFT 240×240 屏幕和一块 PN532 RFID 模块。

支持 ISO14443A 标准卡，包括 **Mifare Classic** 和 **NTAG** 系列。

---

## 硬件接线

### ST7789 TFT (240×240)

| TFT Pin | ESP32 Pin |
|---------|-----------|
| SCK     | 4         |
| MOSI    | 5         |
| RST     | 6         |
| DC      | 7         |
| CS      | 10        |
| BLK     | 13        |

### PN532 (SPI 模式)

| PN532 Pin | ESP32 Pin |
|-----------|-----------|
| SCK       | 17        |
| MISO      | 18        |
| MOSI      | 8         |
| SS        | 3         |
| IRQ       | 11        |
| RST       | 12        |

> 两块模块各走各的 bit-bang SPI，没有共用引脚。

---

## 依赖库

Arduino IDE 库里搜下面两个装上：

- **Adafruit PN532** — by Adafruit
- 内置 `SPI.h` 即可，TFT 驱动是自写的，不需要额外库

---

## 编译烧录

1. Arduino IDE → 开发板选 `ESP32S3 Dev Module`
2. 打开 `sketch_sep6a.ino`，选择串口烧录
3. 打开 **串口监视器**，波特率 **115200**

---

## 使用方法

### 读卡模式（默认）

开机即为读卡模式，屏幕显示 `READY`，把卡靠近 PN532：

- 屏幕显示 `CARD DETECTED!` + UID（十六进制逐字节）
- 串口同步输出 UID
- 同一张卡持续放置不会反复触发（内置去重）
- 拿开卡 3 秒后自动回到待机界面

### 写卡模式

在串口监视器里输入 `w` 进入写卡模式：

```
w   → 进入写卡模式
r   → 退出写卡模式，回到读卡
```

进入写卡模式后，把 NTAG 卡放上去，程序会自动：
1. 识别 UID
2. 逐页写入数据（每页 4 字节）
3. 写成功屏幕显示绿色 `WRITE SUCCESS!` + UID
4. 写失败显示红色提示（放新卡重试，或按 `r` 退出）

---

## 写卡配置

在 `.ino` 文件顶部修改：

```cpp
#define NTAG_START_PAGE  8       // 起始页（0-7 是 UID/厂商区，只读）
#define NTAG_PAGE_COUNT  16      // 总共写多少页

const uint8_t WRITE_DATA[] = "HELLO-RFID-TEST-!";  // 要写入的内容
```

### NTAG 容量参考

| 型号 | 用户页范围 |
|------|-----------|
| NTAG213 | 4 ~ 39 |
| NTAG215 | 4 ~ 129 |
| NTAG216 | 4 ~ 225 |

---

## 代码架构

```
sketch_sep6a.ino
├── 字库              极简 5×7 点阵，PROGMEM 存放
├── TFT SPI 底层      纯 GPIO bit-bang（spiByte / tftCmd / tftDat）
├── ST7789 绘图原语    fillColor / fillRect / drawPixel / drawFastHLine
├── 文字渲染          drawChar / printStr / printNum
├── ST7789 初始化     标准 init sequence
├── UI 画面           showReady / showCard / showWriteMode / showWriteOK / showWriteFail
├── performWrite      NTAG 写卡核心（逐页 ntag2xx_WritePage）
└── loop()            状态机：读模式 / 写模式 + 串口命令解析
```

---

## 已知限制

- TFT 驱动是 bit-bang，刷新速度不如硬件 SPI，但静态 UI 足够
- 写卡仅支持 **NTAG** 系列（Ultralight 协议），Mifare Classic 写卡暂未加入
- `performWrite` 内部不做即时回读验证（WritePage 返回 1 即视为写入成功）

---

## License

MIT