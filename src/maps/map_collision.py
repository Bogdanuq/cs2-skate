"""A CS2 map's collision as one triangle file, for Minecraft's physics (cs2craft.dll streams the triangles near you).

Source 2 Viewer decompiles maps/<map>/world_physics.vmdl_c into DMX meshes (thousands of convex hulls and a few
triangle meshes, CS2 world units, Z up); this reads them all (Blender Source Tools' datamodel.py) and writes
    maps/<map>.cs2col:  "C2CO" u32 version, u32 surfaces, u32 triangles,
                        surfaces x (u8 length, name bytes),
                        triangles x (9 x f32 x0 y0 z0 x1 y1 z1 x2 y2 z2 in CS2 units, u16 surface, u16 flags)
flags bit0: from a convex hull (closed solid), bit1: from the sky / clip shell.
Run:  python map_collision.py de_dust2 [more maps...]"""
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import datamodel  # noqa: E402  (Blender Source Tools' DMX reader, copied next to this file)

CS2 = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
S2V = r"C:\Users\elect\Documents\ClaudeWorkspace\source2_tools\s2v\Source2Viewer-CLI.exe"
OUT = os.path.join(os.path.dirname(HERE), "maps")


def decompile(name):
    """maps/<map>/world_physics.vmdl and its DMX, decompiled once"""
    root = os.path.join(OUT, name)
    vmdl = os.path.join(root, "maps", name, "world_physics.vmdl")
    if not os.path.exists(vmdl):
        os.makedirs(root, exist_ok=True)
        subprocess.run([S2V, "-i", os.path.join(CS2, "maps", name + ".vpk"), "--vpk_filepath",
                        "maps/%s/world_physics.vmdl_c" % name, "-d", "-o", root], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800)
    return root, vmdl


def shapes(vmdl):
    """(file, surface prop, hull?) of every physics shape the vmdl lists"""
    text = open(vmdl, encoding="utf-8", errors="replace").read()
    out = []
    for m in re.finditer(r'_class = "(PhysicsHullFile|PhysicsMeshFile)"\s*filename = "([^"]+)"\s*parent_bone = "[^"]*"\s*'
                         r'surface_prop = "([^"]*)"\s*collision_tags = "([^"]*)"', text):
        out.append((m.group(2), m.group(3), m.group(1) == "PhysicsHullFile"))
    return out


def triangles(args):
    """one DMX's triangles: [(9 floats, sky?)]"""
    path, = args
    dm = datamodel.load(path)
    tris = []
    for dag in dm.root["skeleton"]["children"]:
        mesh = dag["shape"]
        if mesh is None:
            continue
        state = mesh["currentState"]
        pos = [tuple(p) for p in state["position$0"]]
        idx = state["position$0Indices"]
        for fs in mesh["faceSets"]:
            sky = "sky" in fs["material"]["mtlName"].lower() or "clip" in fs["material"]["mtlName"].lower()
            poly = []
            for f in fs["faces"]:
                if f >= 0:
                    poly.append(pos[idx[f]])
                    continue
                for i in range(1, len(poly) - 1):  # a fan
                    a, b, c = poly[0], poly[i], poly[i + 1]
                    tris.append((a + b + c, sky))
                poly = []
    return tris


def convert(name):
    root, vmdl = decompile(name)
    items = shapes(vmdl)
    surfaces = sorted({s for _, s, _ in items})
    sid = {s: i for i, s in enumerate(surfaces)}
    paths = [(os.path.join(root, *f.split("/")),) for f, _, _ in items]
    out = bytearray()
    count = 0
    with ProcessPoolExecutor() as pool:
        for (f, surface, hull), tris in zip(items, pool.map(triangles, paths, chunksize=64)):
            for verts, sky in tris:
                out += struct.pack("<9fHH", *verts, sid[surface], (1 if hull else 0) | (2 if sky else 0))
                count += 1
    head = struct.pack("<4sIII", b"C2CO", 1, len(surfaces), count)
    for s in surfaces:
        b = s.encode()
        head += struct.pack("<B", len(b)) + b
    dst = os.path.join(OUT, name + ".cs2col")
    with open(dst, "wb") as f:
        f.write(head + out)
    print(name, len(items), "shapes,", count, "triangles,", len(surfaces), "surfaces ->", dst, os.path.getsize(dst) // 1024, "KB")


if __name__ == "__main__":
    for m in sys.argv[1:] or ["de_dust2"]:
        convert(m)
