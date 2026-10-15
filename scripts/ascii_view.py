#!/usr/bin/env python3
"""把 PNG 渲染成 ASCII —— 无视觉模型时用来"看"屏幕布局/大块文字。

用法: ascii_view.py IMG.png [width] [x0 y0 x1 y1]
"""
import sys
import numpy as np
from PIL import Image

RAMP = " .:-=+*#%@"


def render(path, width=128, crop=None):
    im = Image.open(path).convert("L")
    if crop:
        im = im.crop(crop)
    w, h = im.size
    # 字符宽高比约 1:2，所以行数减半
    height = max(1, int(width * (h / w) * 0.5))
    small = im.resize((width, height), Image.BILINEAR)
    a = np.asarray(small, dtype=np.float32)
    # 自适应对比度拉伸，弱光画面也能看清结构
    lo, hi = np.percentile(a, 2), np.percentile(a, 98)
    if hi - lo < 1:
        hi = lo + 1
    n = np.clip((a - lo) / (hi - lo), 0, 1)
    idx = (n * (len(RAMP) - 1)).astype(np.int32)
    print(f"# {path} crop={crop} src={w}x{h} -> {width}x{height}  (lo={lo:.0f} hi={hi:.0f})")
    print("+" + "-" * width + "+")
    for row in idx:
        print("|" + "".join(RAMP[v] for v in row) + "|")
    print("+" + "-" * width + "+")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 128
    cr = tuple(int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else None
    render(sys.argv[1], w, cr)
