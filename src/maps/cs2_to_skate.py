"""A CS2 map as a Skate 3 Rust Engine park: <map>.cs2col (map_collision.py: the map's physics triangles) -> <map>.skate.

SKATE08, the engine's portable format (engine/tools/make_skate_demo.py writes the same version; reader:
crates/skate-data/src/skate_map.rs). The collision triangles are both what you skate on and what you see: one
material per CS2 surface type (dirt, wood, metal, ...), tinted, with a tiled concrete texture on planar UVs.
CS2 units are inches, Z up -> metres, Y up: (x, y, z) -> (x, z, -y) * 0.0254 (a rotation, so windings keep).
Sky / clip shells are left out (the sky box would be a ceiling).

Run:  python cs2_to_skate.py de_dust2 [more maps...]   (reads/writes next to this file; writes <map>.skate to skate
      in the engine and <map>_link.skate for CS2)"""
import math
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE = 0.0254
TILE = 2.0  # metres per texture repeat
# the first info_player_terrorist (s2v -d maps/<map>/entities/default_ents.vents_c): CS2 origin x y z, yaw
SPAWNS = {"de_dust2": (-822.365, -795.642, 150.709, 107.0), "de_mirage": (1376.0, -304.0, -144.0, 227.0)}

# a surface prop's look: the first match by substring; (name, tint)
LOOKS = [("dirt", (0.62, 0.50, 0.36)), ("sand", (0.80, 0.70, 0.52)), ("grass", (0.36, 0.48, 0.24)),
         ("wood", (0.55, 0.40, 0.26)), ("metal", (0.55, 0.57, 0.60)), ("glass", (0.60, 0.72, 0.78)),
         ("tile", (0.78, 0.74, 0.66)), ("brick", (0.66, 0.42, 0.32)), ("plaster", (0.86, 0.80, 0.68)),
         ("rock", (0.58, 0.54, 0.48)), ("gravel", (0.52, 0.50, 0.46)), ("water", (0.30, 0.45, 0.55)),
         ("", (0.80, 0.76, 0.68))]  # concrete / default


def read_cs2col(path):
    data = open(path, "rb").read()
    magic, version, nsurf, ntri = struct.unpack_from("<4sIII", data, 0)
    assert magic == b"C2CO" and version == 1, path
    at, surfaces = 16, []
    for _ in range(nsurf):
        n = data[at]
        surfaces.append(data[at + 1:at + 1 + n].decode())
        at += 1 + n
    tris = struct.iter_unpack("<9fHH", data[at:at + ntri * 40])
    return surfaces, tris


def to_skate(p):
    """metres, Y up, on the engine's 1 mm weld grid (skate_world.rs collision_world welds corners to 1 mm: a sliver
    whose corners weld together is a zero-length edge, "Invalid SKATE collision volume")"""
    return tuple(round(v * SCALE * 1000) / 1000 for v in (p[0], p[2], -p[1]))


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def concrete(size=64, seed=1):
    """a soft grey noise tile with seams every half tile (RGBA, mid-grey so the material tint carries the colour)"""
    rng = random.Random(seed)
    px = bytearray()
    for y in range(size):
        for x in range(size):
            v = 200 + rng.randint(-14, 14)
            if x % (size // 2) == 0 or y % (size // 2) == 0:
                v -= 45
            px += bytes((v, v, v, 255))
    return size, px


def convert(name, link=False):
    """link: for CS2 (cs2skate.dll): the same collision, one hidden render triangle (only the skater is drawn)"""
    surfaces, raw = read_cs2col(os.path.join(HERE, name + ".cs2col"))
    looks = []
    for s in surfaces:
        looks.append(next(i for i, (key, _) in enumerate(LOOKS) if key in s.lower()))
    used = sorted({looks[i] for i in range(len(surfaces))})
    material_of = {look: n + 1 for n, look in enumerate(used)}  # one-based

    tris = []
    for t in raw:
        if t[10] & 2:
            continue  # sky / clip
        a, b, c = to_skate(t[0:3]), to_skate(t[3:6]), to_skate(t[6:9])
        n = cross(sub(b, a), sub(c, a))
        length = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        if a == b or b == c or a == c or length < 1e-5:
            continue  # welded shut / degenerate
        tris.append((a, b, c, (n[0] / length, n[1] / length, n[2] / length), material_of[looks[t[9]]]))
    # spatial order: the engine bounds every 64 consecutive collision triangles, so neighbours should be together
    tris.sort(key=lambda t: (int((t[0][0] + 4096) // 8), int((t[0][2] + 4096) // 8), t[0][1]))

    # spawn: the map's first T spawn (feet + facing); the engine's forward is (sin heading, 0, cos heading)
    x, y, z, yaw = SPAWNS[name]
    spawn = to_skate((x, y, z))
    heading = math.atan2(math.cos(math.radians(yaw)), -math.sin(math.radians(yaw)))

    out = bytearray(b"SKATE08\0")
    def u(*v): out.extend(struct.pack("<%dI" % len(v), *v))
    def f(*v): out.extend(struct.pack("<%df" % len(v), *v))
    def s(v):
        b = v.encode()
        u(len(b))
        out.extend(b)

    u(0x12345678)
    s(name)
    f(*spawn, heading)
    # environment (45 floats): the engine demo's daylight
    f(.09, .34, .72, .58, .78, .98, .18, .25, .34, 0, 12, .62, 17, 0)
    f(.045, .10, .26, 1, .32, .10, .05, .035, .06)
    f(.007, .015, .045, .045, .085, .17, .008, .014, .032)
    f(1, .92, .78, .42, .56, .92, 1.25, .18, .32, .11, 1, 1, 1)
    shown = [(tuple(spawn[i] + (0, -1000, 0)[i] + d[i] for i in range(3)) for d in ((0, 0, 0), (0, 0, .01), (.01, 0, 0)))]
    shown = [(*shown[0], (0, 1, 0), 1)] if link else tris
    u(len(used), 2, len(shown) * 3, len(shown) * 3, len(tris), 0, 0, 0, 0)
    for look in used:
        key, tint = LOOKS[look]
        s(key or "concrete")
        u(1)                     # flags
        f(.6, .1, *tint, .85, 0)  # friction, restitution, colour, roughness, emissive
        u(1, 2)                  # albedo, indirect textures
        f(1)                     # indirect strength
        u(0, 0, 0, 0)            # normal, ORM, emissive textures; alpha mode
        f(.5)                    # alpha cutoff
        u(3, 1, 0)               # audio, physics, pattern (the demo's concrete)
    size, px = concrete()
    s("cs2 concrete")
    u(size, size, 1, len(px))
    out.extend(px)
    s("indirect constant")
    u(1, 1, 0, 4)
    out.extend(bytes((64, 64, 64, 255)))
    # render vertices: three per triangle, planar UVs on the face's dominant axis
    for a, b, c, n, m in shown:
        axis = max(range(3), key=lambda i: abs(n[i]))
        uu, vv = [(1, 2), (0, 2), (0, 1)][axis]
        for p in (a, b, c):
            f(*p, *n, p[uu] / TILE, p[vv] / TILE, .5, .5)
            u(m)
    out.extend(struct.pack("<%dI" % (len(shown) * 3), *range(len(shown) * 3)))
    for a, b, c, n, m in tris:
        f(*a, *b, *c)
        u(1, m)  # surface, material
    dst = os.path.join(HERE, name + ("_link" if link else "") + ".skate")
    open(dst, "wb").write(out)
    print("%s: %d triangles, %d materials, spawn %.1f %.1f %.1f -> %s (%d MB)" % (
        name, len(tris), len(used), *spawn, dst, len(out) >> 20))


if __name__ == "__main__":
    for m in sys.argv[1:] or ["de_dust2"]:
        convert(m)
        convert(m, link=True)
