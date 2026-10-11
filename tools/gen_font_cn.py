#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 ESP32 智能收作业系统用的中文字库，两种产物：

  A. font.vlw  —— 拷到 SD 卡根目录，由 LovyanGFX 原生加载。
                  8bit 抗锯齿灰度，观感最好；改字号只要重新生成这个文件。
                  这就是「SD 卡里放字体」的可行做法。

  B. font_cn.h —— 16x16 点阵（1bpp），编译进 flash。
                  零依赖，不插卡也能显示中文，作为兜底。

------------------------------------------------------------------
关于 .ttf：为什么不能直接往 SD 卡里丢一个 ttf
------------------------------------------------------------------
LovyanGFX 的 TTF 解码器在官方仓库的**可选文件** lgfx_ttffont.hpp 里，
默认没有打包进 Arduino 库管理器装的发行版。看 lgfx_fonts.cpp 的 load_font()：

    #ifdef LGFX_TTFFONT_HPP_
      if (文件头是 00 01 00 00 或 'ttcf') { new TTFfont(); }
      else
    #endif
    switch (font_type) { default: new VLWfont(); }

没装那个头文件时，**所有字体一律按 VLW 解析**。所以放一个 .ttf 进去，
loadFont() 会返回 false，或者更糟——按 VLW 的 24 字节头去读出垃圾字形数，
屏幕上一片乱码。能原生加载的就是 .vlw。

.vlw 本来就是「用任意 TTF 在电脑上预渲染出来的字体文件」，
拿 SimHei / 微软雅黑 / 你自己下载的开源字体生成都行，效果等于矢量字体。

------------------------------------------------------------------
用法
------------------------------------------------------------------
    python gen_font_cn.py                          # 同时生成 A 和 B
    python gen_font_cn.py --vlw E:\\font.vlw       # 直接写进 SD 卡盘符
    python gen_font_cn.py --size 16                # 指定字号（像素）
    python gen_font_cn.py --font C:\\Windows\\Fonts\\msyh.ttc
    python gen_font_cn.py --preview tools\\preview.png   # 出效果图检查观感

依赖: pip install Pillow
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow:  pip install Pillow")


# ---------------------------------------------------------------- 字符集

ASCII_FIRST, ASCII_LAST = 0x20, 0x7E      # 半宽 ASCII，中英混排才自然
GB_LEVEL1_ROWS = range(0xB0, 0xD8)        # GB2312 一级汉字，3755 个
EXTRA_PUNCT = "。，、；：？！“”‘’（）《》〈〉【】…—·～％＃×÷±℃"


def collect_codes(scan_paths):
    """要收录的码点：ASCII + GB2312 一级汉字 + 补充标点 + 源码里出现过的字"""
    codes = set(range(ASCII_FIRST, ASCII_LAST + 1))

    for hi in GB_LEVEL1_ROWS:
        for lo in range(0xA1, 0xFF):
            try:
                codes.add(ord(bytes([hi, lo]).decode("gb2312")))
            except UnicodeDecodeError:
                continue

    for ch in EXTRA_PUNCT:
        codes.add(ord(ch))

    # 扫描 UI 源码，把文案里出现过的汉字强制收录 —— 以后改文案也不怕缺字
    for p in scan_paths:
        if not os.path.exists(p):
            continue
        with open(p, "r", encoding="utf-8", errors="ignore") as f:
            for ch in f.read():
                if ord(ch) > 0x2000:
                    codes.add(ord(ch))

    codes.discard(0x20AC)               # GB2312 混进来的欧元符号，用不上
    return sorted(codes)


# ---------------------------------------------------------------- 光栅化

PAD = 48          # 渲染临时画布四周留白，避免负坐标粘贴


def rasterize(font, ch):
    """
    把单个字符渲染成「只含墨迹」的 8bit 灰度图，并算出相对光标/基线的偏移。

    Pillow 的 anchor="ls" 表示「左边、基线」对齐，所以把锚点放在
    (PAD, PAD+ascent) 就能精确知道基线在画布的第几行，不用猜 getbbox 的语义。
    """
    asc, desc = font.getmetrics()
    W = H = 256
    origin_x = PAD
    base_y = PAD + asc

    canvas = Image.new("L", (W, H), 0)
    ImageDraw.Draw(canvas).text((origin_x, base_y), ch, fill=255,
                                font=font, anchor="ls")
    bb = canvas.getbbox()
    if bb is None:                       # 空格等无墨迹字符
        return None

    ink = canvas.crop(bb)
    return {
        "img": ink,
        "w": ink.width,
        "h": ink.height,
        "dx": bb[0] - origin_x,          # 左边缘相对光标的偏移（可为负）
        "dy": base_y - bb[1],            # 基线 -> 墨迹顶端，向上为正（可负）
        "xa": int(round(font.getlength(ch))),
    }


def pick_size(ttf_path, line_h, lo=8, hi=32):
    """
    挑一个像素字号，使得 ascent+descent 恰好不超过 line_h。
    UI 是按 16px 行高排的，字号选大了行会叠在一起，选小了字又太瘦。
    """
    best = lo
    for s in range(lo, hi + 1):
        asc, desc = ImageFont.truetype(ttf_path, s).getmetrics()
        if asc + desc <= line_h:
            best = s
    return best


# ---------------------------------------------------------------- 1) vlw

def emit_vlw(path, codes, ttf_path, size):
    """
    写 VLW 文件。格式与 LovyanGFX 的 VLWfont::loadFont() 严格对应：

      24 字节文件头，6 个大端 u32：
        [0] 字形个数   [1] 版本(忽略)   [2] yAdvance
        [3] 忽略       [4] ascent      [5] descent
      然后 gCount 条 28 字节索引项，每条 7 个大端 u32：
        [0] unicode  [1] 高度  [2] 宽度  [3] xAdvance
        [4] dY(int16，基线到墨迹顶端)  [5] dX(int8，光标到墨迹左边缘)  [6] 保留
      最后紧跟位图数据，每像素 1 字节透明度，每条占 宽x高 字节。
    """
    font = ImageFont.truetype(ttf_path, size)
    asc, desc = font.getmetrics()

    glyphs = []
    for c in codes:
        g = rasterize(font, chr(c))
        glyphs.append((c, g))

    out = bytearray()
    # 文件头里 yAdvance 填什么有讲究：库内部 spaceWidth = yAdvance*2/7，
    # 而文件头 yAdvance 会被 max(header, asc+desc) 兜底。默认只给约 4px 的空格，
    # 中英混排太挤，这里按行高一半反推，让空格接近半宽。
    hdr_yadv = max(asc + desc, (asc + desc) * 7 // 4)
    for v in (len(codes), 0, hdr_yadv, 0, asc, desc):
        out += v.to_bytes(4, "big")

    for c, g in glyphs:
        if g is None:
            vals = (c, 0, 0, int(round(font.getlength(chr(c)))), 0, 0, 0)
        else:
            vals = (c, g["h"], g["w"], g["xa"],
                    g["dy"] & 0xFFFF, g["dx"] & 0xFF, 0)
        for v in vals:
            out += v.to_bytes(4, "big")

    for _c, g in glyphs:
        if g is not None and g["w"] > 0 and g["h"] > 0:
            out += g["img"].tobytes()

    with open(path, "wb") as f:
        f.write(bytes(out))
    return len(out)


# ---------------------------------------------------------------- 2) 点阵

BOX_W, BOX_H = 16, 16
BITMAP_CJK_PX = 15       # 16px 格子里塞 15px 的字，四周留 1px 余量
BITMAP_ASC_PX = 13       # 半宽 8px 格子里塞 13px 的字


def cell_glyph(font, ch, cw):
    """把字符画进 cw x 16 的格子，水平居中、基线统一，返回 1bpp 打包后的字节"""
    g = rasterize(font, ch)
    asc, desc = font.getmetrics()
    baseline = asc + max(0, (BOX_H - (asc + desc)) // 2)

    tmp = Image.new("L", (cw + 2 * PAD, BOX_H + 2 * PAD), 0)
    if g is not None:
        x = PAD + (cw - g["w"]) // 2
        y = PAD + baseline - g["dy"]
        tmp.paste(g["img"], (x, y))
    return pack_msb(tmp.crop((PAD, PAD, PAD + cw, PAD + BOX_H)))


def pack_msb(img):
    """按行打包成 1bpp，高位在左。宽 <=8 -> 1 字节/行，<=16 -> 2 字节/行"""
    px = img.load()
    per = (img.width + 7) // 8
    out = bytearray()
    for y in range(img.height):
        bits = 0
        for x in range(img.width):
            if px[x, y] >= 128:
                bits |= 1 << (img.width - 1 - x)
        for i in range(per - 1, -1, -1):
            out.append((bits >> (i * 8)) & 0xFF)
    return bytes(out)


def rasterize_bitmap(ttf_path, codes):
    f_asc = ImageFont.truetype(ttf_path, BITMAP_ASC_PX)
    f_cjk = ImageFont.truetype(ttf_path, BITMAP_CJK_PX)

    ascii_glyphs = [cell_glyph(f_asc, chr(c), 8)
                    for c in range(ASCII_FIRST, ASCII_LAST + 1)]

    cjk_codes = [c for c in codes if c > ASCII_LAST]
    cjk_glyphs = [cell_glyph(f_cjk, chr(c), BOX_W) for c in cjk_codes]

    return ascii_glyphs, cjk_codes, cjk_glyphs


def emit_bitmap_header(path, ascii_glyphs, cjk_codes, cjk_glyphs):
    lines = [
        "// 自动生成，请勿手改 —— 重新生成: python tools/gen_font_cn.py",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "// 点阵字库：ASCII 半宽 8x16，汉字全宽 16x16，行高 16",
        "#define FONT_CN_HEIGHT     16",
        "#define FONT_CN_ASCII_W    8",
        "#define FONT_CN_CJK_W      16",
        f"#define FONT_CN_ASCII_COUNT {len(ascii_glyphs)}",
        f"#define FONT_CN_CJK_COUNT   {len(cjk_codes)}",
        "",
        "// ASCII 0x20~0x7E，每行 1 字节，高位在左",
        f"const uint8_t font_ascii[{len(ascii_glyphs)}][16] PROGMEM = {{",
    ]
    for g in ascii_glyphs:
        lines.append("  {" + ", ".join(f"0x{b:02X}" for b in g) + "},")
    lines.append("};")
    lines.append("")
    lines.append("// 汉字 Unicode 码点，升序排列，用于二分查找")
    lines.append(f"const uint16_t font_cjk_codes[{len(cjk_codes)}] PROGMEM = {{")
    for i in range(0, len(cjk_codes), 12):
        lines.append("  " + ", ".join(f"0x{c:04X}" for c in cjk_codes[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("// 汉字点阵，每字 32 字节（16 行 x 2 字节），与 font_cjk_codes 一一对应")
    lines.append(f"const uint8_t font_cjk_bitmap[{len(cjk_glyphs)}][32] PROGMEM = {{")
    for g in cjk_glyphs:
        lines.append("  {" + ", ".join(f"0x{b:02X}" for b in g) + "},")
    lines.append("};")
    lines.append("")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


# ---------------------------------------------------------------- 效果图

def emit_preview(path, samples, cjk_map, ascii_glyphs, scale=3):
    """把点阵结果放大画出来，肉眼确认字形没糊、没偏"""
    pad = 6
    line_h = BOX_H + 4
    w = pad * 2 + max(len(s) * BOX_W for s in samples)
    h = pad * 2 + line_h * len(samples)

    img = Image.new("L", (w, h), 24)
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
                width, per = BOX_W, 2
            if g is not None:
                for r in range(BOX_H):
                    row = 0
                    for i in range(per):
                        row = (row << 8) | g[r * per + i]
                    for c in range(width):
                        if row & (1 << (width - 1 - c)):
                            d.point((x + c, y + r), fill=235)
            x += width
        y += line_h

    img = img.resize((w * scale, h * scale), Image.NEAREST)
    img.save(path)


# ---------------------------------------------------------------- main

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, ".."))

    p = argparse.ArgumentParser(description="生成中文字库 (vlw + 点阵)")
    p.add_argument("--font", default=r"C:\Windows\Fonts\simhei.ttf",
                   help="源字体；系统字体或自己的 ttf/ttc/otf 都行")
    p.add_argument("--size", type=int, default=0,
                   help="字号（像素）。0 = 自动挑一个行高不超过 --line-height 的最大字号")
    p.add_argument("--line-height", type=int, default=16,
                   help="自动选字号时的行高上限，配合 UI 的 16px 行距")
    p.add_argument("--vlw", default=os.path.join(root, "font.vlw"),
                   help="vlw 输出路径，可直指 SD 卡盘符，如 E:\\font.vlw")
    p.add_argument("--header", default=os.path.join(root, "font_cn.h"),
                   help="点阵字库输出路径")
    p.add_argument("--preview", default=None, help="效果图 png 输出路径")
    p.add_argument("--scan", default=os.path.join(root, "sketch_sep6a.ino"),
                   help="扫描该源文件里出现的汉字")

    a = p.parse_args()

    if not os.path.exists(a.font):
        sys.exit(f"找不到字体文件: {a.font}")

    codes = collect_codes([a.scan])
    n_ascii = ASCII_LAST - ASCII_FIRST + 1
    print(f"字体源: {a.font}")
    print(f"字符集: ASCII {n_ascii} + 汉字/符号 {len(codes) - n_ascii} = 共 {len(codes)} 个")

    size = a.size if a.size > 0 else pick_size(a.font, a.line_height)
    asc, desc = ImageFont.truetype(a.font, size).getmetrics()
    print(f"字号: {size}px  (ascent={asc} descent={desc} 行高={asc + desc})")

    n = emit_vlw(a.vlw, codes, a.font, size)
    print(f"已写出 {os.path.normpath(a.vlw)}  ({n / 1024:.0f} KB)  -> 拷到 SD 卡根目录")

    ag, cc, cg = rasterize_bitmap(a.font, codes)
    emit_bitmap_header(a.header, ag, cc, cg)
    print(f"已写出 {os.path.normpath(a.header)}  "
          f"(flash 占 {(len(ag) * 16 + len(cg) * 32) / 1024:.0f} KB)")

    if a.preview:
        emit_preview(a.preview, ["作业 3    12/40", "请刷卡登记作业",
                                 "提交成功  张三", "未交名单 1/2"],
                     dict(zip(cc, cg)), ag)
        print(f"效果图: {a.preview}")


if __name__ == "__main__":
    main()
