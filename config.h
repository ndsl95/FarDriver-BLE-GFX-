/**
 * config.h - FarDriver BLE 仪表盘项目配置
 * 
 * 硬件: LCDWiki ES3C28P (ESP32-S3 + ILI9341V 2.8寸 TFT + FT6336G 电容触摸)
 * 引脚定义来源: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
 */

#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  1. 触摸屏配置 (FT6336G, I2C 接口)
//  引脚来源: ES3C28P 官方引脚表
// ============================================================
#define TOUCH_SDA_PIN      16    // FT6336G I2C SDA (GPIO16)
#define TOUCH_SCL_PIN      15    // FT6336G I2C SCL (GPIO15)
#define TOUCH_RST_PIN      18    // FT6336G 复位引脚 (低电平复位, GPIO18)
#define TOUCH_INT_PIN      17    // FT6336G 中断引脚 (触摸时拉低, GPIO17)
#define TOUCH_I2C_ADDR     0x38  // FT6336G I2C 地址 (ESP32-S3 Wire 库直接使用此值)

// ============================================================
//  2. TFT 背光
//  ES3C28P: GPIO45 高电平开背光, 低电平关
// ============================================================
#define TFT_BL_PIN         45    // 背光控制引脚 (GPIO45, 高电平有效)
#define TFT_BL_FREQ        5000  // 背光 PWM 频率 Hz

// ============================================================
//  3. 车辆参数
// ============================================================
#define WHEEL_CIRCUMFERENCE_M  1.35f  // 轮胎周长(米), 根据实际车辆修改

// ============================================================
//  4. BLE 配置
// ============================================================
#define BLE_SERVICE_UUID   "FFE0"  // FarDriver BLE 服务 UUID
#define BLE_CHAR_UUID      "FFEC"  // FarDriver BLE 特性 UUID
#define BLE_KEEPALIVE_INTERVAL_MS  2000  // 心跳包发送间隔

// ============================================================
//  5. 显示参数
// ============================================================
#define LOW_BATT_LIMIT_V   86.0f   // 低电压警告 (V)
#define HIGH_BATT_LIMIT_V  96.0f   // 满电显示 (V)
#define MAX_POWER_KW       20.0f   // 功率条满量程 (kW)

// ============================================================
//  6. 调试
// ============================================================
#define DEBUG_ENABLED       1      // 1=串口输出调试信息
#define DEBUG_RAW_DATA      0      // 1=输出每个运行时地址的原始16进制数据
#endif