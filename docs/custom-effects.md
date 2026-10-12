# Adding your own eye animation

Goal: describe an overlay (for example a heart), upload it to the eyes, and get a new animation button. No
programming needed.

## Four steps

1. Open [efx-prompt.md](efx-prompt.md), copy the whole grey block into Claude on the web (claude.ai), replace the
   idea line with your own idea, and send it.
2. Claude replies with one block of JSON. Copy it into a text file and save it with a name ending in `.json`, for
   example `heart.json`. (A wrong ending is fine, and a file saved as `.efx` also works.)
3. Open the cat's page, `http://eyes-601080.local/`, scroll to **Custom effects**, choose the file, and press
   **Upload and play**. A status line next to the button says whether it worked, or why not. If it does not work,
   copy that message into Claude and ask for a fix (the table in `efx-prompt.md` explains the common ones).
4. The effect now shows up as a ★ button and joins the random pool (if its box is ticked). Press the button to play
   it any time.

Optional: **Eyes Studio** (`uv run python -m eyes_studio`) shows a preview GIF before you upload. Drop the file on
**New effect**, type a name, and press **Send to device**.

The eyes' own page also takes a compiled `.efx` file (made by `efxc build`); it is handled the same way.

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

## Prompt for an assistant

The ready-to-paste prompt, with the format inlined, is in [efx-prompt.md](efx-prompt.md). The full format is in
[efx-format.md](efx-format.md).
