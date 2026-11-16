# The original's keys (2026-11-15)

The original reads its battle keys from `Keys.TDF`, a loose text file in
the game's root folder and not inside any archive. Each line names a
key, with or without Ctrl, Shift or Alt, and the action it runs. Letters
come in a lower and an upper case pair with the same action, so Shift on
a letter changes nothing. Before this change the engine bound the select
keys, Ctrl+D, K, Tab, the digits, the speed keys and the damage bar key
from that file. It left the order letters and most of the function keys
unbound, and it panned the camera on W, A, S and D, which the file binds
to nothing that moves the view.

The second source is the Kingdoms quick reference at
https://guide.coreprime.net/en/kingdoms/quick-reference/, whose keys
were checked one by one in the original game. Every key bound here
appears in it except C (Clear) and F12 (ClearChat), which come from
Keys.TDF alone. It does not mention E, F9 or Ctrl+C, X and V, so for
those the file is the only source.

The rule for the keys is this. A key the file binds to a feature the
engine has is bound, whether or not the quick reference lists it, which
keeps C and F12. A key the engine already had that the original lacks,
such as V for the 3D view or the brackets to turn a building, stays as
long as it takes no key the original binds.

## What the file binds

Order letters, each a `UnitCommand` with the order's name:

| Key | Order |
| --- | --- |
| M | Move |
| A | Attack |
| G | Guard |
| P | Patrol |
| L | Load |
| U | Unload |
| H | Heal |
| C | Clear |
| S | Stop |
| K | ToggleCloak |
| O | ToggleGateOpen |

The other keys this note is about:

| Key | Action |
| --- | --- |
| Pause | TogglePause |
| F1 | the F2 menu |
| F2 | Briefing |
| F3 | MoveToLastReport |
| F4 | NetScorecard |
| F5 to F8 | MapBookmarkRetrieve 1 to 4 |
| Ctrl+F5 to F8 | MapBookmarkStore 1 to 4 |
| F12 | ClearChat |
| Ctrl+F1 to F3 | UnitCommand PrimaryWeapon, SecondaryWeapon, SpecialWeapon |
| Ctrl+S, Ctrl+Shift+S | SaveGame |
| Ctrl+L, Ctrl+Shift+L | LoadGame |
| D | DiplomacyMenu |
| N | MoveToNextUnit |
| T, Shift+T | TrackUnit, TrackUnitReverse |
| Ctrl+M | SelectUnits Monarch, and TrackUnit |
| Ctrl+C, Ctrl+X, Ctrl+V | copy, cut and paste a unit through a clipboard |

Twelve letters are empty in both cases: B, E, F, I, J, Q, R, V, W, X,
Y and Z. E is the one that matters, because no line in the file names
resurrect, so resurrect has no key. F9 is empty too, and the file says
to leave it so, since the game takes screenshots on it. F10, F11,
Shift+F1, Shift+F2, Ctrl+F10, backslash and three Ctrl+Shift function
keys run debug actions.

## Letters and keyboard layouts

The file binds characters: LOWER_A is the key that types a. It even has
SYMBOL_ lines that put the damage bars on characters for French, German,
Italian and Spanish keyboards. The engine reads each letter by scancode,
which is a place on the keyboard named after the US layout. On a QWERTY
keyboard the two agree. On an AZERTY keyboard the scancode for A is the
key labelled Q, so Attack sits under Q there. The Ctrl select keys
already worked this way before this change.

## What the engine does now

- Each order letter presses the sidebar button for its order, through
  the same path a click takes. A targeting order arms on the press and
  a second press disarms it. Stop is given at once. A selection with no
  such button takes nothing, as a click on the empty slot would give,
  and a unit inspected from another side takes nothing. Ctrl, Alt or
  Cmd with a letter is never an order. Cmd is the Windows key off a
  Mac, and a Mac browser keeps Cmd+S, Cmd+A and the like for itself.
  Cmd is refused on the keys this change binds: the order letters, K,
  F1, the function keys, the weapon keys and the bookmarks. The select
  keys, the digits, V, Tab and the turn and speed keys were there
  before and still let it through.
- O presses whichever of Active and Inactive is not lit, as K already
  does for the cloak pair (legacy:151278-151287).
- Ctrl+F1 to F3 press the three weapon buttons, and a weapon button that
  is greyed out takes no key, as it takes no click
  (legacy:329637-329640). Ctrl+F1 no longer opens the menu.
- F5 to F8 put the camera back where Ctrl with the same key stored it,
  in the classic view and the 3D view alike. The 3D view now takes the
  classic camera before its own input when something else moved it,
  which also stops a screen shake from moving the 3D camera for good.
  Before, a 40 frame shake of magnitude 12 left it at 786,779 where it
  had stood at 800,800. A slot nothing was stored in does nothing. The
  four slots last the battle and are not in a save or a replay, since
  they are a view and not the game.
- Pause pauses and resumes a skirmish or a campaign mission. In a replay
  it keeps the replay's own pause.
- F2 raises the briefing of a campaign mission, and putting it away
  leaves the clock as F2 found it (D-019). A skirmish has no briefing.
  While the briefing is up it owns the keys and the mouse. Enter puts
  it away and opens no chat line, F1 opens no menu under it, and the
  click that puts it away gives the selection no order. The briefing a
  mission opens with did all three wrong on main already. F2 made them
  easy to meet, and they are fixed here.
- A press held on the battle while a dialog or a plate comes up gives
  no order when the button is let go. The briefing, the in game menu,
  the Ctrl+D question and the rejoin and snapshot plates each take the
  mouse while they are up and cancel a drag in flight, as a release
  anywhere else does. On main a press held across F1 or the Ctrl+D
  question gave a Move on its release.
- F12 clears the chat lines.
- In battle, Shift, Alt or Cmd with a function key or Pause does
  nothing. For Shift this was seen in the original. So Shift+F2 does
  not raise the briefing, though the file gives Shift+F2 to a debug
  probe the engine leaves out, and Shift+F1 and Alt+F1 no longer open
  the menu. Ctrl+Shift+S and Ctrl+Shift+L still save and load, as the
  file binds them. This is the battle's input only. A replay's own
  Pause and the briefing's dismiss keys still answer with any of them
  held.
- Ctrl+S and Ctrl+L open the in game menu with its save or load dialog
  up, as the menu's own buttons do. The match menu has neither button,
  so in a live match they do nothing, and a replay saves nothing, so
  there Ctrl+S does nothing. A dialog that is cancelled, or a save that
  is made, leaves the menu up behind it, as the menu's own buttons do.
- W, A, S and D no longer pan the camera in either view. The arrows and
  the screen edge still do.
- A box dragged with M held still leaves the monarch out (D-046). The
  press of M has armed Move by then, and the drag drops it.

## Not bound, and why

- Pause in a live match. The original pauses a networked game over the
  wire (packet 0x18, docs/notes/2026-09-04-multiplayer-hapinet.md).
  The engine means to make pause the server's pacing (N-004 in
  docs/MANUAL_DEVIATIONS.md), so Pause waits for that.
- D, the diplomacy dialog: the engine has no such dialog to open.
- E and F9, and the other empty letters: the file binds nothing to them.
- N: the engine has no way to move the view to the next unit.
- T and Shift+T, and the tracking half of Ctrl+M: the engine has no
  camera that follows a unit.
- F3: the engine keeps no last report to move the view to.
- F4: the engine's scorecard opens only when a battle ends.
- Ctrl+C, Ctrl+X and Ctrl+V: the engine has no clipboard of units.
- The debug keys are left out on purpose.

## Ours

The keys are fixed. A player cannot rebind them, where the original
reads them from a file anyone can edit. The letters follow the key's
place and not its character, as above.
