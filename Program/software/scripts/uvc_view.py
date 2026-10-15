#!/usr/bin/env python3
"""uvc_view.py —— 实时显示 Camera_test_tool 经 USB UVC 送出的画面。

设备侧固件把 OV5640 的 MJPEG 流当**标准 USB 摄像头（UVC）**枚举，所以这里只用
OpenCV 的 VideoCapture，不需要任何私有协议、驱动或串口。

用法：
    python scripts/uvc_view.py                     # 自动挑一个能打开的摄像头
    python scripts/uvc_view.py --list              # 只列出可用索引（不显示窗口）
    python scripts/uvc_view.py --index 1           # 指定索引
    python scripts/uvc_view.py --width 1280 --height 720   # 指定分辨率
按键（窗口获得焦点时）：
    q / ESC   退出        s   存一张 PNG 到当前目录

设备支持的分辨率（sdkconfig.defaults 里配的 4 档）：
    320x240@30 / 480x320@30 / 640x480@30 / 1280x720@15
"""

import argparse
import os
import sys
import time

try:
    import cv2
except ImportError:
    sys.exit("缺少 opencv-python，先安装：pip install opencv-python")

# 后端顺序：本设备实测**只有 MSMF 能出帧**——CAP_DSHOW 在 Win11 上直接
# "NOT OPENED"，CAP_ANY 等价于 MSMF。不要改回 DSHOW 优先。
BACKEND_MSMF = getattr(cv2, "CAP_MSMF", 1400)
BACKEND_ANY = cv2.CAP_ANY


def try_open(index, backend, width, height, fps, warm_read=True, tries=3):
    """打开并读一帧验证；失败返回 None。会重试几次，避开设备刚枚举完还没就绪的瞬间。"""
    for attempt in range(tries):
        cap = cv2.VideoCapture(index, backend)
        if not cap.isOpened():
            cap.release()
            time.sleep(1.5)
            continue

        # 让设备走 MJPEG：不设的话某些后端会要求 YUY2，分辨率就上不去
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        if width:
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        if height:
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        if fps:
            cap.set(cv2.CAP_PROP_FPS, fps)

        if not warm_read:
            return cap

        ok, frame = cap.read()          # 读一帧才算真的通了（也触发设备侧开流）
        if ok and frame is not None:
            return cap
        cap.release()
        time.sleep(1.5)
    return None


def scan(width, height, fps, limit=8):
    """返回 [(index, w, h)]，只含能真正读到帧的索引。"""
    found = []
    for i in range(limit):
        cap = try_open(i, BACKEND_MSMF, width, height, fps, tries=1)
        if cap is None and BACKEND_MSMF != BACKEND_ANY:
            cap = try_open(i, BACKEND_ANY, width, height, fps, tries=1)
        if cap is None:
            continue
        w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        found.append((i, w, h))
        cap.release()
    return found


def main():
    ap = argparse.ArgumentParser(description="显示 ESP32-S3 UVC 摄像头的实时画面")
    ap.add_argument("--index", type=int, default=None, help="摄像头索引；不给则自动选第一个可用的")
    ap.add_argument("--list", action="store_true", help="只列出可用索引后退出")
    ap.add_argument("--width", type=int, default=640, help="请求宽度（默认 640）")
    ap.add_argument("--height", type=int, default=480, help="请求高度（默认 480）")
    ap.add_argument("--fps", type=int, default=30, help="请求帧率（默认 30）")
    ap.add_argument("--no-window", action="store_true", help="不开窗口，只打印实测帧率（连通性自检）")
    ap.add_argument("--seconds", type=float, default=0.0, help="配合 --no-window：测这么久后退出")
    args = ap.parse_args()

    print("扫描摄像头索引…")
    avail = scan(args.width, args.height, args.fps)
    if not avail:
        sys.exit("没找到任何可用摄像头。检查：①板子是否插在 PC 上 ②固件是否已烧 UVC 版 "
                 "③设备管理器里是否出现 'ESP32-S3 OV5640 UVC'（相机类）")
    for i, w, h in avail:
        print(f"  index {i}: {w}x{h}")

    if args.list:
        return

    index = args.index if args.index is not None else avail[0][0]
    if args.index is not None and args.index not in [a[0] for a in avail]:
        sys.exit(f"index {args.index} 打不开；可用：{[a[0] for a in avail]}")

    cap = try_open(index, BACKEND_MSMF, args.width, args.height, args.fps)
    if cap is None:
        cap = try_open(index, BACKEND_ANY, args.width, args.height, args.fps)
    if cap is None:
        sys.exit(f"index {index} 打不开")

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f"已打开 index {index}：{w}x{h}（Ctrl+C 或窗口里按 q/ESC 退出）")

    n, t0, fps_avg = 0, time.time(), 0.0
    t_start = time.time()
    try:
        while True:
            ok, frame = cap.read()
            if not ok or frame is None:
                print("读帧失败（设备掉线？）")
                break

            n += 1
            dt = time.time() - t0
            if dt >= 1.0:
                fps_avg = n / dt
                n, t0 = 0, time.time()

            if args.no_window:
                if args.seconds and time.time() - t_start >= args.seconds:
                    break
                continue

            cv2.putText(frame, f"{fps_avg:5.1f} fps  {frame.shape[1]}x{frame.shape[0]}",
                        (8, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
            cv2.imshow("ESP32-S3 UVC (q/ESC quit, s save)", frame)

            k = cv2.waitKey(1) & 0xFF
            if k in (ord("q"), 27):
                break
            if k == ord("s"):
                name = time.strftime("uvc_%Y%m%d_%H%M%S.png")
                cv2.imwrite(name, frame)
                print(f"已保存 {os.path.abspath(name)}")
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        if not args.no_window:
            cv2.destroyAllWindows()

    if args.no_window and fps_avg:
        print(f"实测平均帧率 {fps_avg:.1f} fps @ {w}x{h}")


if __name__ == "__main__":
    main()
