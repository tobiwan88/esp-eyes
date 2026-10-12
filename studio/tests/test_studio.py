import numpy as np
from fastapi.testclient import TestClient
from PIL import Image

from eyes_studio.app import create_app
from eyes_studio.devices import Registry
from mock_device import TOKEN, MockDevice


def gif_bytes(tmp_path):
    frames = []
    for i in range(4):
        a = np.zeros((50, 50, 3), np.uint8)
        a[5 + 8 * i:20 + 8 * i, 10:30] = (255, 0, 0)
        frames.append(Image.fromarray(a))
    p = tmp_path / "t.gif"
    frames[0].save(p, save_all=True, append_images=frames[1:], duration=100)
    return p.read_bytes()


def client(tmp_path, mock):
    reg = Registry(default_token=TOKEN)
    reg.add("eyes-mock", mock.host)
    return TestClient(create_app(reg, tmp_path, mdns=False))


def test_pages_and_devices(tmp_path):
    with MockDevice() as m:
        c = client(tmp_path, m)
        assert "Eyes Studio" in c.get("/").text
        d = c.get("/api/devices").json()
        assert d[0]["id"] == "eyes-mock" and d[0]["status"]["hostname"] == "eyes-mock"


def test_control_proxy(tmp_path):
    with MockDevice() as m:
        c = client(tmp_path, m)
        assert c.post("/api/devices/eyes-mock/eye", json={"style": "anime", "iris": "odd", "play": "wink"}).status_code == 200
        assert c.post("/api/devices/eyes-mock/eye", json={"look": [0.2, -0.4, 800], "auto": True}).status_code == 200
        assert c.post("/api/devices/eyes-mock/play", json={"clip": "a", "loop": True}).status_code == 200
        assert c.post("/api/devices/eyes-mock/stop").status_code == 200
        assert m.eye_cmds == [{"style": "anime", "iris": "odd", "play": "wink"}, {"look": [0.2, -0.4, 800], "auto": True}]
        assert m.eye_state["style"] == "anime" and m.eye_state["anim"] == "wink" and m.eye_state["auto"] is True
        assert m.play_cmds[0]["clip"] == "a" and m.stops == 1
        assert c.post("/api/devices/nope/stop").status_code == 404


def test_eye_rejects_unknown_animation(tmp_path):
    with MockDevice() as m:
        c = client(tmp_path, m)
        r = c.post("/api/devices/eyes-mock/eye", json={"play": "bogus"})
        assert r.status_code == 400
        assert r.json()["error"]["code"] == "E_BAD_JSON"
        assert m.eye_cmds == []


def test_animations_proxy(tmp_path):
    with MockDevice() as m:
        c = client(tmp_path, m)
        a = c.get("/api/devices/eyes-mock/animations").json()
        assert a["styles"][0] == "realistic" and "amber" in a["irises"]
        assert len(a["animations"]) == 31 and a["specials"]["cute"] == "heart_eyes"
        assert a["state"]["style"] == "realistic"
        assert c.get("/api/devices/nope/animations").status_code == 404


def test_prepare_send_delete(tmp_path):
    with MockDevice() as m:
        c = client(tmp_path, m)
        r = c.post("/api/prepare", files={"file": ("t.gif", gif_bytes(tmp_path), "image/gif")}, data={"device": "eyes-mock"})
        assert r.status_code == 200, r.text
        p = r.json()
        assert p["frames"] == 4 and p["size"] > 100
        assert c.get(p["preview"]).headers["content-type"] == "image/gif"
        assert c.post("/api/devices/eyes-mock/clips/blob", json={"prepared": p["id"]}).status_code == 200
        assert m.clips["blob"][:4] == b"EYCL"
        assert c.get("/api/devices/eyes-mock/clips").json()["clips"][0]["name"] == "blob"
        assert c.delete("/api/devices/eyes-mock/clips/blob").status_code == 204
        assert c.delete("/api/devices/eyes-mock/clips/blob").status_code == 404


def test_pool_proxy(tmp_path):
    with MockDevice() as m:
        c = client(tmp_path, m)
        m.clips["fire"] = b"EYCL" + b"\0" * 8
        p = c.get("/api/devices/eyes-mock/pool").json()
        assert len(p["items"]) == 32 and p["interval"] == [2.5, 7.0] and p["auto"] is True
        assert [i["name"] for i in p["items"] if i["kind"] == "clip"] == ["fire"]
        r = c.post("/api/devices/eyes-mock/pool", json={"enabled": ["wink", "fire"], "interval": [1, 4]})
        assert r.status_code == 200
        assert {i["name"] for i in r.json()["items"] if i["enabled"]} == {"wink", "fire"}
        assert m.pool_interval == [1, 4]
        r = c.post("/api/devices/eyes-mock/pool", json={"enabled": ["nope"]})
        assert r.status_code == 400 and r.json()["error"]["message"] == "unknown item: nope"
        assert m.pool_enabled == {"wink", "fire"}
        assert c.post("/api/devices/nope/pool", json={}).status_code == 404
        assert c.get("/api/devices/nope/pool").status_code == 404


def test_prepare_rejects_garbage(tmp_path):
    with MockDevice() as m:
        c = client(tmp_path, m)
        r = c.post("/api/prepare", files={"file": ("x.gif", b"not an image", "image/gif")})
        assert r.status_code == 422


def test_effect_upload_from_json_and_efx(tmp_path):
    import json
    from pathlib import Path

    ex = Path(__file__).resolve().parents[2] / "examples" / "efx"
    with MockDevice() as m:
        c = client(tmp_path, m)
        r = c.post("/api/efx/prepare", files={"file": ("love.json", (ex / "love.json").read_bytes())})
        assert r.status_code == 200, r.text
        prep = r.json()
        assert prep["shapes"] == 3 and c.get(prep["preview"]).content[:3] == b"GIF"
        assert c.post("/api/devices/eyes-mock/efx/love", json={"prepared": prep["id"]}).status_code == 200
        assert c.get("/api/devices/eyes-mock/efx").json()["effects"][0]["name"] == "love"
        assert "love" in c.get("/api/devices/eyes-mock/animations").json()["custom"]
        r2 = c.post("/api/efx/prepare", files={"file": ("x.efx", (ex / "hello.efx").read_bytes())})
        assert r2.status_code == 200
        bad = c.post("/api/efx/prepare", files={"file": ("x.json", json.dumps({"duration": 5}).encode())})
        assert bad.status_code == 422 and "shapes" in bad.text
        assert c.delete("/api/devices/eyes-mock/efx/love").status_code == 204


def test_prepare_accepts_json_uploaded_as_efx(tmp_path):
    from pathlib import Path

    ex = Path(__file__).resolve().parents[2] / "examples" / "efx"
    with MockDevice() as m:
        c = client(tmp_path, m)
        r = c.post("/api/efx/prepare", files={"file": ("x.efx", (ex / "love.json").read_bytes())})
        assert r.status_code == 200, r.text
        assert r.json()["shapes"] == 3
        bad = c.post("/api/efx/prepare", files={"file": ("x.efx", b"\x00garbage")})
        assert bad.status_code == 422 and "not an .efx file and not valid JSON" in bad.text
