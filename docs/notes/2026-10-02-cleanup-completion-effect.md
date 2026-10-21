# Cleanup completion effect

A sweep that finished took the feature away with nothing to show for it.
A finished sweep now plays one effect at the feature's centre, on the
ground or the water there, our own addition (D-044). The place is read
before the feature goes, since removing one moves the others down the
list.

Only a finished sweep plays it. A feature that rots away, is removed by
something else, or whose sweep is called off plays nothing. The mana a
sweep pays back and how long it takes are unchanged. Missing art or a
full effect list drops the effect and never the sweep.

## The art

The retail archive's `anims/deathmagic.gaf` holds two sequences:
`Death01`, 20 frames, and `PurpleDeath`, 15 frames, which a raise
already plays. `Death01` is a dark red dispersal, and the sweep uses it.
Nothing in the game's data ties it to a sweep. The original keeps a
swept feature a moment before it clears the cell and plays no sprite.

It plays at the engine's effect rate, four ticks a frame, and is gone
after 80 ticks, in both views.

## Tests

`reclaim_clears_feature_and_pays_mana` and
`the_sweep_clears_a_corpse_and_keeps_it_from_rotting` check that no
effect plays early, exactly one plays when the feature goes, and for the
first where it stands, that its frames advance and that it ends.
