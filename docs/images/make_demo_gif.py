"""Render docs/images/demo.gif: the idle eyes going through their reactions, using the
same shapes, easing factors and numbers as firmware/src/main.cpp (an illustration,
not a screen recording). Usage: uv run --with pillow python docs/images/make_demo_gif.py
"""

import math
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
OUT = HERE / "demo.gif"

W = 468            # firmware screen size
OUT_SIZE = 320     # gif size
SS = 2             # supersampling for smooth edges
FPS = 20
EYE_W, EYE_H, EYE_GAP, EYE_R = 78, 118, 150, 37
EYE_LOOK_X, EYE_LOOK_Y = 110, 100
CX = CY = W / 2

# timeline (seconds): what the "owner" does
T_IDLE, T_TOUCH, T_PET, T_SPIN, T_SLEEP, T_WAKE = 0.0, 2.6, 6.0, 8.6, 11.6, 14.4
END = 16.0


def finger(t: float):
    """Where the finger is (screen coords) at time t, or None."""
    if T_TOUCH <= t < T_TOUCH + 3.0:  # drag right, then down-left, then up
        u = (t - T_TOUCH) / 3.0
        pts = [(CX, CY), (CX + 180, CY - 20), (CX - 140, CY + 120), (CX - 50, CY - 170)]
        k = min(int(u * 3), 2)
        v = u * 3 - k
        v = v * v * (3 - 2 * v)
        (x0, y0), (x1, y1) = pts[k], pts[k + 1]
        return x0 + (x1 - x0) * v, y0 + (y1 - y0) * v
    if T_PET <= t < T_PET + 1.4:  # stroke the top, left-right
        u = (t - T_PET) / 1.4
        return CX + math.sin(u * 2 * math.pi * 2) * 90, CY - 150
    if T_SPIN <= t < T_SPIN + 1.6:  # two circles round the centre
        a = (t - T_SPIN) / 1.6 * 2 * 2 * math.pi
        return CX + math.cos(a) * 130, CY + math.sin(a) * 130
    if T_WAKE - 0.4 <= t < T_WAKE - 0.15:  # a tap wakes her up
        return CX + 40, CY + 60
    return None


def mood_at(t: float):
    if T_PET + 1.2 <= t < T_PET + 1.2 + 3.0 - 0.6:
        return "happy"
    if T_SPIN + 1.5 <= t < T_SLEEP - 0.2:
        return "dizzy"
    return None


def render_frames():
    frames = []
    x = y = tx = ty = 0.0
    sleep_amt = 0.0
    glances = [(0.25, 0.0, -0.08), (1.3, -0.3, 0.1), (15.2, 0.3, -0.05)]
    blinks = [1.0, 2.2, 5.2, 9.3, T_WAKE + 0.35, 15.6]
    n = int(END * FPS)
    for i in range(n):
        t = i / FPS
        now_ms = t * 1000
        f = finger(t)
        asleep = T_SLEEP <= t < T_WAKE
        if f is not None and not asleep:
            tx = max(-1, min(1, (f[0] - CX) / (W / 2)))
            ty = max(-1, min(1, (f[1] - CY) / (W / 2)))
            k = 0.35
        else:
            k = 0.18
            for gt, gx, gy in glances:
                if t >= gt:
                    tx, ty = gx, gy
            if T_TOUCH + 3.0 <= t < T_PET or T_PET + 1.4 <= t < T_SPIN or T_SPIN + 1.6 <= t < T_SLEEP:
                tx, ty = 0.0, 0.0
            if asleep:
                tx, ty, k = 0.0, 0.35, 0.05
        x += (tx - x) * k
        y += (ty - y) * k
        sleep_amt += ((1.0 if asleep else 0.0) - sleep_amt) * (0.04 * 3 if asleep else 0.15 * 3)  # firmware runs ~3x the frames
        sleep_amt = max(0.0, min(1.0, sleep_amt))
        open_ = 1.0
        for b in blinks:
            if b <= t < b + 0.17:
                open_ = abs((t - b) / 0.17 * 2 - 1)
        open_ *= 1 - sleep_amt
        frames.append(draw(now_ms, x, y, open_, sleep_amt, mood_at(t), f if not asleep or t >= T_WAKE - 0.4 else None))
    return frames


def draw(now, gx, gy, open_, sleep_amt, mood, f):
    S = lambda v: v * SS
    im = Image.new("RGB", (W * SS, W * SS), (0, 0, 0))
    d = ImageDraw.Draw(im)

    def dot(cx, cy, r, c):
        d.ellipse((S(cx - r), S(cy - r), S(cx + r), S(cy + r)), fill=c)

    breath = sleep_amt * math.sin(now / 900) * 4
    ox, oy = gx * EYE_LOOK_X, gy * EYE_LOOK_Y + breath
    if mood == "happy":
        oy += math.sin(now / 700) * 3
    for side in (-1, 1):
        persp = 1 - side * gx * 0.15
        tilt = 1 - abs(gy) * 0.15
        ew = EYE_W * persp
        eh = max(EYE_H * persp * tilt * open_, 8)
        ex, ey = CX + side * EYE_GAP / 2 + ox, CY + oy
        if mood == "dizzy":
            turns, r0, r1 = 2.5, 4, 52
            n = 260
            for i in range(n + 1):
                u = i / n
                a = u * turns * 2 * math.pi + now / 260
                r = r0 + (r1 - r0) * u
                dot(ex + math.cos(a) * r, ey + math.sin(a) * r, 4.2, "white")
            continue
        d.rounded_rectangle((S(ex - ew / 2), S(ey - eh / 2), S(ex + ew / 2), S(ey + eh / 2)),
                            radius=S(min(EYE_R, eh / 2)), fill="white")
        if mood == "happy":
            bottom, rc = ey + eh / 2, ew * 1.35
            dot(ex, bottom - EYE_H * 0.42 + rc, rc, "black")
            d.ellipse((S(ex + side * 22 - 26), S(bottom - 8), S(ex + side * 22 + 26), S(bottom + 20)), fill=(140, 66, 82))
    if mood == "dizzy":
        for i in range(3):
            a = now / 450 + i * 2 * math.pi / 3
            depth = (math.sin(a) + 1) / 2
            x, y = CX + math.cos(a) * 120, CY - 118 + math.sin(a) * 22
            r = 13 + 5 * depth
            k = int(200 + 55 * depth)
            body, beak = (k, k * 210 // 255, 40 * k // 255), (k, k * 120 // 255, 0)
            s = 1 if math.cos(a + math.pi / 2) > 0 else -1
            dot(x, y, r, body)
            dot(x + s * 0.45 * r, y - r * 0.55, r * 0.62, body)
            d.polygon([(S(x + s * r), S(y - r * 0.6)), (S(x + s * 1.45 * r), S(y - r * 0.45)), (S(x + s * r), S(y - r * 0.3))], fill=beak)
            dot(x + s * 0.62 * r, y - r * 0.7, max(1.5, r * 0.12), "black")
    if sleep_amt > 0.9:
        for i in range(3):
            u = (now / 4500 + i / 3) % 1.0
            g = int(200 * (1 - u))
            size = 24 * (1.5 + u * 1.3)  # same path and growth as the firmware
            zx, zy = CX + EYE_GAP / 2 + 50 + u * 70, CY - 70 - u * 110
            # a "Z" drawn as three strokes so no font is needed
            h = size * 0.5
            pts = [(zx - h / 2, zy - h / 2), (zx + h / 2, zy - h / 2), (zx - h / 2, zy + h / 2), (zx + h / 2, zy + h / 2)]
            d.line([(S(px), S(py)) for px, py in pts], fill=(g, g, g), width=int(S(size / 9)), joint="curve")
    if f is not None:  # the finger, as a soft ring
        d.ellipse((S(f[0] - 26), S(f[1] - 26), S(f[0] + 26), S(f[1] + 26)), outline=(90, 185, 255), width=S(4))

    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, im.size[0] - 1, im.size[1] - 1), fill=255)
    out = Image.new("RGB", im.size, (22, 23, 29))  # page-dark around the round screen
    out.paste(im, mask=mask)
    return out.resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)


def main() -> None:
    frames = render_frames()
    # one palette for every frame, chosen from a strip of frames across all the scenes
    # (yellow chicks, pink cheeks, the blue finger ring, grey Zs)
    picks = frames[:: max(1, len(frames) // 24)]
    strip = Image.new("RGB", (OUT_SIZE * len(picks), OUT_SIZE))
    for i, fr in enumerate(picks):
        strip.paste(fr, (i * OUT_SIZE, 0))
    pal = strip.quantize(colors=96, method=Image.Quantize.MEDIANCUT)
    q = [fr.quantize(palette=pal, dither=Image.Dither.NONE) for fr in frames]
    q[0].save(OUT, save_all=True, append_images=q[1:], duration=int(1000 / FPS), loop=0, optimize=True, disposal=1)
    print(f"wrote {OUT}: {len(frames)} frames, {OUT.stat().st_size / 1024:.0f} KiB")


if __name__ == "__main__":
    main()
