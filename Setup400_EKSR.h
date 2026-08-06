// LCDWiki ES3C28P TFT 配置
// 硬件: ESP32-S3 + ILI9341V 2.8寸 (240x320)
// 引脚来源: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
//
// 使用方法:
//   1. 将此文件复制到 Arduino/libraries/TFT_eSPI/ 目录 (与 User_Setup.h 同级)
//   2. 在 User_Setup_Select.h 中添加: #include <Setup400_EKSR.h>

#define USER_SETUP_ID 400

#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ES3C28P 实际 SPI 引脚 (来自官方引脚表)
#define TFT_CS   10    // LCD 片选, 低电平有效 (GPIO10)
#define TFT_MOSI 11    // SPI 写数据 (GPIO11)
#define TFT_SCLK 12    // SPI 时钟 (GPIO12)
#define TFT_MISO 13    // SPI 读数据 (GPIO13)
#define TFT_DC   46    // 命令/数据选择: 高=数据, 低=命令 (GPIO46)
// TFT_RST 使用 EN 引脚 (与 ESP32-S3 主控共享复位), TFT_eSPI 设为 -1 表示不控制
#define TFT_RST  -1

// 背光控制 (GPIO45, 高电平开)
#define TFT_BL   45

// 加载字体
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT

// 强制使用 HSPI (SPI3) 避免 FSPI (SPI2) 与 PSRAM OPI 冲突导致 StoreProhibited 崩溃
#define USE_HSPI_PORT

// SPI 频率
#define SPI_FREQUENCY  40000000
#define SPI_READ_FREQUENCY  6000000
#define SPI_TOUCH_FREQUENCY  2500000