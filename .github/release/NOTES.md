OpenKingdoms VERSION_HERE, an engine for Total Annihilation: Kingdoms.

Play in a browser at [openkingdoms.net](https://openkingdoms.net), or download below and play on the desktop.

This release makes computer players much harder to beat with an early
monarch rush, and armies now fight as they march. All players need 0.6.0:
browser players just reload the page.

## Computer players

- With Monarch Expendable off, a computer player keeps its monarch on its
  own ground. The monarch builds near home, fights at home or where its
  side is as strong, and turns back when outmatched or badly hurt. With
  the option on, it plays as before. (#501)
- Computer units switch between their attacks the way the original's
  computer player does, so a monarch now uses its stronger magic. In
  testing, a monarch that chooses its attack won all 50 duels against one
  that used only its first, and a computer player came through an early
  rush in 56 of 60 games, up from 33. (#501)

## In battle

- Units on a move order fire at what they pass, as in the original. They
  keep walking and never chase, and their weapons take whatever comes in
  reach. Offensive and Defensive fire on the move and Passive holds fire.
  (#500)

## Controls

- Hold M while you drag a box to select every unit of yours in it except
  your monarch. With Shift the box adds to the selection. Ctrl+M still
  selects the monarch. (#498)

## Multiplayer

- A new room holds a dropped player's seat for 60 seconds, down from 150.
  (#498)
- A player who rejoins a match now sees the same ships as everyone else on
  the first tick back. Before, an order given on that tick could put the
  match out of step. (#499)

## Multiplayer compatibility

Everyone in a room must be on the same version. A room hosted on another
version is greyed in the list and says to reload or update. After an
update, refresh the page before you host or join, and have everyone else
in the room do the same. Desktop builds older than this one cannot join
games hosted with it, and the other way round. Replays play only on the
version that recorded them.
