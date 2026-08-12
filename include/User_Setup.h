// PlatformIO: 用项目内自定义配置覆盖 TFT_eSPI 库的默认 User_Setup.h
// 原理: TFT_eSPI 的 User_Setup_Select.h 会执行 #include <User_Setup.h>,
//       项目 include/ 目录在编译搜索路径中优先于库目录, 因此本文件先生效。
// 实际引脚配置见同目录 Setup400_EKSR.h
#include "Setup400_EKSR.h"
