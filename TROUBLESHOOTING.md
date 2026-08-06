# FarDriver BLE 仪表盘 - 问题排查记录

> 硬件: LCDWiki ES3C28P (ESP32-S3 + ILI9341V 2.8寸 IPS + FT6336G 电容触摸)
> 日期: 2026-07-24

---

## 1. TFT_eSPI 字体名称不兼容

**现象**: 编译报错 `'FSS18' was not declared in this scope`

**原因**: 不同版本 TFT_eSPI 内置免费字体名称不同

**修复**: 替换为通用字体名

| 原始 | 替换为 |
|------|--------|
| FSS9 | &FreeSans9pt7b |
| FSS12 | &FreeSans12pt7b |
| FSS18 | &FreeSans18pt7b |
| FSSB24 | &FreeSansBold24pt7b |

---

## 2. NimBLE 2.5.0 API 变更

**现象**: 编译报错类名/方法不存在

**修复**:

| 旧 API | 新 API |
|--------|--------|
| NimBLEAdvertisedDeviceCallbacks | NimBLEScanCallbacks |
| getClientListSize() | getCreatedClientCount() |
| 回调参数非 const | 需加 const 修饰 |
| getServiceUUIDs() (vector) | getServiceUUID() (单个) |

---

## 3. 全局变量重复定义

**现象**: 链接错误 `undefined reference to 'ctr_data'`

**修复**: 在 `FarDriver_BLE.ino` 中添加定义 `ControllerData ctr_data = {0};`

---

## 4. 类型转换窄化

**现象**: 编译警告/错误 `narrowing conversion from 'int' to 'uint16_t'`

**修复**: 添加显式类型转换, 如 `(uint16_t)0x5A5A`

---

## 5. 黑屏 (背光未初始化 + 引脚冲突)

**现象**: 烧录成功但屏幕全黑

**原因**: 两个问题叠加
1. 背光引脚未初始化 PWM
2. TFT_RST 和 TOUCH_INT_PIN 都设为 GPIO 21, 产生冲突

**修复**:
- 背光: 添加 `ledcAttach(TFT_BL_PIN, TFT_BL_FREQ, 8); ledcWrite(TFT_BL_PIN, 255);`
- 引脚冲突: TOUCH_INT_PIN 从 GPIO 21 改为 GPIO 4 (后续又改为 GPIO 17, 依据官方引脚表)
- RST 引脚: 设为 -1, 与 ESP32-S3 主控共享复位

---

## 6. ESP32 Arduino Core 3.x API 变更

**现象**: `'ledcSetup' was not declared`

**修复**: 旧版 `ledcSetup()` + `ledcAttachPin()` 替换为 `ledcAttach(pin, freq, resolution)`

---

## 7. StoreProhibited 崩溃

**现象**: `Core 1 panic'ed (StoreProhibited)` Guru Meditation Error

**原因**: FSPI (SPI2) 与 PSRAM OPI 模式冲突

**修复**: 在 `Setup400_EKSR.h` 中添加 `#define USE_HSPI_PORT`, 强制 TFT 使用 HSPI (SPI3)

---

## 8. BLE 扫描无结果

**现象**: 串口显示 `BLE scan started...` 后无任何输出, 找不到设备

**原因**: 扫描回调中, 先检查 FFE0 服务且要求名称非空, 不满足条件就静默跳过

**修复**: 改为三层匹配策略:
1. MAC 地址精确匹配 (`ab:5e:c5:45:56:10`)
2. 名称包含 "YuanQu"
3. 广播了 FFE0 服务 (兜底)

同时打印所有发现的设备信息 (地址/名称/RSSI/服务)

---

## 9. FT6336G 触摸芯片检测失败

**现象**: `[FT6336G] Vendor:0x0 FW:0x0 Chip:0x0 -- Not FocalTech IC!`

### 9.1 I2C 地址错误
- 初始使用 7 位地址 0x1C, 扫描发现总线上无此设备 (error=2 NACK)
- 全总线扫描发现两个设备: **0x18** 和 **0x38**
- 逐个尝试: 0x18 不是触摸芯片, 0x38 读回 0xA8=0x11 确认为 FocalTech

**结论**: ESP32-S3 Arduino Wire 库在此硬件上直接使用 0x38 作为地址参数

**修复**: `config.h` 中 `TOUCH_I2C_ADDR` 从 0x1C 改为 0x38

### 9.2 缺少硬件复位
- 触摸芯片 RST 引脚 (GPIO18) 未被驱动复位

**修复**: I2C 通信前执行: GPIO18 LOW 10ms -> HIGH 300ms

---

## 10. 触摸时灵时不灵

**现象**: 手指按屏幕有时有反应有时没反应

**原因**: 中断模式下 `getTouch()` 检查 INT 引脚电平, 高电平直接跳过不读

**修复**:
1. 工作模式改为轮询模式 (G_MODE=0)
2. 删除 INT 引脚电平检查
3. I2C 先 100kHz 验证, 通过后提速到 400kHz

---

## 最终硬件引脚配置

| 功能 | 引脚 | 备注 |
|------|------|------|
| TFT CS | GPIO 10 | SPI 片选 |
| TFT MOSI | GPIO 11 | SPI 数据 |
| TFT SCLK | GPIO 12 | SPI 时钟 |
| TFT MISO | GPIO 13 | SPI 读取 |
| TFT DC | GPIO 46 | 命令/数据选择 |
| TFT RST | -1 | 与主控共享复位 |
| TFT BL | GPIO 45 | 背光, 高电平开 |
| Touch SDA | GPIO 16 | I2C 数据 |
| Touch SCL | GPIO 15 | I2C 时钟 |
| Touch RST | GPIO 18 | 低电平复位 |
| Touch INT | GPIO 17 | 中断 (当前未使用) |
| Touch I2C Addr | 0x38 | Wire 库直接使用 |

---

## 11. LVGL Spinner 编译错误

**现象**: 
```
#error "lv_spinner: lv_arc is required. Enable it in lv_conf.h (LV_USE_ARC  1)"
```

**原因**: LVGL 的 spinner（旋转加载动画）内部依赖 arc（圆弧）控件，但 `lv_conf.h` 中 `LV_USE_ARC` 被设为 0。

**修复**: 在 `lv_conf.h` 中将 `#define LV_USE_ARC 0` 改为 `#define LV_USE_ARC 1`

---

## 12. LVGL 文字显示发虚、偏紫色

**现象**: BLE SCAN 页面文字模糊且偏紫色，Connect 按钮由黄色变成蓝色。

**原因**: TFT_eSPI 使用 16 位 RGB565 颜色格式，LVGL 的 `lv_color_hex()` 宏默认按照 24 位格式解析，导致红蓝通道互换。

**修复**: 在 `lv_conf.h` 中确保 `LV_COLOR_16_SWAP` 设为 1：
```c
#define LV_COLOR_16_SWAP 1  // 交换 RGB565 字节序，匹配 TFT_eSPI
```
同时确认 `LV_COLOR_DEPTH` 为 16。

---

## 13. 触摸坐标严重越界 (49008, 16334)

**现象**: 串口日志出现 `[TOUCH] PRESS (49008, 16334)`，坐标远超屏幕范围（240x320），导致按钮永远匹配不到。

**根因分析** (3 层问题叠加):

### 13.1 getTouch 返回未初始化内存（关键 bug）
`ft6336g.cpp` 的 `getTouch(TouchPoint*)` 原实现：TD_STATUS 寄存器报告有触摸（count > 0），但读取到的原始坐标 X >= 320 或 Y >= 240 时，`continue` 跳过了赋值。然而函数最终 `return count`（原始值），而非实际赋值的点数。简单接口 `getTouch(x,y)` 检查 `return > 0` 为真后，读取了 `pts[0]` 的未初始化内存。

**修复**: 引入 `valid_count` 变量追踪实际通过校验的点数，`return valid_count` 而非 `return count`。

### 13.2 FT6336G TD_STATUS 偶发误报
FT6336G 偶尔在没有触摸时报告 TD_STATUS 非零值，导致后续读取坐标寄存器得到随机数据。

**修复**: 连续读取两次 TD_STATUS 寄存器（0x02），只有两次一致才相信有触摸。

### 13.3 坐标变换后缺少二次校验
原始坐标经 swap/flip 变换后可能仍然越界，原实现仅在变换前校验了一次。

**修复**: 变换后的坐标再次检查是否在 LVGL 屏幕范围（240x320）内；同时在 `lv_port_indev.cpp` 的 `touchpad_read()` 回调中增加兜底边界校验。

---

## 14. 触摸延迟 / I2C 总线偶尔锁死

**现象**: 触摸按下到释放间隔长达 10+ 秒：
```
[TOUCH] PRESS  (24, 57)
[TOUCH] RELEASE (232, 219)   ← 11 秒后
```

**原因**: I2C 总线在读取 FT6336G 时偶发挂死，`Wire.requestFrom()` 阻塞直到硬件看门狗介入。

**修复**:
- `Wire.setTimeOut(100)` 设置 100ms I2C 超时（默认无超时）
- `lv_port_indev.cpp` 增加 200ms 软件超时强制释放触摸状态
- `_i2c_read()` 失败时立即返回 0，不阻塞后续帧

---

## 15. Connect 按钮触摸不到 / 触摸坐标不匹配

**现象**: Connect 按钮 (Y=258) 始终无法触发触摸事件，触摸屏幕底部时 rawY=300 被越界校验拦截。

**根因**: 早期坐标系猜错，以为 FT6336G 传感器是横屏方向 (320x240)，实际传感器与屏幕同向 (X:0-239, Y:0-319)。反复尝试了错误的 swapXY/flip 组合导致坐标方向错乱，加上硬截断 `rawY >= 240` 导致屏幕底部触摸被直接丢弃。

### 15.1 校准迭代过程

通过串口 `[CAL]` 诊断日志采集 raw→screen 映射数据，先后尝试了 4 种配置组合：

**第 1 轮 — `_swapXY=false, _flipX=false, _flipY=false`（默认推测）**

触摸 Connect 按钮 (屏幕底部) 时，串口输出:
```
[TOUCH] PRESS  (169, 122)     ← 手指在屏幕下方但 Y=122, 映射大概偏上
[FT6336G] BAD raw: (172,241)  ← 底部触摸 rawY=241 被越界截断!
```
**结论**: 传感器底部 rawY 可达 240+，但 `rawY >= 240` 的硬截断直接把按钮区域丢弃了；坐标方向也未对齐。

**第 2 轮 — `_swapXY=true, _flipX=false, _flipY=false`（误以为是横屏传感器）**

串口输出:
```
[TOUCH] PRESS  (239, 244)     ← 坐标方向完全不对
```
**结论**: swapXY 后坐标乱套，传感器实际就是竖屏同向的。

**第 3 轮 — `_swapXY=true, _flipX=false, _flipY=true`（swapXY + Y 轴翻转）**

为突破硬截断限制，引入软钳位 (v9)，同时加 `[CAL]` 诊断日志输出 raw→screen 映射。串口输出:
```
[CAL] raw(213,0)   -> screen(0,106)   swap=Y flipX=N flipY=Y    ← 左上角
[CAL] raw(33,319)  -> screen(239,286) swap=Y flipX=N flipY=Y    ← 右下角
[CAL] raw(239,319) -> screen(239,80)  swap=Y flipX=N flipY=Y    ← 另一个角
```

| 物理触摸位置 | raw 值 | swapXY+Yflip 变换后 | 正确目标 |
|------------|--------|-------------------|---------|
| 左上角 | (213, 0) | (0, 106) ← Y 偏下 | (0, 0) |
| 右下角 | (33, 319) | (239, 286) ← Y 偏上 | (239, 319) |

**结论**: swapXY 下 Y 映射错位约 ±33px，且物理上下与 screenY 方向相反。传感器与屏幕是同向的，根本不应该 swap。

**第 4 轮 — `_swapXY=false, _flipX=true, _flipY=false`（最终正确映射）**

串口 `[CAL]` 输出:
```
[CAL] raw(120,179) -> screen(119,179) swap=N flipX=Y flipY=N   ← 屏幕中间 ✓
[CAL] raw(164,80)  -> screen(75,80)   swap=N flipX=Y flipY=N   ← 左上方
[CAL] raw(141,280) -> screen(98,280)  swap=N flipX=Y flipY=N   ← Connect 按钮区域 ✓
```

触摸 Connect (Y=280) 后:
```
[TOUCH] PRESS  (98, 280)
[LVGL] Queued connect
Action: connect to 0
Connected: ab:5e:c5:45:56:10
Subscribed to FFEC
BLE Connected!              ← 成功!
```

**结论**: `_swapXY=false, _flipX=true, _flipY=false` 是正确的映射配置。变换公式 `screenX = 239 - rawX, screenY = rawY`。

### 15.2 最终参数

| 参数 | 最终值 | 说明 |
|------|--------|------|
| `_swapXY` | `false` | 传感器与屏幕同向 (竖屏)，不需要交换 |
| `_flipX` | `true` | X 轴翻转 (传感器左→屏幕右) |
| `_flipY` | `false` | Y 轴方向一致，不需要翻转 |
| `_maxX` | 239 | 屏幕 X 范围 (0-239) |
| `_maxY` | 319 | 屏幕 Y 范围 (0-319) |

### 15.3 校准方法论（给其他 ESP32-S3 一体屏的参考）

1. **先诊断再猜测** — 在 getTouch 中加入 raw→screen 映射日志 (`[CAL]`)，避免反复盲猜坐标组合
2. **用软钳位代替硬截断** — 校准阶段不要丢弃越界的 raw 坐标，而是 clamp 到屏幕范围，这样能收集到完整的传感器覆盖范围数据
3. **五点校准法** — 依次触摸屏幕的「中间→左上→右上→左下→右下」，记录每个位置的 raw 值和期望的 screen 值，4 个角的数据足以推导出 swapXY/flipX/flipY 的正确组合
4. **校准完成后清理日志** — 去掉 `[CAL]` 诊断输出，只保留 `[TOUCH] PRESS/RELEASE`

---

## 16. NimBLEAddress 构造函数编译错误

**现象**: `no matching function for call to 'NimBLEAddress::NimBLEAddress(char [20])'`

**原因**: NimBLE 2.5.0 的 `NimBLEAddress` 构造函数不支持单参数 `char[]`，需要 `std::string` + 地址类型。

**修复**:
```cpp
// 旧 (错误)
NimBLEAddress addr(device_list[index].address);

// 新 (正确)
NimBLEAddress addr(std::string(device_list[index].address), 0);  // 0=public address
```

---

## 17. nimble_connect_by_index 扫描结果失效

**现象**: 设备列表中有设备，但点击 Connect 后提示 `Device not found in scan results`。

**原因**: `nimble_stop_scan()` 内部停止了扫描，随后调用 `getResults()` 可能返回空列表。

**修复**: 改为通过 MAC 地址直接创建 `NimBLEAddress` 连接，不再依赖扫描结果缓存。新的 `nimble_connect_by_index` 直接从 `device_list[index].address` 构造 `NimBLEAddress`，调用 `pClient->connect(addr)` 完成连接，订阅 FFE0/FFEC 服务的过程也内联到该函数中。

---

## 关键依赖版本

| 库 | 版本 |
|----|------|
| ESP32 Arduino Core | 3.3.0 |
| TFT_eSPI | 2.5.43 |
| NimBLE-Arduino | 2.5.0 |
| LVGL | 8.x |
| Wire | 3.3.0 |
