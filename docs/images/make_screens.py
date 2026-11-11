"""Render docs/images/screens.png, expressions.png and conversation.png: the round screen
in several states, drawn with the
same shapes and numbers as firmware/src/main.cpp (an illustration, not a screenshot).
Needs the font from the README setup. Usage: uv run --with pillow python docs/images/make_screens.py
"""
import math
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

HERE = Path(__file__).resolve().parent
FONT = str(HERE.parents[1] / "firmware" / "fonts" / "NotoSansJP-Medium.otf")
SS = 2
W = 468; CX = CY = 234
EW, EH, GAP, R = 78, 118, 150, 37
def f(sz): return ImageFont.truetype(FONT, sz * SS)
def screen(state):
    im = Image.new("RGB", (W * SS, W * SS), (0, 0, 0)); d = ImageDraw.Draw(im)
    S = lambda v: v * SS
    def rr(x0, y0, x1, y1, r, c="white"): d.rounded_rectangle((S(x0), S(y0), S(x1), S(y1)), radius=S(r), fill=c)
    def dot(x, y, r, c): d.ellipse((S(x - r), S(y - r), S(x + r), S(y + r)), fill=c)
    def eyes(cy, gx=0, gy=0, scale=1.0, open_=1.0, mood=None):
        for side in (-1, 1):
            persp = 1 - side * gx * 0.15; tilt = 1 - abs(gy) * 0.15
            ew = EW * scale * persp; eh = max(EH * scale * persp * tilt * open_, 8 * scale)
            ex = CX + side * GAP * scale / 2 + gx * 110 * scale; ey = cy + gy * 100 * scale
            if mood == "dizzy":
                n = 300
                for i in range(n + 1):
                    t = i / n; a = t * 2.5 * 2 * math.pi + 1.2; r = 4 + 48 * t
                    dot(ex + math.cos(a) * r, ey + math.sin(a) * r, 4.2, "white")
                continue
            rr(ex - ew / 2, ey - eh / 2, ex + ew / 2, ey + eh / 2, min(R * scale, eh / 2))
            if mood == "happy":
                bottom = ey + eh / 2; Rc = ew * 1.35
                dot(ex, bottom - EH * 0.42 + Rc, Rc, "black")
                d.ellipse((S(ex + side * 22 - 26), S(bottom + 6 - 14), S(ex + side * 22 + 26), S(bottom + 6 + 14)), fill=(140, 66, 82))
    def text(x, y, s, sz, c="white", anchor="mm"): d.text((S(x), S(y)), s, font=f(sz), fill=c, anchor=anchor)
    if state == "idle":
        eyes(CY, gx=0.25, gy=-0.1)
    elif state == "listening":
        ey = CY - 45; eyes(ey, scale=0.85)
        y = ey + EH / 2 + 6
        d.rounded_rectangle((S(CX - 80), S(y), S(CX + 80), S(y + 6)), radius=S(3), fill=(32, 32, 32))
        d.rounded_rectangle((S(CX - 80), S(y), S(CX + 30), S(y + 6)), radius=S(3), fill=(255, 200, 0))
        text(CX, ey + EH / 2 + 50, "聞き取り中", 24)
        text(CX, ey + EH / 2 + 88, "明日の天気は？", 24, (150, 165, 255))
    elif state == "thinking":
        ey = CY - 45; eyes(ey, gx=0.5, gy=-0.8, scale=0.85)
        text(CX, ey + EH / 2 + 50, "考え中…", 24)
        text(CX, ey + EH / 2 + 88, "明日の天気は？", 24, (150, 165, 255))
    elif state == "reply":
        eyes(44, scale=0.2)
        text(CX + 168, 150, "明日の天気は？", 24, (150, 165, 255), "rm")
        for i, line in enumerate(["明日は晴れ。最高15度、", "最低7度で、降水確率は", "0%だよ。"]):
            text(CX - 168, 196 + i * 34, line, 24, "white", "lm")
    elif state == "happy":
        eyes(CY, mood="happy")
    elif state == "dizzy":
        eyes(CY, mood="dizzy")
        for i, a in enumerate([0.6, 2.7, 4.8]):
            depth = (math.sin(a) + 1) / 2; x = CX + math.cos(a) * 120; y = CY - 118 + math.sin(a) * 22; r = 13 + 5 * depth
            k = int(200 + 55 * depth); body = (k, k * 210 // 255, 40 * k // 255); right = math.cos(a + math.pi / 2) > 0; s = 1 if right else -1
            dot(x, y, r, body); dot(x + s * 0.45 * r, y - r * 0.55, r * 0.62, body)
            d.polygon([(S(x + s * r), S(y - r * 0.6)), (S(x + s * 1.45 * r), S(y - r * 0.45)), (S(x + s * r), S(y - r * 0.3))], fill=(k, k * 120 // 255, 0))
            dot(x + s * 0.62 * r, y - r * 0.7, max(1.5, r * 0.12), "black")
    elif state == "asleep":
        eyes(CY + 20, open_=0.0)
        for x, y, sz, g in [(CX + 72, CY - 30, 30, 200), (CX + 105, CY - 78, 44, 150), (CX + 132, CY - 135, 58, 100)]:
            text(x, y, "Z", sz, (g, g, g))
    # round mask
    mask = Image.new("L", im.size, 0); ImageDraw.Draw(mask).ellipse((0, 0, im.size[0] - 1, im.size[1] - 1), fill=255)
    out = Image.new("RGBA", im.size, (0, 0, 0, 0)); out.paste(im, mask=mask)
    return out.resize((W, W), Image.LANCZOS)

def grid(states, cols, out_name):
    tile, pad, label_h = 300, 28, 48
    rows = (len(states) + cols - 1) // cols
    Wimg = cols * tile + (cols + 1) * pad
    Himg = rows * (tile + label_h) + (rows + 1) * pad
    bg = Image.new("RGB", (Wimg, Himg), (22, 23, 29))
    d = ImageDraw.Draw(bg)
    lab = ImageFont.truetype(FONT, 24)
    for i, (st, name) in enumerate(states):
        c, r = i % cols, i // cols
        x = pad + c * (tile + pad)
        y = pad + r * (tile + label_h + pad)
        bez = Image.new("RGBA", (tile, tile), (0, 0, 0, 0))
        ImageDraw.Draw(bez).ellipse((0, 0, tile - 1, tile - 1), fill=(48, 50, 60))
        bg.paste(bez, (x, y), bez)
        scr = screen(st).resize((tile - 20, tile - 20), Image.LANCZOS)
        bg.paste(scr, (x + 10, y + 10), scr)
        d.text((x + tile / 2, y + tile + label_h / 2 + 4), name, font=lab, fill=(200, 202, 212), anchor="mm")
    bg.save(HERE / out_name, optimize=True)
    print(out_name, bg.size)


grid([("idle", "Idle"), ("listening", "Listening"), ("reply", "Reply"),
      ("happy", "Stroked"), ("dizzy", "Circled"), ("asleep", "Asleep")], 3, "screens.png")
grid([("idle", "Idle"), ("happy", "Stroked"), ("dizzy", "Circled"), ("asleep", "Asleep")], 4, "expressions.png")
grid([("listening", "Listening"), ("thinking", "Thinking"), ("reply", "Reply")], 3, "conversation.png")
