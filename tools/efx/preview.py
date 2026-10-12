"""Animated GIF preview of an .efx file: a simple stand-in eye pair with the effect on top.

Mirrors the keyframe maths of cat_eyes_efx.c (interpolation, easing, loop/hold). The eye itself is only
a schematic (the real renderer draws the shapes over the live eye), so judge timing, position and colour.
"""
from __future__ import annotations

import math
import struct
from pathlib import Path

from PIL import Image, ImageDraw

from .compiler import GAZE_SZ, HDR, KEY_SZ, SHAPE_HDR, check

FX_OFF = 296


def _ease(mode: int, u: float) -> float:
    u = min(max(u, 0.0), 1.0)
    if mode == 1:
        return u * u * (3 - 2 * u)
    if mode == 2:
        return u * u
    if mode == 3:
        return 1 - (1 - u) ** 2
    if mode == 4:
        v = u - 1
        return 1 + 2.70158 * v**3 + 1.70158 * v * v
    if mode == 5:
        return 1.0 if u >= 1 else 0.0
    return u


def _c565(c: int) -> tuple[int, int, int]:
    return (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3


def _mix(a: int, b: int, u: float) -> tuple[int, int, int]:
    ca, cb = _c565(a), _c565(b)
    return (round(ca[0] + (cb[0] - ca[0]) * u), round(ca[1] + (cb[1] - ca[1]) * u), round(ca[2] + (cb[2] - ca[2]) * u))


def _shapes(data: bytes):
    ns, ng = data[8], data[9]
    p = HDR + ng * GAZE_SZ
    for _ in range(ns):
        kind, mask, fl, aux, nk = struct.unpack_from("<BBBBB", data, p)
        p += SHAPE_HDR
        keys = [struct.unpack_from("<HhhhH", data, p + i * KEY_SZ) for i in range(nk)]
        p += nk * KEY_SZ
        yield kind, mask, fl, aux, keys


def _state(keys, fl: int, t: float):
    t0, tl = keys[0][0], keys[-1][0]
    if t < t0:
        return None
    if t > tl:
        if fl & 1:
            t = t0 + math.fmod(t - t0, tl - t0) if tl > t0 else t0
        elif fl & 2:
            t = tl
        else:
            return None
    i = 0
    while i < len(keys) - 2 and t >= keys[i + 1][0]:
        i += 1
    a = keys[i]
    b = keys[i + 1] if len(keys) > 1 else a
    u = _ease(fl >> 4, (t - a[0]) / (b[0] - a[0])) if len(keys) > 1 else 0.0
    return (
        a[1] + (b[1] - a[1]) * u,
        a[2] + (b[2] - a[2]) * u,
        a[3] + (b[3] - a[3]) * u,
        _mix(a[4], b[4], min(max(u, 0.0), 1.0)),
    )


def _draw_shape(d: ImageDraw.ImageDraw, kind: int, aux: int, x: float, y: float, r: float, col) -> None:
    ang = aux * 2 * math.pi / 256
    if kind == 0:
        d.ellipse([x - r, y - r, x + r, y + r], fill=col)
    elif kind == 1:
        d.ellipse([x - r, y - r * aux / 32, x + r, y + r * aux / 32], fill=col)
    elif kind == 2:
        d.rectangle([x - r, y - r * aux / 32, x + r, y + r * aux / 32], fill=col)
    elif kind == 3:
        d.ellipse([x - r, y - r, x + r, y + r], fill=col)
        ri = r * aux / 255
        d.ellipse([x - ri, y - ri, x + ri, y + ri], fill=(18, 18, 22))
    elif kind == 4:
        d.ellipse([x - r, y - r * 0.75, x, y + r * 0.25], fill=col)
        d.ellipse([x, y - r * 0.75, x + r, y + r * 0.25], fill=col)
        d.polygon([(x - r * 0.97, y - r * 0.1), (x + r * 0.97, y - r * 0.1), (x, y + r * 0.95)], fill=col)
    elif kind == 5:
        d.line([x, y - r, x, y + r], fill=col, width=2)
        d.line([x - r, y, x + r, y], fill=col, width=2)
        if r > 7:
            d.ellipse([x - r * 0.28, y - r * 0.28, x + r * 0.28, y + r * 0.28], fill=col)
    elif kind == 6:
        d.rectangle([x - 2.5 * r, y - 3.5 * r, x + 2.5 * r, y + 3.5 * r], outline=col, width=max(1, int(r)))
    elif kind == 7:
        d.line([x - math.cos(ang) * r, y - math.sin(ang) * r, x + math.cos(ang) * r, y + math.sin(ang) * r], fill=col, width=3)
    else:
        pts = [(x + math.cos(ang + k * 2.0944 - 1.5708) * r, y + math.sin(ang + k * 2.0944 - 1.5708) * r) for k in range(3)]
        d.polygon(pts, fill=col)


def render_gif(data: bytes, out: Path, fps: int = 20, scale: float = 0.75) -> Path:
    """Write `out` (a looping GIF, both eyes side by side) and return it."""
    info = check(data)
    shapes = list(_shapes(data))
    frames = []
    n = max(2, int(info["duration"] * fps / 1000))
    size = int(240 * scale)
    for fi in range(n):
        t = fi * 1000 / fps
        im = Image.new("RGB", (size * 2 + 8, size), (0, 0, 0))
        for eye in (0, 1):
            tile = Image.new("RGB", (240, 240), (18, 18, 22))
            d = ImageDraw.Draw(tile)
            d.ellipse([16, 50, 224, 190], fill=(236, 226, 205))
            d.ellipse([78, 56, 162, 180], fill=(222, 160, 40))
            d.ellipse([112, 70, 128, 166], fill=(10, 10, 10))
            for kind, mask, fl, aux, keys in shapes:
                if not (mask >> eye) & 1:
                    continue
                st = _state(keys, fl, t)
                if not st or st[2] < 1:
                    continue
                x, y, r, col = st
                if mask & 4 and eye:
                    x -= FX_OFF
                _draw_shape(d, kind, aux, x, y, r, col)
            im.paste(tile.resize((size, size)), (eye * (size + 8), 0))
        frames.append(im)
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=int(1000 / fps), loop=0)
    return Path(out)
