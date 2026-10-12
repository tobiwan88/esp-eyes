# esp_eyes — agent notes

Animated eyes on two GC9A01 round displays driven by an ESP32-C61. Full spec: `eyes-c61-implementation-plan.md` (slice backlog in §7, frozen contracts in §3).

## Tooling
- **ESP-IDF v6.1** lives in `~/esp/esp-idf`, tools in `~/.espressif`. Activate with `source scripts/idf-env.sh` (never source `export.sh` directly: the IDF Python env is 3.13 but the default `python3` is 3.14; the script selects 3.13 via `uv python find 3.13`). Then `idf.py` and `riscv32-esp-elf-g++` are on PATH.
- **uv** manages Python (all versions available) and the project env (`pyproject.toml`, `uv.lock`; dev deps: esptool, esp-idf-size, pyserial, ruff). Run Python tools with `uv run ...`.
- **ffmpeg** is installed (clipprep video input). **Docker is not installed**; Apple `container` exists but avoid it unless really needed. eyes-studio runs as a plain Python app (`uv run`).
- Target: `esp32c61`, 8 MB flash. Host compiler: Apple clang 21 (C++23, `std::expected` OK).
- Note: this machine's `ls` is aliased to something that rejects paths; use `command ls`.

## Wiring
See `docs/wiring.md` (pin table, all GPIOs, `lcd` diagnostics). Current: SCL=6, SDA=5, DC=3, RST=2, CS left=4, **CS right=7**, VCC=5V.

## Commands
- `scripts/check.sh` — the single gate: core-include check, host doctest (ASan/UBSan, C++23, `-Werror`), pytest for `clipprep/` and `studio/`, then `idf.py build`.
- `scripts/check.sh --quick` — skips the IDF build.
- Firmware: `cd firmware && source ../scripts/idf-env.sh && idf.py build` (flash/monitor need the board on USB: `idf.py -p PORT flash monitor`).
- Host tests only: `cmake -S firmware/test/host -B build/host && cmake --build build/host && build/host/host_tests`.

## Conventions
- Layout: `firmware/components/<name>/{core,idf}`. `core/` is pure C++ with no `esp_*`/`freertos/` includes (check.sh enforces it) and is tested on the host; include as `"gfx/core/gfx.hpp"` (components dir is on the include path).
- C++: no exceptions/RTTI/iostream/`std::function`, `std::expected` for errors, `constexpr` tables, `static_assert` as tests, no heap in render paths. Tests: `firmware/test/host/test_*.cpp` (doctest vendored).
- Pixels are RGB565 big-endian (`gfx::Rgb565be`). Circle mask: r²=14470 (calibrated to 48 880 px for 20-line strips).
- Pins/clock come from Kconfig via `firmware/main/config.hpp` (SPI 40 MHz since the wiring was fixed; use 20 MHz for long wires).
- Commit per slice: `<ID>: <title>`; local git only, no CI.

## Status
Plan v2.0 (see plan doc): the procedural eye renderer was replaced by the `cat_eyes` C component (`firmware/components/cat_eyes`, additions: `cat_eyes_get_iris/get_auto/force_full`; one NaN fix in `cat_eyes_render.c`). Default style realistic. Device API: `POST /api/eye {style, iris, play, auto, look, save}`, `GET /api/animations`, embedded page at `GET /`, clips + OTA unchanged. Auth: anonymous by default (`CONFIG_EYES_OPEN_ACCESS`, console `auth open|token`).
Effects (v2.1): 19 extra animations (`food` … `pumpkin`) live in `cat_eyes/src/cat_eyes_fx.c` (see the component README). They are ordinary `cat_anim_t` values placed before `CAT_ANIM_SPECIAL` (which must stay last: `CatModel` random play relies on it); the name tables in `eyes/core/cat.hpp` and `tools/mock_device.py` must match the enum. Costs ~2.3 KB more internal RAM (`CE_MAX_FX` shape lists in `g_ce`). Not yet timed on the board: tinted/full-screen effects redraw all rows each frame.
Board: ESP32-C61 on /dev/cu.usbmodem11101 (USB-Serial/JTAG console; `tools/console.py`, `tools/serial_log.py`), Wi-Fi via console `wifi "<ssid>" <pw>`, device at eyes-600ed0.local.
Boards: #1 MAC d8:85:ac:60:0e:d0 `eyes-600ed0` (192.168.178.86); #2 MAC d8:85:ac:60:10:80 `eyes-601080` (192.168.178.87). Both run the same firmware, open access, same Wi-Fi. Check the MAC with `python -m esptool --port <port> chip-id` before flashing when two boards are attached. After re-cabling the displays, restart the board (or `lcd init`) so the panels are re-initialised.
Gotchas: never set ESP_INTR_FLAG_IRAM on the SPI bus (esp_lcd callback is not IRAM-safe); after changing SDKCONFIG_DEFAULTS rm -rf build; internal RAM is the scarce resource (Wi-Fi/lwIP in PSRAM, 10-line strips).
Panel bring-up: shared RST pulse + SWRESET on both + 120 ms reset-cancel waits (left panel used to get its init inside the reset window). Display diagnostics: console `lcd swap [on|off]` (left content on the CS_R panel), `lcd init [spi_hz]` (re-init, other clock), `lcd test [s]` (row-address pattern on both panels).
Random pool: `engine::Pool` (anim mask + clip names + pause) is chosen via `GET/POST /api/pool`, stored in NVS key `pool`, and used by `Scene` for random mode (cat_eyes' own random pick is off; saccades and blinks still run). In-browser GIF-to-clip encoder: `web/ui/clip_encoder.js` (served at `/clip_encoder.js`, byte-identical to clipprep, `tools/tests/test_js_encoder.py`); the page code path with `ImageDecoder` is not yet tried in a real browser.
Custom effects (v2.2): `.efx` overlay files, spec `docs/efx-format.md`, guide `docs/custom-effects.md`. Interpreter `cat_eyes/src/cat_eyes_efx.c` (pseudo anim id `CE_ANIM_EFX` = `CAT_ANIM_COUNT`, not in the enum/name tables); files in LittleFS `/fs/efx` (16 slots, 4 KB each, `storage::efx_*`); API `GET /api/efx`, `PUT/DELETE /api/efx/<name>`, `POST /api/eye {play:<name>}` (EyeCmd.fx), pool items `kind:"fx"` (stored in `Pool::clips`, `Scene::pick_now` tries `play_custom` first); loader `eyes::set_fx_loader` set by the render task (two PSRAM buffers). Tools: `tools/efxc.py` + `tools/efx/` (compile/check/preview/upload), studio `New effect` card. Examples in `examples/efx/` are shared fixtures of `tools/tests/test_efx.py` and `firmware/test/host/test_efx.cpp` (rebuild the .efx with `efxc build` after compiler changes). Not yet tried on the board.
Open: S7.2 power cuts, S7.3 24 h soak, frame times for halloween/christmas (~70 ms, render-bound), optional date-based default style.
