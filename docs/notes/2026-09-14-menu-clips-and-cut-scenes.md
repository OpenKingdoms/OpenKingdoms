# Menu door clips and the cut scenes

How the original runs its Bink clips: the four doors on the main menu,
the full screen reels (logo, intro, credits) and the arch on the loading
screen. Line anchors are `:NNNNN` in the legacy reference. The engine
side is `src/render/bink_player.c`, `src/render/main_menu.c`,
`src/ui/credits.c`, `src/ui/loading.c` and `src/main.c`.

## Pacing

A clip is stepped by the decoder's own clock. The draw routine waits
on the decoder until the next frame is due, decodes that one frame,
draws it and moves the frame counter on by one (:35289-35341). Nothing
in the game code skips a frame to catch up. The engine keeps the same
rule: one frame at most per tick, and whatever a stalled frame left
over beyond one frame duration is forgotten.

The container header carries the rate. The door clips are 30 fps, the
loading arch 20 fps and the reels 15 fps. The demuxer's guessed rate is
wrong on the one frame stills, so the header's rate is what the player
reads.

## The doors

Each door is a button with eight states (:147778). State 2 is rest,
drawn from the sprite sheet. States 4 to 7 play clips 4 to 7,
`Movies/Gui/<name>N.bik`: 4 is the still after a click, 5 the enter
clip, 6 the hover clip, 7 the leave clip.

Entering sets the hover flag and, from rest, starts clip 5 (:148064-148070).
Each tick the button hands over only once the clip's frame counter has
reached its frame count (:148038-148054): 5 goes to 6, 7 goes to 2,
and 4 goes to 6 while the flag is up or to 2 once it is down. Leaving
is honoured only in state 6, where it clears the flag and starts clip
7 (:148027-148035). A cursor that leaves during the enter clip waits
for it. State 6 has no hand-over of its own, and the decoder wraps a
clip nobody stops (:35341), so the hover clip loops while the cursor
stays. machine6.bik is forty frames of the machine idling and girl6.bik
thirty, which only makes sense as a loop. knight6.bik and snort6.bik
are single frames.

A click drops the door to state 4 (:148002-148011).

## The full screen reels

One player runs the logo at startup (:241882, skipped by the
`-skiplogo` switch, :252020), the intro on the first Story click of a
session (:140763-140767, the flag is never cleared) and the credits from
the Credits door (:140736-140738). The music is paused around the reel.

The reel ends on its last frame, or early on a key. The player's message
loop ends on a character message or a system key message, that is any
key that types a character, Escape and Enter included, and Alt or F10
(:34816-34849). A mouse click is dispatched and ignored, and the mouse
events queued during the reel are drained afterwards (:34793-34797).
Skipping the victory reel also skips the credits that would follow it
(:154103-154106).

## The loading arch

The arch's clip, `Movies/Gui/Loadscreen.bik`, is opened at the
AnimatedControl widget's corner at the clip's own size (:158297-158299).
It is not played. Each tick the frame is set to the load percentage's
share of the frame count, never below the first frame and never past
the last (:158702-158710). The exact float expression is not readable in the
legacy reference, but the inputs are the percentage and the count and the
result is clamped to 1..count.
