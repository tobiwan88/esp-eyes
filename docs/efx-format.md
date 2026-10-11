# .efx: custom effect files

An `.efx` file is a small overlay animation (a heart, sparkles, a ball rolling from one eye to the other) that
the eyes play on top of the normal eye. At most **4096 bytes**, **16 shapes**, **12 keys per shape**. The device keeps
up to **16** of them; the file name becomes the animation name (`POST /api/eye {"play":"<name>"}`).

People never write the binary by hand: write the JSON below and upload it. The cat's page and Eyes Studio accept the
JSON description directly, under any file name (a `.json` name is the convention; `.efx` also works), and convert it
to the binary before sending. The device itself stores only the binary. `efxc build` does the same conversion on the
command line (see `docs/custom-effects.md`). A file that is already binary is recognised by its first bytes `EFX1`,
not by its name.
The reference implementations are `tools/efx/compiler.py` (JSON to binary, validation) and
`firmware/components/cat_eyes/src/cat_eyes_efx.c` (validation, playback). They must agree; `test_efx.py` and `test_efx.cpp`
share the files in `examples/efx/`.

## JSON description

```json
{
  "duration": 3000,                       // ms, 200..30000; the effect ends then
  "mirror": false,                        // optional: world shapes fly right-to-left half of the time
  "gaze": [                               // optional: where the pupils look (0..12 keys)
    {"t": 0,   "x": 0, "y": 0},
    {"t": 400, "x": 0, "y": -0.2, "pupil": 0.8, "lid": 0.0}
  ],
  "shapes": [
    {"kind": "heart", "eyes": "both", "easing": "back", "loop": true,
     "keys": [
       {"t": 0,   "x": 120, "y": 120, "size": 0,  "color": "#ff2a5f"},
       {"t": 500, "x": 120, "y": 120, "size": 52, "color": "#ff2a5f"}
     ]}
  ]
}
```

Coordinates are pixels of one 240x240 display; the eye centre is about (120, 118). Everything between two keys is
interpolated: position, size and colour. Before the first key the shape is hidden; after the last key it is hidden
unless `hold` is true (stays at the last key) or `loop` is true (repeats from the first key).
Size 0 hides a shape (use it to grow or shrink in).

| Shape field | Values |
|---|---|
| `color` | colour for every key of this shape that has no `color` of its own (key-level `color` wins); default `white` |
| `kind` | `circle` (size = radius), `ellipse` (+`ratio` = height/width), `rect` (size = half width, +`ratio`), `ring` (+`inner` 0..1), `heart` (size = half width), `sparkle` (4-point star), `glyph` (size = pixel scale; `glyph`: `0`-`9`, `%`, `euro`, `kana0`-`kana7`), `line` (size = half length, +`angle` degrees), `triangle` (size = radius, +`angle`) |
| `eyes` | `both` (default), `left`, `right` |
| `world` | `true`: x is a position across **both** displays (0..240 left eye, 296..536 right eye, gap 56), so a shape can fly from one eye into the other |
| `easing` | `linear`, `smooth` (default), `in`, `out`, `back` (overshoot), `step` |
| `loop` / `hold` | see above |
| key `color` | `#rrggbb` or `red pink hotpink orange yellow green lime cyan blue purple white black gold gray brown`; overrides the shape `color` |
| key `x`,`y` | -1000..1000; key `size` 0..400; key `t` ms, strictly increasing, below 65536 |

`gaze` keys steer the eyes while the effect plays: `x`,`y` -1..1 (+y down), optional `pupil` 0..1 (omitted = normal)
and `lid` 0..1 (0 open, 1 closed; omitted = normal). Smooth interpolation, value held after the last key.

Budget on the display: a heart and a sparkle use 3 drawing primitives each, all others 1; the display has 40 primitive
slots per eye. Keep an effect to roughly 10 hearts/sparkles or 40 simple shapes; extra primitives are dropped.

## Binary layout (little endian)

```
header, 16 bytes
  0  "EFX1"        4  version = 1        5  flags (bit0 mirror)
  6  u16 duration  8  u8 n_shapes        9  u8 n_gaze
  10 u16 payload_len   12 u16 crc16 (CCITT-FALSE, init 0xFFFF, over the payload)   14 u16 reserved = 0
payload
  n_gaze x   { u16 t, i8 gx (x127), i8 gy (x127), u8 pupil (0 = normal, else v/255), u8 lid (0 = normal, else 1 + v*254) }
  n_shapes x { u8 kind, u8 mask, u8 flags, u8 aux, u8 n_keys,
               n_keys x { u16 t, i16 x, i16 y, i16 size, u16 rgb565 } }
kind : 0 circle 1 ellipse 2 rect 3 ring 4 heart 5 sparkle 6 glyph 7 line 8 triangle
mask : bit0 left eye, bit1 right eye, bit2 world coordinates
flags: bit0 loop, bit1 hold, bits4-7 easing (0 linear 1 smooth 2 in 3 out 4 back 5 step)
aux  : ellipse/rect ratio x32 | ring inner x255 | glyph index | line/triangle angle x256/360
```

A file is rejected (HTTP 422 with a readable reason, nothing stored) when the header, length or checksum is wrong, a
count or value is out of range, or key times do not increase. A running effect is never disturbed by a rejected upload.

## Device API

| Request | Effect |
|---|---|
| `PUT /api/efx/<name>` (body = the binary `.efx`) | store or replace; 507 when all 16 slots are used by other names |
| `GET /api/efx` | `{"effects":[{name,size,duration_ms,shapes}], "slots":16, "max_bytes":4096}` |
| `DELETE /api/efx/<name>` | remove |
| `POST /api/eye {"play":"<name>"}` | play it (404 `unknown animation` if it does not exist) |
| `GET /api/animations` | has `"custom":[names]` next to `animations` |
| `/api/pool` | custom effects appear as items with `"kind":"fx"`; enabled ones take part in random mode |

`<name>` is `[a-z0-9_-]{1,31}` and may not be a built-in animation name or the name of a clip.
