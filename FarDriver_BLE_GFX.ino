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
#include <WiFi.h>          // 仅用于关闭 WiFi 射频 (本项目只用 BLE)
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

/* 校准值版本: 换了油门数据源就 +1, 让旧的校准值自动作废 (否则比例会对不上) */
#define THROTTLE_CAL_VERSION  2

// 从 NVS 读取上次保存的校准值 (setup 中调用)
static void throttle_load_cal(void) {
    if (preferences.getUChar("thrVer", 0) != THROTTLE_CAL_VERSION) {
        throttle_min_adc = 65535; throttle_max_adc = 0;
        preferences.putUChar("thrVer", THROTTLE_CAL_VERSION);
        return;
    }
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

    /* 校准值合理性保护: NVS 存了异常极值(如调试时写入) → 自动重置重新学习 */
    if (throttle_max_adc > 0 && throttle_max_adc < throttle_min_adc) {
        throttle_min_adc = raw; throttle_max_adc = raw; throttle_save_cal();
    }
    uint16_t savedRange = throttle_max_adc - throttle_min_adc;
    if (savedRange > 8000) {   /* 范围过大 = 校准值异常, 重置 */
        throttle_min_adc = raw; throttle_max_adc = raw; throttle_save_cal();
        return 0;
    }

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

static uint32_t connect_ms = 0;      // 本次连接建立时刻
static uint32_t lastStreamCmdMs = 0; // 上次发送数据流启动/心跳帧的时刻
/* 控制器需要持续(每 2s)收到启动帧才会上报状态帧 —— 与原版实现一致 */
#define STREAM_CMD_INTERVAL_MS  BLE_KEEPALIVE_INTERVAL_MS   // 2000ms (config.h)
#define NO_DATA_RECONNECT_MS    10000

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

    /* 周期性的启动/心跳帧不打印, 否则每 2 秒刷屏 */
    bool is_keepalive = (cmd == 0x13 && sub == 0x07);
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

/* 启动并维持控制器数据流: AA 13 EC 07 01 F1 A2 5D
 *
 * fdSendCmd 生成的 8 字节命令与两个实测可用项目 (EKSR_Instrument 原版、
 * bobecek79/ESP32-Fardriver-BLE-Reader 的 keep-alive) 完全一致:
 * 控制器只有在持续收到这一帧时才会推送 16 字节状态帧, 所以连接成功后要
 * 「立即发一次 + 之后每 2 秒重复」。
 *
 * 原实现把它当成兜底逻辑: 只在"5 秒内一帧通知都没收到"时补发一次, 而且
 * 判定用的是单调递增的 notify 计数 (收到过任何通知就永远为真) —— 结果是
 * 连接成功后启动帧几乎永远不会发出, 控制器自然一帧数据都不上报。 */
static void fdStartStream(void) { fdSendCmd(0x13, 0x07, 0x01, 0xF1); }

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

        /* 连接成功后立刻启动数据流 (与原版一致), 之后由 loop 每 2 秒维持 */
        connect_ms = millis();
        fdStartStream();
        lastStreamCmdMs = millis();

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
    connect_ms = 0;
    lastStreamCmdMs = 0;
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
    connect_ms = 0;
    lastStreamCmdMs = 0;
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
//
//  字段布局依据 jackhumbert/fardriver-controllers 的 fardriver.hpp
//  (FardriverData 结构体, 小端), 并与实测帧 + 已验证读取器
//  (bobecek79/ESP32-Fardriver-BLE-Reader) 三方核对一致:
//
//   地址    payload 偏移        含义                      换算
//   0xE2    [0] bits2-3         档位                      bits + 1 → G3
//           [2] / [3]           故障位组 1 / 组 2
//           [6..7] int16 LE     MeasureSpeed (速度测量值)
//   0xE8    [0..1] int16 LE     电池电压                  /10 → V
//           [4..5] int16 LE     线电流                    /4  → A (负=回充)
//           [10..11] int16 LE   ThrottleDepth (油门深度)
//   0xD6    [10..11] int16 LE   控制器(MOS)温度            °C
//   0xF4    [0..1] int16 LE     电机温度                   °C
//           [3]                 电量 SOC                   %
//
//  两个必须注意的点 (也是原来"电压恒为 0"的根因):
//  1) 同一地址会以多个 id 轮转上报 —— 0xE2 出现在 id 0/7/14/21/28..., 0xE8
//     出现在 id 1/8/15/22...。原实现按 id 分派 (case 0/1/4/13), 只有 1/8 的
//     相关帧会命中; 必须按"查表地址"分派。
//  2) 所有多字节字段都是小端 —— 原实现按大端读电压, 84.7V 被读成 2022.7V,
//     再被范围检查丢弃 → 屏幕恒显 0.0V; 电流/温度/档位也都取自错误字节。
// ============================================================
void message_handler(uint8_t *pData)
{
    uint8_t id = pData[1] & 0x3F;
    if (id >= FD_MAX_ADDR_ID) return;

    const uint8_t *payload = &pData[2];
    uint8_t addr = fdParseFrame(pData);   // 同时写入 fdMemMap (信息页用)
    if (addr == 0xFF) return;

    switch (addr) {
        case 0xE2:  // 档位 / 故障位 / 速度测量值
            ctr_data.gear = (uint8_t)(((payload[0] >> 2) & 0x03) + 1);
            ctr_data.fault_byte4 = payload[2];
            ctr_data.fault_byte5 = payload[3];
            ctr_data.rawRpm = (int16_t)(((uint16_t)payload[7] << 8) | payload[6]);
            break;

        case 0xE8:  // 电池电压 / 线电流 / 油门深度
            {
                float v = (int16_t)(((uint16_t)payload[1] << 8) | payload[0]) / 10.0f;
                if (v > 0.0f && v < 200.0f) ctr_data.voltage = v;

                float a = (int16_t)(((uint16_t)payload[5] << 8) | payload[4]) / 4.0f;
                if (a > -400.0f && a < 400.0f) {
                    ctr_data.lineCurrent = a;
                    ctr_data.isRegen = (a < 0.0f);
                }

                /* 油门深度: 负值/异常按 0 处理 (不能直接跳过, 否则松油门后
                 * 会停留在上一次的数值上不下去) */
                int16_t thr = (int16_t)(((uint16_t)payload[11] << 8) | payload[10]);
                ctr_data.throttle = (thr > 0) ? (uint16_t)thr : 0;
            }
            break;

        case 0xD6:  // 控制器 (MOS) 温度
            {
                int t = (int16_t)(((uint16_t)payload[11] << 8) | payload[10]);
                if (t > -40 && t < 200) ctr_data.controller_temp = t;
            }
            break;

        case 0xF4:  // 电机温度 / 电量
            {
                int t = (int16_t)(((uint16_t)payload[1] << 8) | payload[0]);
                if (t > -40 && t < 250) ctr_data.motor_temp = t;
                if (payload[3] <= 100) ctr_data.soc = payload[3];
            }
            break;

        default:
            break;
    }

    // 速度换算: 测量值 × 4 / 极对数 = 轮转速, 再按轮胎周长折算 km/h
    int16_t rpm_pos = (ctr_data.rawRpm < 0) ? 0 : ctr_data.rawRpm;
    ctr_data.rpm = rpm_pos * 4.0f / (float)MOTOR_POLE_PAIRS;
    ctr_data.speed = ctr_data.rpm * WHEEL_CIRCUMFERENCE_M * 60.0f / 1000.0f;

    // 功率 (kW): 正 = 放电, 负 = 回充 (与 UI 配色约定一致)
    ctr_data.power = ctr_data.voltage * ctr_data.lineCurrent / 1000.0f;
}

// ============================================================
//  填充数据并更新当前显示页面 (非活动页面不更新, 减少刷新开销)
// ============================================================
static void update_ui_data(void) {
    /* 数据解耦: ctr_data 由 BLE 回调(Core 0)写入; 本函数在渲染核(Core 1)先做值快照,
     * 渲染全程用本地副本 DashData d, 避免渲染期间持续跨核读取 → 消除数据迁移延迟 */
    DashData d;
    d.speed    = ctr_data.speed;
    d.volt     = ctr_data.voltage;
    d.curr     = ctr_data.lineCurrent;
    d.power    = ctr_data.power;
    d.ctr      = (int8_t)ctr_data.controller_temp;
    d.mot      = (int8_t)ctr_data.motor_temp;
    d.gear     = ctr_data.gear;
    d.thr      = (uint8_t)constrain((int)throttle_to_pct(ctr_data.throttle), 0, 100);
    /* 电量: 优先用控制器上报的 SOC (0xF4 帧 payload[3]); 无效时按电压粗估 */
    if (ctr_data.soc > 0 && ctr_data.soc <= 100) {
        d.bat = ctr_data.soc;
    } else {
        d.bat = (uint8_t)constrain(map((int)(ctr_data.voltage * 10), 600, 1000, 0, 100), 0, 100);
    }

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
        ic.pole    = MOTOR_POLE_PAIRS;
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
        Serial.print(dDrift);

        /* ---- FarDriver 数据流诊断 (定位"连接后没有数据") ----
         * fd_rx   : 累计收到的 BLE 通知帧数
         * badlen  : 长度 != 16 的通知数
         * drx     : 本 5 秒窗口新增帧数 (恒为 0 = 一帧都没上来)
         * last{..}: 最近一帧的 id / 查表地址 / CRC 校验结果 / 原始 hex
         *           —— crc=BAD 说明收到的帧不完整或映射有误 */
        {
            static uint32_t lastRxTotal = 0;
            static uint8_t  lastFid = 0xFF;
            uint32_t rx = nimble_get_rx_total();
            Serial.print(F(" fd_rx="));
            Serial.print(rx);
            Serial.print(F(" badlen="));
            Serial.print(nimble_get_rx_badlen());
            Serial.print(F(" drx="));
            Serial.print(rx - lastRxTotal);
            lastRxTotal = rx;

            uint8_t frame[16];
            uint8_t flen = nimble_get_last_frame(frame);
            if (flen == 16) {
                uint8_t fid = frame[1] & 0x3F;
                Serial.print(F(" last{id="));
                Serial.print(fid);
                if (fid < FD_MAX_ADDR_ID) {
                    Serial.print(F(" addr=0x"));
                    Serial.print(FD_FLASH_ADDR[fid], HEX);
                }
                Serial.print(F(" len=16 crc="));
                Serial.print(fdVerifyCRC(frame) ? F("OK") : F("BAD"));
                Serial.print(F(" hex="));
                for (uint8_t i = 0; i < 16; i++) {
                    if (frame[i] < 0x10) Serial.print('0');
                    Serial.print(frame[i], HEX);
                    Serial.print(' ');
                }
                Serial.print(F("} "));
                lastFid = fid;
            } else if (lastFid != 0xFF) {
                Serial.print(F(" last{len="));
                Serial.print(flen);
                Serial.print(F("} "));
            }
        }
        Serial.println();
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

    /* 关闭 WiFi 射频: 本项目只用 BLE, 关 WiFi 可省电并减少 2.4G 频段干扰
     * (ESP32-S3 WiFi/BLE 共用射频, WIFI_OFF 不影响 NimBLE 的 BLE 功能) */
    WiFi.mode(WIFI_OFF);

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
    /* 核心绑定: loop 运行于 Core 1 (APP_CPU); 触摸(display_loop)与渲染(update_ui_data)
     * 同核串行 → 无跨核竞态、无数据迁移; BLE 协议栈与 diagTask 运行于 Core 0 */
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

            // ============================================================
            //  数据流管理
            //  1) 每 2 秒重复发送启动/心跳帧 (AA 13 EC 07 01 F1 ...):
            //     控制器靠它维持 16 字节状态帧的上报, 连接后必须持续发送
            //  2) 看门狗: 连接后 10 秒内一帧都没收到, 或收到过又静默 10 秒
            //     → 判定数据流未建立, 强制重连
            // ============================================================
            {
                uint32_t nm = nimble_get_last_notify_ms();
                bool streaming = (nm > 0) && (millis() - nm <= NO_DATA_RECONNECT_MS);

                if (!streaming && (millis() - connect_ms > NO_DATA_RECONNECT_MS)) {
                    Serial.println(F("[WD] No data 10s, force reconnect"));
                    nimble_disconnect();
                    conn_state = CS_DISCONNECTED;
                    break;
                }

                if (millis() - lastStreamCmdMs >= STREAM_CMD_INTERVAL_MS) {
                    lastStreamCmdMs = millis();
                    fdStartStream();
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
            connect_ms = 0;
            lastStreamCmdMs = 0;
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
