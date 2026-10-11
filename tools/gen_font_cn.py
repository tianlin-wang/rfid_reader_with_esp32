#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 ESP32 项目用的内置 16x16 点阵中文字库 (font_cn.h)

为什么不用 LovyanGFX 的 loadFont() 从 SD 卡读 TTF？
  1. loadFont() 的第二个参数是字体类型枚举，默认是 ft_vlw，传 TTF 路径会失败；
  2. 依赖 SD 卡：卡没插 / 读不出来时，屏幕上所有中文全变空白；
  3. TTF 光栅化要占几十 KB 堆内存。
内置点阵字库是纯查表，0 依赖、0 内存开销，断电能用。

用法:
    python gen_font_cn.py                       # 默认用黑体，输出到 ../font_cn.h
    python gen_font_cn.py --font msyh.ttc --size 16
    python gen_font_cn.py --preview preview.png # 顺带出一张效果图检查观感

依赖: pip install Pillow
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------- 字符集

# 中英混排时 ASCII 只占半宽，观感才正常
ASCII_FIRST, ASCII_LAST = 0x20, 0x7E

# GB2312 一级汉字：0xB0A1 ~ 0xD7F9，共 3755 个，覆盖绝大多数姓名用字
GB_LEVEL1_ROWS = range(0xB0, 0xD8)

# 常用中文标点（GB2312 里零散分布，单独列出来比整行导入省地方）
EXTRA_PUNCT = "。，、；：？！“”‘’（）《》〈〉【】…—·～％＃＃×÷±℃"

# 万一 UI 里出现字库没覆盖的字，用它兜底
FALLBACK = "?"


def gb2312_level1_chars():
    chars = []
    for hi in GB_LEVEL1_ROWS:
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def scan_source_chars(paths):
    """扫描 .ino / .h 里出现过的所有 CJK 字符，保证 UI 文案 100% 被覆盖"""
    found = set()
    for p in paths:
        if not os.path.exists(p):
            continue
        with open(p, "r", encoding="utf-8", errors="ignore") as f:
            for ch in f.read():
                if ord(ch) > 0x2000:
                    found.add(ch)
    return found


def build_charset(extra_paths):
    codes = set(range(ASCII_FIRST, ASCII_LAST + 1))
    for ch in gb2312_level1_chars():
        codes.add(ord(ch))
    for ch in EXTRA_PUNCT:
        codes.add(ord(ch))
    for ch in scan_source_chars(extra_paths):
        codes.add(ord(ch))
    codes.discard(0x20AC)  # 欧元符号，GB2312 里混进来的噪声
    return sorted(codes)


# ---------------------------------------------------------------- 光栅化

BOX_H = 16  # 字高固定 16px，屏幕 240 宽能排 15 个汉字


def render_glyph(font, ch, width, anchor_x, anchor_y, anchor, size_img):
    """把单个字符光栅化进 width x 16 的 1bpp 位图，返回每行的位列表"""
    img = Image.new("L", (width, BOX_H), 0)
    d = ImageDraw.Draw(img)
    d.text((anchor_x, anchor_y), ch, fill=255, font=font, anchor=anchor)
    rows = []
    px = img.load()
    for y in range(BOX_H):
        bits = 0
        for x in range(width):
            if px[x, y] >= 128:
                bits |= 1 << (width - 1 - x)
        rows.append(bits)
    return rows


def pack(rows, width):
    """每行按大端打包：宽 8 -> 1 字节，宽 16 -> 2 字节"""
    out = bytearray()
    per_row = width // 8
    for bits in rows:
        for i in range(per_row - 1, -1, -1):
            out.append((bits >> (i * 8)) & 0xFF)
    return bytes(out)


# ---------------------------------------------------------------- 输出

def emit_header(path, ascii_glyphs, cjk_codes, cjk_glyphs):
    lines = []
    a = lines.append
    a("// 自动生成，请勿手改 —— 重新生成: python tools/gen_font_cn.py")
    a("#pragma once")
    a("#include <Arduino.h>")
    a("")
    a("// 点阵字库：ASCII 半宽 8x16，汉字全宽 16x16，行高 16")
    a("#define FONT_CN_HEIGHT     16")
    a("#define FONT_CN_ASCII_W    8")
    a("#define FONT_CN_CJK_W      16")
    a(f"#define FONT_CN_ASCII_COUNT {len(ascii_glyphs)}")
    a(f"#define FONT_CN_CJK_COUNT   {len(cjk_codes)}")
    a("")
    a("// ASCII 0x20~0x7E，每行 1 字节，高位在左")
    a(f"const uint8_t font_ascii[{len(ascii_glyphs)}][16] PROGMEM = {{")
    for g in ascii_glyphs:
        a("  {" + ", ".join(f"0x{b:02X}" for b in g) + "},")
    a("};")
    a("")
    a("// 汉字 Unicode 码点，升序排列，用于二分查找")
    a(f"const uint16_t font_cjk_codes[{len(cjk_codes)}] PROGMEM = {{")
    for i in range(0, len(cjk_codes), 12):
        a("  " + ", ".join(f"0x{c:04X}" for c in cjk_codes[i:i + 12]) + ",")
    a("};")
    a("")
    a("// 汉字点阵，每个字 32 字节（16 行 x 2 字节），与 font_cjk_codes 一一对应")
    a(f"const uint8_t font_cjk_bitmap[{len(cjk_glyphs)}][32] PROGMEM = {{")
    for g in cjk_glyphs:
        a("  {" + ", ".join(f"0x{b:02X}" for b in g) + "},")
    a("};")
    a("")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def emit_preview(path, samples, cjk_map, ascii_glyphs):
    """拼一张效果图，肉眼确认字形没被裁掉 / 没糊成一团"""
    pad = 8
    line_h = BOX_H + 6
    w = pad * 2 + max(len(s) * 16 for s in samples)
    h = pad * 2 + line_h * len(samples)
    img = Image.new("L", (w, h), 32)
    d = ImageDraw.Draw(img)
    y = pad
    for s in samples:
        x = pad
        for ch in s:
            code = ord(ch)
            if ASCII_FIRST <= code <= ASCII_LAST:
                g = ascii_glyphs[code - ASCII_FIRST]
                width, per = 8, 1
            else:
                g = cjk_map.get(code)
                width, per = 16, 2
            if g is None:
                x += width
                continue
            for r in range(BOX_H):
                row = 0
                for i in range(per):
                    row = (row << 8) | g[r * per + i]
                for c in range(width):
                    if row & (1 << (width - 1 - c)):
                        d.point((x + c, y + r), fill=255)
            x += width
        y += line_h
    img.save(path)


# ---------------------------------------------------------------- main

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.join(here, "..", "font_cn.h")

    p = argparse.ArgumentParser(description="生成内置 16x16 点阵中文字库")
    p.add_argument("--font", default=r"C:\Windows\Fonts\simhei.ttf",
                   help="源字体 (默认黑体 simhei.ttf；雅黑 msyh.ttc 也可以)")
    p.add_argument("--out", default=os.path.normpath(default_out))
    p.add_argument("--preview", default=None, help="额外输出一张效果图 png")
    p.add_argument("--scan", nargs="*", default=None,
                   help="扫描这些源文件里的汉字并强制收录")
    args = p.parse_args()

    if not os.path.exists(args.font):
        sys.exit(f"找不到字体文件: {args.font}")

    if args.scan is None:
        args.scan = [os.path.join(here, "..", "sketch_sep6a.ino")]

    codes = build_charset(args.scan)
    cjk_codes = [c for c in codes if c > 0x7E]
    print(f"字符集: ASCII {ASCII_LAST - ASCII_FIRST + 1} + 汉字/符号 {len(cjk_codes)}"
          f" = 共 {len(codes)} 个")

    try:
        f_cjk = ImageFont.truetype(args.font, 15)
        f_asc = ImageFont.truetype(args.font, 13)
    except Exception as e:
        sys.exit(f"字体加载失败: {e}")

    ascii_glyphs = []
    for c in range(ASCII_FIRST, ASCII_LAST + 1):
        rows = render_glyph(f_asc, chr(c), 8, 0, 13, "ls", 16)
        ascii_glyphs.append(pack(rows, 8))

    cjk_glyphs = []
    for c in cjk_codes:
        rows = render_glyph(f_cjk, chr(c), 16, 8, 8, "mm", 15)
        cjk_glyphs.append(pack(rows, 16))

    cjk_map = dict(zip(cjk_codes, cjk_glyphs))

    emit_header(args.out, ascii_glyphs, cjk_codes, cjk_glyphs)
    size = os.path.getsize(args.out)
    bitmap_kb = (len(ascii_glyphs) * 16 + len(cjk_glyphs) * 32) / 1024
    print(f"已写出 {args.out}")
    print(f"  源码 {size / 1024:.0f} KB，烧进 flash 占 {bitmap_kb:.0f} KB")

    if args.preview:
        samples = [
            "作业 3        12/40",
            "请刷卡登记作业",
            "提交成功！  张三",
            "未交名单  李四 王五",
            "登记模式  A开新 B上翻",
        ]
        emit_preview(args.preview, samples, cjk_map, ascii_glyphs)
        print(f"效果图: {args.preview}")


if __name__ == "__main__":
    main()
