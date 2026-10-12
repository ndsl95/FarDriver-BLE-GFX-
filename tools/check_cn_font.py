"""烧录前自检: 反解 ui_font_cn.h 并渲染预览图, 确认字库格式与字形数据可用。

为什么需要它: vlw 里所有 int32 都是**大端**(TFT_eSPI 的 readInt32() 先读最高字节),
一旦写成小端, 字形数会被解析成 0 —— 字库看似加载成功, 但一个汉字都画不出来,
而且不会报错、不会崩溃, 很难现场定位。本脚本把这类问题在烧录前拦下来。

用法:
    python tools/check_cn_font.py
输出:
    校验结果 + 预览图 font_preview.png (仅用于自检, 不参与编译)
"""
import os
import re
import struct
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "ui_font_cn.h")
# 预览图写到系统临时目录, 避免污染工程
OUT = os.path.join(tempfile.gettempdir(), "ui_font_preview.png")

# 抽查这些文案, 缺字会直接报错
SAMPLES = [("uiFontCN12", 12, "电压 电流 功率 档位 油门 电量 已连接"),
           ("uiFontCN16", 16, "搜索中 系统信息 断开连接 重新扫描 返回 84.6 V km/h")]


def parse_array(name, text):
    m = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("数组 %s 不存在" % name)
    return bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def load(blob, name):
    if len(blob) < 24:
        raise SystemExit("%s 长度不足" % name)
    gcount, ver, pts, _z, ascent, descent = struct.unpack_from(">6I", blob, 0)
    assert ver == 11, "%s: vlw 版本号应为 11, 实际 %d" % (name, ver)
    assert 0 < gcount < 2000, ("%s: 字形数异常(%d) —— 极可能是字节序写反了"
                               " (vlw 必须大端)" % (name, gcount))
    table, p = [], 24
    for _ in range(gcount):
        code, h, w, adv, gdy, gdx, _ = struct.unpack_from(">7i", blob, p)
        p += 28
        table.append((code, h, w, adv, gdy, gdx))
    bmp = p
    glyphs = {}
    for code, h, w, adv, gdy, gdx in table:
        glyphs[code] = (h, w, adv, gdy, gdx, blob[bmp:bmp + h * w])
        bmp += h * w
    assert bmp <= len(blob), "%s: 位图数据超出数组长度" % name
    return ascent, descent, glyphs, gcount


def render(font_name, sample, glyphs, ascent, descent, fg=(255, 255, 255)):
    img = Image.new("RGB", (440, ascent + descent + 4), (0, 0, 0))
    px = img.load()
    x = 2
    for ch in sample:
        code = ord(ch)
        if code == 0x20:
            x += (ascent + descent) // 4
            continue
        assert code in glyphs, "%s 缺少字形 %r (U+%04X)" % (font_name, ch, code)
        h, w, adv, gdy, gdx, data = glyphs[code]
        top = 2 + ascent - gdy          # 与 TFT_eSPI 一致: cursor_y + maxAscent - gdY
        for row in range(h):
            for col in range(w):
                a = data[row * w + col]
                if a:
                    px[x + gdx + col, top + row] = (fg[0] * a // 255,
                                                    fg[1] * a // 255,
                                                    fg[2] * a // 255)
        x += adv
    return img.crop((0, 0, min(440, max(x + 4, 1)), ascent + descent + 4))


def main():
    text = open(SRC, encoding="utf-8").read()
    tiles = []
    for name, size, sample in SAMPLES:
        ascent, descent, glyphs, gcount = load(parse_array(name, text), name)
        tiles.append(render(name, sample, glyphs, ascent, descent))
        print("%s (声明 %dpx): %d 字形, ascent=%d descent=%d, 抽查文案渲染通过"
              % (name, size, gcount, ascent, descent))
    out = Image.new("RGB", (max(t.width for t in tiles),
                            sum(t.height for t in tiles) + 6), (0, 0, 0))
    y = 0
    for t in tiles:
        out.paste(t, (0, y))
        y += t.height + 6
    out.resize((out.width * 3, out.height * 3), Image.NEAREST).save(OUT)
    print("预览图:", OUT)


if __name__ == "__main__":
    main()
