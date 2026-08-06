/**
 * ft6336g.cpp - FT6336G 电容触摸驱动 v8
 *
 * v8 修复:
 *   - 修复关键bug: getTouch 返回 count 但所有点被过滤后读取未初始化内存
 *   - 坐标映射: ES3C28P 横屏转竖屏 (swapXY=true)
 *   - I2C 读取 TD_STATUS 二次确认 (防误触发)
 *   - 变换后坐标再校验 (防垃圾坐标穿透)
 *   - Wire.setTimeOut(100) 防止 I2C 挂死
 */

#include "ft6336g.h"
#include "config.h"

// ============================================================
//  I2C 辅助
// ============================================================
static int _i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
    memset(buf, 0, len);
    Wire.beginTransmission(addr);
    if (Wire.write(reg) != 1) { Wire.endTransmission(true); return -1; }
    if (Wire.endTransmission(false) != 0) { return -2; }
    uint8_t n = Wire.requestFrom((uint16_t)addr, (uint8_t)len);
    for (uint8_t i = 0; i < n && i < len; i++) { buf[i] = Wire.read(); }
    return n;
}

// ============================================================
//  坐标变换 (swap → flip)
// ============================================================
void FT6336G::_transformCoord(uint16_t *x, uint16_t *y) {
    if (_swapXY) { uint16_t tmp = *x; *x = *y; *y = tmp; }
    if (_flipX)  *x = _maxX - *x;
    if (_flipY)  *y = _maxY - *y;
}

// ============================================================
//  begin
// ============================================================
void FT6336G::begin(int sda, int scl, int intPin, uint8_t addr7bit) {
    _addr    = addr7bit;
    _intPin  = intPin;
    _started = false;

    // ==================================================================
    //  ES3C28P 2.8" 屏幕: 物理 240x320 竖屏 (rotation 0)
    //  FT6336G 传感器坐标与屏幕同向 (X:0-239, Y:0-319)
    //  仅 X 轴翻转: 传感器左侧=屏幕右侧, 传感器右侧=屏幕左侧
    //  Y 轴不需要翻转: 传感器顶部=屏幕顶部, 传感器底部=屏幕底部
    // ==================================================================
    _swapXY = false;
    _flipX  = false;   // X 不翻转
    _flipY  = false;   // Y 不翻转
    _maxX   = 239;
    _maxY   = 319;

    _prevTouched = false;
    _prevX = 0; _prevY = 0;
    _i2cErrCnt = 0;
    _lastReadMs = 0;
    _lastTouched = false;
    _lastCount = 0;
    _lastX = 0; _lastY = 0;
    _lastWeight = 0;
    _maxW4 = 0; _maxW5 = 0;
    _prevSelId = -1; _selNew = false; _selReleased = false;
    _prevHad0 = false; _prevHad1 = false;
    _invalidCnt = 0;
    _lastRecoverMs = 0;
    _recoverTotal = 0;
    _recovering = false;

    if (_intPin >= 0) pinMode(_intPin, INPUT);

    Wire.begin(sda, scl);
    Wire.setClock(100000);
    Wire.setTimeOut(10);   // 10ms 超时: 失败快速返回, 防 I2C 挂死拖死主循环
    delay(50);

    // 硬件复位
    #if defined(TOUCH_RST_PIN) && TOUCH_RST_PIN >= 0
        pinMode(TOUCH_RST_PIN, OUTPUT);
        digitalWrite(TOUCH_RST_PIN, LOW);
        delay(10);
        digitalWrite(TOUCH_RST_PIN, HIGH);
        delay(300);
    #else
        delay(300);
    #endif

    // 验证
    Wire.beginTransmission(_addr);
    if (Wire.endTransmission() != 0) {
        Serial.println(F("[FT6336G] NOT found"));
        return;
    }

    // 轮询模式 (连续更新)
    Wire.beginTransmission(_addr);
    Wire.write(0x80); Wire.write(0x01);
    Wire.endTransmission();

    _started = true;
    Serial.print(F("[FT6336G] OK @400kHz  swapXY="));
    Serial.print(_swapXY ? F("on") : F("off"));
    Serial.print(F(" flipX=")); Serial.print(_flipX ? F("on") : F("off"));
    Serial.print(F(" flipY=")); Serial.println(_flipY ? F("on") : F("off"));
}

// ============================================================
//  I2C 自恢复: 连续失败后重新初始化总线 + 硬件复位触摸芯片
//  (长时间运行后 I2C 总线可能因干扰挂死, 复位后恢复)
// ============================================================
#define I2C_ERR_THRESHOLD 3    // 连续失败 3 次 (≈30ms) 触发恢复
#define RECOVER_COOLDOWN_MS 5000 // 恢复冷却: 5s 内只跳过读取不重复复位 (防恢复风暴)
void FT6336G::_i2cRecover(bool fromWd){
    if (millis() - _lastRecoverMs < RECOVER_COOLDOWN_MS) return;  // 冷却内不重复复位
    _lastRecoverMs = millis();
    _recovering = true;      // 恢复互斥: 期间 getTouch 跳过读取, 防跨核 Wire 竞态
    _i2cErrCnt = 0;
    _recoverTotal++;
    Serial.println(fromWd ? F("[FT6336G] watchdog reset") : F("[FT6336G] I2C fail, resetting..."));
    Wire.end();                 // 释放总线
    delay(10);
    Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    Wire.setClock(100000);
    Wire.setTimeOut(10);
    delay(50);
#if defined(TOUCH_RST_PIN) && TOUCH_RST_PIN >= 0
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, LOW);   // 硬件复位芯片
    delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(50);
#endif
    Wire.beginTransmission(_addr);
    Wire.write(0x80); Wire.write(0x01);  // 重新进入轮询模式
    Wire.endTransmission();
    _recovering = false;
}

// ============================================================
//  getTouch — 分阶段读取 + 返回实际有效点数 (v9: 软钳位版)
// ============================================================
uint8_t FT6336G::getTouch(TouchPoint *points) {
    if (!_started) return 0;

    /* 恢复互斥: recover 执行中(可能在其他核) 或 I2C 已判定失败且处于冷却期
     * → 跳过读取快速返回 (触摸短暂静默, 防跨核 Wire 竞态 + 防失败读取拖死主循环) */
    if (_recovering) return 0;
    if (_i2cErrCnt >= I2C_ERR_THRESHOLD && millis() - _lastRecoverMs < RECOVER_COOLDOWN_MS) return 0;

    // 阶段 1: 读 TD_STATUS
    uint8_t status = 0;
    int n = _i2c_read(_addr, 0x02, &status, 1);
    if (n != 1){
        if (++_i2cErrCnt >= I2C_ERR_THRESHOLD) _i2cRecover(false);  // 总线挂死 → 自恢复
        return 0;
    }
    _i2cErrCnt = 0;   // 读取成功, 清零计数
    _lastReadMs = millis();   // 记录成功读取时间 (诊断用)

    uint8_t raw_count = status & 0x0F;
    if (raw_count == 0) return 0;
    if (raw_count > FT6336G_MAX_POINTS) raw_count = FT6336G_MAX_POINTS;

    // 阶段 2: 按需读取坐标数据 (过滤垃圾点: 压力=0 且坐标=0,0 → 干扰数据)
    uint8_t valid_count = 0;
    for (uint8_t i = 0; i < raw_count; i++) {
        uint8_t buf[6] = {0};
        uint8_t reg   = 0x03 + i * 6;
        int n = _i2c_read(_addr, reg, buf, 6);
        if (n != 6){
            if (++_i2cErrCnt >= I2C_ERR_THRESHOLD) _i2cRecover(false);
            return valid_count;
        }
        _i2cErrCnt = 0;

        uint16_t rawX = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];
        uint16_t rawY = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];

        /* 窗口峰值统计 (诊断用: 验证 buf[4]/buf[5] 哪个是真实压力寄存器) */
        if (buf[4] > _maxW4) _maxW4 = buf[4];
        if (buf[5] > _maxW5) _maxW5 = buf[5];

        /* 无效数据过滤: 无压力且坐标零点 → I2C 干扰垃圾数据
         * (真实触摸 weight>0 且坐标非 (0,0); 总线被干扰拉死时读到全 0) */
        if (buf[4] == 0 && rawX == 0 && rawY == 0) {
            _invalidCnt++;
            continue;
        }
        _invalidCnt = 0;

        // 坐标变换 (swap + flip)
        uint16_t tx = rawX, ty = rawY;
        _transformCoord(&tx, &ty);

        // 软钳位: 限制在屏幕范围内 (0-239, 0-319)
        if (tx >= 240) tx = 239;
        if (ty >= 320) ty = 319;

        points[valid_count].id     = (buf[0] >> 4) & 0x0F;
        points[valid_count].weight = buf[4];
        points[valid_count].x      = tx;
        points[valid_count].y      = ty;
        valid_count++;
    }

    /* 连续无效帧超阈值 → 芯片数据异常, 软复位 (统一由 _i2cRecover 内部冷却防风暴) */
    if (_invalidCnt >= 20) {
        _invalidCnt = 0;
        Serial.println(F("[FT6336G] garbage data, recovering..."));
        _i2cRecover(false);
    }
    return valid_count;
}

// ============================================================
//  getTouch (简单接口)
// ============================================================
bool FT6336G::getTouch(uint16_t *x, uint16_t *y) {
    TouchPoint pts[FT6336G_MAX_POINTS];
    uint8_t valid = getTouch(pts);
    _lastTouched = (valid > 0);   // 记录采样快照 (诊断/看门狗用)
    _lastCount = valid;
    if (valid == 0) {
        /* 无触摸点: 上次有选中点 → 释放事件 (全局松开, 原路径) */
        if (_prevSelId >= 0) _selReleased = true;
        _prevSelId = -1; _selNew = false;
        return false;
    }

    /* 多点选点: 手掌/干扰伪点常驻时, 真正的手指点击优先
     * - 默认返回第一个有效点 (单点触摸任意 id 都正确)
     * - 单点新出现 → 返回新点 (刚按下的手指)
     * - 双点新出现 → 选 weight 大的 (手指压力 > 干扰伪点)
     * - 双点都是旧点 → 保持第一个点 (维持原行为) */
    uint16_t tx=pts[0].x, ty=pts[0].y;   // 默认第一个有效点 (勿用 0 初始化!)
    uint8_t  tw=pts[0].weight;           // 选中点的压力 (诊断用)
    int8_t   selIdx=0;                  // 选中点在 pts 中的下标 (释放跟踪用)
    bool h0=false, h1=false;
    int8_t i0=-1, i1=-1;
    uint16_t x0=0,y0=0,x1=0,y1=0;
    uint8_t  w0=0, w1=0;
    for (uint8_t i=0;i<valid;i++){
        if (pts[i].id==0){ i0=i; x0=pts[i].x; y0=pts[i].y; w0=pts[i].weight; h0=true; }
        else            { i1=i; x1=pts[i].x; y1=pts[i].y; w1=pts[i].weight; h1=true; }
    }
    bool new0 = h0 && !_prevHad0;
    bool new1 = h1 && !_prevHad1;
    bool newSel = false;                 // 选中点是否新出现 (新点=手指, 旧点=伪点/残留)
    if (new0 && new1){
        /* 双点同时新出现: 选 weight 大的 (真实手指压力通常 > 伪点) */
        if (w1 > w0){ tx=x1; ty=y1; tw=w1; selIdx=i1; } else { selIdx=i0; }
        newSel = true;
    } else if (new0) { tx=x0; ty=y0; tw=w0; selIdx=i0; newSel = true; }   // id0 刚出现
    else if (new1) { tx=x1; ty=y1; tw=w1; selIdx=i1; newSel = true; }     // id1 刚出现
    _prevHad0=h0; _prevHad1=h1;

    /* 释放检测: 上次选中的点本次消失 → 选中点已抬起 (伪点常驻时手指松开也能检测) */
    _selReleased = false;
    if (_prevSelId >= 0){
        bool alive = false;
        for (uint8_t i=0;i<valid;i++) if (pts[i].id == (uint8_t)_prevSelId){ alive=true; break; }
        if (!alive) _selReleased = true;
    }
    _prevSelId = pts[selIdx].id;
    _selNew = newSel;

    _lastX = tx; _lastY = ty; _lastWeight = tw;
    *x = tx; *y = ty;
    return true;
}

// ============================================================
//  getEvent
// ============================================================
TouchEvent FT6336G::getEvent(uint16_t *x, uint16_t *y) {
    TouchPoint pts[FT6336G_MAX_POINTS];
    uint8_t valid = getTouch(pts);
    bool touched  = (valid > 0);
    TouchEvent evt = TOUCH_NONE;

    if (touched) {
        *x = pts[0].x; *y = pts[0].y;
        if (!_prevTouched) evt = TOUCH_PRESS;
        else {
            int dx = (int)pts[0].x - (int)_prevX;
            int dy = (int)pts[0].y - (int)_prevY;
            if (dx * dx + dy * dy > 9) evt = TOUCH_MOVE;
        }
        _prevX = pts[0].x; _prevY = pts[0].y;
    } else {
        if (_prevTouched) { evt = TOUCH_RELEASE; *x = _prevX; *y = _prevY; }
    }
    _prevTouched = touched;
    return evt;
}
