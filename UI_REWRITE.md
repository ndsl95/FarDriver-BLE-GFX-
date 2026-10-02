# 横屏 UI 说明

当前工程使用 `ui_v2.cpp` 的 320×240 横屏中文界面，配置及页面布局见 `LANDSCAPE_UI.md`。
`platformio.ini` 排除旧版 `ui_gfx.cpp`，公开 API 仍使用 `ui_gfx.h`。
显示方向为 rotation 1，触摸坐标通过 `display.cpp` 旋转。
BLE、远驱协议、油门校准及断开后重新扫描的业务流程沿用复制前的工程。
竖屏版保留在相邻的 `FarDriver-BLE-GFX--main` 文件夹，恢复竖屏请编译烧录该原工程。
