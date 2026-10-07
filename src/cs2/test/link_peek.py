"""Reads the engine's CS2 link (Local\\Skate3CS2Link) like cs2skate.dll does: prints the newest frame's camera / feet in
CS2 units and saves its pixels (over a checkerboard, to see the transparency) to out.png.
Run while the engine runs with SKATE_CS2_LINK=WxH:  python link_peek.py out.png"""
import ctypes
import math
import mmap
import struct
import sys
import time

from PIL import Image

k = ctypes.windll.kernel32
k.OpenFileMappingW.restype = ctypes.c_void_p
h = k.OpenFileMappingW(4, False, "Local\\Skate3CS2Link")
if not h:
    sys.exit("no link: is the engine running with SKATE_CS2_LINK?")
head = mmap.mmap(-1, 4096, tagname="Local\\Skate3CS2Link", access=mmap.ACCESS_READ)
magic, version, w, h_, latest, frames = struct.unpack_from("<6I", head, 0)
assert magic == 0x4C433353, hex(magic)
size = 4096 + 3 * (64 + w * h_ * 4)
m = mmap.mmap(-1, size, tagname="Local\\Skate3CS2Link", access=mmap.ACCESS_READ)
f0 = struct.unpack_from("<I", m, 20)[0]
time.sleep(1)
latest, f1 = struct.unpack_from("<2I", m, 16)
print("link %dx%d, %d frames/s, latest slot %d" % (w, h_, f1 - f0, latest))
at = 4096 + latest * (64 + w * h_ * 4)
seq, _, frame, px, py, pz, qx, qy, qz, qw, fov, aspect, rx, ry, rz = struct.unpack_from("<IIQ3f4f2f3f", m, at)
print("frame", frame, "camera (park m)", (px, py, pz), "rot", (qx, qy, qz, qw), "vfov %.1f deg aspect %.3f" % (
    math.degrees(fov), aspect))
cs = lambda x, y, z: (x / 0.0254, -z / 0.0254, y / 0.0254)
print("camera (CS2)", "%.0f %.0f %.0f" % cs(px, py, pz), " feet (CS2)", "%.0f %.0f %.0f" % cs(rx, ry, rz))
img = Image.frombuffer("RGBA", (w, h_), bytes(m[at + 64:at + 64 + w * h_ * 4]), "raw", "RGBA", 0, 1)
alpha = img.getchannel("A")
print("opaque pixels:", sum(1 for a in alpha.getdata() if a > 0))
board = Image.new("RGB", (w, h_))
for y in range(0, h_, 16):
    for x in range(0, w, 16):
        board.paste((90, 90, 90) if (x // 16 + y // 16) % 2 else (160, 160, 160), (x, y, x + 16, y + 16))
# premultiplied over the board
bp, ip = board.load(), img.load()
for y in range(h_):
    for x in range(w):
        r, g, b, a = ip[x, y]
        if a:
            br, bg, bb = bp[x, y]
            bp[x, y] = (r + br * (255 - a) // 255, g + bg * (255 - a) // 255, b + bb * (255 - a) // 255)
board.save(sys.argv[1] if len(sys.argv) > 1 else "out.png")
