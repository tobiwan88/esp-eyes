"""efxc: build, check, preview and upload custom eye effects.

  efxc build heart.json            -> heart.efx   (the file you upload)
  efxc check heart.efx             -> summary, or the exact reason it would be rejected
  efxc preview heart.json          -> heart.gif   (open it to see the animation)
  efxc upload heart.json --host eyes-600ed0.local [--name love] [--pool]
  efxc list|delete <name> --host ...

Env: EYES_HOST, EYES_TOKEN.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from efx import EfxError, check, load_any, render_gif
from efx.compiler import slug


def load(path: Path) -> bytes:
    """A JSON description or an .efx binary, whatever the extension."""
    return load_any(path.read_bytes())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="efxc", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("EYES_HOST", "eyes.local"))
    ap.add_argument("--token", default=os.environ.get("EYES_TOKEN", ""))
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("file", type=Path); b.add_argument("-o", type=Path)
    c = sub.add_parser("check"); c.add_argument("file", type=Path)
    p = sub.add_parser("preview"); p.add_argument("file", type=Path); p.add_argument("-o", type=Path)
    u = sub.add_parser("upload"); u.add_argument("file", type=Path); u.add_argument("--name"); u.add_argument("--pool", action="store_true")
    sub.add_parser("list")
    d = sub.add_parser("delete"); d.add_argument("name")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            data = load(a.file)
            out = a.o or a.file.with_suffix(".efx")
            out.write_bytes(data)
            print(f"{out}: {len(data)} bytes")
        elif a.cmd == "check":
            print(json.dumps(check(load(a.file))))
        elif a.cmd == "preview":
            print(render_gif(load(a.file), a.o or a.file.with_suffix(".gif")))
        else:
            from eyes_client import Device

            dev = Device(a.host, a.token)
            if a.cmd == "upload":
                name = a.name or slug(a.file.stem)
                print(json.dumps(dev.upload_efx(name, load(a.file))))
                if a.pool:
                    enabled = [it["name"] for it in dev.pool()["items"] if it["enabled"]]
                    dev.set_pool(enabled=enabled + [name])
                print(f"play it with: eyesctl anim {name}")
            elif a.cmd == "list":
                print(json.dumps(dev.efx_list(), indent=2))
            else:
                dev.delete_efx(a.name)
    except EfxError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
