#!/usr/bin/env python3
"""A persistent virtual pointer and keyboard on a wlroots compositor (sway), for
tools/platform-gui-test.sh: one device each for the whole run, so the client under test keeps its
wl_pointer and wl_keyboard (wlrctl and wtype make a new device per call, and the first events race
the client asking for the new one). The keyboard's keymap is compiled by xkbcli (libxkbcommon-tools)
for WL_INJECT_LAYOUT (us) or read from WL_INJECT_KEYMAP; key numbers are evdev codes. Speaks the wire protocol itself; needs only Python.

Commands, one per line on stdin:
  move X Y            pointer to X,Y on the output (absolute)       button CODE 1|0  (0x110 left)
  wheel V [H]         one discrete wheel step each way (V>0: down)   key CODE 1|0     (evdev code)
  mods DEPRESSED LOCKED   set the modifier masks (Shift 1, Lock 2, Control 4, Mod1 8, Mod4 0x40);
                      key presses of the modifier keys and Caps Lock update them as well
  sleep MS            sync                                           (waits for the compositor)
"""
import array, os, socket, struct, sys, time

class Conn:
    def __init__(self):
        path = os.path.join(os.environ["XDG_RUNTIME_DIR"], os.environ["WAYLAND_DISPLAY"])
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.next = 2
        self.buf = b""
        self.fds = []
        self.handlers = {}

    def new(self, handler=None):
        i = self.next
        self.next += 1
        if handler:
            self.handlers[i] = handler
        return i

    def send(self, obj, op, fmt="", *args, fds=()):
        body = b""
        for f, a in zip(fmt, args):
            if f in "uno":
                body += struct.pack("<I", a)
            elif f == "i":
                body += struct.pack("<i", a)
            elif f == "f":
                body += struct.pack("<i", int(round(a * 256)))
            elif f == "s":
                b = a.encode() + b"\0"
                body += struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)
        msg = struct.pack("<II", obj, (len(body) + 8) << 16 | op) + body
        anc = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))] if fds else []
        self.s.sendmsg([msg], anc)

    def dispatch(self, timeout=None):
        self.s.settimeout(timeout)
        try:
            data, anc, _, _ = self.s.recvmsg(65536, socket.CMSG_SPACE(16 * 4))
        except socket.timeout:
            return
        if not data:
            sys.exit("compositor went away")
        for level, kind, d in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                a = array.array("i")
                a.frombytes(d[: len(d) - len(d) % 4])
                self.fds += list(a)
        self.buf += data
        while len(self.buf) >= 8:
            obj, word = struct.unpack("<II", self.buf[:8])
            size = word >> 16
            if len(self.buf) < size:
                break
            body, self.buf = self.buf[8:size], self.buf[size:]
            if obj in self.handlers:
                self.handlers[obj](word & 0xFFFF, body)

    def roundtrip(self):
        done = []
        cb = self.new(lambda op, body: done.append(1))
        self.send(1, 0, "n", cb)
        while not done:
            self.dispatch()

def string_at(body, at):
    n = struct.unpack_from("<I", body, at)[0]
    return body[at + 4: at + 4 + n - 1].decode(), at + 4 + ((n + 3) & ~3)

# evdev code: modifier mask on the us keymap
MODIFIER_KEYS = {42: 1, 54: 1, 29: 4, 97: 4, 56: 8, 125: 0x40, 126: 0x40, 100: 0x80}

def main():
    c = Conn()
    globals_ = {}
    def registry(op, body):
        if op == 0:
            name = struct.unpack_from("<I", body, 0)[0]
            iface, at = string_at(body, 4)
            version = struct.unpack_from("<I", body, at)[0]
            globals_[iface] = (name, version)
    reg = c.new(registry)
    c.send(1, 1, "n", reg)
    c.roundtrip()
    def bind(iface, version):
        name, have = globals_[iface]
        i = c.new()
        c.send(reg, 0, "usun", name, iface, min(version, have), i)
        return i
    seat = bind("wl_seat", 1)
    vk_manager = bind("zwp_virtual_keyboard_manager_v1", 1)
    vp_manager = bind("zwlr_virtual_pointer_manager_v1", 1)
    vk = c.new()
    c.send(vk_manager, 0, "on", seat, vk)
    text = os.environ.get("WL_INJECT_KEYMAP")
    if text:
        data = open(text, "rb").read()
    else:
        import subprocess
        data = subprocess.run(["xkbcli", "compile-keymap", "--layout", os.environ.get("WL_INJECT_LAYOUT", "us")],
                              capture_output=True, check=True).stdout
    data += b"\0"
    fd = os.memfd_create("keymap")
    os.write(fd, data)
    c.send(vk, 0, "uu", 1, len(data), fds=[fd])  # keymap(format xkb_v1, fd: in the ancillary data, size)
    os.close(fd)
    vp = c.new()
    c.send(vp_manager, 0, "on", seat, vp)
    c.roundtrip()
    t0 = time.monotonic()
    ms = lambda: int((time.monotonic() - t0) * 1000) & 0xFFFFFFFF
    W, H = int(os.environ.get("WL_INJECT_W", "1280")), int(os.environ.get("WL_INJECT_H", "800"))
    depressed = locked = 0
    for line in sys.stdin:
        p = line.split()
        if not p:
            continue
        cmd, a = p[0], [int(x, 0) for x in p[1:]]
        if cmd == "move":
            c.send(vp, 1, "uuuuu", ms(), a[0], a[1], W, H)
            c.send(vp, 4)
        elif cmd == "button":
            c.send(vp, 2, "uuu", ms(), a[0], a[1])
            c.send(vp, 4)
        elif cmd == "wheel":
            for axis, v in ((0, a[0]), (1, a[1] if len(a) > 1 else 0)):
                if v:
                    c.send(vp, 5, "u", 0)  # source: wheel
                    c.send(vp, 7, "uufi", ms(), axis, 15.0 * v, v)  # axis_discrete
            c.send(vp, 4)
        elif cmd == "key":
            c.send(vk, 1, "uuu", ms(), a[0], a[1])
            # A virtual keyboard's client keeps its modifier state (the compositor doesn't work it out
            # from the keys), as a real keyboard's xkb_state would: Shift, Control, Alt, Super, AltGr
            # (Mod5) held, Caps Lock toggled.
            bit = MODIFIER_KEYS.get(a[0])
            if bit:
                depressed = depressed | bit if a[1] else depressed & ~bit
            elif a[0] == 58 and a[1]:
                locked ^= 2
            if bit or a[0] == 58:
                c.send(vk, 2, "uuuu", depressed, 0, locked, 0)
        elif cmd == "mods":
            depressed, locked = a[0], a[1] if len(a) > 1 else 0
            c.send(vk, 2, "uuuu", depressed, 0, locked, 0)
        elif cmd == "sleep":
            time.sleep(a[0] / 1000)
        elif cmd == "sync":
            c.roundtrip()
        print("ok", line.strip(), flush=True)
    c.roundtrip()

main()
