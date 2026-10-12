"""生成 TFT_eSPI 中文字库 (ui_font_cn.h)。

背景: TFT_eSPI 内置字体只有 ASCII, 界面上要显示汉字必须自备字库。
本项目用它的 "平滑字体"(vlw) —— 这是 TFT_eSPI 里唯一天然支持 UTF-8 的字体类型,
并且支持把字库以 C 数组形式直接编进固件 (tft.loadFont(array)), 不需要文件系统。

用法:
    pip install fonttools pillow
    python tools/gen_cn_font.py            # 覆盖生成 ../ui_font_cn.h

要改字库: 改下面的 STRINGS(出现的汉字) 或 SIZES(字号) 后重新运行即可。
vlw 结构见 TFT_eSPI/Extensions/Smooth_font.cpp 的注释:
    头 24B  = 6 x int32: 字形数, 版本(11), 字号, 保留, ascent, descent
    每字形 28B = 7 x int32: unicode, 高, 宽, xAdvance, dY, dX, 保留
    随后 = 各字形位图(每像素 1B alpha, 按字形顺序紧密排列)
    末尾 = 字体名 (TFT_eSPI 从数组加载时不读尾部, 仅按格式惯例保留)

注意: 所有 int32 都是 **大端** —— TFT_eSPI 的 readInt32() 先读最高字节
(vlw 源自 Processing/Java 的 DataOutputStream, 默认就是大端)。写成小端会
导致字形数被解析成 0, 汉字一个都显示不出来。
"""
import io
import os
import struct

from PIL import Image, ImageDraw, ImageFont

# 系统字体: 微软雅黑 (小字号下比黑体清晰)。也可换成 simhei.ttf 等
FONT_PATH = r"C:\Windows\Fonts\msyh.ttc"
OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ui_font_cn.h")

# 界面上会出现的全部中文文案 —— 决定字库包含哪些汉字
STRINGS = [
    # 状态栏
    "就绪", "运动", "蓝牙", "已连接",
    # 卡片 / 行内小标签
    "电压", "电流", "功率", "控温", "机温", "档位", "油门", "电量",
    # 页面标题
    "搜索中", "选择设备", "系统信息",
    # 提示语
    "正在搜索控制器", "大约需要 5 秒", "未找到控制器", "请靠近控制器后重新扫描",
    # 按钮
    "停止扫描", "重新扫描", "连接", "断开连接", "返回",
    # 信息页标签
    "转速", "速度", "电机", "控制器", "额定", "里程",
]

SIZES = [12, 16]     # 12px=卡片/行内小标签, 16px=标题/按钮/状态栏


def build_glyphs(font, ascent, size):
    out = []
    codes = list(range(0x20, 0x7F)) + sorted({ord(c) for s in STRINGS for c in s if ord(c) > 0x7F})
    for code in codes:
        if code == 0x20:                       # 空格由库按 spaceWidth 处理
            out.append((code, 0, 0, round(font.getlength(" ")), 0, 0, b""))
            continue
        pad = size // 2 + 4
        img = Image.new("L", (size * 2 + pad * 2, ascent + size + pad * 2), 0)
        ImageDraw.Draw(img).text((pad, pad + ascent), chr(code), font=font,
                                 fill=255, anchor="ls")
        bb = img.getbbox()
        if bb is None:                         # 该字体缺此字形
            out.append((code, 0, 0, round(font.getlength(chr(code))), 0, 0, b""))
            continue
        out.append((code, bb[2] - bb[0], bb[3] - bb[1], round(font.getlength(chr(code))),
                    ascent - (bb[1] - pad), bb[0] - pad, img.crop(bb).tobytes()))
    return out


def build_vlw(size, name):
    font = ImageFont.truetype(FONT_PATH, size, index=0)
    ascent, descent = font.getmetrics()
    gl = build_glyphs(font, ascent, size)
    # 大端: TFT_eSPI 的 readInt32() 先读最高字节
    data = struct.pack(">6I", len(gl), 11, size, 0, ascent, descent)
    bitmaps = bytearray()
    for code, w, h, adv, gdy, gdx, bmp in gl:
        data += struct.pack(">7i", code, h, w, adv, gdy, gdx, 0)
        bitmaps += bmp
    data += bytes(bitmaps)
    for _ in range(2):                         # 字体名 + PostScript 名
        data += bytes([len(name)]) + name.encode()
    data += bytes([1])                         # 抗锯齿标记
    return data, len(gl), ascent, descent


def main():
    parts = ["// 自动生成, 请勿手改 —— 由 tools/gen_cn_font.py 从系统字体(微软雅黑)生成",
             "// 仅含界面所需的汉字 + ASCII, 供 TFT_eSPI 平滑字体(UTF-8)使用",
             "#pragma once", "#include <stdint.h>", ""]
    report = []
    for size in SIZES:
        blob, count, ascent, descent = build_vlw(size, "uiCN%d" % size)
        parts.append("// uiFontCN%d: %d 字形, ascent=%d descent=%d, %d 字节"
                     % (size, count, ascent, descent, len(blob)))
        buf = io.StringIO()
        buf.write("const uint8_t uiFontCN%d[] = {\n" % size)
        for i in range(0, len(blob), 20):
            buf.write("  " + ",".join("0x%02X" % b for b in blob[i:i + 20]) + ",\n")
        buf.write("};\n")
        parts.append(buf.getvalue().rstrip("\n"))
        parts.append("")
        font = ImageFont.truetype(FONT_PATH, size, index=0)
        for s in STRINGS:
            report.append((size, round(font.getlength(s)), s))

    out_path = os.path.normpath(OUT_PATH)
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(parts) + "\n")

    print("已生成:", out_path)
    print("\n文案像素宽度 (核对是否超出所在面板):")
    for size, w, s in report:
        print("  %2dpx  %3dpx  %s" % (size, w, s))


if __name__ == "__main__":
    main()
