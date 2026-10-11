# Prompt for Claude on the web: make a custom eye effect

Copy the whole grey block below into Claude on the web (claude.ai), replace the idea line with your own idea, and send.
Claude answers with one JSON block. Save it as a `.json` file and upload it on the cat's page (see
[custom-effects.md](custom-effects.md)).

The prompt is self-contained: Claude does not need access to this repository.

````text
You write JSON descriptions of short animated eye effects for two round 240x240 displays. An effect is an overlay
(a heart, sparkles, a ball rolling from one eye to the other) that plays on top of the normal eye.

My idea: [PASTE YOUR IDEA HERE, e.g. "a pink heart pops up over both eyes, beats twice, small hearts float up"]

FORMAT

Top level (an object):
- "name": short lowercase name, letters, digits, "-" or "_", at most 31 characters. Also the button name.
- "duration": milliseconds, 200 to 30000. The effect ends then.
- "mirror": true (optional). World shapes then fly right-to-left half of the time.
- "gaze": optional list of 0 to 12 gaze keys (where the pupils look).
- "shapes": list of 1 to 16 shapes.

Coordinates are pixels of one 240x240 display. The eye centre is about x 120, y 118. Without "world", x and y
are inside each eye's own 240x240 space, and the shape is drawn on both eyes unless "eyes" says otherwise.

Shape fields:
- "kind" (required): circle, ellipse, rect, ring, heart, sparkle, glyph, line, triangle.
  - circle: size = radius.
  - ellipse: size, and "ratio" = height / width (0.03 to 7.9).
  - rect: size = half width, and "ratio" = height / width.
  - ring: size, and "inner" = inner radius as a fraction 0 to 1 (default 0.6).
  - heart: size = half width.
  - sparkle: four-point star, size.
  - glyph: size = pixel scale, and "glyph" = "0" to "9", "%", "euro", "kana0" to "kana7".
  - line: size = half length, and "angle" in degrees.
  - triangle: size = radius, and "angle" in degrees.
- "eyes": "both" (default), "left" or "right".
- "color": a colour for every key of this shape that has no colour of its own. Default "white".
- "easing": "linear", "smooth" (default), "in", "out", "back" (overshoot), or "step".
- "loop": true to repeat from the first key after the last key. Optional.
- "hold": true to stay at the last key after it. Without loop or hold the shape is hidden after its last key.
- "world": true (optional). Then x is a position across BOTH eyes: 0 to 240 is the left eye, 296 to 536 is the right
  eye (gap 56). Use it only for something that travels from one eye to the other.
- "keys": list of 1 to 12 keys.

Keys (inside a shape):
- "t": milliseconds from the start of the effect. Strictly increasing inside the shape, below 65536.
- "x", "y": -1000 to 1000.
- "size": 0 to 400. Size 0 hides the shape, so use it to grow in or shrink out.
- "color" (optional): overrides the shape's "color" for this key.
- Everything between two keys is interpolated: position, size and colour.
- Before the first key the shape is hidden.

Colours: "#rrggbb", or one of these names: red, pink, hotpink, orange, yellow, green, lime, cyan, blue, purple,
white, black, gold, gray, brown.

Gaze keys (optional, at most 12): {"t", "x", "y"} plus optional "pupil" (0 to 1) and "lid" (0 = open, 1 = closed).
x and y are -1 to 1, y positive is down. "t" strictly increasing. The value is held after the last key.

Limits: 16 shapes, 12 keys per shape, 12 gaze keys, the file must stay under 4096 bytes. A heart and a sparkle use
three drawing pieces each, every other kind uses one. Keep an effect to about 10 hearts or sparkles, or about 40
simple shapes. Keep all key times inside "duration".

EXAMPLE (a complete, valid answer; copy its structure, not its content):

```json
{
  "name": "love",
  "duration": 3000,
  "gaze": [
    {"t": 0, "x": 0, "y": 0},
    {"t": 400, "x": 0, "y": -0.2, "pupil": 0.8, "lid": 0.0},
    {"t": 2600, "x": 0, "y": -0.2, "pupil": 0.8, "lid": 0.0},
    {"t": 3000, "x": 0, "y": 0}
  ],
  "shapes": [
    {"kind": "heart", "easing": "back", "loop": true,
     "keys": [
       {"t": 0,   "x": 120, "y": 120, "size": 0,  "color": "#ff2a5f"},
       {"t": 500, "x": 120, "y": 120, "size": 52, "color": "#ff2a5f"},
       {"t": 700, "x": 120, "y": 120, "size": 44, "color": "#ff5c8a"},
       {"t": 900, "x": 120, "y": 120, "size": 52, "color": "#ff2a5f"}
     ]},
    {"kind": "heart", "eyes": "both", "easing": "out",
     "keys": [
       {"t": 900,  "x": 150, "y": 190, "size": 0,  "color": "#ff9ab8"},
       {"t": 1300, "x": 160, "y": 150, "size": 14, "color": "#ff9ab8"},
       {"t": 2500, "x": 175, "y": 40,  "size": 10, "color": "#ffd0dc"}
     ]},
    {"kind": "sparkle", "easing": "smooth", "loop": true,
     "keys": [
       {"t": 600, "x": 60, "y": 70, "size": 0,  "color": "#ffffff"},
       {"t": 900, "x": 60, "y": 70, "size": 12, "color": "#fff0a0"},
       {"t": 1200, "x": 60, "y": 70, "size": 0, "color": "#ffffff"}
     ]}
  ]
}
```

OUTPUT

Reply with exactly one JSON code block and nothing else. The JSON must be strict: no comments, no trailing commas,
no text before or after the block. The user will save it as <short_name>.json (any file name works, and a JSON file
saved as .efx also works).
````

## If the upload says ...

The cat's page shows the reason next to the button. Copy the message into Claude and ask for a fix. The `N` in a
message is a number; `shape N: ` at the start means the problem is in that shape (counted from 1).

| Message (from the page) | Tell Claude |
|---|---|
| `not an .efx file and not valid JSON` | "The file is not JSON. Reply with only the JSON code block, no text around it." |
| `top level must be an object` | "The top level must be one object with name, duration and shapes." |
| `'shapes' needs 1..16 entries` | "Use between 1 and 16 shapes." |
| `'gaze' has at most 12 keys` | "Use at most 12 gaze keys." |
| `duration must be 200..30000 ms` | "Set duration between 200 and 30000 milliseconds." |
| `'duration' must be a number` | "duration must be a plain number of milliseconds, not text." |
| `gaze key times must increase` | "The gaze keys' t values must go up strictly, each one larger than the one before." |
| `gaze x/y must be -1..1` | "Gaze x and y must be between -1 and 1." |
| `pupil and lid must be 0..1` | "Gaze pupil and lid must be between 0 and 1." |
| `shape N: kind must be one of circle, ellipse, rect, ring, heart, sparkle, glyph, line, triangle` | "Shape N has an unknown kind. Use one of the listed kinds." |
| `shape N: eyes must be both, left or right` | "Shape N: eyes must be both, left or right." |
| `shape N: easing must be one of linear, smooth, in, out, back, step` | "Shape N: use one of the listed easing names." |
| `shape N: 'keys' needs 1..12 entries` | "Shape N needs between 1 and 12 keys." |
| `shape N: key times must increase` | "In shape N the key t values must go up strictly. Fix the order." |
| `shape N: x/y within -1000..1000, size within 0..400` | "In shape N keep x and y within -1000 to 1000 and size within 0 to 400." |
| `shape N: bad colour '...': use #rrggbb or one of ...` | "Shape N uses a colour that is not known. Use #rrggbb or one of the listed colour names." |
| `shape N: unknown glyph '...'` | "Shape N: the glyph must be 0 to 9, %, euro, or kana0 to kana7." |
| `shape N: ratio must be 0.03 .. 7.9` | "Shape N: ratio must be between 0.03 and 7.9." |
| `shape N: inner must be 0..1` | "Shape N: inner must be between 0 and 1." |
| `file too big: ... (max 4096); use fewer shapes or keys` | "The effect is too big. Use fewer shapes or fewer keys per shape, or make it shorter." |
| `all effect slots are used` | Not a prompt problem: the device has 16 effects. Delete one on the page first, then upload again. |
| `not an .efx file (bad header)`, `checksum mismatch`, `file length does not match header` | The file is damaged or is a half-saved binary. Ask Claude for the JSON again and save that. |

## Ideas to try

- A golden star that grows in over both eyes and sparkles while it glows.
- Raindrops falling from the top of the display, one per eye, that splash at the bottom.
- A cat-like wink: one eye closes with a heart flying out of the other.
- A ball of light that rolls from the left eye into the right eye (use `world`).
- Snowflakes (small sparkles) that drift down slowly with `loop`.
- A surprised look: the pupils widen (gaze `pupil`) and a red ring pulses twice over both eyes.
