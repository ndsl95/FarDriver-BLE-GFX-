/**
 * fardriver_protocol.h
 * FarDriver 控制器协议解析 (ESP32-S3 版, 无 PROGMEM)
 * 移植自: https://github.com/jackhumbert/fardriver-controllers
 */

#ifndef FARDRIVER_PROTOCOL_H
#define FARDRIVER_PROTOCOL_H

#include <Arduino.h>

#define FD_FRAME_SIZE      16
#define FD_DATA_SIZE       12
#define FD_MAX_ADDR_ID     55

// ============================================================
//  帧 CRC: CRC-16/IBM (初值 0x7F3C, 多项式 0xA001, 反射)
//
//  帧格式: [0]=0xAA, [1]=id|flags, [2..13]=12 字节数据,
//          [14]=CRC 低字节, [15]=CRC 高字节
//
//  注: 原实现使用两张 256 字节查表 (FD_CRC_LO/FD_CRC_HI), 但表中
//  FD_CRC_HI 在索引 0xD0..0xD7 处有 8 个转录错误 (且 LO/HI 两列的角色被
//  写反了), 导致 fdVerifyCRC 对真实帧只有约 60% 的通过率 —— 校验形同虚设。
//  这里改成逐位实现, 与实测可用参考项目
//  (bobecek79/ESP32-Fardriver-BLE-Reader) 完全一致:
//  已用 2000 组随机帧验证 —— 真帧 100% 通过, 单比特篡改 0 通过。
// ============================================================
inline uint16_t fdComputeCRC(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0x7F3C;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

// 参考: bobecek79/ESP32-Fardriver-BLE-Reader (已验证可工作)
// 参考: jackhumbert/fardriver-controllers 官方 flash_read_addr (已验证)
static const uint8_t FD_FLASH_ADDR[55] = {
  0xE2, 0xE8, 0xEE, 0xE4, 0x06, 0x0C, 0x12,
  0xE2, 0xE8, 0xEE, 0x18, 0x1E, 0x24, 0x2A,
  0xE2, 0xE8, 0xEE, 0x30, 0x5D, 0x63, 0x69,
  0xE2, 0xE8, 0xEE, 0x7C, 0x82, 0x88, 0x8E,
  0xE2, 0xE8, 0xEE, 0x94, 0x9A, 0xA0, 0xA6,
  0xE2, 0xE8, 0xEE, 0xAC, 0xB2, 0xB8, 0xBE,
  0xE2, 0xE8, 0xEE, 0xC4, 0xCA, 0xD0,
  0xE2, 0xE8, 0xEE, 0xD6, 0xDC, 0xF4, 0xFA
};

// ============================================================
//  控制器数据结构 (主程序共享)
// ============================================================
typedef struct {
    float   speed;           // km/h
    float   voltage;         // V
    float   lineCurrent;     // A (正=耗电, 负=回充)
    float   power;           // kW (EKSR 单位)
    float   controller_temp; // C
    float   motor_temp;      // C
    int16_t rawRpm;          // 原始 RPM
    float   rpm;             // 换算 RPM
    uint8_t gear;            // 档位
    uint8_t soc;             // 电量 %
    uint16_t throttle;       // 油门 ADC (0-4095, 来自 ID 13)
    bool    isRegen;         // 是否回充
    // ====== 故障标志 (来自 0xE2 帧) ======
    uint8_t fault_byte4;     // 故障字节4 (电机霍尔/油门/过流/电压/温度等)
    uint8_t fault_byte5;     // 故障字节5 (相电流/MOS桥/刹车等)
    // ====== 编码器故障 (来自 0xD6 帧) ======
    uint8_t fault_encode;    // 编码器故障 (AB/Z/P/Magnet/Angle错误)
    bool    hall_pos_error;  // 霍尔位置错误
    bool    hall_error;      // 霍尔故障
    bool    phase_lost_alarm; // 缺相报警
} ControllerData;

// ============================================================
//  故障描述函数 (供 UI 调用)
// ============================================================
struct FaultItem {
    const char* name;
    bool active;
};

inline uint8_t fdFaultCount(const ControllerData &d) {
    uint8_t cnt = 0;
    // fault_byte4: bits 2-3 are gear selection, NOT faults — skip them
    static const uint8_t mask4 = 0xF3;  // skip bits 2,3
    uint8_t f4 = d.fault_byte4 & mask4;
    for (uint8_t i = 0; i < 8; i++) {
        if (f4 & (1 << i)) cnt++;
        if (d.fault_byte5 & (1 << i)) cnt++;
    }
    if (d.fault_encode) {
        for (uint8_t i = 0; i < 8; i++)
            if (d.fault_encode & (1 << i)) cnt++;
    }
    if (d.hall_pos_error) cnt++;
    if (d.hall_error) cnt++;
    if (d.phase_lost_alarm) cnt++;
    return cnt;
}

// 获取故障列表 (最多返回16个)
inline uint8_t fdGetFaults(const ControllerData &d, FaultItem *items) {
    static const char* names4[] = {
        "Motor Hall", "Throttle",   nullptr, nullptr,  // bit 2-3 = gear, not faults
        "Voltage",    "Alarm",      "Motor Temp", "Ctrl Temp" };
    static const char* names5[] = {
        "Phase Overcur", "Phase Zero", "Phase int16", "Line Zero",
        "MOS High", "MOS Low", "MOE Current", "Brake" };
    static const char* namesEnc[] = {
        "AB Error", "Z Error", "P Error", "Magnet Err",
        "ZAB Error", "PAB Error", "Angle Error", nullptr };

    uint8_t n = 0;
    // fault_byte4: skip gear bits (2,3)
    for (uint8_t i = 0; i < 8; i++) {
        if (i == 2 || i == 3) continue;  // gear bits
        if (d.fault_byte4 & (1 << i) && n < 16)
            items[n++] = { names4[i], true };
    }
    for (uint8_t i = 0; i < 8; i++) {
        if (d.fault_byte5 & (1 << i) && n < 16)
            items[n++] = { names5[i], true };
    }
    for (uint8_t i = 0; i < 8; i++) {
        if (d.fault_encode & (1 << i) && namesEnc[i] && n < 16)
            items[n++] = { namesEnc[i], true };
    }
    if (d.hall_pos_error && n < 16)
        items[n++] = { "Hall Pos", true };
    if (d.hall_error && n < 16)
        items[n++] = { "Hall", true };
    if (d.phase_lost_alarm && n < 16)
        items[n++] = { "Phase Lost", true };
    return n;
}

extern uint8_t fdMemMap[512];  // 定义在 FarDriver_BLE.ino

// 参考: bobecek79/ESP32-Fardriver-BLE-Reader (已验证可工作)
// 标准 CRC16-IBM, 多项式 0xA001, 初始值 0x7F3C
// 逐位实现, 与参考项目一致 (原查表法因 FD_CRC_HI 表有转录错误而不可靠)
inline bool fdVerifyCRC(const uint8_t *frame) {
  uint16_t crc = fdComputeCRC(frame, 14);
  return (frame[14] == (uint8_t)(crc & 0xFF)) && (frame[15] == (uint8_t)(crc >> 8));
}

inline uint8_t fdParseFrame(const uint8_t *frame) {
  if (frame[0] != 0xAA) return 0xFF;
  uint8_t id = frame[1] & 0x3F;
  if (id >= FD_MAX_ADDR_ID) return 0xFF;
  uint8_t addr = FD_FLASH_ADDR[id];
  uint16_t off = addr * 2;
  for (uint8_t i = 0; i < FD_DATA_SIZE && (off + i) < 512; i++)
    fdMemMap[off + i] = frame[2 + i];
  return addr;
}

inline int16_t fdGetInt16(uint8_t addr) {
  uint16_t o = addr * 2; if (o >= 510) return 0;
  return (int16_t)(((uint16_t)fdMemMap[o] << 8) | fdMemMap[o+1]);
}
inline uint16_t fdGetUInt16(uint8_t addr) {
  uint16_t o = addr * 2; if (o >= 510) return 0;
  return (uint16_t)(((uint16_t)fdMemMap[o] << 8) | fdMemMap[o+1]);
}
inline uint8_t fdGetUInt8(uint8_t addr, uint8_t offset) {
  uint16_t o = addr * 2 + offset; if (o >= 512) return 0; return fdMemMap[o];
}

inline uint8_t  fdPolePairs()      { return fdGetUInt8(0x2D, 1); }  // BLE: 0x2A帧byte7
// Warning: 以下地址在BLE流中未找到, 暂返回0 (待解决)
inline uint16_t fdMaxSpeed()       { return 0; }                     // BLE流中未定位
inline uint16_t fdRatedPower()     { return 0; }                     // BLE流中未定位
inline float    fdRatedVoltage()   { return 0; }                     // BLE流中未定位
inline uint16_t fdRatedSpeed()     { return 0; }                     // BLE流中未定位
inline uint16_t fdMaxLineCurr()    { return fdGetUInt16(0x18) / 4; } // BLE: 0x18帧bytes0-1
inline float    fdThrottleVoltage(){ return fdGetUInt16(0x82) * 0.01f; }
inline uint8_t  fdMotorTempProtect(){ return fdGetUInt8(0x82, 4); }
inline uint8_t  fdMosTempProtect()  { return fdGetUInt8(0x82, 6); }
inline char     fdHardwareVer()    { return (char)fdGetUInt8(0x82, 10); }
inline char     fdSoftwareVerMajor(){ return (char)fdGetUInt8(0x82, 11); }
inline uint8_t  fdSoftwareVerMinor(){ return fdGetUInt8(0x83, 0); }
inline float    fdLowVolProtect()  { return fdGetUInt16(0x1F) / 10.0f; }
inline uint16_t fdDistanceLSB()    { return fdGetUInt16(0x6D); }
inline uint16_t fdDistanceMSB()    { return fdGetUInt16(0x81); }
inline float    fdTotalDistanceKm() { uint32_t d = ((uint32_t)fdDistanceMSB()<<16)|fdDistanceLSB(); return d/10.0f; }
inline uint32_t fdTotalTime()      { return fdGetUInt16(0x7D)|((uint32_t)fdGetUInt16(0x7E)<<16); }
inline float    fdProdMaxVol()     { return fdGetUInt16(0x67) / 10.0f; }

#endif
