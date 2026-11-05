#!/usr/bin/env python3
"""
Boot the install ISO under qemu, pick the Japanese messages in sysinst
and check that the console really draws them.

    python3 ci/test-ja.py NetBSD-11.0-amd64.iso path/to/src [outdir]

The check is on pixels, not on text: the kanji font the kernel carries
(sys/dev/wsfont/shnmk16.h, indexed by jisx0208.h) is rendered here for
a phrase from the Japanese welcome message, and that bitmap has to
appear somewhere in a screendump.  sysinst's shell then writes to a
second screen, which shows the cells vcons keeps.  Screendumps are written to outdir as PNG so they can
be looked at afterwards.
"""

import json, os, re, socket, struct, subprocess, sys, time, zlib

iso, src = sys.argv[1], sys.argv[2]
outdir = sys.argv[3] if len(sys.argv) > 3 else "."
os.makedirs(outdir, exist_ok=True)


def read_font(path, width, height, mapfile=None):
    """Glyph bitmaps from a wsfont header, as lists of row bit masks.

    With mapfile, the header is a JIS X 0208 font and the glyphs come
    back keyed by the Unicode characters that table maps to them."""
    text = open(path).read()
    data = re.search(r"_data\[\] = \{(.*)\};", text, re.S).group(1)
    stride = (width + 7) // 8
    fonts = {}
    if mapfile:
        # every glyph is preceded by a /* 0xJJJJ */ comment
        for m in re.finditer(r"/\* 0x([0-9a-f]{4}) \*/((?:\s*0x[0-9a-f]{2},)+)", data):
            code = int(m.group(1), 16)
            b = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(2))]
            rows = [int.from_bytes(bytes(b[r*stride:(r+1)*stride]), "big")
                    for r in range(height)]
            fonts[code] = rows
        table = open(mapfile).read()
        ucs = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{4}),",
               re.search(r"_ucs\[\] = \{(.*?)\};", table, re.S).group(1))]
        glyph = [int(x) for x in re.findall(r"(\d+),",
                 re.search(r"_glyph\[\] = \{(.*?)\};", table, re.S).group(1))]
        idx = {}
        for u, g in zip(ucs, glyph):
            j = 0x2121 + (g // 94) * 256 + g % 94
            idx[chr(u)] = fonts.get(j)
        return idx
    first = int(re.search(r"^\s*(\d+),\s*/\* firstchar", text, re.M).group(1))
    b = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", data)]
    idx = {}
    for i in range(len(b) // (height * stride)):
        rows = [int.from_bytes(bytes(b[(i*height+r)*stride:(i*height+r+1)*stride]), "big")
                for r in range(height)]
        idx[chr(first + i)] = rows
    return idx


kanji_font = read_font(os.path.join(src, "sys/dev/wsfont/shnmk16.h"), 16, 16,
                       os.path.join(src, "sys/dev/wsfont/jisx0208.h"))


def render(s):
    """Bit rows of a string as the console draws it, 16 rows of ints."""
    rows = [0] * 16
    width = 0
    for ch in s:
        g, w = kanji_font[ch], 16
        for r in range(16):
            rows[r] = (rows[r] << w) | g[r]
        width += w
    return rows, width


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        pix = f.read(w * h * 3)
    return w, h, pix


def write_png(path, w, h, pix):
    raw = b"".join(b"\0" + pix[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def find(bits, w, h, pix, rows, width):
    """Is the rendered bitmap on screen?  Background is whatever colour
    the top-left pixel has; everything else counts as ink."""
    bg = pix[0:3]
    # one int per screen row, bit set where the pixel is not background
    scan = []
    for y in range(h):
        v = 0
        row = pix[y*w*3:(y+1)*w*3]
        for x in range(w):
            v = (v << 1) | (row[x*3:x*3+3] != bg)
        scan.append(v)
    mask = (1 << width) - 1
    for y in range(h - 16):
        for x in range(w - width):
            shift = w - width - x
            if all(((scan[y+r] >> shift) & mask) == rows[r] for r in range(16)):
                return (x, y)
    return None


KEYS = {" ": (False, "spc"), "\n": (False, "ret"), "\\": (False, "backslash"),
        "'": (False, "apostrophe"), ";": (False, "semicolon"),
        "/": (False, "slash"), "[": (False, "bracket_left"),
        "%": (True, "5"), ">": (True, "dot"), "@": (True, "2")}
KEYS.update((c, (True, c.lower())) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ")


def u8(s):
    """A string as the octal escapes printf(1) turns back into UTF-8."""
    return "".join("\\%03o" % b for b in s.encode("utf-8"))


class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(50):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")
        json.loads(self.f.readline())
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def keys(self, keys):
        for k in keys:
            self.cmd("send-key", keys=[{"type": "qcode", "data": k}])
            time.sleep(0.2)

    def type(self, text):
        """Type at a US keyboard."""
        for ch in text:
            shift, k = KEYS.get(ch, (False, ch))
            keys = (["shift"] if shift else []) + [k]
            self.cmd("send-key", keys=[{"type": "qcode", "data": x}
                                       for x in keys])
            time.sleep(0.05)

    def switch(self, fkey, name):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k}
                                   for k in ("ctrl", "alt", fkey)])
        time.sleep(3)
        return self.dump(name)

    def dump(self, name):
        ppm = os.path.join(outdir, name + ".ppm")
        self.cmd("screendump", filename=ppm)
        w, h, pix = read_ppm(ppm)
        write_png(os.path.join(outdir, name + ".png"), w, h, pix)
        os.unlink(ppm)
        return w, h, pix


def longest_run(w, h, pix):
    """Longest horizontal run of non-background pixels, middle of the screen.

    The menu box is drawn with line characters, so its border is a run
    hundreds of pixels long; boot messages never come near that."""
    bg = pix[0:3]
    best = 0
    for y in range(h // 4, 3 * h // 4):
        run = 0
        row = pix[y*w*3:(y+1)*w*3]
        for x in range(w):
            if row[x*3:x*3+3] != bg:
                run += 1
                best = max(best, run)
            else:
                run = 0
    return best


sock = "/tmp/ja-%d.sock" % os.getpid()
qemu = subprocess.Popen([
    "qemu-system-x86_64", "-m", "1024", "-cdrom", iso, "-boot", "d",
    "-vga", "std", "-display", "none", "-serial", "none",
    "-qmp", "unix:%s,server,nowait" % sock] + os.environ.get("QEMU_ARGS", "").split())
try:
    q = QMP(sock)

    # The CD's boot.cfg asks the boot loader for a VESA mode, so a
    # 1024x768 screen means the console goes through rasops rather than
    # the VGA character generator, and the box means sysinst is far
    # enough along to be asking for the language.
    for i in range(60):
        time.sleep(5)
        w, h, pix = q.dump("boot")
        if (w, h) == (1024, 768) and longest_run(w, h, pix) > 200:
            break
    else:
        sys.exit("the language menu never appeared on a framebuffer "
                 "(last screen %dx%d)" % (w, h))
    print("language menu up on %dx%d after %ds" % (w, h, (i + 1) * 5))

    # The catalogs are listed in name order after the built-in English:
    # de es fr ja pl, so Japanese is the fifth entry.  The letter moves
    # the cursor, Enter takes it.
    q.keys(["e", "ret"])

    # One phrase from the message window, which sysinst fills through
    # msgc, and one from the menu box, which goes through menuc: the
    # two take different paths to the screen, and a glyph can come out
    # whole on one and in halves on the other.
    phrases = ["このメニュー形式のツールは", "キーボードの種類"]
    todo = [(p, ) + render(p) for p in phrases]
    for i in range(12):
        time.sleep(5)
        w, h, pix = q.dump("japanese")
        found = [(p, find(None, w, h, pix, rows, width))
                 for p, rows, width in todo]
        if all(pos is not None for _, pos in found):
            break
    else:
        missing = [p for p, pos in found if pos is None]
        sys.exit("not found on the screen: %s" % ", ".join(missing))
    for p, pos in found:
        print("found %r at %s" % (p, pos))

    # Leave sysinst for its shell: keep the keyboard, then Exit.
    q.keys(["ret"])
    time.sleep(3)
    q.keys(["x", "ret"])
    time.sleep(5)

    # A second screen, written while it is not shown.  Nothing reaches
    # the framebuffer until the switch, so what appears then comes from
    # the cells vcons keeps.  Each line has one job:
    #   3  wide glyphs at all
    #   4  A over the left half of a glyph takes the right half with it
    #   5  a Latin-1 byte the wide font also has stays one cell wide
    #   6  a cut-short sequence and an overlong form print nothing
    #   7  a wide glyph with one column left wraps to the next line
    cols = w // 8
    away_text = (r"\033%G\033[2J\033[3;1H" + u8("さしすせそ")
                 + r"\033[4;1H" + u8("たちつ") + r"\rA"
                 + r"\033[5;1H\033%@\327\033%G" + u8("なに")
                 + r"\033[6;1H\343\201Z\300\257" + u8("はひ")
                 + r"\033[7;" + str(cols) + "H" + u8("まみ"))
    home_text = r"\033[2J\033[3;1H" + u8("らりるれろ") + r"\n"
    # printf(1) takes % as a conversion
    q.type("wsconscfg 1; printf '%s' >/dev/ttyE1; printf '%s'\n"
           % (away_text.replace("%", "%%"), home_text))

    home = render("らりるれろ")
    for i in range(12):
        time.sleep(5)
        w, h, pix = q.dump("shell")
        if find(None, w, h, pix, *home):
            break
    else:
        sys.exit("the shell never printed on the first screen")

    w, h, away = q.switch("f2", "japanese-away")
    if away == pix:
        sys.exit("the second screen was not shown")

    # where each string has to be: x, and the screen line
    want = [("さしすせそ", 0, 3), ("ちつ", 16, 4), ("なに", 8, 5),
            ("はひ", 8, 6), ("まみ", 0, 8)]
    for p, x, row in want:
        at = find(None, w, h, away, *render(p))
        if at != (x, (row - 1) * 16):
            sys.exit("%r at %s, not at (%d, %d)"
                     % (p, at, x, (row - 1) * 16))
        print("found %r at %s" % (p, at))
    if find(None, w, h, away, *render("たち")):
        sys.exit("the left half of a glyph survived being overwritten")
    bg = away[0:3]
    for r in range(3 * 16, 4 * 16):
        for c in range(8, 16):
            if away[(r * w + c) * 3:(r * w + c) * 3 + 3] != bg:
                sys.exit("the right half of an overwritten glyph is "
                         "still on the screen")

    # and back, then there again: both directions repaint from the cells
    w, h, back = q.switch("f1", "japanese-back")
    if find(None, w, h, back, *home) is None:
        sys.exit("lost in a screen switch: らりるれろ")
    w, h, again = q.switch("f2", "japanese-again")
    if again != away:
        sys.exit("the second screen came back different")
    print("both screens repainted")
    print("Japanese console OK")
finally:
    qemu.terminate()
    qemu.wait()
