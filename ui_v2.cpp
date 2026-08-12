/*
 * ui_v2.cpp - ES3C28P 240x320 portrait UI, second generation
 *
 * The BLE/protocol layer only sees the stable API in ui_gfx.h.  This file is
 * intentionally independent of the old ui_gfx.cpp so the original UI remains
 * available as a reference and can be restored by changing platformio.ini.
 */
#include "ui_gfx.h"
#include "ft6336g.h"
#include <TFT_eSPI.h>
#include <Arduino.h>
#include <math.h>
#include <string.h>

extern TFT_eSPI tft;
extern FT6336G touchPanel;

namespace {

enum Page : uint8_t { PAGE_SCAN, PAGE_LIST, PAGE_DASH, PAGE_INFO };

// RGB565 palette: high contrast in daylight, restrained accent colours at night.
constexpr uint16_t C_BG      = 0x0861;
constexpr uint16_t C_PANEL   = 0x10E3;
constexpr uint16_t C_PANEL_2 = 0x1924;
constexpr uint16_t C_LINE    = 0x2986;
constexpr uint16_t C_TEXT    = 0xFFFF;
constexpr uint16_t C_MUTED   = 0x8410;
constexpr uint16_t C_BLUE    = 0x2D7F;
constexpr uint16_t C_CYAN    = 0x07FF;
constexpr uint16_t C_GREEN   = 0x4EEB;
constexpr uint16_t C_AMBER   = 0xFD20;
constexpr uint16_t C_RED     = 0xF9E7;

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 320;
constexpr int MAX_DEVICES = 5;

// 仪表指针满量程速度（km/h）。以后只需修改此值即可调整映射范围。
constexpr float GAUGE_MAX_SPEED_KMH = 150.0f;
// 中间速度数字最多显示三位，超过此值时保持显示 999。
constexpr float SPEED_DISPLAY_MAX_KMH = 999.0f;
// SmoothSpeed 平滑系数：0~1，数值越小越平滑，数值越大响应越快。
constexpr float SMOOTH_SPEED_FACTOR = 0.22f;
// 超速闪烁参数：超过阈值后，每隔 SPEED_BLINK_INTERVAL_MS 切换一次显隐。
constexpr float SPEED_BLINK_THRESHOLD_KMH = 90.0f;
constexpr uint32_t SPEED_BLINK_INTERVAL_MS = 300;
// 数字颜色渐变系数：0~1，数值越小过渡越柔和。
constexpr float SPEED_COLOUR_BLEND_FACTOR = 0.18f;

struct Button {
    int16_t x, y, w, h;
    const char *label;
    uint16_t accent;
    bool outline;
};

const Button BTN_SCAN_CANCEL = {20, 266, 200, 44, "STOP SCAN", C_CYAN, true};
const Button BTN_RESCAN      = {8, 270, 104, 42, "RESCAN", C_CYAN, true};
const Button BTN_CONNECT     = {120, 270, 112, 42, "CONNECT", C_RED, true};
const Button BTN_DISCONNECT  = {8, 291, 104, 23, "DISCONNECT", C_RED, true};
const Button BTN_DETAILS     = {120, 291, 112, 23, "SYSTEM INFO", C_GREEN, true};
const Button BTN_BACK        = {176, 8, 56, 36, "BACK", C_CYAN, true};

Page page = PAGE_SCAN;
SemaphoreHandle_t uiMutex = nullptr;

char deviceName[MAX_DEVICES][24] = {};
int8_t deviceRssi[MAX_DEVICES] = {};
int deviceCount = 0;
int selectedDevice = -1;

uint32_t lastSpinnerMs = 0;
uint8_t spinnerStep = 0;

uint32_t buttonPressCount = 0;
uint32_t fireCount = 0;
uint32_t driftCount = 0;

void lockUi() {
    if (uiMutex) xSemaphoreTakeRecursive(uiMutex, portMAX_DELAY);
}

void unlockUi() {
    if (uiMutex) xSemaphoreGiveRecursive(uiMutex);
}

void text(const char *value, int16_t x, int16_t y, uint16_t colour,
          uint8_t size = 1, uint8_t datum = TL_DATUM) {
    tft.setTextDatum(datum);
    tft.setTextColor(colour, C_BG);
    tft.setTextSize(size);
    tft.drawString(value, x, y, 1);
}

void panel(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fill = C_PANEL) {
    tft.fillRoundRect(x, y, w, h, 8, fill);
    tft.drawRoundRect(x, y, w, h, 8, C_LINE);
}

void drawButton(const Button &b, bool pressed = false) {
    uint16_t fill = pressed ? b.accent : (b.outline ? C_PANEL : b.accent);
    uint16_t stroke = b.accent;
    uint16_t fg = pressed || !b.outline ? C_TEXT : b.accent;
    tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, 8, stroke);
    text(b.label, b.x + b.w / 2, b.y + b.h / 2, fg, 1, MC_DATUM);
}

bool contains(const Button &b, int16_t x, int16_t y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void header(const char *title, const char *eyebrow = "FARDRIVER") {
    tft.fillRect(0, 0, SCREEN_W, 50, C_BG);
    text(eyebrow, 8, 7, C_MUTED, 1);
    text(title, 8, 22, C_TEXT, 2);
    tft.drawFastHLine(8, 49, 224, C_LINE);
}

void drawSignal(int16_t x, int16_t y, int8_t rssi, uint16_t colour) {
    int bars = rssi >= -55 ? 4 : rssi >= -68 ? 3 : rssi >= -80 ? 2 : 1;
    for (int i = 0; i < 4; ++i) {
        int h = 4 + i * 3;
        tft.fillRect(x + i * 5, y + 14 - h, 3, h, i < bars ? colour : C_LINE);
    }
}

void drawDevice(int index) {
    const int y = 54 + index * 42;
    const bool selected = index == selectedDevice;
    const uint16_t fill = selected ? C_PANEL_2 : C_PANEL;
    tft.fillRoundRect(8, y, 224, 36, 7, fill);
    tft.drawRoundRect(8, y, 224, 36, 7, selected ? C_CYAN : C_LINE);
    if (selected) tft.fillRoundRect(8, y, 4, 36, 2, C_CYAN);
    text(deviceName[index], 18, y + 8, C_TEXT, 1);
    char rssi[12];
    snprintf(rssi, sizeof(rssi), "%d", deviceRssi[index]);
    text(rssi, 202, y + 9, C_MUTED, 1, TR_DATUM);
    drawSignal(207, y + 10, deviceRssi[index], selected ? C_CYAN : C_MUTED);
}

void drawListPage() {
    page = PAGE_LIST;
    tft.fillScreen(C_BG);
    header("SELECT DEVICE");
    if (!deviceCount) {
        text("NO CONTROLLER FOUND", 120, 125, C_MUTED, 1, MC_DATUM);
        text("Move closer and scan again", 120, 147, C_MUTED, 1, MC_DATUM);
    } else {
        for (int i = 0; i < deviceCount; ++i) drawDevice(i);
    }
    drawButton(BTN_RESCAN);
    drawButton(BTN_CONNECT, false);
}

void drawSpinner() {
    if (millis() - lastSpinnerMs < 75) return;
    lastSpinnerMs = millis();
    const int cx = 120, cy = 138, r = 36;
    tft.fillCircle(cx, cy, r + 4, C_BG);
    for (int i = 0; i < 12; ++i) {
        const float a = (i * 30 - 90) * DEG_TO_RAD;
        uint16_t c = (i == spinnerStep) ? C_CYAN : (i + 1 == spinnerStep ? C_BLUE : C_LINE);
        int x0 = cx + cosf(a) * 25, y0 = cy + sinf(a) * 25;
        int x1 = cx + cosf(a) * 36, y1 = cy + sinf(a) * 36;
        tft.drawWideLine(x0, y0, x1, y1, 4, c);
    }
    spinnerStep = (spinnerStep + 1) % 12;
}

void labelValue(const char *label, const char *value, int16_t x, int16_t y,
                int16_t w, uint16_t valueColour = C_TEXT) {
    text(label, x + 8, y + 7, C_MUTED, 1);
    text(value, x + w - 8, y + 23, valueColour, 2, BR_DATUM);
}

void arcSegment(int cx, int cy, int radius, int startAngle, int endAngle,
                uint16_t colour, int thickness) {
    for (int r = radius - thickness / 2; r <= radius + thickness / 2; ++r) {
        int oldX = 0, oldY = 0;
        for (int a = startAngle; a <= endAngle; a += 3) {
            float rad = a * DEG_TO_RAD;
            int x = cx + cosf(rad) * r;
            int y = cy + sinf(rad) * r;
            if (a != startAngle) tft.drawLine(oldX, oldY, x, y, colour);
            oldX = x; oldY = y;
        }
    }
}

void drawGaugeFace() {
    const int cx = 120, cy = 92, radius = 66;
    // Four discrete colour zones reproduce the reference without expensive glow effects.
    arcSegment(cx, cy, radius, 140, 198, C_GREEN, 8);
    arcSegment(cx, cy, radius, 203, 261, C_BLUE, 8);
    arcSegment(cx, cy, radius, 266, 324, C_AMBER, 8);
    arcSegment(cx, cy, radius, 329, 400, C_RED, 8);
    for (int a = 140; a <= 400; a += 10) {
        float rad = a * DEG_TO_RAD;
        bool major = ((a - 140) % 30) == 0;
        int r0 = major ? 50 : 54;
        int r1 = 59;
        tft.drawLine(cx + cosf(rad) * r0, cy + sinf(rad) * r0,
                     cx + cosf(rad) * r1, cy + sinf(rad) * r1,
                     major ? C_TEXT : C_MUTED);
    }
}

uint16_t gaugeZoneColour(int angle) {
    if (angle <= 200) return C_GREEN;
    if (angle <= 264) return C_BLUE;
    if (angle <= 327) return C_AMBER;
    return C_RED;
}

uint8_t blendChannel(uint8_t current, uint8_t target, float factor) {
    const int delta = (int)target - (int)current;
    if (!delta) return current;
    int step = (int)roundf(delta * factor);
    if (!step) step = delta > 0 ? 1 : -1;
    return (uint8_t)((int)current + step);
}

uint16_t blendColour565(uint16_t current, uint16_t target, float factor) {
    uint8_t cr = (current >> 11) & 0x1F;
    uint8_t cg = (current >> 5) & 0x3F;
    uint8_t cb = current & 0x1F;
    const uint8_t tr = (target >> 11) & 0x1F;
    const uint8_t tg = (target >> 5) & 0x3F;
    const uint8_t tb = target & 0x1F;
    cr = blendChannel(cr, tr, factor);
    cg = blendChannel(cg, tg, factor);
    cb = blendChannel(cb, tb, factor);
    return (uint16_t)(cr << 11) | (uint16_t)(cg << 5) | cb;
}

constexpr int GAUGE_CX = 120;
constexpr int GAUGE_CY = 92;
constexpr int GAUGE_POINTER_INNER = 48;
constexpr int GAUGE_POINTER_TIP = 78;  // Extends beyond the colour band's outer edge.

void drawGaugePointer(int angle) {
    const float rad = angle * DEG_TO_RAD;
    const float cs = cosf(rad), sn = sinf(rad);
    const int x0 = GAUGE_CX + cs * GAUGE_POINTER_INNER;
    const int y0 = GAUGE_CY + sn * GAUGE_POINTER_INNER;
    const int x1 = GAUGE_CX + cs * GAUGE_POINTER_TIP;
    const int y1 = GAUGE_CY + sn * GAUGE_POINTER_TIP;
    tft.drawWideLine(x0, y0, x1, y1, 7, C_RED);
    tft.drawWideLine(x0, y0, x1, y1, 3, C_TEXT);

    const float sideX = -sn * 5.0f, sideY = cs * 5.0f;
    const int baseX = GAUGE_CX + cs * 70;
    const int baseY = GAUGE_CY + sn * 70;
    tft.fillTriangle(x1, y1,
                     baseX + sideX, baseY + sideY,
                     baseX - sideX, baseY - sideY, C_RED);
}

void eraseGaugePointer(int angle) {
    const float rad = angle * DEG_TO_RAD;
    const float cs = cosf(rad), sn = sinf(rad);
    const int x0 = GAUGE_CX + cs * GAUGE_POINTER_INNER;
    const int y0 = GAUGE_CY + sn * GAUGE_POINTER_INNER;
    const int x1 = GAUGE_CX + cs * GAUGE_POINTER_TIP;
    const int y1 = GAUGE_CY + sn * GAUGE_POINTER_TIP;
    tft.drawWideLine(x0, y0, x1, y1, 11, C_BG);
}

void metricCard(int index, const char *label, uint16_t accent) {
    int x = 4 + index * 47;
    panel(x, 165, 43, 48, C_PANEL);
    tft.fillRect(x + 6, 169, 31, 2, accent);
    text(label, x + 21, 204, C_MUTED, 1, BC_DATUM);
}

void resetDashCache();
void resetInfoCache();

// Fixed-width redraw helper. Cache strings are always normal NUL-terminated strings.
void updateValue(char *cache, size_t cacheSize, const char *value,
                 int16_t x, int16_t y, int16_t w, int16_t h,
                 uint16_t colour, uint8_t size, uint8_t datum = TR_DATUM,
                 uint16_t background = C_PANEL) {
    if (strncmp(cache, value, cacheSize) == 0) return;
    strlcpy(cache, value, cacheSize);
    tft.fillRect(x, y, w, h, background);
    int16_t tx = datum == MC_DATUM ? x + w / 2 : x + w - 4;
    int16_t ty = datum == MC_DATUM ? y + h / 2 : y + 2;
    text(value, tx, ty, colour, size, datum);
}

char speedCache[12] = "";
char powerCache[12] = "";
char voltCache[12] = "";
char currCache[12] = "";
char ctrCache[12] = "";
char motCache[12] = "";
char gearCache[12] = "";
char thrCache[12] = "";
char batCache[12] = "";
uint8_t lastThrottle = 255;
uint8_t lastBattery = 255;
int lastGaugeAngle = 140;
float smoothSpeed = 0.0f;
uint16_t currentSpeedColour = C_GREEN;
bool lastSpeedVisible = true;

void resetDashCache() {
    speedCache[0] = powerCache[0] = voltCache[0] = currCache[0] = '\0';
    ctrCache[0] = motCache[0] = gearCache[0] = thrCache[0] = batCache[0] = '\0';
    lastThrottle = lastBattery = 255;
}

char infoCache[15][16] = {};

void resetInfoCache() {
    for (auto &item : infoCache) item[0] = '\0';
}

void infoRow(const char *label, int row, int col) {
    int x = col ? 122 : 8;
    int y = 56 + row * 49;
    panel(x, y, 110, 43);
    text(label, x + 7, y + 6, C_MUTED, 1);
}

void putInfo(int slot, const char *value, int row, int col, uint16_t colour = C_TEXT) {
    int x = col ? 122 : 8;
    int y = 56 + row * 49;
    updateValue(infoCache[slot], sizeof(infoCache[slot]), value, x + 5, y + 18, 100, 21,
                colour, 1, TR_DATUM);
}

int hitTarget(int16_t x, int16_t y) {
    switch (page) {
        case PAGE_SCAN: return contains(BTN_SCAN_CANCEL, x, y) ? 1 : 0;
        case PAGE_LIST:
            if (contains(BTN_RESCAN, x, y)) return 1;
            if (contains(BTN_CONNECT, x, y)) return 2;
            for (int i = 0; i < deviceCount; ++i)
                if (x >= 8 && x < 232 && y >= 54 + i * 42 && y < 90 + i * 42) return 10 + i;
            return 0;
        case PAGE_DASH:
            if (contains(BTN_DISCONNECT, x, y)) return 1;
            if (contains(BTN_DETAILS, x, y)) return 2;
            return 0;
        case PAGE_INFO: return contains(BTN_BACK, x, y) ? 1 : 0;
    }
    return 0;
}

void drawPressed(int id, bool pressed) {
    if (page == PAGE_SCAN && id == 1) drawButton(BTN_SCAN_CANCEL, pressed);
    else if (page == PAGE_LIST && id == 1) drawButton(BTN_RESCAN, pressed);
    else if (page == PAGE_LIST && id == 2) drawButton(BTN_CONNECT, pressed);
    else if (page == PAGE_DASH && id == 1) drawButton(BTN_DISCONNECT, pressed);
    else if (page == PAGE_DASH && id == 2) drawButton(BTN_DETAILS, pressed);
    else if (page == PAGE_INFO && id == 1) drawButton(BTN_BACK, pressed);
}

void selectDevice(int index) {
    int old = selectedDevice;
    selectedDevice = index;
    if (old >= 0 && old < deviceCount) drawDevice(old);
    drawDevice(index);
}

} // namespace

void (*onUiConnect)(int) = nullptr;
void (*onUiRescan)(void) = nullptr;
void (*onUiCancelScan)(void) = nullptr;
void (*onUiDisconnect)(void) = nullptr;
void (*onUiBack)(void) = nullptr;
void (*onUiOpenInfo)(void) = nullptr;

void uiBegin(void) {
    if (!uiMutex) uiMutex = xSemaphoreCreateRecursiveMutex();
    tft.setTextWrap(false);
    page = PAGE_SCAN;
}

void uiShowScan(void) {
    lockUi();
    page = PAGE_SCAN;
    tft.startWrite();
    tft.fillScreen(C_BG);
    header("SEARCHING");
    panel(20, 78, 200, 154);
    text("Looking for controllers", 120, 190, C_TEXT, 1, MC_DATUM);
    text("This usually takes 5 seconds", 120, 211, C_MUTED, 1, MC_DATUM);
    drawButton(BTN_SCAN_CANCEL);
    tft.endWrite();
    lastSpinnerMs = 0;
    spinnerStep = 0;
    unlockUi();
}

void uiShowList(void) {
    lockUi(); tft.startWrite(); drawListPage(); tft.endWrite(); unlockUi();
}

void uiScanClear(void) {
    lockUi();
    deviceCount = 0;
    selectedDevice = -1;
    bool redraw = page == PAGE_LIST;
    if (redraw) { tft.startWrite(); drawListPage(); tft.endWrite(); }
    unlockUi();
}

void uiScanAdd(const char *name, int8_t rssi) {
    if (!name || deviceCount >= MAX_DEVICES) return;
    lockUi();
    strlcpy(deviceName[deviceCount], name, sizeof(deviceName[deviceCount]));
    deviceRssi[deviceCount] = rssi;
    int added = deviceCount++;
    if (page == PAGE_LIST) { tft.startWrite(); drawDevice(added); tft.endWrite(); }
    unlockUi();
}

void uiShowDash(void) {
    lockUi(); tft.startWrite();
    page = PAGE_DASH;
    tft.fillScreen(C_BG);
    // Compact top status bar, matching the supplied READY / SPORT / BT language.
    text("READY", 8, 7, C_GREEN, 1);
    tft.drawFastVLine(59, 5, 17, C_LINE);
    text("SPORT", 68, 7, C_AMBER, 1);
    tft.drawFastVLine(119, 5, 17, C_LINE);
    text("BT", 128, 7, C_BLUE, 1);
    text("CONNECTED", 151, 7, C_CYAN, 1);
    tft.drawFastHLine(4, 26, 232, C_LINE);

    drawGaugeFace();
    text("--", 120, 79, C_TEXT, 5, MC_DATUM);
    text("km/h", 120, 118, C_MUTED, 1, MC_DATUM);
    tft.drawFastHLine(86, 127, 68, C_BLUE);
    lastGaugeAngle = 140;
    smoothSpeed = 0.0f;
    currentSpeedColour = C_GREEN;
    lastSpeedVisible = true;
    drawGaugePointer(lastGaugeAngle);
    panel(79, 133, 82, 27, C_PANEL_2);
    text("GEAR", 88, 143, C_MUTED, 1);
    text("-", 149, 146, C_TEXT, 2, MR_DATUM);

    metricCard(0, "VOLT", C_BLUE);
    metricCard(1, "AMPS", C_CYAN);
    metricCard(2, "POWER", C_GREEN);
    metricCard(3, "CTRL", C_AMBER);
    metricCard(4, "MOTOR", C_RED);

    panel(4, 219, 232, 30, C_PANEL);
    text("THROTTLE", 10, 224, C_MUTED, 1);
    tft.fillRoundRect(62, 229, 122, 8, 3, C_LINE);
    text("--%", 229, 224, C_TEXT, 1, TR_DATUM);
    panel(4, 254, 232, 30, C_PANEL);
    text("BATTERY", 10, 259, C_MUTED, 1);
    tft.fillRoundRect(62, 264, 122, 8, 3, C_LINE);
    text("--%", 229, 259, C_TEXT, 1, TR_DATUM);
    drawButton(BTN_DISCONNECT); drawButton(BTN_DETAILS);
    resetDashCache();
    tft.endWrite(); unlockUi();
}

void uiDashUpdate(const DashData *d) {
    if (!d || page != PAGE_DASH) return;
    char value[16];
    lockUi(); tft.startWrite();
    const float targetSpeed = constrain(d->speed, 0.0f, SPEED_DISPLAY_MAX_KMH);
    smoothSpeed += (targetSpeed - smoothSpeed) * SMOOTH_SPEED_FACTOR;
    if (fabsf(targetSpeed - smoothSpeed) < 0.05f) smoothSpeed = targetSpeed;
    const float displaySpeed = constrain(smoothSpeed, 0.0f, SPEED_DISPLAY_MAX_KMH);
    snprintf(value, sizeof(value), "%.0f", displaySpeed);
    const int gaugeAngle = 140 + (int)(constrain(smoothSpeed, 0.0f, GAUGE_MAX_SPEED_KMH)
                                      / GAUGE_MAX_SPEED_KMH * 260.0f);
    const uint16_t targetSpeedColour = gaugeZoneColour(gaugeAngle);
    const uint16_t nextSpeedColour = blendColour565(currentSpeedColour, targetSpeedColour,
                                                     SPEED_COLOUR_BLEND_FACTOR);
    const bool speedVisible = d->speed <= SPEED_BLINK_THRESHOLD_KMH ||
                              ((millis() / SPEED_BLINK_INTERVAL_MS) & 1U) == 0;
    if (gaugeAngle != lastGaugeAngle ||
        strncmp(speedCache, value, sizeof(speedCache)) != 0 ||
        nextSpeedColour != currentSpeedColour || speedVisible != lastSpeedVisible) {
        strlcpy(speedCache, value, sizeof(speedCache));
        currentSpeedColour = nextSpeedColour;
        lastSpeedVisible = speedVisible;
        eraseGaugePointer(lastGaugeAngle);
        drawGaugeFace();
        tft.fillRect(72, 57, 96, 49, C_BG);
        if (speedVisible) text(value, 120, 81, currentSpeedColour, 5, MC_DATUM);
        text("km/h", 120, 118, C_MUTED, 1, MC_DATUM);
        tft.drawFastHLine(86, 127, 68, C_BLUE);
        lastGaugeAngle = gaugeAngle;
        drawGaugePointer(lastGaugeAngle);
    }
    snprintf(value, sizeof(value), "%.1f", d->volt);
    updateValue(voltCache, sizeof(voltCache), value, 8, 176, 35, 19, C_BLUE, 1, MC_DATUM);
    snprintf(value, sizeof(value), "%.1f", d->curr);
    updateValue(currCache, sizeof(currCache), value, 55, 176, 35, 19, C_CYAN, 1, MC_DATUM);
    snprintf(value, sizeof(value), "%.2f", d->power);
    updateValue(powerCache, sizeof(powerCache), value, 102, 176, 35, 19, d->power < 0 ? C_GREEN : C_AMBER, 1, MC_DATUM);
    snprintf(value, sizeof(value), "%d", d->ctr);
    updateValue(ctrCache, sizeof(ctrCache), value, 149, 176, 35, 19, d->ctr >= 90 ? C_RED : C_TEXT, 1, MC_DATUM);
    snprintf(value, sizeof(value), "%d", d->mot);
    updateValue(motCache, sizeof(motCache), value, 196, 176, 35, 19, d->mot >= 90 ? C_RED : C_TEXT, 1, MC_DATUM);
    snprintf(value, sizeof(value), "%u%%", d->thr);
    updateValue(thrCache, sizeof(thrCache), value, 194, 224, 37, 16, C_TEXT, 1);
    if (lastThrottle != d->thr) {
        lastThrottle = d->thr;
        tft.fillRoundRect(62, 229, 122, 8, 3, C_LINE);
        if (d->thr) tft.fillRoundRect(62, 229, 122 * d->thr / 100, 8, 3, C_AMBER);
    }
    snprintf(value, sizeof(value), "%u%%", d->bat);
    updateValue(batCache, sizeof(batCache), value, 194, 259, 37, 16, d->bat < 20 ? C_RED : C_GREEN, 1);
    if (lastBattery != d->bat) {
        lastBattery = d->bat;
        tft.fillRoundRect(62, 264, 122, 8, 3, C_LINE);
        if (d->bat) tft.fillRoundRect(62, 264, 122 * d->bat / 100, 8, 3, d->bat < 20 ? C_RED : C_GREEN);
    }
    snprintf(value, sizeof(value), "G%u", d->gear);
    if (strncmp(gearCache, value, sizeof(gearCache)) != 0) {
        strlcpy(gearCache, value, sizeof(gearCache));
        tft.fillRect(126, 138, 29, 18, C_PANEL_2);
        text(value, 151, 146, C_TEXT, 2, MR_DATUM);
    }
    tft.endWrite(); unlockUi();
}

void uiShowInfo(void) {
    lockUi(); tft.startWrite();
    page = PAGE_INFO;
    tft.fillScreen(C_BG);
    header("SYSTEM INFO");
    drawButton(BTN_BACK);
    const char *labels[10] = {"RPM", "SPEED", "POWER", "VOLTAGE", "CURRENT", "MOTOR", "CTRL", "GEAR", "RATED", "DISTANCE"};
    for (int i = 0; i < 10; ++i) infoRow(labels[i], i / 2, i % 2);
    resetInfoCache();
    tft.endWrite(); unlockUi();
}

void uiInfoUpdate(const LiveInfo *l, const CfgInfo *c) {
    if (!l || page != PAGE_INFO) return;
    char value[20];
    lockUi(); tft.startWrite();
    snprintf(value, sizeof(value), "%.0f", l->rpm); putInfo(0, value, 0, 0, C_CYAN);
    snprintf(value, sizeof(value), "%.1f km/h", l->speed); putInfo(1, value, 0, 1);
    snprintf(value, sizeof(value), "%+.2f kW", l->power); putInfo(2, value, 1, 0, l->power < 0 ? C_GREEN : C_AMBER);
    snprintf(value, sizeof(value), "%.1f V", l->volt); putInfo(3, value, 1, 1);
    snprintf(value, sizeof(value), "%.1f A", l->curr); putInfo(4, value, 2, 0);
    snprintf(value, sizeof(value), "%.0f C", l->mot); putInfo(5, value, 2, 1, l->mot >= 90 ? C_RED : C_TEXT);
    snprintf(value, sizeof(value), "%.0f C", l->ctr); putInfo(6, value, 3, 0, l->ctr >= 90 ? C_RED : C_TEXT);
    snprintf(value, sizeof(value), "G%u", l->gear); putInfo(7, value, 3, 1, C_BLUE);
    if (c) {
        snprintf(value, sizeof(value), "%.0fV %.1fkW", c->ratedV, c->ratedKW); putInfo(8, value, 4, 0);
        snprintf(value, sizeof(value), "%.1f km", c->totalKm); putInfo(9, value, 4, 1);
    }
    tft.endWrite(); unlockUi();
}

namespace {

void fireTarget(int id) {
    switch (page) {
        case PAGE_SCAN: if (id == 1 && onUiCancelScan) onUiCancelScan(); break;
        case PAGE_LIST:
            if (id == 1 && onUiRescan) onUiRescan();
            else if (id == 2 && selectedDevice >= 0 && onUiConnect) onUiConnect(selectedDevice);
            break;
        case PAGE_DASH:
            if (id == 1 && onUiDisconnect) onUiDisconnect();
            else if (id == 2 && onUiOpenInfo) onUiOpenInfo();
            break;
        case PAGE_INFO: if (id == 1 && onUiBack) onUiBack(); break;
    }
}

} // namespace

uint32_t uiGetBtnPressCnt(void) { return buttonPressCount; }
uint32_t uiGetFireCnt(void) { return fireCount; }
uint32_t uiGetDriftCnt(void) { return driftCount; }

void uiLoop(void) {
    static uint32_t lastPoll = 0, pressSince = 0, lastReset = 0;
    static int pressedId = -1;
    static int16_t lastX = -1, lastY = -1;
    static uint32_t movement = 0;
    static bool selectedPointWasNew = false;
    if (page == PAGE_SCAN) { lockUi(); tft.startWrite(); drawSpinner(); tft.endWrite(); unlockUi(); }
    if (millis() - lastPoll < 10) return;
    lastPoll = millis();

    int16_t x, y; bool pressed;
    if (!touchPoll(&x, &y, &pressed)) return;
    bool selectedReleased = touchPanel.getSelReleased();
    bool selectedNew = touchPanel.getSelNew();
    int fireId = 0;

    lockUi(); tft.startWrite();
    if (pressed) {
        if (selectedNew || !pressSince) { movement = 0; lastX = x; lastY = y; }
        else if (lastX >= 0) {
            movement += abs(x - lastX) + abs(y - lastY);
            lastX = x; lastY = y;
        }
        if (movement > 60) {
            ++driftCount;
            if (pressedId > 0) drawPressed(pressedId, false);
            pressedId = -1; pressSince = 0; selectedPointWasNew = false;
            movement = 0; lastX = lastY = -1;
            tft.endWrite(); unlockUi(); return;
        }
        if (!pressSince) pressSince = millis();
        if (millis() - pressSince >= 6000) {
            if (pressedId > 0) drawPressed(pressedId, false);
            pressedId = -1; pressSince = 0; selectedPointWasNew = false;
            tft.endWrite(); unlockUi();
            if (millis() - lastReset >= 30000) { lastReset = millis(); touchPanel.recover(); }
            return;
        }
        int current = hitTarget(x, y);
        if (current != pressedId) {
            if (pressedId > 0) drawPressed(pressedId, false);
            if (page == PAGE_LIST && current >= 10 && current - 10 < deviceCount) {
                selectDevice(current - 10); ++fireCount;
            }
            pressedId = current;
            selectedPointWasNew = selectedNew;
            if (pressedId > 0) { drawPressed(pressedId, true); ++buttonPressCount; }
        } else if (selectedNew) selectedPointWasNew = true;
    }
    // FT6336G may keep a drifting ghost point after the selected finger is
    // released. Honour the driver's selected-point release before waiting for
    // the global pressed state to become false.
    if (selectedReleased && pressedId >= 0 && selectedPointWasNew) {
        fireId = pressedId;
        if (fireId > 0) drawPressed(fireId, false);
        pressedId = -1; pressSince = 0; selectedPointWasNew = false;
        movement = 0; lastX = lastY = -1;
    } else if (!pressed && pressedId >= 0) {
        fireId = pressedId;
        if (fireId > 0) drawPressed(fireId, false);
        pressedId = -1; pressSince = 0; selectedPointWasNew = false;
        movement = 0; lastX = lastY = -1;
    }
    tft.endWrite(); unlockUi();
    if (fireId > 0) { ++fireCount; fireTarget(fireId); }
}
