"""The virtual pad: drives skate3rust.exe through the xinput1_4.dll proxy (shared memory Local\\Skate3VirtualPad).

  pad.py install                  copy the proxy next to skate3rust.exe
  pad.py launch [args...]         start the game in XInput mode (args go to the game, e.g. --map X.skate)
  pad.py shot out.png             screenshot of the game window
  pad.py press a [secs]           tap a button (a b x y start back lb rb ls rs up down left right)
  pad.py hold k=v ... secs        hold sticks/triggers/buttons, then release (lx ly rx ry -1..1, lt rt 0..1, buttons=1)
  pad.py set k=v ...              set and leave held;  pad.py release  lets go of everything
  pad.py run file                 one command per line (press / hold / set / release / wait secs / shot path)
"""
import ctypes, mmap, os, shutil, struct, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.join(HERE, "..", "skate3rust", "skate3rust-windows-x64")
EXE = os.path.join(GAME, "skate3rust.exe")
BUTTONS = {"up": 0x0001, "down": 0x0002, "left": 0x0004, "right": 0x0008, "start": 0x0010, "back": 0x0020,
           "ls": 0x0040, "rs": 0x0080, "lb": 0x0100, "rb": 0x0200, "a": 0x1000, "b": 0x2000, "x": 0x4000, "y": 0x8000}
FMT = "<III H BB hhhh"  # magic active packet | buttons lt rt lx ly rx ry
MAGIC = 0x44415056
ctypes.windll.user32.SetProcessDPIAware()  # real pixels for the window size


class Pad:
    def __init__(self):
        self.m = mmap.mmap(-1, struct.calcsize(FMT), tagname="Local\\Skate3VirtualPad")
        self.state = {"buttons": 0, "lt": 0, "rt": 0, "lx": 0, "ly": 0, "rx": 0, "ry": 0}

    def write(self):
        s = self.state
        packet = (struct.unpack_from("<I", self.m, 8)[0] + 1) & 0xFFFFFFFF
        struct.pack_into(FMT, self.m, 0, MAGIC, 1, packet, s["buttons"], s["lt"], s["rt"], s["lx"], s["ly"], s["rx"],
                         s["ry"])

    def set(self, **kv):
        for k, v in kv.items():
            if k in BUTTONS:
                bit = BUTTONS[k]
                self.state["buttons"] = self.state["buttons"] | bit if float(v) else self.state["buttons"] & ~bit
            elif k in ("lt", "rt"):
                self.state[k] = int(max(0.0, min(1.0, float(v))) * 255)
            else:  # sticks: y up is positive, like XInput
                self.state[k] = int(max(-1.0, min(1.0, float(v))) * 32767)
        self.write()

    def release(self):
        self.state = dict.fromkeys(self.state, 0)
        self.write()

    def press(self, button, secs=0.15):
        self.set(**{button: 1})
        time.sleep(secs)
        self.set(**{button: 0})
        time.sleep(0.1)

    def hold(self, secs, **kv):
        self.set(**kv)
        time.sleep(secs)
        self.release()


def window():
    """The game's window (the newest skate3rust.exe with one: the one `launch` started)."""
    u = ctypes.windll.user32
    out = subprocess.run(["powershell", "-NoProfile", "-c", "(Get-Process skate3rust | ? MainWindowHandle -ne 0 | "
                          "sort StartTime | select -Last 1).Id"], capture_output=True, text=True).stdout
    pids = {int(out)} if out.strip() else set()
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(h, _):
        pid = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(pid))
        r = wintypes.RECT()
        if pid.value in pids and u.IsWindowVisible(h) and u.GetClientRect(h, ctypes.byref(r)):
            found.append((r.right * r.bottom, h))
        return True

    u.EnumWindows(each, 0)
    return max(found)[1] if found else None


def shot(path):
    """The window's client area via PrintWindow (works behind other windows; no focus change)."""
    from PIL import Image
    u, g = ctypes.windll.user32, ctypes.windll.gdi32
    h = window()
    if not h:
        sys.exit("skate3rust.exe has no window")
    r = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(r))
    w, ht = r.right, r.bottom
    screen = u.GetDC(0)
    dc, bmp = g.CreateCompatibleDC(screen), g.CreateCompatibleBitmap(screen, w, ht)
    g.SelectObject(dc, bmp)
    u.PrintWindow(h, dc, 3)  # client only | PW_RENDERFULLCONTENT
    buf = ctypes.create_string_buffer(w * ht * 4)
    header = struct.pack("<IiiHHIIiiII", 40, w, -ht, 1, 32, 0, 0, 0, 0, 0, 0)
    g.GetDIBits(dc, bmp, 0, ht, buf, header, 0)
    g.DeleteObject(bmp), g.DeleteDC(dc), u.ReleaseDC(0, screen)
    Image.frombuffer("RGB", (w, ht), buf.raw, "raw", "BGRX", 0, 1).save(path)
    print("saved", path)


def kv(args):
    return dict(a.split("=", 1) for a in args)


def command(pad, words):
    cmd, args = words[0], words[1:]
    if cmd == "press":
        pad.press(args[0], float(args[1]) if len(args) > 1 else 0.15)
    elif cmd == "hold":
        pad.hold(float(args[-1]), **kv(args[:-1]))
    elif cmd == "set":
        pad.set(**kv(args))
    elif cmd == "release":
        pad.release()
    elif cmd == "wait":
        time.sleep(float(args[0]))
    elif cmd == "shot":
        shot(args[0])
    else:
        sys.exit("unknown command " + cmd)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "install":
        shutil.copy2(os.path.join(HERE, "xinput1_4.dll"), GAME)
        print("installed", os.path.join(GAME, "xinput1_4.dll"))
    elif cmd == "launch":
        env = dict(os.environ, SKATE3_INPUT="xinput")
        p = subprocess.Popen([EXE] + sys.argv[2:], cwd=GAME, env=env, creationflags=subprocess.DETACHED_PROCESS)
        print("started pid", p.pid)
    elif cmd == "run":
        pad = Pad()
        for line in open(sys.argv[2], encoding="utf-8"):
            if line.split() and not line.startswith("#"):
                command(pad, line.split())
    else:
        command(Pad(), sys.argv[1:])


if __name__ == "__main__":
    main()
