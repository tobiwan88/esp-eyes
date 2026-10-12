"""efx: compile, check and preview custom eye effects (.efx files).

JSON in, a small binary out that the device accepts at PUT /api/efx/<name>.
Format: docs/efx-format.md.
"""
from .compiler import EfxError, check, compile_efx, compile_file, decode, load_any
from .preview import render_gif

__all__ = ["EfxError", "check", "compile_efx", "compile_file", "decode", "load_any", "render_gif"]
