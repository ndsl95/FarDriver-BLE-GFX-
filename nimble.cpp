/**
 * nimble.cpp - BLE 通信层实现
 * 扫描并收集 FarDriver 设备列表, 支持手动选择连接和断开
 */

#include "nimble.h"
#include "config.h"
#include "fardriver_protocol.h"   // fdComputeCRC (帧/写命令共用的 CRC-16/IBM)
#include <NimBLEDevice.h>

volatile bool is_connected = false;
volatile bool service_found = false;
volatile bool scan_complete = false;
volatile bool scan_active = false;

static const NimBLEAdvertisedDevice* advDevice = nullptr;

// 设备列表
static BLEDeviceInfo device_list[MAX_BLE_DEVICES];
static int device_count = 0;

// 当前连接的设备索引
static int connected_index = -1;

extern void message_handler(uint8_t* pData);

// ============================================================
//  判断设备是否为远驱控制器
//  策略1: 名称包含 "YuanQu"
//  策略2: 广播了 FFE0 服务
//  策略3: MAC 地址匹配 (ab:5e:c5:45:56:10)
// ============================================================
static bool is_far_driver_device(const NimBLEAdvertisedDevice* dev) {
    // 策略1: 名称包含 "YuanQu"
    std::string name = dev->getName();
    if (name.length() > 0 && name.find("YuanQu") != std::string::npos)
        return true;

    // 策略2: 广播了 FFE0 服务
    if (dev->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID)))
        return true;

    // 策略3: MAC 地址匹配 (NimBLE 返回小写格式)
    std::string addr = dev->getAddress().toString();
    if (addr == "ab:5e:c5:45:56:10")
        return true;

    return false;
}

// ============================================================
//  添加设备到列表 (仅添加远驱控制器, 按 MAC 地址去重)
// ============================================================
static void add_device(const NimBLEAdvertisedDevice* dev) {
    // 仅收集远驱控制器, 过滤掉其他 BLE 设备
    if (!is_far_driver_device(dev)) return;

    std::string addr = dev->getAddress().toString();
    std::string name = dev->getName();

    // 检查是否已在列表中
    for (int i = 0; i < device_count; i++) {
        if (strcmp(device_list[i].address, addr.c_str()) == 0) {
            // 已存在, 更新 RSSI
            device_list[i].rssi = dev->getRSSI();
            return;
        }
    }

    // 新设备, 添加到列表
    if (device_count >= MAX_BLE_DEVICES) return;

    strncpy(device_list[device_count].address, addr.c_str(), sizeof(device_list[0].address) - 1);
    device_list[device_count].address[sizeof(device_list[0].address) - 1] = '\0';

    strncpy(device_list[device_count].name, name.c_str(), sizeof(device_list[0].name) - 1);
    device_list[device_count].name[sizeof(device_list[0].name) - 1] = '\0';

    device_list[device_count].rssi = dev->getRSSI();
    device_list[device_count].isFarDriver = true;
    device_count++;

    // 打印日志
    Serial.print(F("[BLE] FarDriver: "));
    Serial.print(addr.c_str());
    Serial.print(F(" \""));
    Serial.print(name.length() > 0 ? name.c_str() : "(none)");
    Serial.print(F("\" RSSI="));
    Serial.println(dev->getRSSI());
}

// ============================================================
//  BLE 客户端回调
// ============================================================
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) {
    pClient->updateConnParams(6, 16, 0, 100);
  }
  void onDisconnect(NimBLEClient* pClient, int reason) {
    Serial.print(pClient->getPeerAddress().toString().c_str());
    Serial.println(F(" Disconnected"));
    is_connected = false;
  }
  bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) {
    return true;
  }
};

// ============================================================
//  扫描回调: 收集所有设备到列表
// ============================================================
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    add_device(advertisedDevice);
  }
};
static ScanCallbacks scanCB;

// ============================================================
//  BLE 通知回调: 接收 16 字节 FarDriver 帧
// ============================================================
static int notify_count = 0;

// 接收诊断统计 (只在 BLE 回调里累加, 由 diagTask 打印,
// 避免在 BLE 线程里占用 Serial 与主循环抢占)
static volatile uint32_t rx_total = 0;    // 通知帧总数
static volatile uint32_t rx_badlen = 0;   // 长度 != 16 的帧数
static uint8_t  rx_last_frame[16] = {0};  // 最近一帧原始数据
static volatile uint8_t rx_last_len = 0;  // 最近一帧长度

void nimble_reset_notify_count(void) {
  notify_count = 0;
  rx_total = 0;
  rx_badlen = 0;
  rx_last_len = 0;
}

int nimble_get_notify_count(void) {
  return notify_count;
}

uint32_t nimble_get_rx_total(void)  { return rx_total; }
uint32_t nimble_get_rx_badlen(void) { return rx_badlen; }

uint8_t nimble_get_last_frame(uint8_t *buf) {
  uint8_t n = rx_last_len;
  if (n && buf) memcpy(buf, rx_last_frame, 16);
  return n;
}

// 通知回调抑制标志 (由 display.cpp 在 Save 期间设置)
volatile bool nimble_suppress_notify_log = false;

/* 上次收到数据通知的时间戳 (数据流看门狗用) */
static uint32_t last_notify_ms = 0;
void nimble_reset_last_notify(void) { last_notify_ms = 0; }
uint32_t nimble_get_last_notify_ms(void) { return last_notify_ms; }

static void notifyCB(NimBLERemoteCharacteristic* pChar, uint8_t* pData,
                       size_t length, bool isNotify) {
  notify_count++;
  last_notify_ms = millis();   // 记录最新数据时间 (看门狗)

  /* 诊断统计: 记录帧数与最近一帧原始内容 (不打印, 避免 BLE 线程抢 Serial)
   * 注: 用显式读-改-写代替 volatile 上的 ++ (C++20 已弃用该写法) */
  rx_total = rx_total + 1u;
  if (length >= 16) {
    memcpy(rx_last_frame, pData, 16);
    rx_last_len = 16;
  } else {
    rx_badlen = rx_badlen + 1u;
    rx_last_len = (uint8_t)length;
  }

  // 帧 hex 日志已关闭, 避免 BLE 线程与主循环的 Serial 竞争产生乱码
  if (length == 16) {
    message_handler(pData);
  }
}

// ============================================================
//  全局对象
// ============================================================
static ClientCallbacks clientCB;
static NimBLERemoteService* pSvc = nullptr;
static NimBLERemoteCharacteristic* pRemChar = nullptr;   // notify 特征 (接收数据)
static NimBLERemoteCharacteristic* pWriteChar = nullptr;  // write 特征 (发送命令)
static NimBLEClient* pClient = nullptr;

bool nimble_notify_ready(void) { return pRemChar != nullptr; }
bool nimble_write_ready(void)  { return pWriteChar != nullptr; }

// ============================================================
//  特性发现与订阅 (两条连接路径共用)
//
//  数据/命令通道固定为 FFEC —— 与实测可用的 EKSR_Instrument 原版、
//  bobecek79/ESP32-Fardriver-BLE-Reader 一致:
//    - notify: 控制器 16 字节状态帧从这里上报
//    - write : 「启动数据流/心跳」命令也从这同一个特性下发
//  找不到 FFEC 时才退化为「第一个可通知 / 可写特性」。
//
//  (修复: 原实现取枚举到的第一个可 notify 特性作为数据通道, 并可能把命令
//   写到 FFEF 等其它特性上; 一旦 FFE0 下有多个特性, 就会订阅到错误的通道,
//   表现为"已连接但一帧数据都收不到")
// ============================================================
static bool setupCharacteristics(void) {
  if (!pSvc) return false;

  auto chars = pSvc->getCharacteristics(true);
  Serial.print(F("Found "));
  Serial.print(chars.size());
  Serial.println(F(" characteristics:"));
  for (auto c : chars) {
    Serial.print(F("  "));
    Serial.print(c->getUUID().toString().c_str());
    Serial.print(F(" n=")); Serial.print(c->canNotify());
    Serial.print(F(" r=")); Serial.print(c->canRead());
    Serial.print(F(" w=")); Serial.print(c->canWrite());
    Serial.print(F(" wn=")); Serial.println(c->canWriteNoResponse());
  }

  // ---- notify 通道: 优先 FFEC ----
  NimBLERemoteCharacteristic *notifyChar = pSvc->getCharacteristic(BLE_CHAR_UUID);
  if (notifyChar && !notifyChar->canNotify()) {
    Serial.print(F("  ")); Serial.print(BLE_CHAR_UUID);
    Serial.println(F(" is not notify-capable, looking for another"));
    notifyChar = nullptr;
  }
  if (!notifyChar) {
    for (auto c : chars) {
      if (c->canNotify()) { notifyChar = c; break; }
    }
  }
  if (!notifyChar) {
    Serial.println(F("No notify characteristic found!"));
    return false;
  }
  Serial.print(F("Notify char: "));
  Serial.println(notifyChar->getUUID().toString().c_str());
  if (!notifyChar->subscribe(true, notifyCB)) {
    Serial.println(F("Subscribe FAILED"));
    return false;
  }
  Serial.println(F("Subscribed OK"));
  pRemChar = notifyChar;

  // ---- write 通道: 优先 FFEC, 其次 FFEF, 最后任意可写特性 ----
  NimBLERemoteCharacteristic *writeChar = pSvc->getCharacteristic(BLE_CHAR_UUID);
  if (writeChar && !(writeChar->canWrite() || writeChar->canWriteNoResponse())) writeChar = nullptr;
  if (!writeChar) {
    writeChar = pSvc->getCharacteristic("FFEF");
    if (writeChar && !(writeChar->canWrite() || writeChar->canWriteNoResponse())) writeChar = nullptr;
  }
  if (!writeChar) {
    for (auto c : chars) {
      if (c->canWrite() || c->canWriteNoResponse()) { writeChar = c; break; }
    }
  }
  pWriteChar = writeChar ? writeChar : notifyChar;
  Serial.print(F("Write char: "));
  Serial.println(pWriteChar->getUUID().toString().c_str());
  return true;
}

// ============================================================
//  连接到服务器 (使用 advDevice)
// ============================================================
bool connectToServer() {
  if (!advDevice) {
    Serial.println(F("No device selected"));
    return false;
  }

  if (NimBLEDevice::getCreatedClientCount()) {
    pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
  }

  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCB, false);
    pClient->setConnectionParams(6, 16, 0, 100);
    pClient->setConnectTimeout(5000);
  }

  if (!pClient->connect(advDevice, false)) {
    Serial.println(F("Connect failed"));
    return false;
  }

  Serial.print(F("Connected: "));
  Serial.println(pClient->getPeerAddress().toString().c_str());

  pSvc = pClient->getService(BLE_SERVICE_UUID);
  if (!pSvc) {
    Serial.println(F("Service FFE0 not found"));
    pClient->disconnect();
    return false;
  }
  if (!setupCharacteristics()) {
    pClient->disconnect();
    return false;
  }
  return true;
}

// ============================================================
//  启动 BLE 扫描 (收集所有设备)
// ============================================================
void nimble_start(void) {
  service_found = false;
  scan_complete = false;
  scan_active = true;
  device_count = 0;

  NimBLEDevice::init("");
  NimBLEDevice::setPower(9);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCB);
  pScan->setInterval(45);
  pScan->setWindow(15);
  pScan->setActiveScan(true);
  pScan->start(0);  // 连续扫描
  Serial.println(F("BLE scan started..."));
}

// ============================================================
//  停止扫描
// ============================================================
void nimble_stop_scan(void) {
  if (scan_active) {
    NimBLEDevice::getScan()->stop();
    scan_active = false;
    scan_complete = true;
    Serial.print(F("Scan complete, found "));
    Serial.print(device_count);
    Serial.println(F(" devices"));
  }
}

// ============================================================
//  获取设备数量
// ============================================================
int nimble_get_device_count(void) {
  return device_count;
}

// ============================================================
//  获取设备信息
// ============================================================
BLEDeviceInfo* nimble_get_device_info(int index) {
  if (index < 0 || index >= device_count) return nullptr;
  return &device_list[index];
}

// ============================================================
//  按索引连接设备 (通过 BLE 地址直连, 不依赖扫描结果)
// ============================================================
bool nimble_connect_by_index(int index) {
  if (index < 0 || index >= device_count) {
    Serial.println(F("Invalid device index"));
    return false;
  }

  nimble_stop_scan();

  // 通过 MAC 地址直接连接, 不需要扫描结果
  NimBLEAddress addr(std::string(device_list[index].address), 0);
  Serial.print(F("Connecting to: "));
  Serial.println(device_list[index].address);

  if (NimBLEDevice::getCreatedClientCount()) {
    pClient = NimBLEDevice::getClientByPeerAddress(addr);
  }

  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCB, false);
    pClient->setConnectionParams(6, 16, 0, 100);
    pClient->setConnectTimeout(5000);
  }

  if (!pClient->connect(addr, false)) {
    Serial.println(F("Connect failed"));
    return false;
  }

  Serial.print(F("Connected: "));
  Serial.println(pClient->getPeerAddress().toString().c_str());

  connected_index = index;

  pSvc = pClient->getService(BLE_SERVICE_UUID);
  if (pSvc) {
    Serial.println(F("Service FFE0 found"));
  } else {
    Serial.println(F("Service FFE0 not found"));
    pClient->disconnect();
    return false;
  }
  if (!setupCharacteristics()) {
    pClient->disconnect();
    return false;
  }

  is_connected = true;
  nimble_reset_notify_count();
  Serial.println(F("BLE Connected!"));
  return true;
}

// ============================================================
//  断开连接
// ============================================================
void nimble_disconnect(void) {
  if (pClient && pClient->isConnected()) {
    pClient->disconnect();
    Serial.println(F("Disconnected by user"));
  }
  is_connected = false;
  service_found = false;
  connected_index = -1;
  pRemChar = nullptr;
  pWriteChar = nullptr;
  pSvc = nullptr;
}

// ============================================================
//  发送数据 (keep-alive 等)
// ============================================================
bool nimble_send(uint8_t *pData, uint16_t len) {
  NimBLERemoteCharacteristic* wc = pWriteChar ? pWriteChar : pRemChar;
  if (!wc) {
    Serial.println(F("[SEND] no characteristic"));
    return false;
  }
  bool ok = wc->writeValue(pData, len, false);
  if (!ok) {
    Serial.print(F("[SEND] writeValue failed, len="));
    Serial.println(len);
  }
  return ok;
}

// ============================================================
//  WriteAddr 格式写入 (2字节值)
//  Frame: AA C6 addr addr val_lo val_hi crc_lo crc_hi
//  CRC 与状态帧相同 (CRC-16/IBM, 初值 0x7F3C, 多项式 0xA001),
//  统一使用 fardriver_protocol.h 中已验证的 fdComputeCRC。
//  (原实现在此复制了一份查表法, 表内有转录错误且 LO/HI 角色写反,
//   会导致写参数命令校验和错误)
// ============================================================
static void calcFDCRC(uint8_t *frame, uint8_t len) {
  uint16_t crc = fdComputeCRC(frame, len);
  frame[len] = (uint8_t)(crc & 0xFF);
  frame[len + 1] = (uint8_t)(crc >> 8);
}

bool nimble_write_addr(uint8_t addr, uint16_t value) {
  uint8_t frame[8];
  frame[0] = 0xAA;
  frame[1] = 0xC6;  // 0xC0 + 6 (4 header + 2 data)
  frame[2] = addr;
  frame[3] = addr;
  frame[4] = value & 0xFF;
  frame[5] = (value >> 8) & 0xFF;
  calcFDCRC(frame, 6);

  Serial.print(F("[WRITE] addr=0x"));
  Serial.print(addr, HEX);
  Serial.print(F(" val="));
  Serial.print(value);
  bool ok = nimble_send(frame, 8);
  Serial.println(ok ? F(" OK") : F(" FAIL"));
  return ok;
}

bool nimble_write_addr8(uint8_t addr, uint8_t value) {
  uint8_t frame[7];
  frame[0] = 0xAA;
  frame[1] = 0xC5;  // 0xC0 + 5 (4 header + 1 data)
  frame[2] = addr;
  frame[3] = addr;
  frame[4] = value;
  calcFDCRC(frame, 5);

  Serial.print(F("[WRITE] addr=0x"));
  Serial.print(addr, HEX);
  Serial.print(F(" val8="));
  Serial.print(value);
  bool ok = nimble_send(frame, 7);
  Serial.println(ok ? F(" OK") : F(" FAIL"));
  return ok;
}

bool nimble_save_param(uint8_t cmd, uint8_t sub, uint8_t v1, uint8_t v2) {
  uint8_t frame[8];
  frame[0] = 0xAA;
  frame[1] = cmd;
  frame[2] = ~cmd;
  frame[3] = sub;
  frame[4] = v1;
  frame[5] = v2;
  frame[6] = frame[0] + frame[1] + frame[2] + frame[3] + frame[4] + frame[5];
  frame[7] = ~frame[6];

  Serial.print(F("[SAVE] cmd=0x"));
  Serial.print(cmd, HEX);
  Serial.print(F(" sub=0x"));
  Serial.print(sub, HEX);
  bool ok = nimble_send(frame, 8);
  Serial.println(ok ? F(" OK") : F(" FAIL"));
  return ok;
}
