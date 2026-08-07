# FarDriver BLE 仪表盘

南京远驱 (FarDriver) 控制器 BLE 仪表盘，基于 [EKSR_Instrument](https://github.com/magicmicros/EKSR_Instrument) 项目移植适配，运行在 ESP32-S3 一体板上。

## 项目简介

本项目通过 BLE (低功耗蓝牙) 连接南京远驱控制器，实时读取并显示控制器的运行数据，包括电压、速度、功率、转速、温度等信息。硬件平台为 ESP32-S3 一体板，集成了 2.8 寸 ILI9341V TFT 彩屏和 FT6336G 电容触摸屏。

原始项目 EKSR_Instrument 采用不同的硬件平台，本项目在保留其核心 FarDriver 协议解析逻辑的基础上，针对 ESP32-S3 一体板重新适配了显示驱动、触摸驱动和 BLE 通信层 (使用 ESP-IDF 内置的 NimBLE 替代原版的 BLEx86)。

## 功能特性

- **BLE 连接 FarDriver 控制器** -- 自动扫描并连接 (服务 UUID: 0xFFE0, 特性 UUID: 0xFFEC)
- **GFX 直绘 UI** -- 基于 TFT_eSPI 直接渲染 (无 LVGL 依赖)，四页面: 扫描 / 设备列表 / 主仪表盘 / 信息
- **PSRAM Sprite 仪表盘** -- 仪表背景渲染到 PSRAM Sprite 一次，指针移动仅推送局部小区域，刷新流畅且无指针残影
- **电容触摸交互** -- FT6336G I2C 触摸控制器，按下反馈 + 防抖 (松开去抖 / 移动容差 / 触发冷却)
- **多页面仪表盘** -- 主页面 (速度/功率/RPM 等核心数据) + 信息页面 (温度/电压等详细信息)
- **自动重连** -- BLE 断开后自动重启扫描
- **Keep-alive 心跳** -- 每 2 秒发送心跳包保持连接
- **串口调试输出** -- 可通过串口查看实时解析数据

## UI 设计

- **配色** -- 深黑背景 + 渐变仪表色带 (蓝→青→绿→黄→橙→红)，数据按类别着色
- **主仪表盘** -- 弧形仪表 (色带半径 72..84) + 指针 (半径 58..88, 白色主体 + 红色描边 + 红色针尖三角越过色带); 中央速度整数居中显示; 油门/电量进度条; kW 值取绝对值显示 (负=回充绿色, 正=红色)
- **页面布局** -- 无顶部品牌栏; 仪表盘经右下角按钮进信息页; 列表页 Rescan/Connect 按钮; 设备列表自动选中第一台

## 硬件要求

| 组件 | 规格说明 |
|------|----------|
| 主控 | ESP32-S3 一体板 |
| 屏幕 | ILI9341V, 2.8 寸, 320x240, SPI 接口 |
| 触摸 | FT6336G, 电容触摸, I2C 接口 |
| 控制器 | 南京远驱 (FarDriver) BLE 控制器 |

## 开发环境

- **Arduino IDE**: 2.x
- **ESP32 Arduino Core**: 3.x
- **Python**: 3.x (仅用于 ESP32 Core 首次安装时的下载工具)

### 需要安装的库

| 库名称 | 说明 | 来源 |
|--------|------|------|
| TFT_eSPI | 高性能 TFT 显示驱动库, by Bodmer | Arduino Library Manager |
| NimBLE | BLE 协议栈 | ESP32 Arduino Core 内置 |
| Preferences | Flash 键值存储 | ESP32 Arduino Core 内置 |
| Wire | I2C 通信 | ESP32 Arduino Core 内置 |

## 安装步骤

### 1. 安装 ESP32 Arduino Core

1. 打开 Arduino IDE，进入 **文件 -> 首选项**
2. 在 "附加开发板管理器网址" 中添加:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. 进入 **工具 -> 开发板 -> 开发板管理器**，搜索 `esp32`，安装 **ESP32 Arduino Core 3.x**

### 2. 安装 TFT_eSPI 库并配置

1. 在 Arduino Library Manager 中搜索 `TFT_eSPI` 并安装 (by Bodmer)
2. 找到 TFT_eSPI 库的安装目录，路径通常为:
   ```
   C:\Users\<用户名>\Documents\Arduino\libraries\TFT_eSPI\
   ```
3. 将 `Setup400_EKSR.h` 文件复制到该目录下的 `TFT_eSPI` 文件夹中 (与 `User_Setup.h` 同级)

### 3. 修改 TFT_eSPI 的 User_Setup_Select.h

打开 `TFT_eSPI` 库目录下的 `User_Setup_Select.h`，添加或取消注释以下行:

```cpp
#include <User_Setup.h>          // 默认配置 (注释掉)
// #include <User_Setup_Select.h> // 默认已打开

// 添加你的自定义配置:
#include <Setup400_EKSR.h>        // ESP32-S3 一体板引脚配置
```

或者在文件末尾直接添加:

```cpp
#ifndef USER_SETUP_FOUND
#define USER_SETUP_FOUND
#include <Setup400_EKSR.h>
#endif
```

### 4. 编译上传

1. 用 Arduino IDE 打开 `BLE/FarDriver_BLE.ino`
2. 确保同目录下包含所有依赖的头文件:
   - `config.h` -- 引脚定义和常量配置
   - `fardriver_protocol.h` -- FarDriver 协议解析
   - `nimble.h` -- BLE 通信封装
   - `ft6336g.h` -- 触摸驱动
   - `display.h` -- 显示界面管理
3. 选择正确的开发板和端口 (参见下方开发板设置)
4. 点击上传

## TFT_eSPI 配置说明

### Setup400_EKSR.h 放置位置

```
Arduino/libraries/TFT_eSPI/
  ├── User_Setup.h
  ├── User_Setup_Select.h    <-- 修改此文件
  ├── Setup400_EKSR.h        <-- 放置在此处
  ├── TFT_eSPI.h
  ├── TFT_eSPI.cpp
  └── ...
```

### User_Setup_Select.h 修改方法

在 `User_Setup_Select.h` 文件中，找到文件末尾，确保 `Setup400_EKSR.h` 被包含。最简单的方法是注释掉默认的 `#include <User_Setup.h>`，改为:

```cpp
// #include <User_Setup.h>
#include <Setup400_EKSR.h>
```

### SPI 频率与 PSRAM 冲突规避

- **SPI 频率 80MHz** -- `Setup400_EKSR.h` 中 `SPI_FREQUENCY 80000000` (ESP32-S3 SPI 硬件上限)；若实测花屏/乱码，回退 `40000000` 或 `64000000`
- **`USE_HSPI_PORT` 必须保留** -- ESP32-S3 开启 OPI PSRAM 后，强制 TFT 走 SPI3 (HSPI) 以避开 FSPI (SPI2) 与 OPI PSRAM 的冲突，否则会 StoreProhibited 崩溃

## 引脚说明

### TFT SPI 引脚

| 引脚 | 功能 | ESP32-S3 引脚 |
|------|------|---------------|
| TFT_MOSI | SPI 数据输出 | GPIO 11 |
| TFT_MISO | SPI 数据输入 | GPIO 13 |
| TFT_SCLK | SPI 时钟 | GPIO 12 |
| TFT_CS | 片选 | GPIO 10 |
| TFT_DC | 数据/命令选择 | GPIO 46 |
| TFT_RST | 复位 | -1 (与主控共享) |
| TFT_BL | 背光控制 | GPIO 45 |

### 触摸 I2C 引脚

| 引脚 | 功能 | ESP32-S3 引脚 |
|------|------|---------------|
| TOUCH_SDA | I2C 数据 | GPIO 16 |
| TOUCH_SCL | I2C 时钟 | GPIO 15 |
| TOUCH_RST | 复位 | GPIO 18 |
| TOUCH_INT | 触摸中断 | GPIO 17 (当前未使用, 轮询模式) |
| I2C 地址 | - | 0x38 |

> 引脚来源: LCDWiki ES3C28P 官方引脚表 (https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)

## 引脚排查方法

如果编译上传后屏幕无显示或触摸无反应，引脚配置可能不正确，请按以下步骤排查:

1. **查看一体板原理图** -- 这是最直接的方法。购买时通常会附带原理图或引脚定义图，上面标注了 TFT SPI 和触摸 I2C 的具体引脚分配。
2. **万用表测通** -- 用万用表的蜂鸣档，一头接 ESP32-S3 的某个 GPIO 引脚，另一头接屏幕排线座的对应引脚，确认物理连接关系。
3. **查阅商品页面** -- 很多 ESP32-S3 一体板的淘宝/商品详情页会标注详细的引脚对照表。
4. **搜索同款板子** -- 在 GitHub 或技术论坛搜索同型号的一体板，看看其他开发者的引脚配置。
5. **逐个尝试** -- 对于 SPI MOSI/CLK 等关键引脚，常见配置有:
   - MOSI: GPIO11, GPIO35, GPIO47
   - SCLK: GPIO12, GPIO36, GPIO48
   - CS: GPIO10, GPIO37, GPIO21
   - DC: GPIO9, GPIO4, GPIO47

## 运行时数据映射说明

当前 `message_handler` 中的地址映射基于对 EKSR_Instrument 原版代码的分析，属于**候选映射值**，需要通过实测验证和调整。

### 当前映射表

| 数据 | 内存地址 | 帧地址 (id) | 系数 | 单位 | 状态 |
|------|----------|-------------|------|------|------|
| 电压 | 0xE2 | id=0 | x0.1 | V | 候选 |
| 油门 | 0xE3 | id=0 | x1 | - | 候选 |
| 控制器温度 | 0xE8 | id=1 | x0.1 | °C | 候选 |
| 电机温度 | 0xE9 | id=1 | x0.1 | °C | 候选 |
| 档位 | 0xEB | id=1 | x1 | - | 候选 |
| 转速 RPM | 0xEC | id=1 | x1 | RPM | 候选 |
| 速度 | 0xEE | id=2 | x0.1 | km/h | 候选 |
| 功率 | 0xEF | id=2 | x0.01 | kW | 候选 |

### 如何验证映射

1. 编译上传固件后，打开串口监视器 (115200 波特率)
2. 连接 FarDriver 控制器后，观察串口输出的 `[FD]` 调试信息
3. 对照控制器的实际参数 (如用原厂 App 显示的数据)，确认每个地址对应的实际数据
4. 根据实测结果修改 `FarDriver_BLE.ino` 中 `message_handler` 的地址映射和系数

## 开发板设置

在 Arduino IDE -> 工具 中进行如下配置:

| 设置项 | 推荐值 | 说明 |
|--------|--------|------|
| Board | ESP32S3 Dev Module | 选择 ESP32-S3 开发板 |
| USB CDC On Boot | Enabled | 串口输出需要 |
| Flash Size | 8MB | 根据实际板子调整 (常见: 4MB / 8MB / 16MB) |
| PSRAM | OPI PSRAM | 必须开启 (仪表盘 Sprite 约 75KB 依赖 PSRAM; 未开启自动回退整表重绘) |
| Upload Speed | 921600 | 上传波特率，越快越好 |
| CPU Frequency | 240 MHz | 最高频率 |
| Arduino Runs On | Core 1 | BLE 运行在 Core 0 |

## 注意事项

- **首次使用需确认 TFT SPI 引脚配置** -- 不同厂家的 ESP32-S3 一体板引脚分配可能不同，务必根据实际硬件修改 `config.h` 和 `Setup400_EKSR.h`。
- **BLE 需要先配对 FarDriver 控制器** -- 部分 FarDriver 控制器需要先通过手机 App 配对后才允许 BLE 连接; 也有些控制器支持直接扫描连接，请根据实际情况操作。
- **运行时数据映射可能需要调整** -- `message_handler` 中的地址映射是基于 EKSR_Instrument 分析的候选值，不同型号/固件版本的 FarDriver 控制器可能有不同的数据布局，需要实测验证。
- **message_handler 中的地址映射是候选值** -- 当前标注为"候选"的映射需通过串口调试输出与实际控制器数据进行比对后确认。
- **NimBLE 与 WiFi 不可同时使用** -- ESP32 的 BLE 和 WiFi 共用 2.4GHz 射频，本项目仅使用 BLE，`setup()` 中已 `WiFi.mode(WIFI_OFF)` 关闭 WiFi 射频（省电并减少频段干扰）。
- **BLE 连接距离** -- 有效距离通常在 10 米以内，实际使用中建议保持仪表盘与控制器之间无金属遮挡。
- **必须开启 OPI PSRAM** -- 仪表盘 Sprite (200x192, 约 75KB) 依赖 PSRAM 分配；未开启时自动回退为整表重绘（功能可用但刷新较慢）。在 Arduino IDE 工具菜单选择 PSRAM = OPI PSRAM。
- **`USE_HSPI_PORT` 不可移除** -- 这是 OPI PSRAM 与 TFT 共存的关键配置（见上节）。
- **SPI 80MHz 花屏处理** -- 若出现花屏/随机横线，将 `SPI_FREQUENCY` 改回 40MHz 或 64MHz。
- **触摸与绘制并发安全** -- 所有绘制 API 由全局递归互斥锁保护，`uiScanAdd` 等可安全地从其他任务调用；触摸读取永远在 SPI 事务之外。
