#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
烧录前的静态自检 —— 只查那些"编译报错但原因极难看出"的坑。

用法:
    python tools/preflight.py [sketch目录]

检查项:
  1. 源码是否带 UTF-8 BOM（会让整个文件的 #define 全部失效）
  2. #include <SD.h> 是否在 #include <LovyanGFX.hpp> 之前
  3. SD 卡上是否存在 font.vlw（可选，用 --sd 指定盘符）
"""
import os
import sys

BOM = b"\xef\xbb\xbf"


def check_bom(root):
    bad = []
    for name in sorted(os.listdir(root)):
        if not os.path.isfile(os.path.join(root, name)):
            continue
        if not name.endswith((".ino", ".h", ".cpp", ".hpp")):
            continue
        with open(os.path.join(root, name), "rb") as f:
            head = f.read(3)
        if head == BOM:
            bad.append(name)
    return bad


def check_include_order(ino_path):
    """返回 (sd_line, lgfx_line)；任一为 None 表示没找到。"""
    sd = lgfx = None
    with open(ino_path, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, 1):
            s = line.strip()
            if s.startswith("#include") and "<SD.h>" in s and sd is None:
                sd = i
            if s.startswith("#include") and "LovyanGFX" in s and lgfx is None:
                lgfx = i
    return sd, lgfx


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))
    ino = os.path.join(root, "sketch_sep6a.ino")

    print("=" * 60)
    print("preflight:", root)
    print("=" * 60)
    ok = True

    # 1. BOM
    bad = check_bom(root)
    if bad:
        ok = False
        print("[FAIL] 以下文件带 UTF-8 BOM，编译会报 stray '#'：")
        for n in bad:
            print("       -", n)
        print("       修复：用编辑器另存为 'UTF-8 (无 BOM)'")
    else:
        print("[ OK ] 无 UTF-8 BOM")

    # 2. include 顺序
    if not os.path.isfile(ino):
        print("[SKIP] 没找到", ino)
    else:
        sd, lgfx = check_include_order(ino)
        if sd is None or lgfx is None:
            print("[SKIP] SD.h / LovyanGFX.hpp 未同时出现 "
                  "(SD=%s, LGFX=%s)" % (sd, lgfx))
        elif sd < lgfx:
            print("[ OK ] include 顺序正确：SD.h(%d) 在 LovyanGFX.hpp(%d) 之前"
                  % (sd, lgfx))
        else:
            ok = False
            print("[FAIL] include 顺序反了：LovyanGFX.hpp 在 %d 行，SD.h 在 %d 行"
                  % (lgfx, sd))
            print("       loadFont(SD, path) 会报 "
                  "'invalid new-expression of abstract class type'")

    # 3. font.vlw
    sd_drive = None
    for a in sys.argv[2:]:
        if a.startswith("--sd="):
            sd_drive = a[5:]
    if sd_drive:
        p = os.path.join(sd_drive, "font.vlw")
        if os.path.isfile(p):
            print("[ OK ] SD 卡有 font.vlw (%.0f KB)"
                  % (os.path.getsize(p) / 1024.0))
        else:
            print("[FAIL] SD 卡根目录没有 font.vlw，会退回内置点阵字库")

    print("=" * 60)
    print("结论:", "可以编译" if ok else "有问题，先修上面 FAIL 项")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
