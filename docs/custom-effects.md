# Adding your own eye animation

Goal: describe an overlay (for example a heart), turn it into **one small file**, drop that file on the eyes, and
get a new animation button.

## The short way (browser only)

1. Get a `.json` description (ask an assistant, see the prompt below, or start from `examples/efx/love.json`).
2. Open **Eyes Studio** (`uv run python -m eyes_studio`) and drop the file on **New effect**. You see a preview GIF.
3. Type a name and press **Send to device**. The new button (marked with a star) appears; it also shows under
   **Effects on device** and in the random pool list.

Already have an `.efx` file? Drop it the same way, or open the eyes' own page (`http://eyes-600ed0.local/`), section
**Custom effects**, choose the file, press **Upload effect**.

## Command line

```
cd tools
uv run python efxc.py preview ../examples/efx/love.json   # writes love.gif, open it
uv run python efxc.py build   ../examples/efx/love.json   # writes love.efx
uv run python efxc.py check   ../examples/efx/love.efx    # says exactly what is wrong, if anything
EYES_HOST=eyes-600ed0.local uv run python efxc.py upload ../examples/efx/love.json --pool
```

then `eyesctl anim love` (or the button) plays it.

## Slots

16 slots. Uploading under a name that exists replaces that effect; a 17th new name is refused with
"all effect slots are used". Delete one first (`efxc delete <name>` or the Delete button).

## Prompt for an assistant (copy and fill in the idea)

> Write a JSON file for the eyes "efx" format described in `docs/efx-format.md`. Idea: *<describe it, e.g. "a pink
> heart pops up over both eyes, beats twice, small hearts float up">*. Rules: duration 2000-4000 ms; coordinates are
> pixels on a 240x240 round display (centre 120,118); use at most 10 shapes and 6 keys per shape; keys need strictly
> increasing `t`; use `world: true` only for something travelling from one eye to the other (x 0..240 left eye,
> 296..536 right eye); use size 0 to hide a shape. Give only the JSON. Then check it with
> `efxc preview file.json` and fix anything that looks wrong.
