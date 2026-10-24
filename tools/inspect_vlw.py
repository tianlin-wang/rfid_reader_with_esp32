#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
检查一个 .vlw 字库文件 —— 零依赖，不用装 PIL。

算法照抄 LovyanGFX (src/lgfx/v1/lgfx_fonts.cpp) 的 VLWfont::loadFont()，
所以这里算出来的行高/字形数，跟板子上实际会渲染出来的完全一致。

文件格式:
  24 字节头（大端 6 个 u32）：gCount, version, yAdvance(pt), discard, ascent, descent
  之后 gCount 条 28 字节索引（大端 7 个 u32）：
      unicode, height, width, xAdvance, dY(int16), dX(int8), reserved
  再往后每字形 w*h 字节的 8bit alpha 位图

行高 = maxAscent + maxDescent（由所有字形的 dY / height 统计得出，
       不是文件头里那个 yAdvance —— 那个是"磅值"）。

用法:
    python inspect_vlw.py 路径/到/字库.vlw
    python inspect_vlw.py D:/          # 列出 D 盘根目录所有 .vlw 并逐个检查
"""
import os
import struct
import sys

# UI 里会出现的常用字，拿来抽查覆盖情况
SAMPLE = "作业请刷卡登记提交成功失败未名单学生姓名编号读卡器屏幕字库状态人数第次已无卡内写入"

MAGIC = {
    b"\x00\x01\x00\x00": "TrueType (.ttf)",
    b"OTTO": "OpenType (.otf)",
    b"true": "TrueType (.ttf)",
    b"ttcf": "TrueType Collection (.ttc)",
    b"wOFF": "WOFF",
    b"wOF2": "WOFF2",
}


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def be16s(b, off):
    return struct.unpack_from(">h", b, off)[0]


def inspect(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(4)
        f.seek(0)
        header = f.read(24)

    print("=" * 62)
    print("文件 :", path)
    print("大小 : %.1f KB" % (size / 1024.0))

    kind = MAGIC.get(head)
    if kind:
        print("!! 这不是 VLW，是 %s。" % kind)
        print("   改后缀没用（格式不同）。要用 tools/gen_font_cn.py 从 .ttf 生成真 VLW。")
        return False
    if len(header) < 24:
        print("!! 文件不足 24 字节，肯定不是 VLW")
        return False

    gCount = be32(header, 0)
    version = be32(header, 4)
    yAdvPt = be32(header, 8)
    hdr_ascent = be32(header, 16)
    hdr_descent = be32(header, 20)

    print("字形数 : %d    格式版本: %d" % (gCount, version))
    print("文件头: 磅值=%d  ascent=%d  descent=%d" % (yAdvPt, hdr_ascent, hdr_descent))

    if gCount == 0 or gCount > 200000:
        print("!! 字形数不合理，文件头大概率是坏的")
        return False

    with open(path, "rb") as f:
        f.seek(24)
        idx = f.read(gCount * 28)
    if len(idx) < gCount * 28:
        print("!! 索引表被截断（文件不完整 / 拷贝没拷完）")
        return False

    # ---- 照抄 LovyanGFX 的统计逻辑 ----
    max_ascent = abs(hdr_ascent)
    max_descent = abs(hdr_descent)
    need = 24 + gCount * 28
    codes = []
    for i in range(gCount):
        off = i * 28
        unicode = be32(idx, off)
        height = be32(idx, off + 4)
        width = be32(idx, off + 8)
        # LovyanGFX 读法：dY = (int16_t)getSwap32(buf[4]) -> 落在第 18~19 字节
        #                  dX = (int8_t)getSwap32(buf[5])  -> 落在第 23 字节
        dY = struct.unpack_from(">h", idx, off + 18)[0]
        codes.append(unicode)
        need += width * height
        if (unicode > 0xFF) or ((unicode > 0x20) and (unicode < 0xA0) and (unicode != 0x7F)):
            if max_ascent < dY and unicode != 0x3000:
                max_ascent = dY
            if max_descent < (height - dY) and unicode != 0x3000:
                max_descent = height - dY
    line_h = max_ascent + max_descent

    print("实际行高: %d   (ascent %d + descent %d)" % (line_h, max_ascent, max_descent))
    print("理论总长: %d 字节 / 实际 %d 字节  ->  %s"
          % (need, size, "一致" if need == size else "不一致（文件可能损坏或被截断）"))

    asc = sorted(codes)
    ascii_n = sum(1 for c in asc if c < 0x80)
    cjk = sum(1 for c in asc if 0x4E00 <= c <= 0x9FFF)
    other = sum(1 for c in asc if c >= 0x80 and not (0x4E00 <= c <= 0x9FFF))
    print("覆盖  : ASCII %d, 常用汉字 %d, 其它 %d" % (ascii_n, cjk, other))

    if codes != asc:
        print("!! 字形码点不是升序 —— LovyanGFX 用二分查找，会加载失败")

    have = set(codes)
    missing = [c for c in SAMPLE if ord(c) not in have]
    if missing:
        print("!! 缺这些常用字: %s" % "".join(missing))
    else:
        print("常用字抽查: 全部齐全")

    if line_h == 16:
        print("行高 16 —— 和 UI 设计的 16px 完全一致，不缩放，效果最好")
    else:
        print("行高 %d —— 固件会自动按 %.2f 倍缩放到 16px，尺寸对但可能略糊"
              % (line_h, 16.0 / line_h if line_h else 1.0))
    print("=" * 62)
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    target = sys.argv[1]
    if os.path.isdir(target):
        found = [f for f in sorted(os.listdir(target)) if f.lower().endswith(".vlw")]
        if not found:
            print("%s 目录下没有 .vlw 文件。现有内容：" % target)
            for f in sorted(os.listdir(target))[:40]:
                p = os.path.join(target, f)
                print("   %-30s %s" % (f, "<目录>" if os.path.isdir(p)
                                       else "%.1f KB" % (os.path.getsize(p) / 1024.0)))
            return 1
        for f in found:
            inspect(os.path.join(target, f))
        return 0

    if not os.path.isfile(target):
        print("找不到:", target)
        return 1
    return 0 if inspect(target) else 1


if __name__ == "__main__":
    sys.exit(main())
