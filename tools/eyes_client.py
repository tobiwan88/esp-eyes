"""HTTP client for the eyes device API (plan §5.3). Used by eyesctl and eyes-studio."""
from __future__ import annotations

import time
from pathlib import Path
from typing import Any

import requests


class DeviceError(RuntimeError):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(f"{status} {code}: {message}")
        self.status, self.code, self.message = status, code, message


class Device:
    def __init__(self, host: str, token: str = "", timeout: float = 10.0):
        self.base = host if host.startswith("http") else f"http://{host}"
        self.token = token
        self.timeout = timeout

    def _req(self, method: str, path: str, **kw) -> requests.Response:
        headers = kw.pop("headers", {})
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        r = requests.request(method, self.base + path, headers=headers, timeout=kw.pop("timeout", self.timeout), **kw)
        if r.status_code >= 400:
            try:
                e = r.json()["error"]
                raise DeviceError(r.status_code, e["code"], e["message"])
            except (ValueError, KeyError):
                raise DeviceError(r.status_code, "E_HTTP", r.text[:100])
        return r

    def status(self) -> dict[str, Any]:
        return self._req("GET", "/api/status").json()

    def eye(self, **fields) -> None:
        """POST /api/eye with any subset of style/iris/play/auto/look/save."""
        self._req("POST", "/api/eye", json=fields)

    def animations(self) -> dict[str, Any]:
        return self._req("GET", "/api/animations").json()

    def play(self, clip: str | None = None, *, left: str | None = None, right: str | None = None, loop: bool = False,
             then: str = "eyes") -> None:
        body: dict[str, Any] = {"loop": loop, "then": then}
        if clip:
            body["clip"] = clip
        if left:
            body["left"] = left
        if right:
            body["right"] = right
        self._req("POST", "/api/play", json=body)

    def stop(self) -> None:
        self._req("POST", "/api/stop", json={})

    def clips(self) -> dict[str, Any]:
        return self._req("GET", "/api/clips").json()

    def upload(self, name: str, data: bytes | Path) -> dict[str, Any]:
        payload = Path(data).read_bytes() if isinstance(data, Path) else data
        return self._req("PUT", f"/api/clips/{name}", data=payload, timeout=120).json()

    def delete(self, name: str) -> None:
        self._req("DELETE", f"/api/clips/{name}")

    def efx_list(self) -> dict[str, Any]:
        return self._req("GET", "/api/efx").json()

    def upload_efx(self, name: str, data: bytes) -> dict[str, Any]:
        return self._req("PUT", f"/api/efx/{name}", data=data, timeout=30).json()

    def delete_efx(self, name: str) -> None:
        self._req("DELETE", f"/api/efx/{name}")

    def pool(self) -> dict[str, Any]:
        return self._req("GET", "/api/pool").json()

    def set_pool(self, enabled: list[str] | None = None, interval: list[float] | None = None) -> dict[str, Any]:
        """POST /api/pool. 'enabled' replaces the whole enabled set; omitted fields are left unchanged."""
        body: dict[str, Any] = {}
        if enabled is not None:
            body["enabled"] = list(enabled)
        if interval is not None:
            body["interval"] = list(interval)
        return self._req("POST", "/api/pool", json=body).json()

    def ota(self, firmware: bytes | Path) -> None:
        payload = Path(firmware).read_bytes() if isinstance(firmware, Path) else firmware
        self._req("POST", "/api/ota", data=payload, timeout=300)

    def wait_up(self, timeout: float = 60) -> dict[str, Any]:
        end = time.time() + timeout
        while True:
            try:
                return self.status()
            except Exception:
                if time.time() > end:
                    raise
                time.sleep(1)
