/**
 * nimble.cpp - BLE 通信层实现
 * 扫描并收集 FarDriver 设备列表, 支持手动选择连接和断开
 */

#include "nimble.h"
#include "config.h"
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

void nimble_reset_notify_count(void) {
  notify_count = 0;
}

int nimble_get_notify_count(void) {
  return notify_count;
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
  if (pSvc) {
    // 枚举所有特性，分别查找 notify 和 write 特征
    auto chars = pSvc->getCharacteristics(true);
    for (auto c : chars) {
      if (c->canNotify() && !pRemChar) {
        pRemChar = c;
        pRemChar->subscribe(true, notifyCB);
        Serial.print(F("Subscribed notify: "));
        Serial.println(c->getUUID().toString().c_str());
      }
      if ((c->canWrite() || c->canWriteNoResponse()) && !pWriteChar) {
        pWriteChar = c;
        Serial.print(F("Found write: "));
        Serial.println(c->getUUID().toString().c_str());
      }
    }
    // 备选: 按 UUID 查找
    if (!pRemChar) {
      pRemChar = pSvc->getCharacteristic("FFEC");
      if (pRemChar && pRemChar->canNotify())
        pRemChar->subscribe(true, notifyCB);
    }
    if (!pWriteChar) {
      pWriteChar = pSvc->getCharacteristic("FFEF");
      if (!pWriteChar && pRemChar && (pRemChar->canWrite() || pRemChar->canWriteNoResponse()))
        pWriteChar = pRemChar;
    }
    if (!pRemChar) {
      Serial.println(F("No notify characteristic found!"));
      pClient->disconnect();
      return false;
    }
  } else {
    Serial.println(F("Service FFE0 not found"));
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

    // 枚举所有特性，分别查找 notify 和 write 特征
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

        // 查找 notify 特征 (优先 FFEC，用于接收数据)
        if (c->canNotify() && !pRemChar) {
          std::string uuid = c->getUUID().toString();
          // 优先匹配 FFEC，但如果是其他可通知特征也先用着
          pRemChar = c;
          if (pRemChar->subscribe(true, notifyCB)) {
            Serial.print(F("  -> Subscribed notify: "));
            Serial.println(uuid.c_str());
          } else {
            Serial.println(F("  -> Subscribe FAILED"));
          }
        }
        // 查找 write 特征 (优先 FFEF，用于发送命令)
        if ((c->canWrite() || c->canWriteNoResponse()) && !pWriteChar) {
          pWriteChar = c;
          Serial.print(F("  -> Found write: "));
          Serial.println(c->getUUID().toString().c_str());
        }
      }

    // 备选: 直接按 UUID 查找 FFEC (notify) 和 FFEF (write)
    if (!pRemChar) {
      pRemChar = pSvc->getCharacteristic("FFEC");
      if (pRemChar && pRemChar->canNotify()) {
        pRemChar->subscribe(true, notifyCB);
        Serial.println(F("Subscribed to FFEC (fallback)"));
      }
    }
    if (!pWriteChar) {
      pWriteChar = pSvc->getCharacteristic("FFEF");
      if (pWriteChar && (pWriteChar->canWrite() || pWriteChar->canWriteNoResponse())) {
        Serial.println(F("Found write FFEF (fallback)"));
      } else {
        pWriteChar = nullptr;
      }
    }
    // 如果实在找不到 write 特征，回退用 notify 特征
    if (!pWriteChar) {
      if (pRemChar && (pRemChar->canWrite() || pRemChar->canWriteNoResponse())) {
        pWriteChar = pRemChar;
        Serial.println(F("No dedicated write char, using notify char for write"));
      } else {
        Serial.println(F("WARNING: No write characteristic found! Commands may fail."));
        pWriteChar = pRemChar;  // 最后尝试
      }
    }

    if (!pRemChar) {
      Serial.println(F("No notify characteristic found!"));
      pClient->disconnect();
      return false;
    }
  } else {
    Serial.println(F("Service FFE0 not found"));
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
// ============================================================
static const uint8_t FD_CRC_HI_NIM[256] = {
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
  0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
  0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,
  0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40,0x01,0xC0,0x80,0x41,
  0x01,0xC0,0x80,0x41,0x00,0xC1,0x81,0x40
};
static const uint8_t FD_CRC_LO_NIM[256] = {
  0x00,0xC0,0xC1,0x01,0xC3,0x03,0x02,0xC2,0xC6,0x06,0x07,0xC7,0x05,0xC5,0xC4,0x04,
  0xCC,0x0C,0x0D,0xCD,0x0F,0xCF,0xCE,0x0E,0x0A,0xCA,0xCB,0x0B,0xC9,0x09,0x08,0xC8,
  0xD8,0x18,0x19,0xD9,0x1B,0xDB,0xDA,0x1A,0x1E,0xDE,0xDF,0x1F,0xDD,0x1D,0x1C,0xDC,
  0x14,0xD4,0xD5,0x15,0xD7,0x17,0x16,0xD6,0xD2,0x12,0x13,0xD3,0x11,0xD1,0xD0,0x10,
  0xF0,0x30,0x31,0xF1,0x33,0xF3,0xF2,0x32,0x36,0xF6,0xF7,0x37,0xF5,0x35,0x34,0xF4,
  0x3C,0xFC,0xFD,0x3D,0xFF,0x3F,0x3E,0xFE,0xFA,0x3A,0x3B,0xFB,0x39,0xF9,0xF8,0x38,
  0x28,0xE8,0xE9,0x29,0xEB,0x2B,0x2A,0xEA,0xEE,0x2E,0x2F,0xEF,0x2D,0xED,0xEC,0x2C,
  0xE4,0x24,0x25,0xE5,0x27,0xE7,0xE6,0x26,0x22,0xE2,0xE3,0x23,0xE1,0x21,0x20,0xE0,
  0xA0,0x60,0x61,0xA1,0x63,0xA3,0xA2,0x62,0x66,0xA6,0xA7,0x67,0xA5,0x65,0x64,0xA4,
  0x6C,0xAC,0xAD,0x6D,0xAF,0x6F,0x6E,0xAE,0xAA,0x6A,0x6B,0xAB,0x69,0xA9,0xA8,0x68,
  0x78,0xB8,0xB9,0x79,0xBB,0x7B,0x7A,0xBA,0xBE,0x7E,0x7F,0xBF,0x7D,0xBD,0xBC,0x7C,
  0xB4,0x74,0x75,0xB5,0x77,0xB7,0xB6,0x76,0x72,0xB2,0xB3,0x73,0xB1,0x71,0x70,0xB0,
  0x50,0x90,0x91,0x51,0x93,0x53,0x52,0x92,0x96,0x56,0x57,0x97,0x55,0x95,0x94,0x54,
  0x9C,0x5C,0x5D,0x9D,0x5F,0x9F,0x9E,0x5E,0x5A,0x9A,0x9B,0x5B,0x99,0x59,0x58,0x98,
  0x88,0x48,0x49,0x89,0x4B,0x8B,0x8A,0x4A,0x4E,0x8E,0x8F,0x4F,0x8D,0x4D,0x4C,0x8C,
  0x44,0x84,0x85,0x45,0x87,0x47,0x46,0x86,0x82,0x42,0x43,0x83,0x41,0x81,0x80,0x40
};

static void calcFDCRC(uint8_t *frame, uint8_t len) {
  uint8_t a = 0x3C, b = 0x7F;
  for (uint8_t i = 0; i < len; i++) {
    uint8_t idx = a ^ frame[i];
    a = b ^ FD_CRC_HI_NIM[idx];
    b = FD_CRC_LO_NIM[idx];
  }
  frame[len] = a;
  frame[len + 1] = b;
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
