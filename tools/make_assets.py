#!/usr/bin/env python3
"""
Generates 3DBrowse's artwork so the repo only needs to hold source:

  assets/icon.png     48x48 HOME Menu icon
  assets/banner.png   256x128 HOME Menu banner
  assets/banner.wav   banner jingle
  assets/qr.png       FBI Remote Install QR code for the latest .cia
  preview.png         README preview, laid out by the real page engine
                      (needs a compiled tools/preview_host, see below)

Usage: python3 tools/make_assets.py [path/to/preview_host]
Needs Pillow and qrcode (apt: python3-pil python3-qrcode).
"""
import glob
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
CIA_URL = "https://github.com/meerok/Meer_3DBrowse/releases/latest/download/3DBrowse.cia"

PAPER = (245, 242, 235)
INK = (30, 28, 24)
MUTED = (133, 126, 114)
HAIR = (220, 213, 199)
BAR = (236, 231, 220)
ACCENT = (217, 72, 43)
LINK = (34, 87, 176)
FIELD = (255, 253, 248)
CODE = (232, 226, 213)
FOCUS = (245, 211, 198)


def find_font(names, fallback):
    for n in names:
        hits = glob.glob(f"/usr/share/fonts/**/{n}", recursive=True)
        if hits:
            return hits[0]
    hits = glob.glob(f"/usr/share/fonts/**/{fallback}", recursive=True)
    return hits[0] if hits else None


REGULAR = find_font(["Inter-Regular.otf", "Inter-Regular.ttf", "Inter.ttc"], "DejaVuSans.ttf")
BOLD = find_font(["Inter-Bold.otf", "Inter-Bold.ttf", "Inter-SemiBold.otf"], "DejaVuSans-Bold.ttf")


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.load_default()


def rounded(draw, box, r, fill):
    draw.rounded_rectangle(box, radius=r, fill=fill)


# ------------------------------------------------------------------ the mark

def draw_mark(img, x, y, s, bg=INK, fg=PAPER, accent=ACCENT):
    """3DBrowse mark: the two 3DS screens, with lines of text and one link."""
    d = ImageDraw.Draw(img)
    rounded(d, (x, y, x + s, y + s), int(s * 0.22), bg)
    # top screen
    tw, th = s * 0.66, s * 0.36
    tx, ty = x + (s - tw) / 2, y + s * 0.14
    rounded(d, (tx, ty, tx + tw, ty + th), max(1, int(s * 0.05)), fg)
    lw = max(1, round(s * 0.045))
    for i, (frac, col) in enumerate([(0.72, bg), (0.5, accent), (0.62, bg)]):
        ly = ty + th * (0.26 + i * 0.25)
        d.rectangle((tx + tw * 0.14, ly, tx + tw * 0.14 + tw * frac * 0.8, ly + lw), fill=col)
    # bottom screen
    bw, bh = s * 0.52, s * 0.3
    bx, by = x + (s - bw) / 2, ty + th + s * 0.08
    rounded(d, (bx, by, bx + bw, by + bh), max(1, int(s * 0.05)), fg)
    d.ellipse((bx + bw * 0.62, by + bh * 0.3, bx + bw * 0.62 + bh * 0.4, by + bh * 0.7), fill=accent)


def make_icon():
    big = Image.new("RGBA", (192, 192), (0, 0, 0, 0))
    draw_mark(big, 8, 8, 176)
    big.resize((48, 48), Image.LANCZOS).save(os.path.join(ASSETS, "icon.png"))


def make_banner():
    W, H = 256, 128
    S = 4
    img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rounded(d, (8 * S, 20 * S, (W - 8) * S, (H - 20) * S), 10 * S, PAPER)
    d.rectangle((8 * S + 10 * S, (H - 20) * S - 3 * S, (W - 8) * S - 10 * S, (H - 20) * S), fill=PAPER)
    draw_mark(img, 22 * S, 38 * S, 52 * S)
    f = font(BOLD, 30 * S)
    d.text((84 * S, 40 * S), "3DBrowse", font=f, fill=INK)
    fw = d.textlength("3DBrowse", font=f)
    d.rectangle((84 * S, 74 * S, 84 * S + fw, 76 * S), fill=ACCENT)
    d.text((84 * S, 79 * S), "by Meer", font=font(REGULAR, 12 * S), fill=MUTED)
    img.resize((W, H), Image.LANCZOS).save(os.path.join(ASSETS, "banner.png"))


def make_wav():
    """Two soft notes, like a page settling into place."""
    rate = 22050
    dur = 1.6
    n = int(rate * dur)
    notes = [(0.0, 659.25), (0.16, 987.77)]
    frames = bytearray()
    for i in range(n):
        t = i / rate
        v = 0.0
        for start, f in notes:
            if t >= start:
                tt = t - start
                env = min(1.0, tt * 120) * math.exp(-tt * 4.2)
                v += env * (math.sin(2 * math.pi * f * tt) + 0.25 * math.sin(4 * math.pi * f * tt))
        frames += struct.pack("<h", int(max(-1, min(1, v * 0.33)) * 32767))
    with wave.open(os.path.join(ASSETS, "banner.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def make_qr():
    import qrcode
    qr = qrcode.QRCode(border=2, box_size=8, error_correction=qrcode.constants.ERROR_CORRECT_M)
    qr.add_data(CIA_URL)
    qr.make(fit=True)
    qr.make_image(fill_color="black", back_color="white").convert("RGB").save(os.path.join(ASSETS, "qr.png"))


# ------------------------------------------------------------------ preview

def make_preview(host_bin):
    S = 2
    LF = 30.0        # line height at scale 1, same as the 3DS system font
    FS = 24.0        # font size that matches LF
    reg = font(REGULAR, int(FS))
    widths = [reg.getlength(chr(c)) for c in range(32, 127)]
    tmp = tempfile.mkdtemp()
    wpath = os.path.join(tmp, "w.txt")
    with open(wpath, "w") as f:
        f.write(f"{LF}\n" + " ".join(f"{w:.3f}" for w in widths))
    out = subprocess.run([host_bin, wpath, os.path.join(ROOT, "tools", "preview.html"), "300", "0.5"],
                         capture_output=True, text=True, check=True).stdout

    runs, boxes, title = [], [], ""
    for ln in out.splitlines():
        p = ln.split("\t")
        if p[0] == "T":
            title = p[1]
        elif p[0] == "R":
            runs.append((float(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5]), int(p[6]), int(p[7]), p[8]))
        elif p[0] == "B":
            boxes.append((int(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5]), int(p[6]), p[7] if len(p) > 7 else ""))

    TOPBAR, TOOLBAR = 22, 34
    TOPV, BOTV = 240 - TOPBAR, 240 - TOOLBAR
    pad, gap = 28, 14
    Wc = 400 + pad * 2
    Hc = 240 * 2 + gap + pad * 2
    canvas = Image.new("RGB", (Wc * S, Hc * S), (38, 36, 32))
    top = Image.new("RGB", (400 * S, 240 * S), PAPER)
    bot = Image.new("RGB", (320 * S, 240 * S), PAPER)
    focus_link = 1   # show the D-pad focus on the second link

    fonts = {}

    def fnt(sz):
        k = round(sz * S * 2) / 2
        if k not in fonts:
            fonts[k] = font(REGULAR, k)
        return fonts[k]

    def draw_slice(img, view_top, view_h, offx, offy):
        d = ImageDraw.Draw(img)
        X = lambda v: v * S
        x0 = offx + 10
        for kind, bx, by, bw, bh, ref, label in boxes:
            if by + bh < view_top or by > view_top + view_h:
                continue
            sx, sy = x0 + bx, offy + by - view_top
            if kind == 1:
                d.rectangle((X(sx), X(sy), X(sx + bw), X(sy + 1)), fill=HAIR)
            elif kind == 2:
                d.rectangle((X(sx), X(sy), X(sx + bw), X(sy + bh)), fill=ACCENT)
            elif kind == 3:
                r = bw / 2
                d.ellipse((X(sx), X(sy), X(sx + 2 * r), X(sy + 2 * r)), fill=ACCENT if ref == 0 else MUTED)
            elif kind == 4:
                ftype = ref
                f = fnt(FS * 0.5)
                if ftype == 4:
                    d.rounded_rectangle((X(sx), X(sy), X(sx + bw), X(sy + bh)), radius=5 * S, fill=INK)
                    d.text((X(sx + bw / 2), X(sy + bh / 2)), label, font=f, fill=PAPER, anchor="mm")
                else:
                    d.rounded_rectangle((X(sx), X(sy), X(sx + bw), X(sy + bh)), radius=5 * S, fill=FIELD,
                                        outline=HAIR, width=S)
                    d.text((X(sx + 7), X(sy + bh / 2)), label, font=f, fill=MUTED, anchor="lm")
        for x, y, w, h, sc, st, link, txt in runs:
            if y + h < view_top or y > view_top + view_h:
                continue
            sx, sy = x0 + x, offy + y - view_top
            col = INK
            if st & 8:
                col = LINK
            elif st & 16:
                col = MUTED
            hot = link == focus_link
            if hot:
                d.rounded_rectangle((X(sx - 3), X(sy - 1), X(sx + w + 3), X(sy + h + 1)), radius=4 * S, fill=FOCUS)
            elif st & 4:
                d.rectangle((X(sx - 2), X(sy), X(sx + w + 2), X(sy + h)), fill=CODE)
            f = fnt(FS * sc)
            base = sy + h * 0.80
            d.text((X(sx), X(base)), txt, font=f, fill=col, anchor="ls")
            if st & 1:
                d.text((X(sx + 0.6), X(base)), txt, font=f, fill=col, anchor="ls")
            if st & (8 | 128):
                d.rectangle((X(sx), X(sy + h - 2), X(sx + w), X(sy + h - 2 + (1.4 if hot else 0.8))),
                            fill=ACCENT if hot else col)

    draw_slice(top, 0, TOPV, 40, TOPBAR)
    draw_slice(bot, TOPV, BOTV, 0, 0)

    # top bar
    d = ImageDraw.Draw(top)
    X = lambda v: v * S
    d.rectangle((0, 0, X(400), X(TOPBAR)), fill=BAR)
    d.rectangle((0, X(TOPBAR - 1), X(400), X(TOPBAR)), fill=HAIR)
    # padlock
    d.ellipse((X(11 + 0.4), X(5 + 0.4), X(11 + 7.6), X(5 + 7.6)), fill=MUTED)
    d.ellipse((X(11 + 2), X(5 + 2), X(11 + 6), X(5 + 6)), fill=BAR)
    d.rounded_rectangle((X(11), X(10), X(19), X(17)), radius=int(1.5 * S), fill=MUTED)
    f = fnt(15)
    d.text((X(25), X(TOPBAR / 2)), title, font=f, fill=INK, anchor="lm")
    tw = d.textlength(title, font=f) / S
    d.text((X(25 + tw + 8), X(TOPBAR / 2 + 0.5)), "example.com", font=fnt(13.5), fill=MUTED, anchor="lm")
    d.text((X(390), X(TOPBAR / 2)), "4:21", font=f, fill=INK, anchor="rm")
    cw = d.textlength("4:21", font=f) / S
    wx = 400 - 18 - cw - 14
    for i in range(3):
        bh = 4 + i * 3
        d.rectangle((X(wx + i * 4), X(16 - bh), X(wx + i * 4 + 3), X(16)), fill=INK)

    # toolbar
    d = ImageDraw.Draw(bot)
    y0 = 240 - TOOLBAR
    cy = y0 + TOOLBAR / 2
    d.rectangle((0, X(y0), X(320), X(240)), fill=BAR)
    d.rectangle((0, X(y0), X(320), X(y0 + 1)), fill=HAIR)

    def chevron(cx, dirn, col):
        dd = 2.5 * dirn
        d.line([(X(cx + dd), X(cy - 6)), (X(cx - dd), X(cy)), (X(cx + dd), X(cy + 6))], fill=col, width=2 * S)

    chevron(20, 1, INK)
    chevron(60, -1, HAIR)
    # reload arc
    r = 6.5
    d.arc((X(100 - r), X(cy - r), X(100 + r), X(cy + r)), start=-34, end=246, fill=INK, width=int(1.8 * S))
    ex, ey = 100 + math.cos(-0.6) * r, cy + math.sin(-0.6) * r
    d.polygon([(X(ex - 4), X(ey - 1)), (X(ex + 3), X(ey - 2)), (X(ex), X(ey + 4))], fill=INK)
    d.rounded_rectangle((X(124), X(y0 + 6), X(244), X(y0 + 28)), radius=11 * S, fill=FIELD, outline=HAIR, width=S)
    d.text((X(184), X(y0 + 17)), "example.com", font=fnt(13.5), fill=INK, anchor="mm")
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rr = 3.4 if i % 2 else 8.0
        pts.append((X(266 + math.cos(a) * rr), X(cy + 0.5 + math.sin(a) * rr)))
    d.polygon(pts, fill=ACCENT)
    for i in (-1, 0, 1):
        d.rectangle((X(295), X(cy + i * 5 - 1), X(309), X(cy + i * 5 + 1)), fill=INK)

    def paste_rounded(img, x, y):
        mask = Image.new("L", img.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, img.size[0] - 1, img.size[1] - 1), radius=6 * S, fill=255)
        canvas.paste(img, (x, y), mask)

    paste_rounded(top, pad * S, pad * S)
    paste_rounded(bot, (pad + 40) * S, (pad + 240 + gap) * S)
    canvas.save(os.path.join(ROOT, "preview.png"))


def main():
    os.makedirs(ASSETS, exist_ok=True)
    make_icon()
    make_banner()
    make_wav()
    make_qr()
    if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
        make_preview(sys.argv[1])
    print("assets written")


if __name__ == "__main__":
    main()
