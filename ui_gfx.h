#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct { float speed, volt, curr, power; int8_t ctr, mot; uint8_t gear, thr, bat; } DashData;
typedef struct { float rpm, speed, power, volt, curr, mot, ctr; uint8_t gear; } LiveInfo;
typedef struct { float ratedV, ratedKW, totalKm; uint16_t pole, maxRpm; char hw[12], sw[12]; } CfgInfo;

void uiBegin(void);
void uiLoop(void);                      // loop() 里一直调
void uiShowScan(void);
void uiShowList(void);
void uiShowDash(void);
void uiShowInfo(void);

void uiScanClear(void);
void uiScanAdd(const char *name, int8_t rssi);
void uiDashUpdate(const DashData *d);
void uiInfoUpdate(const LiveInfo *l, const CfgInfo *c);

/* 业务回调，按需赋值 */
extern void (*onUiConnect)(int sel);
extern void (*onUiRescan)(void);
extern void (*onUiCancelScan)(void);
extern void (*onUiDisconnect)(void);
extern void (*onUiBack)(void);
extern void (*onUiOpenInfo)(void);

/* 触摸健康统计 (失灵检测用): 按钮锁定次数 / fire 触发次数 / 漂移解锁次数 */
uint32_t uiGetBtnPressCnt(void);
uint32_t uiGetFireCnt(void);
uint32_t uiGetDriftCnt(void);

/* 你的触摸驱动实现：有采样返回 true */
extern bool touchPoll(int16_t *x, int16_t *y, bool *pressed);
