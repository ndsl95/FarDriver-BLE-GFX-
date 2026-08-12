/**
 * display.cpp - 显示模块实现 (GFX 直绘版)
 * TFT_eSPI 直接绘制, 由 ui_gfx.cpp 负责各页面渲染与触摸交互
 */
#include "display.h"
#include "ui_gfx.h"
#include <TFT_eSPI.h>
#include "ft6336g.h"
#include "config.h"

extern TFT_eSPI tft;
extern FT6336G touchPanel;

void display_begin(void)
{
    tft.init();
    tft.setRotation(0);

    /* ILI9341V 屏需 INVON 才能正常显色 (Setup400 未定义 TFT_INVERSION_ON) */
    tft.invertDisplay(true);
    tft.fillScreen(TFT_BLACK);

    uiBegin();

#if TFT_BL_PIN > 0
    /* ESP32 Arduino Core 3.x 移除了 ledcSetup/ledcAttachPin (改用新 API);
       此处按内核版本兼容 2.x 与 3.x */
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttach(TFT_BL_PIN, TFT_BL_FREQ, 8);
    ledcWrite(TFT_BL_PIN, 255);
#else
    ledcSetup(0, TFT_BL_FREQ, 8);
    ledcAttachPin(TFT_BL_PIN, 0);
    ledcWrite(0, 255);
#endif
#endif
}

void display_loop(void)
{
    uiLoop();
}

/* touchPoll: GFX UI 调用, 采样触摸状态; 返回 true 表示采样成功(含抬起) */
bool touchPoll(int16_t *x, int16_t *y, bool *pressed)
{
    uint16_t tx = 0, ty = 0;
    bool t = touchPanel.getTouch(&tx, &ty);
    *x = tx;
    *y = ty;
    *pressed = t;
    return true;
}
