#!/usr/bin/env python
"""检查 cam_dump 收回来的 JPEG 是否真的是有效图像（结构 + 像素统计）。

用法: python tools/check_jpeg.py out/cam_hd.jpg [out/cam_vga.jpg ...]
"""
import sys
from PIL import Image, ImageStat, ImageFilter


def describe(path):
    im = Image.open(path)
    im.load()                      # 真正解码，坏数据在这里抛异常
    rgb = im.convert("RGB")
    st = ImageStat.Stat(rgb)
    # 转灰度后测边缘能量：全黑/纯色/随机噪声的特征不同
    g = rgb.convert("L")
    edges = g.filter(ImageFilter.FIND_EDGES)
    e_stat = ImageStat.Stat(edges)
    colors = rgb.getcolors(maxcolors=1 << 24)
    uniq = len(colors) if colors else -1

    # 中间一行的主色：开 `bar on`（传感器彩条）时应看到 8 个左右大色块，
    # 且红(255,0,0)/绿(0,255,0)/蓝(0,0,255) 不串位 —— 用来判定 DVP 数据通路正确
    w, h = rgb.size
    from collections import Counter
    row = Counter(rgb.getpixel((x, h // 2)) for x in range(0, w, 4))
    top = ", ".join(f"{c}x{n}" for c, n in row.most_common(4))

    return (f"{path}: {im.format} {im.size} mode={im.mode} "
            f"mean=({st.mean[0]:.1f},{st.mean[1]:.1f},{st.mean[2]:.1f}) "
            f"stddev=({st.stddev[0]:.1f},{st.stddev[1]:.1f},{st.stddev[2]:.1f}) "
            f"unique_colors={uniq} edge_mean={e_stat.mean[0]:.2f} "
            f"row_top_colors=[{top}]")


def main(argv):
    rc = 0
    for p in argv[1:]:
        try:
            print(describe(p))
        except Exception as e:                      # noqa: BLE001
            print(f"{p}: 解码失败 {type(e).__name__}: {e}")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
