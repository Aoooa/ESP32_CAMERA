#!/usr/bin/env python3
"""cam_cmd.py —— 往 Camera_test_tool 的调试 console 发**单条命令**并回收输出。

用法：
  python scripts/cam_cmd.py --port COM14 "stat"
  python scripts/cam_cmd.py --port COM14 --wait 8 "shot 5"
  python scripts/cam_cmd.py --port COM14 "burst 30"

要点（与 Embedded_debug_tool/scripts/con_cmd.py 一致）：
  * **不做 DTR/RTS 翻转**，否则每次连接都会把板子复位。
  * 只读固定时长后返回，不做交互。
"""
import argparse
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.1
    ser.dsrdtr = False
    ser.rtscts = False
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def send_command(port: str, baud: int, cmd: str, wait: float) -> bytes:
    """发一条命令，返回硬截止/静默之前的全部输出。"""
    with open_port(port, baud) as ser:
        ser.reset_input_buffer()
        if cmd:
            ser.write((cmd + "\r\n").encode())
            ser.flush()
        start = time.time()
        hard = start + wait
        idle = start + min(wait, 0.6)
        buf = bytearray()
        while time.time() < hard and time.time() < idle:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                idle = min(hard, time.time() + 0.4)
        return bytes(buf)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wait", type=float, default=3.0, help="发送后最多读取秒数")
    ap.add_argument("--log", help="把原始输出按字节写进该文件（UTF-8，不经 PowerShell 改写）")
    ap.add_argument("command", nargs="*", default=["help"])
    args = ap.parse_args()

    out = send_command(args.port, args.baud, " ".join(args.command), args.wait)
    if args.log:
        with open(args.log, "wb") as f:
            f.write(out)
    sys.stdout.write(out.decode("utf-8", errors="replace"))
    if not out:
        sys.stderr.write("(no echo: COM 口假死？先 esptool 硬复位再测)\n")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
