/**
 * FarDriver_BLE.ino — 远驱控制器 BLE 仪表盘 (TFT_eSPI GFX 直绘)
 *
 * 硬件: ESP32-S3 + ILI9341V 2.8" TFT + FT6336G 电容触摸
 * 通信: BLE → FarDriver 控制器 (服务 FFE0 / 特性 FFEC/FFEF)
 * UI:   TFT_eSPI GFX 直绘 (ui_gfx.cpp), 表盘, 扫描动画, 设备选择, 信息页
 */
#include "config.h"
#include "fardriver_protocol.h"
#include "nimble.h"
#include "ft6336g.h"
#include "display.h"
#include "ui_gfx.h"

#include <Preferences.h>
#include <TFT_eSPI.h>

// 全局对象
Preferences preferences;
FT6336G touchPanel;
TFT_eSPI tft = TFT_eSPI();
ControllerData ctr_data = {0};
uint8_t fdMemMap[512] = {0};

// 油门自校准 (最小值/最大值, 持久化到 NVS, 断电不丢失)
static uint16_t throttle_min_adc = 4095;
static uint16_t throttle_max_adc = 0;

// 从 NVS 读取上次保存的校准值 (setup 中调用)
static void throttle_load_cal(void) {
    throttle_min_adc = preferences.getUShort("thrMin", 4095);
    throttle_max_adc = preferences.getUShort("thrMax", 0);
}

// 保存校准值到 NVS (仅在极值变化时调用, 频率低, 不影响 flash 寿命)
static void throttle_save_cal(void) {
    preferences.putUShort("thrMin", throttle_min_adc);
    preferences.putUShort("thrMax", throttle_max_adc);
}

static float throttle_to_pct(uint16_t raw) {
    bool changed = false;
    // 最小值: 记录松开油门时的最小 ADC
    if (raw < throttle_min_adc && raw > 0) { throttle_min_adc = raw; changed = true; }
    // 最大值: 记录拧到底时的最大 ADC
    if (raw > throttle_max_adc) { throttle_max_adc = raw; changed = true; }
    if (changed) throttle_save_cal();   // 极值变化才写入, 避免频繁擦写

    uint16_t range = throttle_max_adc - throttle_min_adc;
    if (range < 20) range = 4095 - throttle_min_adc;   /* 未完成最大值校准前回退到满量程 */

    if (range == 0) return 0;
    float pct = (float)((int32_t)raw - (int32_t)throttle_min_adc) / (float)range * 100.0f;
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

// ============================================================
//  连接状态
// ============================================================
typedef enum {
    CS_SCANNING,   // 扫描动画
    CS_LIST,       // 设备选择列表 (等待用户)
    CS_CONNECTED,
    CS_DISCONNECTED,
} ConnectionState;
ConnectionState conn_state = CS_SCANNING;

static uint32_t scan_start_time = 0;
#define SCAN_DURATION_MS  5000

static uint32_t lastKeepAlive = 0;
static bool     tried_open = false;
static uint32_t no_data_since = 0;
#define KEEPALIVE_INTERVAL_MS  3000

// 当前页面追踪
typedef enum { PG_SCAN, PG_LIST, PG_DASH, PG_INFO } ActivePage;
static ActivePage active_page = PG_SCAN;

// ============================================================
//  BLE 命令发送
// ============================================================
static void fdSendCmd(uint8_t cmd, uint8_t sub, uint8_t v1, uint8_t v2) {
    uint8_t frame[8];
    frame[0] = 0xAA;
    frame[1] = cmd;
    frame[2] = ~cmd;
    frame[3] = sub;
    frame[4] = v1;
    frame[5] = v2;
    frame[6] = frame[0] + frame[1] + frame[2] + frame[3] + frame[4] + frame[5];
    frame[7] = ~frame[6];

    bool is_keepalive = (cmd == 0x13 && sub == 0x07 && v1 == 0x5F);
    if (!is_keepalive) {
        Serial.print(F("[CMD] 0x"));
        Serial.print(cmd, HEX);
        Serial.print(F("/0x"));
        Serial.print(sub, HEX);
    }
    if (!nimble_send(frame, 8)) {
        if (!is_keepalive) Serial.println(F(" FAIL"));
    } else {
        if (!is_keepalive) Serial.println(F(" OK"));
    }
}

static void fdOpen(void)   { fdSendCmd(0x13, 0x07, 0x01, 0xF1); }
static void fdKeepAlive(void) { fdSendCmd(0x13, 0x07, 0x5F, 0x5F); }

// ============================================================
//  UI 回调: 连接设备 (按列表索引)
// ============================================================
static void on_ui_connect(int sel) {
    if (sel < 0 || sel >= nimble_get_device_count()) return;
    BLEDeviceInfo *dev = nimble_get_device_info(sel);
    Serial.print(F("UI Connect: "));
    Serial.println(dev ? dev->name : "?");

    if (nimble_connect_by_index(sel)) {
        nimble_reset_last_notify();   // 重置看门狗时间戳
        conn_state = CS_CONNECTED;
        active_page = PG_DASH;
        uiShowDash();
        Serial.println(F("Connected!"));
    } else {
        Serial.println(F("Connect failed"));
    }
}

// ============================================================
//  UI 回调: 返回主页面 (Info → Dash)
// ============================================================
static void on_ui_back(void) {
    active_page = PG_DASH;
    uiShowDash();
}

// ============================================================
//  UI 回调: 进入信息页 (Dash → Info)
// ============================================================
static void on_ui_open_info(void) {
    active_page = PG_INFO;
    uiShowInfo();
}

// ============================================================
//  UI 回调: 取消扫描 (扫描动画页 Cancel)
//  → 停止扫描并显示设备列表, 由用户选择
// ============================================================
static void on_ui_cancel_scan(void) {
    Serial.println(F("Scan cancelled"));
    nimble_stop_scan();
    show_device_list();
    conn_state = CS_LIST;
}

// ============================================================
//  UI 回调: 设备列表页重新扫描
// ============================================================
static void on_ui_rescan(void) {
    Serial.println(F("Rescan"));
    uiScanClear();
    nimble_disconnect();
    tried_open = false;
    no_data_since = 0;
    nimble_reset_notify_count();

    active_page = PG_SCAN;
    uiShowScan();
    nimble_start();
    scan_start_time = millis();
    conn_state = CS_SCANNING;
}

// ============================================================
//  UI 回调: 断开连接
// ============================================================
static void on_ui_disconnect(void) {
    Serial.println(F("UI Disconnect"));
    nimble_disconnect();
    tried_open = false;
    no_data_since = 0;
    nimble_reset_notify_count();
    delay(200);

    active_page = PG_SCAN;
    uiShowScan();
    nimble_start();
    scan_start_time = millis();
    conn_state = CS_SCANNING;
}

// ============================================================
//  BLE notify 回调: 解析 FarDriver 帧
// ============================================================
void message_handler(uint8_t *pData)
{
    uint8_t id = pData[1] & 0x3F;
    if (id >= FD_MAX_ADDR_ID) return;

    uint8_t *payload = &pData[2];
    fdParseFrame(pData);

    switch (id) {
        case 0:  // RPM, gear, current
            ctr_data.gear = ((payload[2] >> 2) & 0x03) - 1;
            if (ctr_data.gear > 2) ctr_data.gear = 3;
            ctr_data.rawRpm = (int16_t)((payload[4] << 8) | payload[5]);
            ctr_data.fault_byte4 = payload[2];
            ctr_data.fault_byte5 = payload[3];
            {
                float iq = (int16_t)((payload[8] << 8) | payload[9]) / 100.0f;
                float id = (int16_t)((payload[10] << 8) | payload[11]) / 100.0f;
                ctr_data.lineCurrent = sqrtf(iq * iq + id * id);
                ctr_data.isRegen = (iq < 0) || (id < 0);
            }
            break;

        case 1:  // Voltage
            {
                float v = ((payload[0] << 8) | payload[1]) / 10.0f;
                if (v >= 0 && v <= 200.0f) ctr_data.voltage = v;
            }
            break;

        case 4:  // Controller temp
            {
                int t = (int8_t)payload[2];
                if (t > -40 && t < 150) ctr_data.controller_temp = t;
            }
            break;

        case 13:  // Motor temp + throttle
            ctr_data.throttle = ((uint16_t)payload[2] << 8) | payload[3];
            {
                int t = (int8_t)payload[0];
                if (t > -40 && t < 250) ctr_data.motor_temp = t;
            }
            break;
    }

    // 浮点换算
    float pole_pairs = fdPolePairs() > 0 ? (float)fdPolePairs() : 20.0f;
    int16_t rpm_pos = (ctr_data.rawRpm < 0) ? 0 : ctr_data.rawRpm;
    ctr_data.rpm = rpm_pos * 4.0f / pole_pairs;
    ctr_data.speed = ctr_data.rpm * WHEEL_CIRCUMFERENCE_M * 60.0f / 1000.0f;

    if (ctr_data.voltage > 0) {
        ctr_data.power = -ctr_data.lineCurrent * ctr_data.voltage / 1000.0f;
    }
}

// ============================================================
//  填充数据并更新当前显示页面 (非活动页面不更新, 减少刷新开销)
// ============================================================
static void update_ui_data(void) {
    DashData d;
    d.speed    = ctr_data.speed;
    d.volt     = ctr_data.voltage;
    d.curr     = ctr_data.lineCurrent;
    d.power    = ctr_data.power;
    d.ctr      = (int8_t)ctr_data.controller_temp;
    d.mot      = (int8_t)ctr_data.motor_temp;
    d.gear     = ctr_data.gear;
    d.thr      = (uint8_t)constrain((int)throttle_to_pct(ctr_data.throttle), 0, 100);
    d.bat      = (uint8_t)constrain(map((int)(ctr_data.voltage * 10), 600, 1000, 0, 100), 0, 100);

    if (active_page == PG_DASH) {
        uiDashUpdate(&d);
    } else if (active_page == PG_INFO) {
        static uint32_t lastInfoUpdate = 0;
        if (millis() - lastInfoUpdate < 300) return;  // Info 页 300ms 节流 (人眼读数字 3次/秒足够)
        lastInfoUpdate = millis();
        LiveInfo il;
        il.rpm   = ctr_data.rpm;
        il.speed = ctr_data.speed;
        il.power = ctr_data.power;
        il.volt  = ctr_data.voltage;
        il.curr  = ctr_data.lineCurrent;
        il.mot   = ctr_data.motor_temp;
        il.ctr   = ctr_data.controller_temp;
        il.gear  = ctr_data.gear;

        CfgInfo ic;
        ic.ratedV  = fdRatedVoltage();
        ic.ratedKW = fdRatedPower() / 1000.0f;
        ic.pole    = fdPolePairs();
        ic.maxRpm  = fdMaxSpeed();
        snprintf(ic.hw, sizeof(ic.hw), "%c", fdHardwareVer());
        snprintf(ic.sw, sizeof(ic.sw), "%c.%d",
                 fdSoftwareVerMajor(), fdSoftwareVerMinor());
        ic.totalKm = fdTotalDistanceKm();
        uiInfoUpdate(&il, &ic);
    }
}

// ============================================================
//  填充设备选择列表并显示
// ============================================================
static void show_device_list(void) {
    uiScanClear();
    int cnt = nimble_get_device_count();
    for (int i = 0; i < cnt; i++) {
        BLEDeviceInfo *dev = nimble_get_device_info(i);
        if (!dev) continue;
        const char *label = dev->name[0] ? dev->name : dev->address;
        uiScanAdd(label, dev->rssi);
    }
    Serial.print(F("List: "));
    Serial.println(cnt);
    active_page = PG_LIST;
    uiShowList();
}

// ============================================================
//  诊断心跳: 独立任务每 5 秒打印系统状态
//  (主循环卡死也能打印 → 精确定位卡点)
// ============================================================
volatile uint32_t g_loop_cnt = 0;     // 主循环执行计数
volatile uint32_t g_ui_done_ms = 0;    // display_loop 完成时刻
volatile uint32_t g_loop_done_ms = 0;  // 整轮 loop 完成时刻

static void diagTask(void *arg) {
    uint32_t lastCnt = 0, lastMs = 0;
    uint32_t lastPB = 0, lastFC = 0, lastRec = 0, lastRestart = 0;
    uint32_t lastDrift = 0;
    bool     deadWarned = false;   // 隐形失灵已复位过 (下个窗口仍无 fire → 重启)
    bool     driftWarned = false;  // 漂移风暴已复位过 (下个窗口仍风暴 → 重启)
    for (;;) {
        delay(5000);
        uint32_t now = millis();
        uint32_t cnt = g_loop_cnt;
        uint32_t rate = (lastCnt > 0) ? (cnt - lastCnt) * 1000 / (now - lastMs) : 0;
        lastCnt = cnt; lastMs = now;

        uint32_t pb = uiGetBtnPressCnt();
        uint32_t fc = uiGetFireCnt();
        uint32_t rec = touchPanel.getRecoverTotal();
        uint32_t drift = uiGetDriftCnt();
        uint32_t dPB = pb - lastPB, dFC = fc - lastFC, dRec = rec - lastRec;
        uint32_t dDrift = drift - lastDrift;
        lastDrift = drift;

        /* 复位风暴: 5s 内 ≥3 次自恢复 → 芯片无法自愈, 重启整机 (60s 冷却防循环重启) */
        if (dRec >= 3 && now - lastRestart >= 60000) {
            lastRestart = now;
            Serial.println(F("[WD] recover storm, ESP restart"));
            delay(100);
            ESP.restart();
        }

        /* 漂移风暴: 5s 内 ≥5 次漂移解锁 → 伪点占满芯片 2 点容量, 手指无法上报
         * 第一窗口: 复位芯片; 复位后仍风暴 (driftWarned=true) → 重启整机 */
        if (dDrift >= 5) {
            if (driftWarned) {
                Serial.println(F("[WD] drift storm, ESP restart"));
                lastRestart = now;
                delay(100);
                ESP.restart();
            } else {
                driftWarned = true;
                Serial.println(F("[WD] drift storm, resetting chip"));
                touchPanel.recover();
            }
        } else {
            driftWarned = false;
        }

        /* 隐形失灵: 用户按了按钮(≥2次)但 0 次成功响应 → 芯片报点异常 (无粘滞无伪点, 常规防御不触发)
         * 第一窗口: 复位芯片; 复位后仍无响应 (deadWarned=true) → 重启整机
         * (列表页卡片选中已计入成功响应, 无需排除) */
        if (dPB >= 2 && dFC == 0) {
            if (deadWarned) {
                Serial.println(F("[WD] dead touch, ESP restart"));
                lastRestart = now;
                delay(100);
                ESP.restart();
            } else {
                deadWarned = true;
                Serial.println(F("[WD] dead touch, resetting chip"));
                touchPanel.recover();
            }
        } else {
            deadWarned = false;
        }
        lastPB = pb; lastFC = fc; lastRec = rec;

        Serial.print(F("[DIAG] loop="));
        Serial.print(cnt);
        Serial.print(F(" rate="));
        Serial.print(rate);
        Serial.print(F("/s heap="));
        Serial.print(ESP.getFreeHeap());
        Serial.print(F(" st="));
        Serial.print(conn_state);
        Serial.print(F(" pg="));
        Serial.print(active_page);
        Serial.print(F(" conn="));
        Serial.print(is_connected);
        Serial.print(F(" nAgo="));
        uint32_t nm = nimble_get_last_notify_ms();
        Serial.print(nm ? (now - nm) : 0xFFFFFFFF);
        Serial.print(F(" uiAgo="));
        Serial.print(g_ui_done_ms ? (now - g_ui_done_ms) : 0xFFFFFFFF);
        Serial.print(F(" lAgo="));
        Serial.print(g_loop_done_ms ? (now - g_loop_done_ms) : 0xFFFFFFFF);
        Serial.print(F(" tAgo="));
        Serial.print(touchPanel.getLastReadMs() ? (now - touchPanel.getLastReadMs()) : 0xFFFFFFFF);
        Serial.print(F(" tErr="));
        Serial.print(touchPanel.getI2CErrCount());
        Serial.print(F(" tch="));
        Serial.print(touchPanel.getLastTouched() ? 1 : 0);
        Serial.print(F(" tcnt="));
        Serial.print(touchPanel.getLastCount());
        Serial.print(F(" txy="));
        Serial.print(touchPanel.getLastX());
        Serial.print(F(","));
        Serial.print(touchPanel.getLastY());
        Serial.print(F(" tw="));
        Serial.print(touchPanel.getLastWeight());
        Serial.print(F(" wmx="));
        Serial.print(touchPanel.getMaxW());
        Serial.print(F(" w5="));
        Serial.print(touchPanel.getMaxW5());
        Serial.print(F(" pb="));
        Serial.print(dPB);
        Serial.print(F(" fc="));
        Serial.print(dFC);
        Serial.print(F(" dc="));
        Serial.println(dDrift);
        touchPanel.resetMaxW();   // 窗口峰值已打印, 清零进入下个窗口
    }
}

// ============================================================
//  setup
// ============================================================
void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println(F("FarDriver BLE Instrument (GFX)"));

    preferences.begin("fardriver", false);
    throttle_load_cal();   // 加载上次保存的油门校准值

    // 触摸
    Serial.println(F("Init Touch..."));
    touchPanel.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_INT_PIN, TOUCH_I2C_ADDR);

    // 数据
    memset(fdMemMap, 0, sizeof(fdMemMap));
    memset((void*)&ctr_data, 0, sizeof(ctr_data));

    // 注册 UI 回调
    onUiConnect    = on_ui_connect;
    onUiDisconnect = on_ui_disconnect;
    onUiCancelScan = on_ui_cancel_scan;
    onUiRescan     = on_ui_rescan;
    onUiBack       = on_ui_back;
    onUiOpenInfo   = on_ui_open_info;

    // 初始化显示
    Serial.println(F("Init Display..."));
    display_begin();
    uiShowScan();
    active_page = PG_SCAN;

    // 启动 BLE 扫描
    Serial.println(F("Start BLE scan..."));
    nimble_start();
    scan_start_time = millis();
    conn_state = CS_SCANNING;

    Serial.println(F("Ready"));

    // 启动诊断任务 (独立 core, 主循环卡死也能打印)
    xTaskCreatePinnedToCore(diagTask, "diag", 4096, NULL, 1, NULL, 0);
}

// ============================================================
//  loop
// ============================================================
void loop()
{
    g_loop_cnt++;                    // 主循环心跳
    static uint32_t lastDataUpdate = 0;

    // UI 处理 (GFX 直绘 + 触摸状态机)
    display_loop();
    g_ui_done_ms = millis();         // UI 处理完成时刻

    // ============================================================
    //  连接状态机
    // ============================================================
    switch (conn_state) {
        case CS_SCANNING:
            if (millis() - scan_start_time >= SCAN_DURATION_MS) {
                nimble_stop_scan();
                Serial.print(F("Scan done, "));
                Serial.print(nimble_get_device_count());
                Serial.println(F(" devices"));

                // 显示设备选择列表, 由用户手动选择连接
                show_device_list();
                conn_state = CS_LIST;
            }
            break;

        case CS_LIST:
            // 等待用户在列表页选择设备或重新扫描 (通过 UI 回调)
            break;

        case CS_CONNECTED:
            if (!is_connected) {
                conn_state = CS_DISCONNECTED;
                break;
            }

            // 数据流看门狗: 已收到过数据但 10 秒无新通知 → 数据流故障, 强制重连
            // (修复: 原 notify_count 计数单调递增永不减少, 数据停止后无法检测)
            if (nimble_get_last_notify_ms() > 0 &&
                millis() - nimble_get_last_notify_ms() > 10000) {
                Serial.println(F("[WD] No data 10s, force reconnect"));
                nimble_disconnect();
                conn_state = CS_DISCONNECTED;
                break;
            }

            // 数据流管理
            {
                int cur = nimble_get_notify_count();
                if (cur > 0) {
                    no_data_since = 0;
                } else if (no_data_since == 0) {
                    no_data_since = millis();
                }

                if (!tried_open && no_data_since > 0 && millis() - no_data_since > 5000) {
                    tried_open = true;
                    Serial.println(F("[FD] Trying Open..."));
                    fdOpen();
                    lastKeepAlive = millis();
                } else if (tried_open && millis() - lastKeepAlive >= KEEPALIVE_INTERVAL_MS) {
                    lastKeepAlive = millis();
                    fdKeepAlive();
                }
            }

            // 数据更新 (100ms 节流; 值不变不重绘由 fieldR 缓存+指针角度缓存保证)
            {
                uint32_t now = millis();
                if (now - lastDataUpdate >= 100) {
                    lastDataUpdate = now;
                    update_ui_data();
                }
            }
            break;

        case CS_DISCONNECTED:
            Serial.println(F("Disconnected, rescanning..."));
            nimble_disconnect();
            tried_open = false;
            no_data_since = 0;
            nimble_reset_notify_count();
            delay(300);

            active_page = PG_SCAN;
            uiShowScan();
            nimble_start();
            scan_start_time = millis();
            conn_state = CS_SCANNING;
            break;
    }
    g_loop_done_ms = millis();       // 整轮 loop 完成时刻
}
