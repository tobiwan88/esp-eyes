"""JSON description -> .efx binary (and back), mirroring cat_eyes_efx.c validation."""
from __future__ import annotations

import binascii
import json
import re
import struct
from pathlib import Path
from typing import Any

MAX_BYTES, MAX_SHAPES, MAX_KEYS, MAX_GAZE = 4096, 16, 12, 12
HDR, GAZE_SZ, SHAPE_HDR, KEY_SZ = 16, 6, 5, 10

KINDS = ["circle", "ellipse", "rect", "ring", "heart", "sparkle", "glyph", "line", "triangle"]
EASINGS = ["linear", "smooth", "in", "out", "back", "step"]
GLYPHS = {**{str(i): i for i in range(10)}, "%": 10, "euro": 11, "€": 11, **{f"kana{i}": 12 + i for i in range(8)}}
COLORS = {
    "red": "#ff2020", "pink": "#ff6fa8", "hotpink": "#ff3c8c", "orange": "#ff9a1a", "yellow": "#ffe030",
    "green": "#30d040", "lime": "#a0ff30", "cyan": "#30e0f0", "blue": "#3070ff", "purple": "#9040e0",
    "white": "#ffffff", "black": "#000000", "gold": "#ffc820", "gray": "#808080", "brown": "#8a5a2a",
}
EYES = {"both": 3, "left": 1, "right": 2}


class EfxError(ValueError):
    pass


def rgb565(c: str | int | list[int]) -> int:
    """'#rrggbb', a colour name, or [r,g,b]; same rounding as the firmware's RGB565 macro."""
    if isinstance(c, str):
        c = COLORS.get(c.lower(), c)
        m = re.fullmatch(r"#([0-9a-fA-F]{6})", c)
        if not m:
            raise EfxError(f"bad colour {c!r}: use #rrggbb or one of {', '.join(sorted(COLORS))}")
        v = int(m.group(1), 16)
        r, g, b = v >> 16, (v >> 8) & 255, v & 255
    elif isinstance(c, list) and len(c) == 3:
        r, g, b = c
    else:
        raise EfxError(f"bad colour {c!r}")
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)  # CRC-16/CCITT-FALSE


def _num(d: dict, key: str, default: float | None = None) -> float:
    v = d.get(key, default)
    if not isinstance(v, (int, float)) or isinstance(v, bool):
        raise EfxError(f"{key!r} must be a number")
    return float(v)


def _aux(kind: str, s: dict) -> int:
    if kind in ("ellipse", "rect"):
        v = round(_num(s, "ratio", 1.0) * 32)  # height / width
        if not 1 <= v <= 255:
            raise EfxError("ratio must be 0.03 .. 7.9")
        return v
    if kind == "ring":
        v = round(_num(s, "inner", 0.6) * 255)
        if not 0 <= v <= 255:
            raise EfxError("inner must be 0..1")
        return v
    if kind == "glyph":
        g = str(s.get("glyph", "0"))
        if g not in GLYPHS:
            raise EfxError(f"unknown glyph {g!r}: use 0-9, %, euro, kana0..kana7")
        return GLYPHS[g]
    if kind in ("line", "triangle"):
        return round(_num(s, "angle", 0.0) % 360 / 360 * 256) % 256
    return 0


def compile_efx(doc: dict[str, Any]) -> bytes:
    """Validate a description and return the .efx bytes. Raises EfxError with a readable message."""
    if not isinstance(doc, dict):
        raise EfxError("top level must be an object")
    dur = round(_num(doc, "duration"))
    shapes, gaze = doc.get("shapes"), doc.get("gaze", [])
    if not isinstance(shapes, list) or not 1 <= len(shapes) <= MAX_SHAPES:
        raise EfxError(f"'shapes' needs 1..{MAX_SHAPES} entries")
    if not isinstance(gaze, list) or len(gaze) > MAX_GAZE:
        raise EfxError(f"'gaze' has at most {MAX_GAZE} keys")
    if not 200 <= dur <= 30000:
        raise EfxError("duration must be 200..30000 ms")
    out = bytearray()
    last = -1
    for g in gaze:
        t = round(_num(g, "t"))
        if t <= last or t > 65535:
            raise EfxError("gaze key times must increase")
        last = t
        gx, gy = _num(g, "x", 0), _num(g, "y", 0)
        if not (-1 <= gx <= 1 and -1 <= gy <= 1):
            raise EfxError("gaze x/y must be -1..1")
        pupil = round(_num(g, "pupil", 0) * 255) if "pupil" in g else 0   # 0 = default
        lid = 1 + round(_num(g, "lid") * 254) if "lid" in g else 0        # 0 = default; 0 open .. 1 closed
        if not (0 <= pupil <= 255 and 0 <= lid <= 255):
            raise EfxError("pupil and lid must be 0..1")
        out += struct.pack("<HbbBB", t, round(gx * 127), round(gy * 127), pupil, lid)
    for n, s in enumerate(shapes, 1):
        w = f"shape {n}"
        kind = s.get("kind")
        if kind not in KINDS:
            raise EfxError(f"{w}: kind must be one of {', '.join(KINDS)}")
        eyes = s.get("eyes", "both")
        if eyes not in EYES:
            raise EfxError(f"{w}: eyes must be both, left or right")
        easing = s.get("easing", "smooth")
        if easing not in EASINGS:
            raise EfxError(f"{w}: easing must be one of {', '.join(EASINGS)}")
        keys = s.get("keys")
        if not isinstance(keys, list) or not 1 <= len(keys) <= MAX_KEYS:
            raise EfxError(f"{w}: 'keys' needs 1..{MAX_KEYS} entries")
        mask = EYES[eyes] | (4 if s.get("world") else 0)
        flags = (1 if s.get("loop") else 0) | (2 if s.get("hold") else 0) | (EASINGS.index(easing) << 4)
        try:
            out += struct.pack("<BBBBB", KINDS.index(kind), mask, flags, _aux(kind, s), len(keys))
            lt = -1
            for k in keys:
                t = round(_num(k, "t"))
                if t <= lt or t > 65535:
                    raise EfxError(f"{w}: key times must increase")
                lt = t
                x, y, size = round(_num(k, "x")), round(_num(k, "y")), round(_num(k, "size"))
                if not (-1000 <= x <= 1000 and -1000 <= y <= 1000 and 0 <= size <= 400):
                    raise EfxError(f"{w}: x/y within -1000..1000, size within 0..400")
                # key colour overrides the shape colour, which overrides white
                out += struct.pack("<HhhhH", t, x, y, size, rgb565(k.get("color", s.get("color", "white"))))
        except EfxError:
            raise
        except (TypeError, struct.error) as e:
            raise EfxError(f"{w}: {e}") from e
    flags = 1 if doc.get("mirror") else 0
    data = struct.pack("<4sBBHBBHHH", b"EFX1", 1, flags, dur, len(shapes), len(gaze), len(out), crc16(bytes(out)), 0) + bytes(out)
    if len(data) > MAX_BYTES:
        raise EfxError(f"file too big: {len(data)} bytes (max {MAX_BYTES}); use fewer shapes or keys")
    check(data)
    return data


def load_any(data: bytes) -> bytes:
    """Bytes of either an .efx binary or a JSON description (UTF-8, optional BOM); the name is irrelevant.

    Returns validated .efx bytes. A binary is checked as-is; JSON is compiled."""
    if data[:4] == b"EFX1":
        check(data)
        return data
    try:
        doc = json.loads(data.decode("utf-8-sig").strip())
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise EfxError("not an .efx file and not valid JSON") from e
    if not isinstance(doc, dict):
        raise EfxError("not an .efx file and not valid JSON")
    return compile_efx(doc)


def check(data: bytes) -> dict[str, Any]:
    """Mirror of cat_eyes_efx_check(): raises EfxError, else returns the header summary."""
    if len(data) < HDR or data[:4] != b"EFX1":
        raise EfxError("not an .efx file (bad header)")
    if len(data) > MAX_BYTES:
        raise EfxError("file length does not match header")
    _, ver, flags, dur, ns, ng, plen, crc, _res = struct.unpack("<4sBBHBBHHH", data[:HDR])
    if ver != 1:
        raise EfxError("unsupported efx version")
    if len(data) != HDR + plen:
        raise EfxError("file length does not match header")
    if crc16(data[HDR:]) != crc:
        raise EfxError("checksum mismatch")
    if not (200 <= dur <= 30000 and 1 <= ns <= MAX_SHAPES and ng <= MAX_GAZE):
        raise EfxError("bad shape or gaze count")
    p = HDR
    last = -1
    for _ in range(ng):
        t = struct.unpack_from("<H", data, p)[0]
        if t <= last:
            raise EfxError("key times must increase")
        last, p = t, p + GAZE_SZ
    for _ in range(ns):
        kind, mask, fl, aux, nk = struct.unpack_from("<BBBBB", data, p)
        if kind > 8 or mask & 3 == 0 or mask & ~7 or fl >> 4 > 5 or (kind == 6 and aux >= 20):
            raise EfxError("bad shape field")
        if not 1 <= nk <= MAX_KEYS:
            raise EfxError("bad shape or gaze count")
        p += SHAPE_HDR
        lt = -1
        for _ in range(nk):
            t, x, y, s, _c = struct.unpack_from("<HhhhH", data, p)
            if t <= lt:
                raise EfxError("key times must increase")
            if not (-1000 <= x <= 1000 and -1000 <= y <= 1000 and 0 <= s <= 400):
                raise EfxError("value out of range")
            lt, p = t, p + KEY_SZ
    if p != len(data):
        raise EfxError("file length does not match header")
    return {"duration": dur, "shapes": ns, "gaze_keys": ng, "bytes": len(data), "mirror": bool(flags & 1)}


def decode(data: bytes) -> dict[str, Any]:
    """Binary -> the JSON description (lossy only in rounding)."""
    info = check(data)
    _, _, _flags, dur, ns, ng, *_ = struct.unpack("<4sBBHBBHHH", data[:HDR])
    p = HDR
    gaze = []
    for _ in range(ng):
        t, gx, gy, pu, lid = struct.unpack_from("<HbbBB", data, p)
        g: dict[str, Any] = {"t": t, "x": round(gx / 127, 3), "y": round(gy / 127, 3)}
        if pu:
            g["pupil"] = round(pu / 255, 3)
        if lid:
            g["lid"] = round((lid - 1) / 254, 3)
        gaze.append(g)
        p += GAZE_SZ
    shapes = []
    for _ in range(ns):
        kind, mask, fl, aux, nk = struct.unpack_from("<BBBBB", data, p)
        p += SHAPE_HDR
        s: dict[str, Any] = {"kind": KINDS[kind], "easing": EASINGS[fl >> 4]}
        if mask & 3 != 3:
            s["eyes"] = "left" if mask & 1 else "right"
        if mask & 4:
            s["world"] = True
        if fl & 1:
            s["loop"] = True
        if fl & 2:
            s["hold"] = True
        if kind in (1, 2):
            s["ratio"] = aux / 32
        elif kind == 3:
            s["inner"] = round(aux / 255, 3)
        elif kind == 6:
            s["glyph"] = next(k for k, v in GLYPHS.items() if v == aux and k != "€")
        elif kind in (7, 8):
            s["angle"] = round(aux / 256 * 360, 1)
        keys = []
        for _ in range(nk):
            t, x, y, size, c = struct.unpack_from("<HhhhH", data, p)
            r, g_, b = (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3
            keys.append({"t": t, "x": x, "y": y, "size": size, "color": f"#{r:02x}{g_:02x}{b:02x}"})
            p += KEY_SZ
        s["keys"] = keys
        shapes.append(s)
    doc: dict[str, Any] = {"duration": dur, "shapes": shapes}
    if gaze:
        doc["gaze"] = gaze
    if info["mirror"]:
        doc["mirror"] = True
    return doc


def compile_file(path: Path) -> bytes:
    try:
        doc = json.loads(Path(path).read_text())
    except json.JSONDecodeError as e:
        raise EfxError(f"{path}: invalid JSON ({e})") from e
    return compile_efx(doc)


def slug(name: str) -> str:
    s = re.sub(r"[^a-z0-9_-]", "_", name.lower())[:31]
    return s or "effect"


__all__ = ["EfxError", "check", "compile_efx", "compile_file", "crc16", "decode", "load_any", "rgb565", "slug"]
