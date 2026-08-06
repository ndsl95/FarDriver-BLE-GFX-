/**
 * ft6336g.h - FT6336G 电容触摸驱动 (改进版)
 *
 * 改进内容:
 *   - 支持 2 点触摸
 *   - 触摸按压/释放事件检测
 *   - 坐标翻转/交换 (适配不同屏幕方向)
 *   - 触控模式配置 (握手模式/中断触发模式)
 *   - 修正 I2C 地址为 7 位
 *   - 防重复触发
 */

#ifndef FT6336G_H
#define FT6336G_H

#include <Arduino.h>
#include <Wire.h>

#define FT6336G_MAX_POINTS  2

// 触摸事件类型
typedef enum {
    TOUCH_NONE = 0,
    TOUCH_PRESS,      // 手指按下
    TOUCH_MOVE,        // 手指滑动
    TOUCH_RELEASE,     // 手指抬起
} TouchEvent;

// 单个触摸点
typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  id;       // 触摸点 ID (0 或 1)
    uint8_t  weight;   // 按压力度 (0-255)
} TouchPoint;

class FT6336G {
public:
    void begin(int sda, int scl, int intPin, uint8_t addr7bit = 0x1C);

    /** 读取触摸状态, 返回触摸点数 (0/1/2) */
    uint8_t getTouch(TouchPoint *points);

    /** 简单接口: 获取第一个触摸点坐标, 返回是否被触摸 */
    bool getTouch(uint16_t *x, uint16_t *y);

    /** 检测触摸事件 (需要连续调用才能检测 PRESS/RELEASE) */
    TouchEvent getEvent(uint16_t *x, uint16_t *y);

    /** 设置坐标翻转 */
    void setFlip(bool flipX, bool flipY) { _flipX = flipX; _flipY = flipY; }

    /** 设置坐标交换 (横竖屏切换时可能需要) */
    void setSwapXY(bool swap) { _swapXY = swap; }

    /** 设置最大坐标 (用于翻转换算, 默认 240x320) */
    void setMaxXY(uint16_t maxX, uint16_t maxY) { _maxX = maxX; _maxY = maxY; }

    /** 是否初始化成功 */
    bool isReady() const { return _started; }

    /** 当前 I2C 连续失败计数 (诊断用, 0=正常) */
    uint8_t getI2CErrCount() const { return _i2cErrCnt; }

    /** 最近一次成功读取的时间戳 (诊断用, 0=从未读取) */
    uint32_t getLastReadMs() const { return _lastReadMs; }

    /** 最近一次采样: 是否检测到触摸 (诊断用, 判断芯片状态粘滞) */
    bool getLastTouched() const { return _lastTouched; }

    /** 最近一次采样触摸点数 (诊断用: 1=单点 2=多点) */
    uint8_t getLastCount() const { return _lastCount; }

    /** 最近一次采样返回点的压力值 (诊断用: 区分真实手指 vs 干扰伪点) */
    uint8_t getLastWeight() const { return _lastWeight; }

    /** 窗口内 buf[4] 峰值 (诊断用: 验证压力寄存器位置, 打印后需 reset) */
    uint8_t getMaxW() const { return _maxW4; }

    /** 窗口内 buf[5] 峰值 (诊断用: 验证压力寄存器位置, 打印后需 reset) */
    uint8_t getMaxW5() const { return _maxW5; }

    /** 清零窗口峰值 (诊断打印后调用) */
    void resetMaxW() { _maxW4 = 0; _maxW5 = 0; }

    /** 选中点释放事件: 上次选中的触摸点本次消失 (读后自动清零)
     * 用于伪点常驻时检测手指松开 → fire 不再依赖全局无触摸 */
    bool getSelReleased() { bool r = _selReleased; _selReleased = false; return r; }

    /** 最近一次采样选中点是否为新出现 (新点=刚按下的手指, 区分伪点用) */
    bool getSelNew() const { return _selNew; }

    /** 最近一次采样坐标 (诊断用) */
    uint16_t getLastX() const { return _lastX; }
    uint16_t getLastY() const { return _lastY; }

    /** 状态粘滞时强制复位芯片 (按下超时看门狗用) */
    void recover(void) { _i2cRecover(true); }

    /** 自恢复总次数 (诊断+失灵检测用: 5s 内≥3次=复位风暴) */
    uint32_t getRecoverTotal() const { return _recoverTotal; }

private:
    uint8_t  _addr;
    int      _intPin;
    bool     _started;
    bool     _flipX, _flipY, _swapXY;
    uint16_t _maxX, _maxY;
    bool     _prevTouched;
    uint16_t _prevX, _prevY;
    uint16_t _lastEventMs;
    uint8_t  _i2cErrCnt;   // I2C 连续失败计数 (超阈值触发自恢复)
    uint32_t _lastReadMs;   // 最近成功读取时间戳 (诊断用)
    bool     _lastTouched;  // 最近一次采样触摸状态 (诊断用)
    uint8_t  _lastCount;    // 最近一次采样触摸点数 (诊断用)
    uint16_t _lastX, _lastY; // 最近一次采样坐标 (诊断用)
    uint8_t  _lastWeight;    // 最近一次采样返回点的压力值 (诊断用)
    uint8_t  _maxW4, _maxW5; // 窗口峰值 (buf4/buf5, 验证压力寄存器用)
    int8_t   _prevSelId;   // 上次采样选中点 id (-1=无), 释放检测用
    bool     _selNew;      // 最近采样选中点是否新出现
    bool     _selReleased; // 选中点释放事件 (读后清零)
    bool     _prevHad0, _prevHad1;  // 各触摸点上次是否出现 (多点选新点用)
    uint8_t  _invalidCnt;   // 连续无效帧计数 (垃圾数据自恢复用)
    uint32_t _lastRecoverMs; // 上次软复位时间 (复位冷却)
    uint32_t _recoverTotal; // 自恢复累计次数 (诊断/失灵检测用)
    volatile bool _recovering; // 恢复执行中标志 (getTouch 期间跳过读取, 防跨核 Wire 竞态)

    void _readRegisters(uint8_t reg, uint8_t *buf, uint8_t len);
    void _transformCoord(uint16_t *x, uint16_t *y);
    void _i2cRecover(bool fromWd);  // 总线/芯片复位 (fromWd: 看门狗主动复位 vs I2C 失败自恢复)
};
#endif