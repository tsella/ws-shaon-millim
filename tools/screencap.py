#!/usr/bin/env python3
"""
tools/screencap.py — Live Hardware Framebuffer Screen-Capture Tool for ws-millim

Connects to the ESP32-S3 over USB-Serial/JTAG (/dev/cu.usbmodem*) using POSIX
termios (no external pyserial dependency needed), optionally executes a remote
command (SHOW_CLOCK, SHOW_SETTINGS, SPEAK), triggers SCREENCAP, decodes the RLE
RGB565 hardware shadow VRAM stream, and saves a 466x466 PNG.
"""
import argparse
import base64
import glob
import os
import select
import termios
import time
from PIL import Image


def find_port(explicit_port=None):
    if explicit_port:
        return explicit_port
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise RuntimeError("No /dev/cu.usbmodem* port found")
    return ports[0]


class PosixSerial:
    def __init__(self, port: str, baud: int = 115200):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        # Raw 8N1 mode
        attrs[0] = 0  # iflag
        attrs[1] = 0  # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag
        attrs[3] = 0  # lflag
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def drain_input(self, duration=0.15):
        t0 = time.time()
        while time.time() - t0 < duration:
            r, _, _ = select.select([self.fd], [], [], 0.03)
            if r:
                try:
                    os.read(self.fd, 4096)
                except BlockingIOError:
                    pass
        self.buf = b""

    def write(self, data: bytes):
        os.write(self.fd, data)

    def readline(self, timeout=0.25) -> bytes:
        t0 = time.time()
        while time.time() - t0 < timeout:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                return line + b"\n"
            r, _, _ = select.select([self.fd], [], [], 0.04)
            if r:
                try:
                    chunk = os.read(self.fd, 4096)
                    if chunk:
                        self.buf += chunk
                except BlockingIOError:
                    pass
        return b""


def capture_screen(port: str, out_png: str, pre_cmd: str = None, circular_mask: bool = True):
    with PosixSerial(port, 115200) as ser:
        ser.drain_input(0.2)

        if pre_cmd:
            ser.write(f"{pre_cmd}\n".encode("ascii"))
            t0 = time.time()
            while time.time() - t0 < 2.5:
                line = ser.readline(0.2).decode("utf-8", errors="replace").strip()
                if line:
                    print(f"[dev] {line}")
                if "<<<OK" in line:
                    break
            time.sleep(0.3)

        ser.write(b"SCREENCAP\n")

        in_frame = False
        b64_chunks = []
        width, height = 466, 466
        t_start = time.time()

        while time.time() - t_start < 15.0:
            raw = ser.readline(0.25)
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not in_frame:
                if "<<<SCREENCAP_BEGIN" in line:
                    in_frame = True
                    print(f"Receiving {width}x{height} hardware VRAM dump...")
                elif line:
                    print(f"[log] {line}")
            else:
                if line.startswith("B64:"):
                    b64_chunks.append(line[4:])
                elif "<<<SCREENCAP_END>>>" in line:
                    print("Frame received!")
                    break

        if not b64_chunks:
            raise RuntimeError("Timed out waiting for SCREENCAP frame")

        blob = base64.b64decode("".join(b64_chunks))
        img = Image.new("RGB", (width, height), (0, 0, 0))
        px_out = img.load()

        idx = 0
        total = width * height
        pos = 0
        while pos + 2 < len(blob) and idx < total:
            run = blob[pos]
            lo = blob[pos + 1]
            hi = blob[pos + 2]
            pos += 3

            # LV_COLOR_16_SWAP=1 stores big-endian RGB565 in memory (lo = MSB, hi = LSB)
            v565 = (lo << 8) | hi
            r = ((v565 >> 11) & 0x1F) * 255 // 31
            g = ((v565 >> 5) & 0x3F) * 255 // 63
            b = (v565 & 0x1F) * 255 // 31

            for _ in range(run):
                if idx >= total:
                    break
                y, x = divmod(idx, width)
                px_out[x, y] = (r, g, b)
                idx += 1

        if circular_mask:
            cx, cy, r_max = width / 2.0, height / 2.0, width / 2.0
            for y in range(height):
                for x in range(width):
                    if (x - cx) ** 2 + (y - cy) ** 2 > r_max ** 2:
                        px_out[x, y] = (12, 14, 18)

        img.save(out_png)
        print(f"Saved {width}x{height} screenshot to {out_png} ({idx}/{total} pixels decoded)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None, help="Serial port (default: auto-detect /dev/cu.usbmodem*)")
    ap.add_argument("-o", "--out", required=True, help="Output PNG path")
    ap.add_argument("-c", "--cmd", default=None, help="Optional command before capture (SHOW_CLOCK, SHOW_SETTINGS, SPEAK)")
    args = ap.parse_args()
    capture_screen(find_port(args.port), args.out, args.cmd)
