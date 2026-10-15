#!/usr/bin/env python
"""把 `dump 1` 的原始串口流原样存盘，并离线分析完整性（排查传输丢/多字节用）。

用法: python tools/raw_capture.py COM14 out/raw_vga.txt [分辨率]
"""
import base64
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "scripts"))
from cam_cmd import open_port  # noqa: E402

HDR = re.compile(rb"B64-BEGIN seq=(\d+) len=(\d+) crc=([0-9a-fA-F]{8})")
NOT_B64 = re.compile(rb"[^A-Za-z0-9+/=]")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM14"
    out = sys.argv[2] if len(sys.argv) > 2 else "out/raw_capture.txt"
    size = sys.argv[3] if len(sys.argv) > 3 else None

    with open_port(port, 115200) as s:
        if size:
            s.reset_input_buffer()
            s.write(f"size {size}\r\n".encode())
            s.flush()
            time.sleep(2.5)
            while s.read(65536):
                pass
        s.reset_input_buffer()
        s.write(b"dump 1\r\n")
        s.flush()

        buf = bytearray()
        last = time.time()
        while time.time() - last < 8.0:
            chunk = s.read(65536)
            if chunk:
                buf += chunk
                last = time.time()
                if b"B64-END" in bytes(buf):
                    break

    raw = bytes(buf)
    with open(out, "wb") as f:
        f.write(raw)
    print(f"saved {len(raw)} bytes -> {out}")

    m = HDR.search(raw)
    if not m:
        print("!! 没找到 B64-BEGIN，原始前 500 字节：")
        print(raw[:500])
        return 1
    seq, length, crc = int(m.group(1)), int(m.group(2)), m.group(3).decode()
    e = raw.find(b"B64-END", m.end())
    print(f"declared: seq={seq} len={length} crc={crc}  END@{e}")

    region = raw[m.end():e] if e > 0 else raw[m.end():]
    payload = NOT_B64.sub(b"", region)
    print(f"区间字节={len(region)} base64字符={len(payload)} "
          f"期望字符={4 * ((length + 2) // 3)}")

    # 区间里所有“不是纯 base64 行”的内容原样列出（交错日志就是这里露出来的）
    lines = region.splitlines()
    bad = [ln for ln in lines if not re.fullmatch(rb"[A-Za-z0-9+/]{4,}={0,2}", ln)]
    print(f"区间行数={len(lines)} 非纯base64行={len(bad)}")
    for ln in bad[:10]:
        print("   >>", repr(ln[:120]))

    data = base64.b64decode(payload + b"=" * ((4 - len(payload) % 4) % 4))
    got = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
    print(f"解码 {len(data)}B (期望 {length}) crc={got} (期望 {crc.lower()}) "
          f"jpeg={data[:2].hex()}..{data[-2:].hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
