// *************************************************************************************************
// 根据硬件信息.md 的接线配置 —— ST7789V 240×240 TFT
//
//    屏幕引脚  | NodeMCU | GPIO
//    ----------|---------|------
//    DC        | D3      | GPIO0
//    CS        | GND     | —        (片选低电平，直连 GND)
//    SCK       | D5      | GPIO14
//    SDA(MOSI) | D7      | GPIO13
//    REST      | D4      | GPIO2
//    LEDA      | NPN 集电极 | GPIO5  (LOW = 亮)
//    LEDK      | GND     | —
//
//   背光：GPIO5 LOW = 亮，HIGH = 灭
// *************************************************************************************************

#define USER_SETUP_LOADED  // 通知 TFT_eSPI 使用此文件，跳过库默认的 User_Setup.h

#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define TFT_MOSI   13      // D7 — SPI 数据 (MOSI)
#define TFT_SCLK   14      // D5 — SPI 时钟
#define TFT_CS     -1      // CS 接地，设为 -1 禁用片选
#define TFT_DC      0      // D3 — 数据/命令
#define TFT_RST     2      // D4 — 复位
#define TFT_BL      5      // GPIO5 — 背光控制 (NPN 三极管)
#define TFT_BACKLIGHT_ON LOW   // 背光低电平开启

// SPI 频率
#define SPI_FREQUENCY  20000000   // 20 MHz

// 字体加载（仅保留代码中实际使用的字体，以节省 Flash 空间）
#define LOAD_FONT2  // 小字体，用于 IP、温度等
#define LOAD_FONT7  // 7 段数码管大字体，用于时钟时分
#define SMOOTH_FONT // 自定义 VLW 字体支持（ZdyLwFont_20、FxLED_32）

// SPI 接口设置
#define TFT_SPI_PORT 1   // ESP8266 使用 SPI 通道 1 (HSPI)
