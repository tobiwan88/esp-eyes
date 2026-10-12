"""The browser compiler (firmware/components/web/ui/efx_compile.js) must match tools/efx/compiler.py exactly.

Node runs the JS side once per test with a batch of requests on stdin; the Python side computes the expected
bytes and error strings with the same inputs.
"""
import base64
import copy
import json
import shutil
import subprocess
from pathlib import Path

import pytest
from efx import EfxError, check, compile_efx, load_any
from efx.compiler import slug

ROOT = Path(__file__).resolve().parents[2]
JS = ROOT / "firmware" / "components" / "web" / "ui" / "efx_compile.js"
EXAMPLES = ROOT / "examples" / "efx"

pytestmark = pytest.mark.skipif(shutil.which("node") is None, reason="node not on PATH")

DRIVER = r"""
const E = require(process.argv[1]);
const reqs = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const hex = (u8) => Buffer.from(u8).toString('hex');
const out = reqs.map((c) => {
  try {
    if (c.op === 'compile') return { hex: hex(E.compile(c.doc)) };
    if (c.op === 'from') {
      const r = E.fromBytes(Buffer.from(c.b64, 'base64'));
      return { hex: hex(r.bytes), converted: r.converted, info: r.info };
    }
    if (c.op === 'check') return { info: E.check(Buffer.from(c.hex, 'hex')) };
    if (c.op === 'name') return { name: E.suggestName(c.doc, c.filename) };
    throw new Error('bad op');
  } catch (e) {
    return { error: e.message };
  }
});
process.stdout.write(JSON.stringify(out));
"""


def run_js(reqs):
    res = subprocess.run(["node", "-e", DRIVER, str(JS)], input=json.dumps(reqs), capture_output=True,
                         text=True, timeout=120)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def py_compile(doc):
    try:
        return {"hex": compile_efx(doc).hex()}
    except EfxError as e:
        return {"error": str(e)}


def py_from(raw):
    try:
        data = load_any(raw)
    except EfxError as e:
        return {"error": str(e)}
    return {"hex": data.hex(), "converted": raw[:4] != b"EFX1", "info": check(data)}


def py_check(data):
    try:
        return {"info": check(data)}
    except EfxError as e:
        return {"error": str(e)}


def py_name(doc, filename):
    name = doc.get("name")
    if isinstance(name, str) and name.strip():
        return slug(name)
    return slug(Path(filename).stem) if filename else "effect"


def shape(kind="circle", keys=None, **extra):
    s = {"kind": kind, "keys": keys if keys is not None else [{"t": 0, "x": 0, "y": 0, "size": 10}]}
    s.update(extra)
    return s


def key(t, x=0, y=0, size=10, **extra):
    k = {"t": t, "x": x, "y": y, "size": size}
    k.update(extra)
    return k


# Valid documents that exercise rounding, ranges, colour rules and the limits.
EDGE = {
    "negative_angles": {"duration": 1000, "shapes": [
        shape("line", angle=-90), shape("triangle", angle=-1), shape("line", angle=720.5),
        shape("line", angle=-0.0001), shape("triangle", angle=359.999)]},
    "half_rounding": {"duration": 1001.5, "shapes": [
        shape("ellipse", ratio=0.046875, keys=[key(0.5, x=-2.5, y=3.5, size=2.5), key(3.5, size=3.5),
                                                key(5.5, size=0.5)]),
        shape("rect", ratio=0.078125),
        shape("ring", inner=0.5),
        shape("line", angle=45.5)]},
    "gaze_pupil_lid": {"duration": 3000, "gaze": [
        {"t": 0, "x": -1, "y": 1, "pupil": 0.5, "lid": 0.25},
        {"t": 100, "pupil": 0.0, "lid": 1.0},
        {"t": 200, "x": 0.5, "y": -0.5, "pupil": 1.0},
        {"t": 300},
        {"t": 400, "lid": 0.0},
        {"t": 500, "x": 0.3333, "y": -0.7777}],
        "shapes": [shape()]},
    "glyphs": {"duration": 2000, "shapes": [
        shape("glyph", glyph="€"), shape("glyph", glyph="euro"), shape("glyph", glyph="%"),
        shape("glyph", glyph="kana7"), shape("glyph", glyph=7), shape("glyph", glyph="0"),
        shape("glyph", glyph="kana0"), shape("glyph")]},
    "all_kinds_and_easings": {"duration": 4000, "shapes": [
        shape(kind, eyes=eyes, easing=easing, loop=loop, hold=hold, world=world)
        for kind, eyes, easing, loop, hold, world in [
            ("circle", "left", "linear", True, False, True),
            ("ellipse", "right", "smooth", False, True, False),
            ("rect", "both", "in", True, True, True),
            ("ring", "left", "out", False, False, False),
            ("heart", "right", "back", True, False, False),
            ("sparkle", "both", "step", False, True, True),
            ("line", "left", "linear", True, True, True),
            ("triangle", "right", "smooth", False, False, False),
        ]]},
    "shape_colour_default": {"duration": 1000, "shapes": [
        shape(color="gold", keys=[key(0), key(500, color="#FF8000")]),
        shape(keys=[key(0, color="PINK"), key(500, color="Red")]),
        shape(color="hotpink", keys=[key(0, color=[255, 128, 0]), key(500)]),
        shape(color="cyan", keys=[key(0, color="green")]),
        shape(color="not_a_colour_but_unused", keys=[key(0, color="blue")]),
    ]},
    "sixteen_shapes_twelve_keys": {"duration": 30000, "shapes": [
        shape("heart", keys=[key(t * 100, x=t * 10 - 50, y=-t, size=20 + t, color="red") for t in range(12)],
              world=True, loop=True) for _ in range(16)]},
    "mirror_truthiness": {"duration": 1000, "mirror": "no", "shapes": [shape(world={}, loop=[])]},
    "mirror_empty_list": {"duration": 1000, "mirror": [], "shapes": [shape(world=[1])]},
    "mirror_true": {"duration": 1000, "mirror": True, "shapes": [shape()]},
    "extra_fields_ignored": {"duration": 1000, "name": "x", "description": "y", "unknown": 3,
                             "shapes": [shape(note="z")]},
    "negative_colour_ints": {"duration": 1000, "shapes": [shape(keys=[key(0, color=[-1, 0, 0])])]},
    "huge_time_and_size_limits": {"duration": 200, "shapes": [
        shape(keys=[key(0, x=-1000, y=1000, size=400), key(65535, x=1000, y=-1000, size=0)])]},
    "ring_default_inner": {"duration": 500, "shapes": [shape("ring")]},
    "utf8_name": {"duration": 500, "name": "été ☀", "shapes": [shape("glyph", glyph="€")]},
}

# Invalid documents: the messages must be identical, including the unprefixed EfxError ones.
INVALID = {
    "duration_low": {"duration": 100, "shapes": [shape()]},
    "no_shapes": {"duration": 1000, "shapes": []},
    "too_many_shapes": {"duration": 1000, "shapes": [shape() for _ in range(17)]},
    "bad_kind": {"duration": 1000, "shapes": [shape("square")]},
    "bad_eyes": {"duration": 1000, "shapes": [shape(eyes="both2")]},
    "bad_easing": {"duration": 1000, "shapes": [shape(easing="bounce")]},
    "no_keys": {"duration": 1000, "shapes": [shape(keys=[])]},
    "key_times": {"duration": 1000, "shapes": [shape(keys=[key(10), key(10)])]},
    "key_range": {"duration": 1000, "shapes": [shape(keys=[key(0, x=1001)])]},
    "unknown_colour": {"duration": 1000, "shapes": [shape(color="magenta")]},
    "unknown_glyph": {"duration": 1000, "shapes": [shape("glyph", glyph="x")]},
    "glyph_true": {"duration": 1000, "shapes": [shape("glyph", glyph=True)]},
    "ratio_range": {"duration": 1000, "shapes": [shape("rect", ratio=9)]},
    "gaze_range": {"duration": 1000, "gaze": [{"t": 0, "x": 2}], "shapes": [shape()]},
    "gaze_times": {"duration": 1000, "gaze": [{"t": 5}, {"t": 5}], "shapes": [shape()]},
    "gaze_lid": {"duration": 1000, "gaze": [{"t": 0, "lid": 2}], "shapes": [shape()]},
    "missing_size": {"duration": 1000, "shapes": [{"kind": "circle", "keys": [{"t": 0, "x": 0, "y": 0}]}]},
    "angle_string": {"duration": 1000, "shapes": [shape("line", angle="abc")]},
    "colour_list_short": {"duration": 1000, "shapes": [shape(color=[1, 2])]},
    "colour_null": {"duration": 1000, "shapes": [shape(color=None)]},
    "colour_float": {"duration": 1000, "shapes": [shape(color=[7.5, 1, 1])]},
    "colour_str_item": {"duration": 1000, "shapes": [shape(color=["a", 1, 1])]},
    "colour_negative_blue": {"duration": 1000, "shapes": [shape(keys=[key(0, color=[0, 0, -1])])]},
    "top_level_list": [1, 2, 3],
    "gaze_not_list": {"duration": 1000, "gaze": None, "shapes": [shape()]},
}


def valid_docs():
    docs = {p.stem + "_example": json.loads(p.read_text()) for p in sorted(EXAMPLES.glob("*.json"))}
    docs.update(EDGE)
    return docs


def test_examples_and_edge_docs_match_python():
    docs = valid_docs()
    names = list(docs)
    got = run_js([{"op": "compile", "doc": docs[n]} for n in names])
    for n, g in zip(names, got):
        expected = py_compile(docs[n])
        assert "error" not in expected, (n, expected)
        assert g == expected, n


def test_half_to_even_is_applied():
    # duration 1001.5 rounds to 1002 (even); 1000.5 would round to 1000 (even)
    assert compile_efx(EDGE["half_rounding"]) == compile_efx(dict(EDGE["half_rounding"], duration=1002))
    doc = dict(EDGE["half_rounding"], duration=1000.5)
    assert compile_efx(doc) == compile_efx(dict(EDGE["half_rounding"], duration=1000))


def test_invalid_docs_have_identical_messages():
    names = list(INVALID)
    docs = [INVALID[n] for n in names]
    got = run_js([{"op": "compile", "doc": d} for d in docs])
    for n, d, g in zip(names, docs, got):
        expected = py_compile(d)
        assert "error" in expected, n
        assert g == expected, n


def test_shape_colour_resolution_is_key_over_shape_over_white():
    doc = {"duration": 1000, "shapes": [shape(color="gold", keys=[key(0), key(500, color="red")])]}
    data = compile_efx(doc)
    plain = compile_efx({"duration": 1000, "shapes": [shape(keys=[key(0, color="gold"), key(500, color="red")])]})
    assert data == plain
    got = run_js([{"op": "compile", "doc": doc}])[0]
    assert got == {"hex": data.hex()}


def test_check_matches_python_for_valid_and_corrupt_binaries():
    good = compile_efx(json.loads((EXAMPLES / "hello.json").read_text()))
    flipped = bytearray(good)
    flipped[40] ^= 0x10
    bad_magic = b"EFX2" + good[4:]
    bad_version = good[:4] + b"\x02" + good[5:]
    truncated = good[:-1]
    cases = [good, bytes(flipped), bad_magic, bad_version, truncated, b"EFX1", b""]
    got = run_js([{"op": "check", "hex": c.hex()} for c in cases])
    for c, g in zip(cases, got):
        assert g == py_check(c), c.hex()


def test_from_bytes_binary_json_bom_and_garbage():
    hello_json = (EXAMPLES / "hello.json").read_bytes()
    hello_efx = (EXAMPLES / "hello.efx").read_bytes()
    love_json_named = json.loads((EXAMPLES / "love.json").read_text())  # "name" only; file name is irrelevant
    love_json_named["name"] = "golden_star"
    cases = {
        "binary": hello_efx,
        "json": hello_json,
        "json_bom": b"\xef\xbb\xbf" + hello_json,
        "json_whitespace": b"\r\n  " + hello_json + b"\n\n",
        "json_named_love": json.dumps(love_json_named).encode("utf-8"),
        "json_unicode_glyph": json.dumps(EDGE["glyphs"], ensure_ascii=False).encode("utf-8"),
        "garbage": b"hello world",
        "empty": b"",
        "json_array": b"[1, 2]",
        "json_string": b'"EFX1"',
        "not_utf8": b"\xff\xfe\x00{garbage",
        "short": b"EF",
        "bad_binary": b"EFX1" + b"\x00" * 30,
        "invalid_doc": json.dumps(INVALID["duration_low"]).encode("utf-8"),
    }
    names = list(cases)
    got = run_js([{"op": "from", "b64": base64.b64encode(cases[n]).decode()} for n in names])
    for n, g in zip(names, got):
        expected = py_from(cases[n])
        assert g == expected, n
    assert got[names.index("garbage")] == {"error": "not an .efx file and not valid JSON"}
    assert got[names.index("json_bom")]["converted"] is True
    assert got[names.index("binary")]["converted"] is False


def test_suggest_name_matches_slug_rules():
    cases = [
        ({"name": "Golden Star!"}, "My Effect.json"),
        ({"name": ""}, "My Effect.json"),
        ({"name": "   "}, "Spaces Here.JSON"),
        ({}, "x.efx"),
        ({}, ""),
        ({"name": "a" * 40}, "b.json"),
    ]
    got = run_js([{"op": "name", "doc": d, "filename": f} for d, f in cases])
    for (d, f), g in zip(cases, got):
        assert g == {"name": py_name(d, f)}, (d, f)
        assert len(g["name"]) <= 31


def test_fixture_bytes_are_reproduced():
    # the checked-in binaries are what the JS compiler must reproduce from the JSON sources
    for p in sorted(EXAMPLES.glob("*.json")):
        doc = json.loads(p.read_text())
        assert compile_efx(doc) == (EXAMPLES / (p.stem + ".efx")).read_bytes(), p.name
    # the JS side compiles the same examples from a deep copy too (no mutation of the input)
    doc = copy.deepcopy(EDGE["gaze_pupil_lid"])
    assert run_js([{"op": "compile", "doc": doc}])[0] == py_compile(doc)
