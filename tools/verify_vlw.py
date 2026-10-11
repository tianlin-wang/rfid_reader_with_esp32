#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
离线验证 font.vlw —— 完全照抄 LovyanGFX lgfx_fonts.cpp 里
VLWfont::loadFont() / updateFontMetric() / drawChar() 的算法，
把同样的字符串渲染成 PNG。屏幕上是什么样，这里就该是什么样。

用法:
    python verify_vlw.py ../font.vlw out.png
"""

import struct
import sys

from PIL import Image


def swap32(b):
    return struct.unpack(">I", b)[0]


class VLW:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()

        hdr = struct.unpack(">6I", self.data[:24])
        self.gCount = hdr[0]
        self.yAdvance = hdr[2]
        self.ascent = abs(hdr[4])
        self.descent = abs(hdr[5])

        if self.gCount == 0:
            raise SystemExit("字形数为 0，文件头不对")

        self.maxAscent = self.ascent
        self.maxDescent = self.descent
        self.yAdvance = max(self.yAdvance, self.ascent + self.descent)
        self.spaceWidth = self.yAdvance * 2 // 7

        self.entries = []
        bitmapPtr = 24 + self.gCount * 28
        for i in range(self.gCount):
            off = 24 + i * 28
            f = struct.unpack(">7I", self.data[off:off + 28])
            unicode, height, width, xa, dy, dx = f[0], f[1], f[2], f[3], f[4], f[5]
            w8, xadv8 = width & 0xFF, xa & 0xFF
            dX = dx if dx < 128 else dx - 256
            dY = dy if dy < 32768 else dy - 65536

            if (unicode > 0xFF) or (0x20 < unicode < 0xA0 and unicode != 0x7F):
                if self.maxAscent < dY and unicode != 0x3000:
                    self.maxAscent = dY
                if self.maxDescent < (height - dY) and unicode != 0x3000:
                    self.maxDescent = height - dY

            self.entries.append({
                "code": unicode, "h": height, "w": w8,
                "xa": xadv8, "dy": dY, "dx": dX, "ptr": bitmapPtr,
            })
            bitmapPtr += w8 * height

        self.yAdvance = self.maxAscent + self.maxDescent
        self.height = self.yAdvance
        self.baseline = self.maxAscent

        # getUnicodeIndex 要求升序
        codes = [e["code"] for e in self.entries]
        if codes != sorted(codes):
            raise SystemExit("码点表不是升序，lower_bound 会查错")
        self.codes = codes

    def find(self, code):
        """std::lower_bound 的等价实现"""
        if not self.codes or self.codes[-1] < code:
            return None
        lo, hi = 0, len(self.codes)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.codes[mid] < code:
                lo = mid + 1
            else:
                hi = mid
        return lo if lo < len(self.codes) and self.codes[lo] == code else None

    def draw_char(self, img, x, y, code, fg=255):
        """等价于 VLWfont::drawChar，size=1"""
        if code == 0x20:
            return self.spaceWidth
        i = self.find(code)
        if i is None:
            return self.spaceWidth           # 缺字兜底

        e = self.entries[i]
        w, h = e["w"], e["h"]
        xoffset = e["dx"]
        yoffset = self.maxAscent - e["dy"]
        px = img.load()
        W, H = img.size

        for gy in range(h):
            py = y + yoffset + gy
            if py < 0 or py >= H:
                continue
            base = e["ptr"] + gy * w
            for gx in range(w):
                a = self.data[base + gx]
                if a == 0:
                    continue
                pxx = x + xoffset + gx
                if pxx < 0 or pxx >= W:
                    continue
                if a >= 255:
                    px[pxx, py] = fg
                else:
                    old = px[pxx, py]
                    px[pxx, py] = int(old + (fg - old) * a / 255)
        return e["xa"]

    def draw(self, img, x, y, text, fg=255):
        cx = x
        for ch in text:
            cx += self.draw_char(img, cx, y, ord(ch), fg)
        return cx

    def width(self, text):
        total = 0
        for ch in text:
            if ch == " ":
                total += self.spaceWidth
            else:
                i = self.find(ord(ch))
                total += self.entries[i]["xa"] if i is not None else self.spaceWidth
        return total


def main():
    vlw_path = sys.argv[1] if len(sys.argv) > 1 else "../font.vlw"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "vlw_check.png"

    f = VLW(vlw_path)
    print(f"字形数  : {f.gCount}")
    print(f"ascent  : {f.ascent}  descent: {f.descent}")
    print(f"行高    : {f.yAdvance}  baseline: {f.baseline}")
    print(f"空格宽  : {f.spaceWidth}")

    # 文件长度校验，防止位图截断
    need = 24 + f.gCount * 28 + sum(e["w"] * e["h"] for e in f.entries)
    print(f"文件大小: {len(f.data)}  理论需要: {need}  -> "
          f"{'一致' if need == len(f.data) else '不一致!'}")

    samples = [
        "作业 3       12/40",
        "请刷卡登记作业",
        "提交成功  张三",
        "未交名单 1/2",
        "ABCDEFG abcdefg 0123",
        "缺少：按钮 屏幕 SD卡",
    ]

    scale = 3
    w = 8 + max(f.width(s) for s in samples)
    h = 8 + f.yAdvance * len(samples)
    img = Image.new("L", (w, h), 16)
    y = 4
    for s in samples:
        f.draw(img, 4, y, s)
        y += f.yAdvance
    img.resize((w * scale, h * scale), Image.NEAREST).save(out_path)
    print(f"渲染结果: {out_path}  ({w}x{h} -> {w*scale}x{h*scale})")


if __name__ == "__main__":
    main()
