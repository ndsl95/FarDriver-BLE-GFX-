/**
 * nimble.h - BLE 通信层 (NimBLE)
 * 扫描 FarDriver 控制器, 提供设备列表选择和连接管理
 */

#ifndef NIMBLE_H
#define NIMBLE_H

#include <Arduino.h>

// ============================================================
//  BLE 设备信息结构
// ============================================================
#define MAX_BLE_DEVICES  15

struct BLEDeviceInfo {
    char address[20];    // MAC 地址字符串
    char name[32];       // 设备名称
    int8_t rssi;         // 信号强度
    bool isFarDriver;    // 是否为远驱控制器
};

// ============================================================
//  状态变量
// ============================================================
extern volatile bool is_connected;
extern volatile bool service_found;
extern volatile bool scan_complete;     // 扫描是否完成
extern volatile bool scan_active;      // 是否正在扫描

// ============================================================
//  扫描和设备列表
// ============================================================

/** 启动 BLE 扫描 (连续模式, 需调用 nimble_stop_scan 停止) */
void nimble_start(void);

/** 停止扫描 */
void nimble_stop_scan(void);

/** 获取已发现的设备数量 */
int nimble_get_device_count(void);

/** 上次收到数据通知的时间戳 (0=从未收到, 数据流看门狗用) */
uint32_t nimble_get_last_notify_ms(void);

/** 重置通知时间戳 (连接/重连时调用) */
void nimble_reset_last_notify(void);

/** 获取指定索引的设备信息 */
BLEDeviceInfo* nimble_get_device_info(int index);

/** 按索引连接设备 */
bool nimble_connect_by_index(int index);

/** 断开当前连接 */
void nimble_disconnect(void);

/** 连接到服务器 (使用已存储的设备) */
bool connectToServer(void);

/** 发送数据 */
bool nimble_send(uint8_t *pData, uint16_t len);

/** 写参数到控制器 (WriteAddr 格式, 2字节值) */
bool nimble_write_addr(uint8_t addr, uint16_t value);

/** 写参数到控制器 (WriteAddr 格式, 1字节值) */
bool nimble_write_addr8(uint8_t addr, uint8_t value);

/** 通过旧版命令保存参数 */
bool nimble_save_param(uint8_t cmd, uint8_t sub, uint8_t v1, uint8_t v2);

/** 重置 notify 计数器 (新连接时调用) */
void nimble_reset_notify_count(void);

/** 获取 notify 计数 */
int nimble_get_notify_count(void);

#endif
