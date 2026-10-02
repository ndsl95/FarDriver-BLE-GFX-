/*
 * ui_v2.cpp - ES3C28P 320x240 landscape customer UI
 *
 * The BLE/protocol layer only sees the stable API in ui_gfx.h.  This file is
 * intentionally independent of the old ui_gfx.cpp so the original UI remains
 * available as a reference and can be restored by changing platformio.ini.
 */
#include "ui_gfx.h"
#include "ft6336g.h"
#include "ui_chinese_font.h"
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

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
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

const Button BTN_SCAN_CANCEL = {64, 202, 192, 30, "停止扫描", C_CYAN, true};
const Button BTN_RESCAN      = {8, 205, 146, 27, "重新扫描", C_CYAN, true};
const Button BTN_CONNECT     = {166, 205, 146, 27, "连接", C_GREEN, true};
const Button BTN_DISCONNECT  = {4, 211, 104, 23, "断开连接", C_RED, true};
const Button BTN_DETAILS     = {208, 211, 108, 23, "详细信息", C_GREEN, true};
const Button BTN_BACK        = {260, 8, 52, 30, "返回", C_CYAN, true};

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

uint32_t nextCodepoint(const char *&value) {
    const uint8_t first = (uint8_t)*value++;
    if (first < 0x80) return first;
    const int extra = first < 0xE0 ? 1 : first < 0xF0 ? 2 : 3;
    uint32_t code = first & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
    for (int i = 0; i < extra; ++i) {
        if (((uint8_t)*value & 0xC0) != 0x80) return 0xFFFD;
        code = (code << 6) | ((uint8_t)*value++ & 0x3F);
    }
    return code;
}

int chineseTextWidth(const char *value, uint8_t size) {
    int width = 0;
    while (*value) width += (nextCodepoint(value) < 0x80 ? 6 : 12) * size;
    return width;
}

template<typename Canvas>
void drawChineseText(Canvas &canvas, const char *value, int16_t x, int16_t y,
                     uint16_t colour, uint8_t size, uint8_t datum) {
    const int width = chineseTextWidth(value, size);
    const int height = 12 * size;
    if (datum % 3 == 1) x -= width / 2;
    else if (datum % 3 == 2) x -= width;
    if (datum / 3 == 1) y -= height / 2;
    else if (datum / 3 == 2) y -= height;
    while (*value) {
        const uint32_t code = nextCodepoint(value);
        if (code < 0x80) {
            canvas.drawChar(x, y + 2 * size, code, colour, colour, size);
            x += 6 * size;
            continue;
        }
        const ChineseGlyph *glyph = nullptr;
        for (const auto &candidate : chineseGlyphs) {
            if (candidate.code == code) { glyph = &candidate; break; }
        }
        if (glyph) {
            for (int row = 0; row < 12; ++row) {
                const uint16_t bits = glyph->rows[row];
                for (int col = 0; col < 12;) {
                    if (!(bits & (1U << (11 - col)))) { ++col; continue; }
                    const int start = col++;
                    while (col < 12 && (bits & (1U << (11 - col)))) ++col;
                    canvas.fillRect(x + start * size, y + row * size,
                                 (col - start) * size, size, colour);
                }
            }
        } else {
            canvas.drawRect(x + size, y + size, 10 * size, 10 * size, colour);
        }
        x += 12 * size;
    }
}

template<typename Canvas>
void textOn(Canvas &canvas, const char *value, int16_t x, int16_t y, uint16_t colour,
          uint8_t size = 1, uint8_t datum = TL_DATUM) {
    for (const char *p = value; *p; ++p) {
        if ((uint8_t)*p >= 0x80) {
            drawChineseText(canvas, value, x, y, colour, size, datum);
            return;
        }
    }
    canvas.setTextDatum(datum);
    canvas.setTextColor(colour, colour);
    canvas.setTextSize(size);
    canvas.drawString(value, x, y, 1);
}

void text(const char *value, int16_t x, int16_t y, uint16_t colour,
          uint8_t size = 1, uint8_t datum = TL_DATUM) {
    textOn(tft, value, x, y, colour, size, datum);
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

void header(const char *title, const char *eyebrow = "远驱仪表") {
    tft.fillRect(0, 0, SCREEN_W, 46, C_BG);
    text(eyebrow, 8, 4, C_MUTED, 1);
    text(title, 8, 20, C_TEXT, 2);
}

void drawSignal(int16_t x, int16_t y, int8_t rssi, uint16_t colour) {
    int bars = rssi >= -55 ? 4 : rssi >= -68 ? 3 : rssi >= -80 ? 2 : 1;
    for (int i = 0; i < 4; ++i) {
        int h = 4 + i * 3;
        tft.fillRect(x + i * 5, y + 14 - h, 3, h, i < bars ? colour : C_LINE);
    }
}

void drawDevice(int index) {
    const int y = 46 + index * 29;
    const bool selected = index == selectedDevice;
    panel(8, y, 304, 25, selected ? C_PANEL_2 : C_PANEL);
    tft.drawRoundRect(8, y, 304, 25, 8, selected ? C_CYAN : C_LINE);
    tft.fillCircle(18, y + 12, 3, selected ? C_CYAN : C_MUTED);
    // UTF-8 aware truncation leaves the signal area free.
    char name[24] = {};
    size_t used = 0;
    int width = 0;
    const char *cursor = deviceName[index];
    while (*cursor) {
        const char *previous = cursor;
        uint32_t code = nextCodepoint(cursor);
        const size_t bytes = cursor - previous;
        const int glyphWidth = code < 128 ? 6 : 12;
        if (width + glyphWidth > 190 || used + bytes >= sizeof(name)) break;
        memcpy(name + used, previous, bytes);
        used += bytes;
        width += glyphWidth;
    }
    text(name, 29, y + 12, C_TEXT, 1, ML_DATUM);
    char rssi[12];
    snprintf(rssi, sizeof(rssi), "%d dBm", deviceRssi[index]);
    text(rssi, 266, y + 12, C_MUTED, 1, MR_DATUM);
    drawSignal(280, y + 5, deviceRssi[index], selected ? C_CYAN : C_MUTED);
}

void drawListPage() {
    page = PAGE_LIST;
    tft.fillScreen(C_BG);
    header("选择设备");
    char count[24];
    snprintf(count, sizeof(count), "发现 %d 台设备", deviceCount);
    text(count, 312, 7, C_CYAN, 1, TR_DATUM);
    if (!deviceCount) {
        text("未发现控制器", 160, 110, C_MUTED, 1, MC_DATUM);
        text("请靠近控制器重新扫描", 160, 135, C_MUTED, 1, MC_DATUM);
    } else {
        for (int i = 0; i < deviceCount; ++i) drawDevice(i);
    }
    drawButton(BTN_RESCAN);
    drawButton(BTN_CONNECT);
}

void drawSpinner() {
    if (millis() - lastSpinnerMs < 75) return;
    lastSpinnerMs = millis();
    const int cx = 76, cy = 118, r = 35;
    tft.fillCircle(cx, cy, r + 4, C_PANEL);
    for (int i = 0; i < 12; ++i) {
        const float a = (i * 30 - 90) * DEG_TO_RAD;
        const uint16_t c = i == spinnerStep ? C_CYAN : (i + 1) % 12 == spinnerStep ? C_BLUE : C_LINE;
        tft.drawWideLine(cx + cosf(a) * 25, cy + sinf(a) * 25,
                        cx + cosf(a) * 35, cy + sinf(a) * 35, 4, c);
    }
    spinnerStep = (spinnerStep + 1) % 12;
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

constexpr int GAUGE_CX = 77;
constexpr int GAUGE_CY = 94;
constexpr int GAUGE_Y = 20;
constexpr int GAUGE_W = 150;
constexpr int GAUGE_H = 156;
TFT_eSprite gaugeCanvas(&tft);
bool gaugeSpriteReady = false;

struct MetricLayout { int16_t x, y, w, h; const char *label; uint16_t accent; uint8_t size; };
const MetricLayout metrics[5] = {
    {158, 35, 73, 55, "电压/V", C_BLUE, 2},
    {237, 35, 73, 55, "电流/A", C_CYAN, 2},
    {158, 99, 48, 63, "功率/kW", C_GREEN, 2},
    {210, 99, 48, 63, "控温/℃", C_AMBER, 2},
    {262, 99, 48, 63, "电温/℃", C_RED, 2}
};
uint16_t lastMetricColour[5] = {};

// Compose the entire left instrument before pushing it: no needle erase artifacts.
template<typename Canvas>
void renderGauge(Canvas &canvas, int yOffset, const char *speed,
                 const char *gear, int angle, uint16_t colour, bool visible) {
    const int cx = GAUGE_CX, cy = GAUGE_CY - yOffset;
    const int starts[4] = {140, 203, 266, 329};
    const int ends[4] = {198, 261, 324, 400};
    const uint16_t colours[4] = {C_GREEN, C_BLUE, C_AMBER, C_RED};
    for (int zone = 0; zone < 4; ++zone) {
        for (int r = 59; r <= 65; ++r) {
            int oldX = 0, oldY = 0;
            for (int a = starts[zone]; a <= ends[zone]; ++a) {
                const float rad = a * DEG_TO_RAD;
                const int x = cx + cosf(rad) * r, y = cy + sinf(rad) * r;
                if (a > starts[zone]) canvas.drawLine(oldX, oldY, x, y, colours[zone]);
                oldX = x; oldY = y;
            }
        }
    }
    for (int a = 140; a <= 400; a += 10) {
        const float rad = a * DEG_TO_RAD;
        const bool major = (a - 140) % 30 == 0;
        const int inner = major ? 49 : 52;
        canvas.drawLine(cx + cosf(rad) * inner, cy + sinf(rad) * inner,
                        cx + cosf(rad) * 57, cy + sinf(rad) * 57,
                        major ? C_TEXT : C_MUTED);
    }
    const float rad = angle * DEG_TO_RAD;
    const float cs = cosf(rad), sn = sinf(rad);
    const int x0 = cx + cs * 46, y0 = cy + sn * 46;
    const int x1 = cx + cs * 70, y1 = cy + sn * 70;
    for (int width = 3; width >= 1; width -= 2) {
        const int dx = -sn * width, dy = cs * width;
        const uint16_t needle = width == 3 ? C_RED : C_TEXT;
        canvas.fillTriangle(x0 + dx, y0 + dy, x0 - dx, y0 - dy, x1 + dx, y1 + dy, needle);
        canvas.fillTriangle(x0 - dx, y0 - dy, x1 + dx, y1 + dy, x1 - dx, y1 - dy, needle);
    }
    if (visible) textOn(canvas, speed, 77, 81 - yOffset, colour, 3, MC_DATUM);
    textOn(canvas, "km/h", 77, 107 - yOffset, C_MUTED, 1, MC_DATUM);
    textOn(canvas, "档位", 51, 150 - yOffset, C_MUTED, 1, MC_DATUM);
    textOn(canvas, gear, 91, 150 - yOffset, C_TEXT, strlen(gear) > 1 ? 3 : 4, MC_DATUM);
}

void drawGauge(const char *speed, const char *gear, int angle, uint16_t colour, bool visible) {
    if (gaugeSpriteReady) {
        gaugeCanvas.fillSprite(C_BG);
        renderGauge(gaugeCanvas, GAUGE_Y, speed, gear, angle, colour, visible);
        gaugeCanvas.pushSprite(0, GAUGE_Y);
    } else {
        tft.fillRect(0, GAUGE_Y, GAUGE_W, GAUGE_H, C_BG);
        renderGauge(tft, 0, speed, gear, angle, colour, visible);
    }
}

void metricCard(int index) {
    const auto &m = metrics[index];
    panel(m.x, m.y, m.w, m.h);
    tft.fillRect(m.x + 7, m.y + 4, m.w - 14, 2, m.accent);
    text("--", m.x + m.w / 2, m.y + 26, m.accent, m.size, MC_DATUM);
    text(m.label, m.x + m.w / 2, m.y + m.h - 16, C_MUTED, 1, TC_DATUM);
}

void resetDashCache();
void resetInfoCache();

// Fixed-width redraw helper. Cache strings are always normal NUL-terminated strings.
bool updateValue(char *cache, size_t cacheSize, const char *value,
                 int16_t x, int16_t y, int16_t w, int16_t h,
                 uint16_t colour, uint8_t size, uint8_t datum = TR_DATUM,
                 uint16_t background = C_PANEL) {
    if (strncmp(cache, value, cacheSize) == 0) return false;
    strlcpy(cache, value, cacheSize);
    tft.fillRect(x, y, w, h, background);
    int16_t tx = datum == MC_DATUM ? x + w / 2 : x + w - 4;
    int16_t ty = (datum == MC_DATUM || datum == MR_DATUM) ? y + h / 2 : y + 2;
    text(value, tx, ty, colour, size, datum);
    return true;
}

// 中文标签高于旧英文字体，数值背景刷新后必须最后重绘标签。
void updateMetric(int index, char *cache, size_t cacheSize,
                  const char *value, uint16_t colour) {
    const auto &m = metrics[index];
    const uint8_t size = strlen(value) * 6 * m.size <= m.w - 8 ? m.size : 1;
    if (lastMetricColour[index] != colour) { cache[0] = '\0'; lastMetricColour[index] = colour; }
    if (updateValue(cache, cacheSize, value, m.x + 4, m.y + 14, m.w - 8, 24,
                    colour, size, MC_DATUM)) {
        // Labels are the final layer, even after a numeric background update.
        text(m.label, m.x + m.w / 2, m.y + m.h - 16, C_MUTED, 1, TC_DATUM);
    }
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

char infoCache[15][24] = {};

void resetInfoCache() {
    for (auto &item : infoCache) item[0] = '\0';
}

struct InfoLayout { int16_t x, y; const char *label; };
const InfoLayout infoLayout[9] = {
    {8, 48, "转速"}, {111, 48, "速度"}, {214, 48, "功率"},
    {8, 101, "电压"}, {111, 101, "电流"}, {8, 154, "电机温度"},
    {111, 154, "控制器温度"}, {214, 101, "档位"}, {214, 154, "额定参数"}
};
void infoRow(int slot) {
    const auto &r = infoLayout[slot];
    panel(r.x, r.y, 98, 47);
    text(r.label, r.x + 7, r.y + 5, C_MUTED, 1);
    text("--", r.x + 91, r.y + 33, C_TEXT, 1, MR_DATUM);
}
void putInfo(int slot, const char *value, uint16_t colour = C_TEXT) {
    if (slot == 9) {
        updateValue(infoCache[slot], sizeof(infoCache[slot]), value,
                    134, 211, 172, 18, colour, 1, MR_DATUM);
        return;
    }
    const auto &r = infoLayout[slot];
    updateValue(infoCache[slot], sizeof(infoCache[slot]), value,
                r.x + 5, r.y + 22, 88, 21, colour, slot == 7 ? 2 : 1, MR_DATUM);
}

int hitTarget(int16_t x, int16_t y) {
    switch (page) {
        case PAGE_SCAN: return contains(BTN_SCAN_CANCEL, x, y) ? 1 : 0;
        case PAGE_LIST:
            if (contains(BTN_RESCAN, x, y)) return 1;
            if (contains(BTN_CONNECT, x, y)) return 2;
            for (int i = 0; i < deviceCount; ++i)
                if (x >= 8 && x < 312 && y >= 46 + i * 29 && y < 71 + i * 29) return 10 + i;
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
    gaugeCanvas.setColorDepth(16);
    gaugeSpriteReady = gaugeCanvas.createSprite(GAUGE_W, GAUGE_H) != nullptr;
    Serial.println(gaugeSpriteReady ? "[UI] landscape 320x240, gauge sprite ready" : "[UI] landscape 320x240, direct-render fallback");
    page = PAGE_SCAN;
}

void uiShowScan(void) {
    lockUi(); page = PAGE_SCAN; tft.startWrite();
    tft.fillScreen(C_BG);
    header("正在搜索");
    text("蓝牙 搜索中", 312, 7, C_CYAN, 1, TR_DATUM);
    panel(8, 48, 304, 141);
    text("正在查找控制器", 206, 103, C_TEXT, 1, MC_DATUM);
    text("扫描约需五秒", 206, 127, C_MUTED, 1, MC_DATUM);
    text("请保持控制器通电", 206, 148, C_MUTED, 1, MC_DATUM);
    drawButton(BTN_SCAN_CANCEL);
    lastSpinnerMs = 0; spinnerStep = 0;
    tft.endWrite(); unlockUi();
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
    if (!name) return;
    lockUi();
    if (deviceCount >= MAX_DEVICES) { unlockUi(); return; }
    strlcpy(deviceName[deviceCount], name, sizeof(deviceName[deviceCount]));
    deviceRssi[deviceCount] = rssi;
    ++deviceCount;
    if (selectedDevice < 0) selectedDevice = 0;
    if (page == PAGE_LIST) { tft.startWrite(); drawListPage(); tft.endWrite(); }
    unlockUi();
}

void uiShowDash(void) {
    lockUi(); tft.startWrite(); page = PAGE_DASH;
    tft.fillScreen(C_BG);
    const char *btStatus = "已连接";
    text(btStatus, SCREEN_W - 8, 7, C_CYAN, 1, TR_DATUM);
    text("蓝牙", SCREEN_W - 8 - chineseTextWidth(btStatus, 1) - 6,
         7, C_BLUE, 1, TR_DATUM);
    lastGaugeAngle = 140; smoothSpeed = 0.0f;
    currentSpeedColour = C_GREEN; lastSpeedVisible = true;
    drawGauge("--", "-", 140, C_TEXT, true);
    for (int i = 0; i < 5; ++i) metricCard(i);
    panel(4, 180, 152, 25);
    text("电量", 12, 187, C_MUTED, 1);
    tft.fillRoundRect(40, 190, 73, 6, 3, C_LINE);
    text("--%", 149, 193, C_TEXT, 1, MR_DATUM);
    panel(164, 180, 152, 25);
    text("油门", 172, 187, C_MUTED, 1);
    tft.fillRoundRect(200, 190, 73, 6, 3, C_LINE);
    text("--%", 309, 193, C_TEXT, 1, MR_DATUM);
    drawButton(BTN_DISCONNECT); drawButton(BTN_DETAILS);
    resetDashCache();
    tft.endWrite(); unlockUi();
}

void uiDashUpdate(const DashData *d) {
    if (!d) return;
    lockUi();
    if (page != PAGE_DASH) { unlockUi(); return; }
    tft.startWrite();
    char value[16], gear[8];
    const float targetSpeed = constrain(d->speed, 0.0f, SPEED_DISPLAY_MAX_KMH);
    smoothSpeed += (targetSpeed - smoothSpeed) * SMOOTH_SPEED_FACTOR;
    if (fabsf(targetSpeed - smoothSpeed) < 0.05f) smoothSpeed = targetSpeed;
    snprintf(value, sizeof(value), "%.0f", smoothSpeed);
    snprintf(gear, sizeof(gear), "%u", d->gear);
    const int gaugeAngle = 140 + (int)(constrain(smoothSpeed, 0.0f, GAUGE_MAX_SPEED_KMH)
                                      / GAUGE_MAX_SPEED_KMH * 260.0f);
    const uint16_t nextColour = blendColour565(currentSpeedColour, gaugeZoneColour(gaugeAngle),
                                              SPEED_COLOUR_BLEND_FACTOR);
    const bool visible = d->speed <= SPEED_BLINK_THRESHOLD_KMH ||
                         ((millis() / SPEED_BLINK_INTERVAL_MS) & 1U) == 0;
    if (gaugeAngle != lastGaugeAngle || strcmp(speedCache, value) || strcmp(gearCache, gear) ||
        nextColour != currentSpeedColour || visible != lastSpeedVisible) {
        strlcpy(speedCache, value, sizeof(speedCache));
        strlcpy(gearCache, gear, sizeof(gearCache));
        currentSpeedColour = nextColour; lastSpeedVisible = visible; lastGaugeAngle = gaugeAngle;
        drawGauge(value, gear, gaugeAngle, nextColour, visible);
    }
    snprintf(value, sizeof(value), "%.1f", d->volt);
    updateMetric(0, voltCache, sizeof(voltCache), value, C_BLUE);
    snprintf(value, sizeof(value), "%.1f", d->curr);
    updateMetric(1, currCache, sizeof(currCache), value, C_CYAN);
    snprintf(value, sizeof(value), "%.2f", d->power);
    updateMetric(2, powerCache, sizeof(powerCache), value, d->power < 0 ? C_GREEN : C_AMBER);
    snprintf(value, sizeof(value), "%d", d->ctr);
    updateMetric(3, ctrCache, sizeof(ctrCache), value, d->ctr >= 90 ? C_RED : C_TEXT);
    snprintf(value, sizeof(value), "%d", d->mot);
    updateMetric(4, motCache, sizeof(motCache), value, d->mot >= 90 ? C_RED : C_TEXT);
    const uint8_t battery = min((int)d->bat, 100), throttle = min((int)d->thr, 100);
    snprintf(value, sizeof(value), "%u%%", battery);
    updateValue(batCache, sizeof(batCache), value, 117, 185, 35, 16,
                battery < 20 ? C_RED : C_GREEN, 1, MR_DATUM);
    if (lastBattery != battery) {
        lastBattery = battery;
        tft.fillRoundRect(40, 190, 73, 6, 3, C_LINE);
        if (battery) tft.fillRoundRect(40, 190, 73 * battery / 100, 6, 3, battery < 20 ? C_RED : C_GREEN);
    }
    snprintf(value, sizeof(value), "%u%%", throttle);
    updateValue(thrCache, sizeof(thrCache), value, 277, 185, 35, 16, C_TEXT, 1, MR_DATUM);
    if (lastThrottle != throttle) {
        lastThrottle = throttle;
        tft.fillRoundRect(200, 190, 73, 6, 3, C_LINE);
        if (throttle) tft.fillRoundRect(200, 190, 73 * throttle / 100, 6, 3, C_AMBER);
    }
    tft.endWrite(); unlockUi();
}

void uiShowInfo(void) {
    lockUi(); tft.startWrite(); page = PAGE_INFO;
    tft.fillScreen(C_BG);
    header("详细信息"); drawButton(BTN_BACK);
    for (int i = 0; i < 9; ++i) infoRow(i);
    panel(8, 208, 304, 24);
    text("累计里程", 16, 214, C_MUTED, 1);
    text("-- km", 302, 220, C_TEXT, 1, MR_DATUM);
    resetInfoCache();
    tft.endWrite(); unlockUi();
}

void uiInfoUpdate(const LiveInfo *l, const CfgInfo *c) {
    if (!l) return;
    lockUi();
    if (page != PAGE_INFO) { unlockUi(); return; }
    tft.startWrite(); char value[24];
    snprintf(value, sizeof(value), "%.0f rpm", l->rpm); putInfo(0, value, C_CYAN);
    snprintf(value, sizeof(value), "%.1f km/h", l->speed); putInfo(1, value, C_BLUE);
    snprintf(value, sizeof(value), "%+.2f kW", l->power); putInfo(2, value, l->power < 0 ? C_GREEN : C_AMBER);
    snprintf(value, sizeof(value), "%.1f V", l->volt); putInfo(3, value, C_BLUE);
    snprintf(value, sizeof(value), "%.1f A", l->curr); putInfo(4, value, C_CYAN);
    snprintf(value, sizeof(value), "%.0f ℃", l->mot); putInfo(5, value, l->mot >= 90 ? C_RED : C_TEXT);
    snprintf(value, sizeof(value), "%.0f ℃", l->ctr); putInfo(6, value, l->ctr >= 90 ? C_RED : C_TEXT);
    snprintf(value, sizeof(value), "%u", l->gear); putInfo(7, value, C_BLUE);
    if (c) {
        snprintf(value, sizeof(value), "%.0fV %.1fkW", c->ratedV, c->ratedKW); putInfo(8, value);
        snprintf(value, sizeof(value), "%.1f km", c->totalKm); putInfo(9, value, C_GREEN);
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
