# The loading screen backdrop (2026-09-10)

Symptom that started this: a skirmish loaded in the browser showed a black
hole where the original shows a stained glass window.

## What the original draws

The load screen is `data/guis/loadscreen.gui` and nothing else
(legacy:158058). Three layers make the picture, in the order the file
authors them.

| Widget | Rect | Art |
|---|---|---|
| `LoadScreen` (root) | 0, 0, 640x480 | `loadingbg.gaf` / `LoadingBG` frame 0 |
| `Background` | 168, 46, 423x351 | `loadingbw.gaf` / `LoadingBW` frame 0 |
| `AnimatedControl` | 168, 46, 423x351 | `loadingc.gaf` / `LoadingC` frame 0 |

`LoadingBG` is a stone wall with an empty arch cut out of it. `LoadingBW`
is a stained glass window unlit and `LoadingC` is that same window lit.
Each GAF carries its own palette in a `.pcx` of the same stem next to it,
so none of them go through the central palette table.

On top of that the original starts `Movies\Gui\Loadscreen.bik` at the
`AnimatedControl` widget's top left corner (legacy:158297). It passes the
player only the x and y of the widget, never a size, so the clip plays at
its own 422x351 and the last column of the authored glass stays visible
beside it. When the clip cannot start, the still art is what the player
sees, which is the case in the browser because that build has no FFmpeg.

The dialog is authored with placeholder text and with per player progress
rows for network games. The original clears and hides all of that before
the screen is ever drawn (legacy:158031). It blanks `LoadText`, sets
`Percent` to `0%`, writes the map name into `LoadMap`, and hides
`PlayerName<N>`, `PlayerPercent<N>` and `PlayerProgress<N>` for eight
slots. Per frame it then writes the current phase into `LoadText` and the
percentage into `Percent` (legacy:158380).

## Why ours drew something else

The `.gui` reader had no record shape for widget type 16, the progress
bar, so it fell through to the guess of one field and desynced on the
first one in the file. `loadscreen.gui` authors `MainProgress` as the
fourth widget, ahead of both glass layers, so everything from there on
was dropped. The wall drew, the arch stayed empty, and only a clip
covered it. With no clip the arch was black.

A progress bar writes a version, then value, step and max, then a byte
flag that is read back only above version 1 (legacy:328245,
legacy:328267). The base record follows. Every shipped dialog is version
2, so all four fields are present:

```
16 2 50 1 100 0
```

`loadscreen.gui` and `byupdateconsole.gui` are the only shipped dialogs
that use the type.

## Loose ends

`data/anims/loading.gaf` holds a single 640x480 composite of the wall
with the unlit window already in it. No dialog references it. It reads
like an earlier version of the screen kept around after the art was split
into three layers.
