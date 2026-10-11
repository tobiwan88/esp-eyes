import json
import struct
from pathlib import Path

import pytest
from efx import EfxError, check, compile_efx, compile_file, decode, load_any, render_gif
from efx.compiler import rgb565
from eyes_client import Device, DeviceError
from mock_device import TOKEN, MockDevice

EXAMPLES = Path(__file__).resolve().parents[2] / "examples" / "efx"
MIN = {"duration": 1000, "shapes": [{"kind": "circle", "keys": [{"t": 0, "x": 10, "y": 20, "size": 5, "color": "red"}]}]}


@pytest.mark.parametrize("name", ["love", "hello", "golden_star"])
def test_examples_match_committed_binaries(name):
    # the .efx files are what the firmware host tests load: they must be rebuilt when the compiler changes
    assert compile_file(EXAMPLES / f"{name}.json") == (EXAMPLES / f"{name}.efx").read_bytes()


def test_header_and_layout():
    d = compile_efx(MIN)
    assert d[:4] == b"EFX1" and d[4] == 1
    assert struct.unpack_from("<H", d, 6)[0] == 1000 and d[8] == 1 and d[9] == 0
    assert len(d) == 16 + 5 + 10
    assert check(d)["bytes"] == len(d)
    assert struct.unpack_from("<H", d, 16 + 5 + 8)[0] == 0xF904  # "red" = #ff2020 in RGB565, like the firmware macro


def test_roundtrip():
    d = compile_efx(json.loads((EXAMPLES / "love.json").read_text()))
    assert compile_efx(decode(d)) == d


@pytest.mark.parametrize("mutate,msg", [
    (lambda j: j.update(duration=50), "duration"),
    (lambda j: j.update(shapes=[]), "shapes"),
    (lambda j: j["shapes"][0].update(kind="blob"), "kind"),
    (lambda j: j["shapes"][0].update(eyes="middle"), "eyes"),
    (lambda j: j["shapes"][0].update(easing="wobble"), "easing"),
    (lambda j: j["shapes"][0]["keys"][0].update(color="#12"), "colour"),
    (lambda j: j["shapes"][0]["keys"][0].update(size=9999), "size"),
    (lambda j: j["shapes"][0]["keys"].append({"t": 0, "x": 0, "y": 0, "size": 1}), "increase"),
    (lambda j: j.update(shapes=j["shapes"] * 17), "shapes"),
])
def test_compile_errors_are_readable(mutate, msg):
    j = json.loads(json.dumps(MIN))
    mutate(j)
    with pytest.raises(EfxError, match=msg):
        compile_efx(j)


def test_check_rejects_damage():
    d = bytearray(compile_efx(MIN))
    for i, msg in [(0, "bad header"), (4, "version"), (len(d) - 1, "checksum")]:
        bad = bytearray(d)
        bad[i] ^= 0xFF
        with pytest.raises(EfxError, match=msg):
            check(bytes(bad))
    with pytest.raises(EfxError, match="length"):
        check(bytes(d[:-1]))


def test_preview_gif(tmp_path):
    out = render_gif((EXAMPLES / "love.efx").read_bytes(), tmp_path / "love.gif")
    assert out.read_bytes()[:3] == b"GIF"


def test_device_upload_play_pool_delete():
    data = (EXAMPLES / "love.efx").read_bytes()
    with MockDevice() as m:
        dev = Device(m.host, TOKEN)
        assert dev.upload_efx("love", data)["shapes"] == 3
        assert [e["name"] for e in dev.efx_list()["effects"]] == ["love"]
        assert "love" in dev.animations()["custom"]
        dev.eye(play="love")
        assert m.eye_state["anim"] == "love"
        assert {"name": "love", "kind": "fx", "enabled": False} in dev.pool()["items"]
        dev.set_pool(enabled=["love", "wink"])
        assert m.pool_enabled == {"love", "wink"}
        with pytest.raises(DeviceError) as e:
            dev.upload_efx("bad", data[:-3])
        assert e.value.status == 422
        with pytest.raises(DeviceError):
            dev.upload_efx("wink", data)  # built-in name
        dev.delete_efx("love")
        assert dev.efx_list()["effects"] == []
        with pytest.raises(DeviceError):
            dev.eye(play="love")


def test_all_slots_used():
    data = (EXAMPLES / "hello.efx").read_bytes()
    with MockDevice() as m:
        dev = Device(m.host, TOKEN)
        for i in range(16):
            dev.upload_efx(f"fx{i}", data)
        with pytest.raises(DeviceError) as e:
            dev.upload_efx("one-more", data)
        assert e.value.status == 507
        dev.upload_efx("fx3", data)  # replacing is fine


def _key_color(data: bytes, key_index: int = 0) -> int:
    # one-shape, no-gaze files only: header, shape header, then 10-byte keys (colour at +8)
    return struct.unpack_from("<H", data, 16 + 5 + key_index * 10 + 8)[0]


def test_shape_color_is_default_for_keys_and_keys_override():
    base = {"duration": 1000, "shapes": [{"kind": "circle", "color": "red", "keys": [
        {"t": 0, "x": 0, "y": 0, "size": 5},
        {"t": 500, "x": 0, "y": 0, "size": 5, "color": "blue"},
    ]}]}
    d = compile_efx(base)
    assert _key_color(d, key_index=0) == 0xF904  # shape colour "red"
    assert _key_color(d, key_index=1) == rgb565("blue")  # key override wins
    assert _key_color(compile_efx(MIN)) == 0xF904
    no_color = json.loads(json.dumps(MIN))
    del no_color["shapes"][0]["keys"][0]["color"]
    assert _key_color(compile_efx(no_color)) == 0xFFFF  # neither set: white


def test_golden_star_example_compiles_with_its_colours():
    doc = json.loads((EXAMPLES / "golden_star.json").read_text())
    data = compile_efx(doc)
    assert data == (EXAMPLES / "golden_star.efx").read_bytes()
    colours = {s["keys"][0]["color"] for s in decode(data)["shapes"]}  # RGB565-rounded
    assert colours == {"#f8b000", "#f8d400", "#f8f4b0", "#f8fcf8", "#f8f8d8"}


def test_load_any_binary_and_json_with_bom():
    binary = (EXAMPLES / "love.efx").read_bytes()
    assert load_any(binary) == binary
    doc = (EXAMPLES / "love.json").read_bytes()
    assert load_any(b"\xef\xbb\xbf" + doc) == compile_efx(json.loads(doc))
    assert load_any(b"  \n" + doc + b"\n") == compile_efx(json.loads(doc))


@pytest.mark.parametrize("data", [b"\x00\x01garbage", b"", b"[1, 2]", b"\xff\xfe\x00", b"{not json"])
def test_load_any_rejects_garbage(data):
    with pytest.raises(EfxError, match="not an .efx file and not valid JSON"):
        load_any(data)


def test_load_any_checks_binary():
    binary = bytearray((EXAMPLES / "love.efx").read_bytes())
    binary[-1] ^= 0xFF
    with pytest.raises(EfxError, match="checksum"):
        load_any(bytes(binary))


def test_json_saved_as_efx_builds_and_checks(tmp_path):
    import efxc

    src = tmp_path / "heart.efx"  # JSON content under an .efx name
    src.write_bytes((EXAMPLES / "love.json").read_bytes())
    assert efxc.main(["build", str(src)]) == 0
    out = tmp_path / "heart.efx"
    assert out.read_bytes() == compile_efx(json.loads((EXAMPLES / "love.json").read_text()))
    assert efxc.main(["check", str(out)]) == 0
    assert efxc.main(["build", str(src), "-o", str(tmp_path / "b.efx")]) == 0
    assert (tmp_path / "b.efx").read_bytes() == out.read_bytes()
