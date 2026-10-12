"""In-process mock of the device API for tests."""
from __future__ import annotations

import json
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from efx import EfxError
from efx import check as efx_check

TOKEN = "testtoken"
EFX_SLOTS = 16

STYLES = ["realistic", "flat", "line", "anime", "cute", "halloween", "christmas"]
IRISES = ["amber", "green", "blue", "copper", "violet", "odd"]
ANIMATIONS = ["wink", "slow_blink", "roll", "side_eye", "hunt", "startle", "nictitate", "sleepy",
              "cross_eyed", "happy", "glance",
              "food", "fly", "paper_plane", "bubble", "spiral", "slots", "euro", "fish", "rain", "snow",
              "leaf", "day_night", "paw", "spider", "loading", "pacman", "matrix", "disco", "pumpkin",
              "special"]
SPECIALS = {"anime": "sparkle", "cute": "heart_eyes", "halloween": "demon_flare", "christmas": "twinkle"}
# play accepts the animations, the meta names "special" / "random", and the style special names.
PLAY_NAMES = set(ANIMATIONS) | {"special", "random"} | set(SPECIALS.values())
EYE_FIELDS = {"style", "iris", "play", "auto", "look", "save"}
POOL_INTERVAL = [2.5, 7.0]


def validate_pool(body, known: set[str]) -> str | None:
    """Return an error message for an invalid POST /api/pool body, or None if it is valid."""
    if not isinstance(body, dict):
        return "body must be a JSON object"
    if "enabled" in body:
        enabled = body["enabled"]
        if not isinstance(enabled, list):
            return "enabled must be a list of names"
        for name in enabled:
            if not isinstance(name, str) or name not in known:
                return f"unknown item: {name}"
    if "interval" in body:
        iv = body["interval"]
        ok = (isinstance(iv, list) and len(iv) == 2
              and all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in iv))
        if not ok or not (1 <= iv[0] <= iv[1] <= 120):
            return "interval must be [min,max] seconds with 1 <= min <= max <= 120"
    return None


def validate_eye(body, extra_play=()) -> str | None:
    """Return an error message for an invalid POST /api/eye body, or None if it is valid."""
    if not isinstance(body, dict):
        return "body must be a JSON object"
    unknown = set(body) - EYE_FIELDS
    if unknown:
        return f"unknown field(s): {', '.join(sorted(unknown))}"
    if "style" in body and body["style"] not in STYLES:
        return f"unknown style {body['style']!r}"
    if "iris" in body and body["iris"] not in IRISES:
        return f"unknown iris {body['iris']!r}"
    if "play" in body and body["play"] not in PLAY_NAMES and body["play"] not in (extra_play or ()):
        return f"unknown animation {body['play']!r}"
    if "auto" in body and not isinstance(body["auto"], bool):
        return "auto must be a boolean"
    if "save" in body and not isinstance(body["save"], bool):
        return "save must be a boolean"
    if "look" in body:
        look = body["look"]
        if not isinstance(look, list) or len(look) not in (2, 3):
            return "look must be [gx,gy] or [gx,gy,hold_ms]"
        if not all(isinstance(v, (int, float)) and not isinstance(v, bool) and -1 <= v <= 1 for v in look[:2]):
            return "look gx,gy must be numbers in -1..1"
        if len(look) == 3 and (not isinstance(look[2], int) or isinstance(look[2], bool) or look[2] < 0):
            return "look hold_ms must be a non-negative integer"
    return None


class MockDevice:
    def __init__(self):
        self.clips: dict[str, bytes] = {}
        self.efx: dict[str, bytes] = {}
        self.eye_cmds: list[dict] = []
        self.play_cmds: list[dict] = []
        self.eye_state = {"style": "realistic", "iris": "amber", "auto": False, "anim": None}
        self.stops = 0
        self.pool_enabled: set[str] = set(ANIMATIONS)
        self.pool_interval: list[float] = list(POOL_INTERVAL)
        mock = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):  # silence
                pass

            def _send(self, code, body=None):
                data = json.dumps(body).encode() if body is not None else b""
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def _err(self, code, name, msg):
                self._send(code, {"error": {"code": name, "message": msg}})

            def _auth(self):
                if self.headers.get("Authorization") != f"Bearer {TOKEN}":
                    self._err(401, "E_UNAUTHORIZED", "wrong token")
                    return False
                return True

            def _body(self):
                return self.rfile.read(int(self.headers.get("Content-Length", 0)))

            def do_GET(self):
                if self.path == "/api/status":
                    return self._send(200, {"version": "mock", "hostname": "eyes-mock", "fps": 30.0, "uptime_s": 1,
                                            "heap_internal": 100000, "heap_min": 90000, "frame_ms_max": 20,
                                            "source": {"left": "eyes", "right": "eyes"}, "fs_free": 3000000,
                                            "clip_max_bps": 1500000, "spi_hz": 20000000, "ip": "127.0.0.1", "rssi": -50,
                                            "eye": dict(mock.eye_state)})
                if self.path == "/api/animations":
                    return self._send(200, {"styles": STYLES, "irises": IRISES, "animations": ANIMATIONS,
                                            "specials": SPECIALS, "custom": list(mock.efx), "state": dict(mock.eye_state)})
                if not self._auth():
                    return
                if self.path == "/api/clips":
                    return self._send(200, {"clips": [{"name": n, "size": len(d), "frames": 1, "fps": 10.0} for n, d in mock.clips.items()],
                                            "free": 3000000})
                if self.path == "/api/pool":
                    return self._send(200, mock.pool_body())
                if self.path == "/api/efx":
                    return self._send(200, {"effects": [{"name": n, "size": len(d), "duration_ms": int.from_bytes(d[6:8], "little"),
                                                         "shapes": d[8]} for n, d in mock.efx.items()],
                                            "slots": EFX_SLOTS, "max_bytes": 4096})
                self._err(404, "E_NOT_FOUND", "no route")

            def do_POST(self):
                if not self._auth():
                    return
                body = self._body()
                if self.path == "/api/eye":
                    try:
                        cmd = json.loads(body)
                    except ValueError:
                        return self._err(400, "E_BAD_JSON", "body is not valid JSON")
                    problem = validate_eye(cmd, tuple(mock.efx))
                    if problem:
                        return self._err(400, "E_BAD_JSON", problem)
                    mock.eye_cmds.append(cmd)
                    for k in ("style", "iris", "auto"):
                        if k in cmd:
                            mock.eye_state[k] = cmd[k]
                    if "play" in cmd:
                        mock.eye_state["anim"] = cmd["play"]
                    return self._send(202, {"queued": True})
                if self.path == "/api/play":
                    mock.play_cmds.append(json.loads(body))
                    return self._send(202, {"queued": True})
                if self.path == "/api/stop":
                    mock.stops += 1
                    return self._send(202, {"queued": True})
                if self.path == "/api/pool":
                    try:
                        cmd = json.loads(body)
                    except ValueError:
                        return self._err(400, "E_BAD_JSON", "body is not valid JSON")
                    problem = validate_pool(cmd, set(mock.pool_names()))
                    if problem:
                        return self._err(400, "E_BAD_JSON", problem)
                    if "enabled" in cmd:
                        mock.pool_enabled = set(cmd["enabled"])
                    if "interval" in cmd:
                        mock.pool_interval = list(cmd["interval"])
                    return self._send(200, mock.pool_body())
                self._err(404, "E_NOT_FOUND", "no route")

            def do_PUT(self):
                if not self._auth():
                    return
                body = self._body()
                name = self.path.rsplit("/", 1)[-1]
                if self.path.startswith("/api/efx/"):
                    if not re.fullmatch(r"[a-z0-9_-]{1,31}", name) or name in PLAY_NAMES or name in mock.clips:
                        return self._err(400, "E_BAD_NAME", "name must match ^[a-z0-9_-]{1,31}$ and not be built in")
                    if len(body) > 4096:
                        return self._err(413, "E_TOO_LARGE", "effect file over 4096 bytes")
                    try:
                        info = efx_check(body)
                    except EfxError as e:
                        return self._err(422, "E_BAD_CLIP", str(e))
                    if name not in mock.efx and len(mock.efx) >= EFX_SLOTS:
                        return self._err(507, "E_NO_SPACE", "all effect slots are used: delete one, or reuse a name to replace it")
                    mock.efx[name] = body
                    return self._send(201, {"name": name, "size": len(body), "duration_ms": info["duration"], "shapes": info["shapes"]})
                if body[:4] != b"EYCL":
                    return self._err(422, "E_BAD_CLIP", "invalid clip")
                mock.clips[name] = body
                self._send(201, {"name": name, "size": len(body), "frames": 1})

            def do_DELETE(self):
                if not self._auth():
                    return
                name = self.path.rsplit("/", 1)[-1]
                if self.path.startswith("/api/efx/"):
                    if mock.efx.pop(name, None) is None:
                        return self._err(404, "E_NOT_FOUND", "no such effect")
                    mock.pool_enabled.discard(name)
                    return self._send(204)
                if mock.clips.pop(name, None) is None:
                    return self._err(404, "E_NOT_FOUND", "no such clip")
                mock.pool_enabled.discard(name)
                self._send(204)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def pool_names(self) -> list[str]:
        return ANIMATIONS + list(self.clips) + list(self.efx)

    def pool_body(self) -> dict:
        items = [{"name": n, "kind": "anim", "enabled": n in self.pool_enabled} for n in ANIMATIONS]
        items += [{"name": n, "kind": "clip", "enabled": n in self.pool_enabled} for n in self.clips]
        items += [{"name": n, "kind": "fx", "enabled": n in self.pool_enabled} for n in self.efx]
        return {"items": items, "interval": list(self.pool_interval), "auto": True}

    @property
    def host(self) -> str:
        return f"127.0.0.1:{self.server.server_address[1]}"

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *a):
        self.server.shutdown()
        self.server.server_close()
