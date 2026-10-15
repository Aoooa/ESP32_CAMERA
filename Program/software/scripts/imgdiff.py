#!/usr/bin/env python3
"""两帧截图差异量化 —— 判断屏幕上是否有"活动画面"。

用途：无法用视觉模型时，用像素统计判定窗口里是实时视频（帧间变化大）
还是静态画面/黑屏/错误提示（帧间变化≈0）。
"""
import sys
import numpy as np
from PIL import Image


def stat(path_a, path_b, region=None):
    a = np.asarray(Image.open(path_a).convert("L"), dtype=np.int16)
    b = np.asarray(Image.open(path_b).convert("L"), dtype=np.int16)
    if region:
        x0, y0, x1, y1 = region
        a = a[y0:y1, x0:x1]
        b = b[y0:y1, x0:x1]
    d = np.abs(a - b)
    changed = int((d > 8).sum())
    total = int(d.size)
    print(f"  region={region or 'full'} size={a.shape[1]}x{a.shape[0]}")
    print(f"    changed>8 : {changed} / {total} = {100.0 * changed / total:.3f}%")
    print(f"    mean|diff|: {float(d.mean()):.3f}")
    # 画面内容本身是否"丰富"（纯色/黑屏 std 很低）
    print(f"    frame std : {float(a.std()):.2f}   unique levels={len(np.unique(a))}")
    return changed / total


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: imgdiff.py A.png B.png [x0 y0 x1 y1]")
        sys.exit(2)
    reg = None
    if len(sys.argv) >= 7:
        reg = tuple(int(v) for v in sys.argv[3:7])
    print("== image diff ==")
    frac = stat(sys.argv[1], sys.argv[2], reg)
    verdict = "MOVING/ACTIVE (video likely playing)" if frac > 0.005 else "STATIC (no live video)"
    print(f"  VERDICT: {verdict}")
