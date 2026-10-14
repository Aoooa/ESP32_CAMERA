#!/usr/bin/env python3
"""cam_dump.py —— 让固件抓 JPEG 帧并通过 console 口 base64 外发，PC 侧还原成 .jpg。

用法：
  python scripts/cam_dump.py --port COM14 -o out/cam.jpg
  python scripts/cam_dump.py --port COM14 --size hd -o out/hd.jpg
  python scripts/cam_dump.py --port COM14 --bar -o out/bar.jpg      # 传感器彩条
  python scripts/cam_dump.py --port COM14 --frames 3 --outdir out

一次串口会话内完成（不重复开关端口，避免丢掉标记行）：
可选先发 size/quality/bar，再发 `dump <n>`，然后按下面**顶行标记**切出每帧区间
（标记不能带前缀，否则 "cam" 这种合法 base64 字符会混进码流）：

  SET=<调用号> SIZE=<W>x<H> FMT=<pixformat>
  B64-BEGIN seq=<全局帧号> len=<字节数> crc=<CRC32>
  <base64 …>
  B64-END seq=<全局帧号>

区间内只保留 base64 字符再解码，最后用 **长度 + CRC32** 双重校验才落盘。
校验不过自动重抓（`--retries`）：主 console 是 USB-Serial/JTAG，没有流控。
"""
import argparse
import base64
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cam_cmd import open_port  # noqa: E402

HDR = re.compile(rb"B64-BEGIN seq=(\d+) len=(\d+) crc=([0-9a-fA-F]{8})")
SET = re.compile(rb"SET=(\d+) SIZE=(\d+)x(\d+) FMT=(\d+)")
NOT_B64 = re.compile(rb"[^A-Za-z0-9+/=]")


def cmd(ser, text, drain_s=0.5):
    """在已打开的会话里发一条命令并把回显吃掉。"""
    ser.reset_input_buffer()
    ser.write((text + "\r\n").encode())
    ser.flush()
    end = time.time() + drain_s
    while time.time() < end:
        ser.read(65536)


def collect(ser, want, per_frame_wait, idle_quit=5.0):
    """读流直到收够 want 帧或静默/超时。返回 (frames, 残留字节)。

    不按“整行是不是 base64”过滤：一次 read 的边界会把一行码流切成两半，
    半行长度 <4 会被行过滤器整块丢掉（实测 QSXGA 稳定少 6 字节就是这么来的）。
    按 BEGIN/END 标记切区间、区间内只留 base64 字符，天然抗分包抗断行。
    """
    buf = bytearray()
    frames = []
    hard = time.time() + per_frame_wait * max(want, 1)
    last_rx = time.time()

    while len(frames) < want and time.time() < hard:
        chunk = ser.read(65536)
        if chunk:
            buf += chunk
            last_rx = time.time()
        elif time.time() - last_rx > idle_quit:
            break

        m = HDR.search(bytes(buf))
        if not m:
            if len(buf) > 64:          # 防标记本身被分包切断，只留尾巴
                del buf[:-64]
            continue
        e = bytes(buf).find(b"B64-END", m.end())
        if e < 0:
            continue                   # 这一帧还没收完，继续读
        # 只取到 END 标记所在行**之前**：END 行自己带 "[cam] " 前缀，
        # 而 "cam" 三个字母是合法 base64 字符，直接切到 e 会每帧多出 3 个字符
        # （长度校验就会差 2 字节 —— 实测踩过）。
        cut = bytes(buf).rfind(b"\n", m.end(), e)
        if cut < 0:
            cut = e
        payload = NOT_B64.sub(b"", bytes(buf[m.end():cut]))
        # BEGIN 前面最近的一条 SET 行给出该帧的分辨率/格式（固件每帧都打）
        sm = None
        for x in SET.finditer(bytes(buf[:m.start()])):
            sm = x
        wh = f"{sm.group(2).decode()}x{sm.group(3).decode()}" if sm else "unknown"
        frames.append((int(m.group(1)), int(m.group(2)),
                       m.group(3).decode("ascii"), payload, wh))
        del buf[:e]

    return frames, bytes(buf)


def save_frame(seq, payload, crc_hex, expect_len, path):
    exp_chars = 4 * ((expect_len + 2) // 3)
    if len(payload) > exp_chars:
        # 区间里混进了别的 base64 合法字符（交错日志/标记前缀），按声明长度截断；
        # 截断后 CRC 仍要一致，否则一样判失败重抓。
        print(f"[warn] seq={seq} 多出 {len(payload) - exp_chars} 个字符，已按声明长度截断")
        payload = payload[:exp_chars]
    data = base64.b64decode(payload + b"=" * ((4 - len(payload) % 4) % 4))
    got_crc = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
    if len(data) != expect_len:
        print(f"[FAIL] seq={seq} 长度不符: {len(data)} != {expect_len} "
              f"(base64 收到 {len(payload)}/{exp_chars} 字符)")
        return False
    if got_crc != crc_hex.lower():
        print(f"[FAIL] seq={seq} CRC 不符: {got_crc} != {crc_hex.lower()}")
        return False
    with open(path, "wb") as f:
        f.write(data)
    print(f"[ OK ] seq={seq} -> {path}  {len(data)}B  "
          f"jpeg={data[:2].hex()}..{data[-2:].hex()} (期望 ffd8..ffd9)")
    return data[:2].hex() == "ffd8" and data[-2:].hex() == "ffd9"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--size", help="抓图前先设置分辨率，如 qvga/vga/hd/uxga/qsxga")
    ap.add_argument("--quality", type=int, help="0-63，越小越清晰")
    ap.add_argument("--bar", action="store_true", help="先打开传感器彩条")
    ap.add_argument("--settle", type=float, default=1.5,
                    help="改参数后等自动曝光/白平衡稳定的秒数")
    ap.add_argument("-o", "--out", help="单帧输出文件名")
    ap.add_argument("--outdir", default="out", help="多帧输出目录（默认 out/）")
    ap.add_argument("--frame-wait", type=float, default=120.0,
                    help="每帧最多等多少秒（base64 走 console 口，大图慢）")
    ap.add_argument("--retries", type=int, default=3,
                    help="校验失败时重抓几次")
    args = ap.parse_args()

    targets = None
    if args.frames == 1 and args.out:
        targets = [args.out]
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    else:
        os.makedirs(args.outdir, exist_ok=True)

    with open_port(args.port, args.baud) as ser:
        if args.size:
            cmd(ser, f"size {args.size}", 2.5)
            time.sleep(args.settle)
        if args.quality is not None:
            cmd(ser, f"quality {args.quality}", 1.5)
            time.sleep(args.settle)
        if args.bar:
            cmd(ser, "bar on", 1.5)
            time.sleep(args.settle)

        for attempt in range(1, max(1, args.retries) + 1):
            ser.reset_input_buffer()
            ser.write(f"dump {args.frames}\r\n".encode())
            ser.flush()
            frames, _ = collect(ser, args.frames, args.frame_wait)

            if not frames:
                print(f"[尝试 {attempt}] 没收到任何帧（摄像头没出图？console 被别的程序占着？）")
                continue

            ok = 0
            for k, (seq, length, crc, payload, wh) in enumerate(frames):
                path = (targets[k] if targets
                        else os.path.join(args.outdir, f"cam_{wh}_{seq:04d}.jpg"))
                if save_frame(seq, payload, crc, length, path):
                    ok += 1
                else:
                    print(f"        该帧声明 {length}B / {wh}")
            if ok == len(frames):
                print(f"收到 {len(frames)} 帧，{ok} 帧校验+JPEG 头尾都通过（尝试 {attempt}）")
                return 0
            print(f"[尝试 {attempt}] {len(frames)} 帧里只有 {ok} 帧通过校验，重抓")

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
