"""eyes-studio: prepare clips, preview, upload, control devices (plan §3.8)."""
from __future__ import annotations

import os
import tempfile
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

from clipprep.preview import preview_gif
from clipprep.writer import encode
from efx import EfxError, load_any, render_gif
from efx import check as efx_check
from eyes_client import DeviceError
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from clipprep import Budget, prepare

from .devices import Registry

STATIC = Path(__file__).parent / "static"


class Play(BaseModel):
    clip: str | None = None
    left: str | None = None
    right: str | None = None
    loop: bool = False
    then: str = "eyes"


class Send(BaseModel):
    prepared: str


def create_app(registry: Registry | None = None, work_dir: Path | None = None, mdns: bool = True) -> FastAPI:
    reg = registry or Registry()
    work = Path(work_dir or os.environ.get("EYES_WORK", tempfile.gettempdir())) / "eyes-studio"
    work.mkdir(parents=True, exist_ok=True)

    @asynccontextmanager
    async def lifespan(app):
        if mdns:
            reg.start_mdns()
        yield

    app = FastAPI(title="eyes-studio", lifespan=lifespan)

    def dev(id_: str):
        d = reg.get(id_)
        if not d:
            raise HTTPException(404, "unknown device")
        return d

    @app.exception_handler(DeviceError)
    async def device_error(_, e: DeviceError):
        return JSONResponse({"error": {"code": e.code, "message": e.message}}, status_code=e.status if e.status != 401 else 502)

    @app.get("/api/devices")
    def devices():
        out = []
        for e in reg.entries():
            try:
                out.append({"id": e.id, "host": e.host, "status": dev(e.id).status()})
            except Exception as ex:  # noqa: BLE001
                out.append({"id": e.id, "host": e.host, "error": str(ex)})
        return out

    @app.get("/api/devices/{id_}/animations")
    def animations(id_: str):
        return dev(id_).animations()

    @app.post("/api/devices/{id_}/eye")
    def eye(id_: str, body: dict):
        dev(id_).eye(**body)
        return {"queued": True}

    @app.post("/api/devices/{id_}/play")
    def play(id_: str, body: Play):
        dev(id_).play(body.clip, left=body.left, right=body.right, loop=body.loop, then=body.then)
        return {"queued": True}

    @app.post("/api/devices/{id_}/stop")
    def stop(id_: str):
        dev(id_).stop()
        return {"queued": True}

    @app.get("/api/devices/{id_}/clips")
    def clips(id_: str):
        return dev(id_).clips()

    @app.get("/api/devices/{id_}/pool")
    def get_pool(id_: str):
        return dev(id_).pool()

    @app.post("/api/devices/{id_}/pool")
    def set_pool(id_: str, body: dict):
        return dev(id_).set_pool(enabled=body.get("enabled"), interval=body.get("interval"))

    @app.delete("/api/devices/{id_}/clips/{name}")
    def delete(id_: str, name: str):
        dev(id_).delete(name)
        return Response(status_code=204)

    @app.post("/api/prepare")
    def prepare_clip(file: UploadFile = File(...), device: str | None = Form(None), fps: float | None = Form(None),
                     region: str | None = Form(None), fit: str = Form("cover"), bg: str = Form("#000000"),
                     loop: bool = Form(False), max_frames: int | None = Form(None)):
        pid = uuid.uuid4().hex[:12]
        src = work / f"{pid}{Path(file.filename or 'in.gif').suffix}"
        src.write_bytes(file.file.read())
        budget = Budget()
        if device:
            try:
                st = dev(device).status()
                budget = Budget(max_bytes=min(1_500_000, max(st["fs_free"] - 65536, 0)), max_bps=int(st["clip_max_bps"] * 0.8))
            except Exception:  # noqa: BLE001
                pass
        try:
            reg_t = tuple(int(v) for v in region.split(",")) if region else "full"
            p = prepare(src, eye_region=reg_t, fps=fps, max_frames=max_frames, fit=fit, bg=bg, loop=loop, budget=budget)
        except Exception as ex:
            raise HTTPException(422, f"cannot prepare: {ex}") from ex
        finally:
            src.unlink(missing_ok=True)
        (work / f"{pid}.clip").write_bytes(encode(p.frames, p.loop))
        preview_gif(p, work / f"{pid}.gif")
        return {"id": pid, "size": p.size, "frames": len(p.frames), "fps": round(p.fps, 1), "max_bps": int(p.max_bps()),
                "notes": p.notes, "preview": f"/api/prepared/{pid}/preview.gif"}

    @app.get("/api/prepared/{pid}/preview.gif")
    def preview(pid: str):
        f = work / f"{Path(pid).name}.gif"
        if not f.exists():
            raise HTTPException(404)
        return FileResponse(f, media_type="image/gif")

    @app.post("/api/devices/{id_}/clips/{name}")
    def send(id_: str, name: str, body: Send):
        f = work / f"{Path(body.prepared).name}.clip"
        if not f.exists():
            raise HTTPException(404, "prepared clip not found")
        return dev(id_).upload(name, f)

    # ---- custom effects (.efx): a .json description or a ready .efx file ----
    @app.post("/api/efx/prepare")
    def prepare_efx(file: UploadFile = File(...)):
        raw = file.file.read(1_000_000)
        try:
            data = load_any(raw)
            info = efx_check(data)
        except EfxError as ex:
            raise HTTPException(422, str(ex)) from ex
        pid = uuid.uuid4().hex[:12]
        (work / f"{pid}.efx").write_bytes(data)
        render_gif(data, work / f"{pid}.efx.gif")
        return {"id": pid, "size": len(data), "duration_ms": info["duration"], "shapes": info["shapes"],
                "preview": f"/api/prepared/{pid}/efx_preview.gif"}

    @app.get("/api/prepared/{pid}/efx_preview.gif")
    def efx_preview(pid: str):
        f = work / f"{Path(pid).name}.efx.gif"
        if not f.exists():
            raise HTTPException(404)
        return FileResponse(f, media_type="image/gif")

    @app.get("/api/devices/{id_}/efx")
    def efx_list(id_: str):
        return dev(id_).efx_list()

    @app.post("/api/devices/{id_}/efx/{name}")
    def efx_send(id_: str, name: str, body: Send):
        f = work / f"{Path(body.prepared).name}.efx"
        if not f.exists():
            raise HTTPException(404, "prepared effect not found")
        return dev(id_).upload_efx(name, f.read_bytes())

    @app.delete("/api/devices/{id_}/efx/{name}")
    def efx_delete(id_: str, name: str):
        dev(id_).delete_efx(name)
        return Response(status_code=204)

    @app.get("/")
    def index():
        return FileResponse(STATIC / "index.html")

    app.mount("/static", StaticFiles(directory=STATIC), name="static")
    return app


app = create_app() if os.environ.get("EYES_STUDIO_AUTOSTART") else None
