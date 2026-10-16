# Unused animation assets (2026-10-16)

A probe of the 373 GAF files in `data/anims/` inside the retail `data.hpi`
archive. This lists effects and animations that ship with the game but are
not yet consumed by the engine. Each section names the GAF file and its
sequences with frame counts, the original engine feature that used it, and
what the engine would need to display it.

## Effect asset table (Visuals_Init :161407-161505)

The original loads these GAF handles at startup. Items marked with a check
are already drawn by the engine. Unmarked items are available but unused.

### Smoke

- `data/anims/smoke.gaf`
  - `Smoke01` (16 frames)
  - `Smoke02` (16 frames)

Used by the original for weapon `smoketrail` puffs (one puff every
`smokedelay` frames along a projectile's flight path), `startsmoke` (a puff
at the muzzle on fire), and `endsmoke` (a puff at the impact point).

Engine location: `src/render/units.c` projectile tick loop (~line 2100-2260).
Weapon TDF fields `smoketrail`, `smokedelay`, `startsmoke`, `endsmoke` are
documented in docs/notes/2026-09-04-rendering-visuals.md but not parsed.
Add fields to UnitWeaponDef in `include/tak_unit.h`, parse in the weapon
loader (~line 5950), and spawn smoke puffs in the projectile step function.

### Flame (in use)

- [x] `data/anims/flame.gaf` / `fire.gaf`
  - `flame` / `fire01`, `fire02`

Used for flame-stream projectile particles. Already implemented via
`proj_sprite_index("flame", "flame")`.

### Radiance

- `data/anims/radiated.gaf`
  - `Radiance01` (6 frames)
  - `Radiance02` (6 frames)
  - `Radiance03` (6 frames)
  - `Radiance04` (6 frames)
  - `Radiance05` (6 frames)

The original draws these as radiation/magic aura overlays on units affected
by certain weapon types. The visuals table names `radiance02`, `radiance03`,
`radiance04` as separate handles.

Engine location: no status effect system exists yet. Would need a per-unit
status flag in the Unit struct (`include/tak_unit.h`) and a draw pass in
the unit render loop (`src/render/units.c` ~line 15400).

### No-build indicator

- `data/anims/nobuild.gaf`
  - `NobuildX` (1 frame)

Drawn under the cursor when a building placement is invalid (terrain
blocked, overlapping, insufficient clearance). The engine already rejects
invalid placements but shows no visual indicator.

Engine location: `src/render/units.c` `Units_RenderBuildGhostFacing` (~line
13796) draws the placement preview. The footprint validity check is
`Units_CanBuildAt` in the same file. Overlay `NobuildX` on each cell that
fails the check, using the same blit path as the build ghost.

### Mindspin (in use)

- [x] `data/anims/mindspin.gaf` via `flameburst.gaf`
  - `BigMindRing` (9 frames, in `flameburst.gaf`)

Already used for the Harpy mind-control spin. Loaded via
`proj_sprite_index("mindspin", "mindspin")`.

### Transport effects (in use)

- [x] `data/anims/transportfx.gaf`
  - `TransSwirl` (11 frames)

Already used for transport load/unload swirl.

### Death magic (in use)

- [x] `data/anims/deathmagic.gaf`
  - `Death01` (20 frames) -- cleanup completion flash
  - `PurpleDeath` (15 frames) -- resurrection flash

Both sequences are in use.

## Explosion and impact effects

### fx.gaf (weapon explosions)

- `data/anims/fx.gaf` (30 entries)
  - `Explosion` (23 frames) -- generic explosion
  - `smoke 1` (12 frames) -- impact smoke
  - `alfboom1` (8 frames), `alfboom2` (8 frames) -- alternate booms
  - `steam` (12 frames) -- steam/gas effect
  - `Explode2` (19 frames) -- medium explosion
  - `Explode3` (23 frames) -- large explosion
  - `radlogo` (10 frames) -- radiation logo effect
  - `h2oboom1` (10 frames), `H2oBoom2` (14 frames) -- water splash explosions
  - `cannonshell` (10 frames) -- cannon impact
  - `flamestream` (20 frames) -- sustained flame
  - `H2o` (12 frames) -- water splash
  - `Nuke1` (25 frames) -- nuclear/massive explosion
  - `ultrashell` (6 frames) -- heavy projectile impact
  - `radlogohigh` (1 frame) -- radiation marker
  - `fire1` (10 frames) -- fire burst
  - `Explode4` (10 frames), `Explode5` (10 frames) -- smaller explosions
  - `smoke 2` (16 frames) -- heavier smoke
  - `shadow` (1 frame) -- projectile shadow
  - `nuclogo` (10 frames) -- nuke logo
  - `lavasplash` (10 frames), `lavasplashlg` (10 frames), `lavasplashsm` (9 frames)
  - `PlasmaSm` (8 frames), `PlasmaMd` (8 frames) -- plasma impacts
  - `blueshot` (6 frames) -- blue projectile hit
  - `redshot` (10 frames) -- red projectile hit
  - `parablast` (8 frames) -- parabolic blast

Engine location: `src/render/units.c` `load_explosion_classes` (~line 885)
parses `gamedata/explosions/explosions.tdf`. `spawn_impact_effect` (~line
1011) draws the sprite at impact. Weapons bind via `explosionclass`,
`waterexplosionclass`, `lavaexplosionclass` in weapon TDF. The loader and
impact renderer are in place. Water and lava explosion variants are parsed
but the water/lava surface detection to pick them is not built.

### flameburst.gaf

- `data/anims/flameburst.gaf` (7 entries)
  - `Redburst2` (16 frames) -- red flame burst
  - `RedBurp` (8 frames) -- small red burst
  - `FlameburstSm` (11 frames) -- small flame burst
  - `FlameburstMed` (12 frames) -- medium flame burst
  - `Small Burst` (8 frames) -- generic small burst
  - `Small Burst2` (7 frames) -- variant
  - `BigMindRing` (9 frames) -- mind-control ring (used by Harpy)

Flame burst effects referenced by explosion classes for fire-type weapons.

### rings.gaf (magic rings and blue fire)

- `data/anims/rings.gaf` (7 entries)
  - `Big Burst01` (11 frames) -- large magic burst
  - `Big Burst02` (9 frames) -- variant
  - `Blue Burst01` (11 frames) -- blue magic burst
  - `Blue Burst02` (9 frames) -- variant
  - `BlueFire01` (6 frames) -- blue fire ring
  - `BlueFire02` (6 frames) -- variant
  - `BlueFire04` (10 frames) -- larger blue fire

The visuals table lists `BlueFire` as a global handle. These are magic ring
effects drawn around spell-casting units and spell impact areas.

Engine location: god-power weapons are parsed as `nimbus` weapons in
weapon TDF. The spell cast animation would draw the ring around the
casting monarch. No spell rendering system exists yet. The `nimbus`
side-data field names the per-side nimbus GAF.

## Building and construction

### flames.gaf (building damage fire)

- `data/anims/flames.gaf` (11 entries)
  - `Flame Small` (26 frames) -- small damage fire
  - `Flame Medium` (26 frames) -- medium damage fire
  - `Flame Large` (14 frames), `Flame Large2` (14 frames) -- large fires
  - `Flame Small2` through `Flame Small5` (9-11 frames) -- variants
  - `Flame Medium2` (15 frames), `Flame Medium3` (6 frames) -- variants
  - `Flame Large3` (12 frames) -- variant

Engine location: `src/render/cob_vm.c` `OP_EMIT_SFX` (~line 873) is a
no-op that consumes its operands. The COB scripts call EMIT-SFX with a
piece index and sfx type to request fire/smoke on a building piece.
Implementing this means mapping sfx types to `flames.gaf` sequences and
drawing them at the named piece's world position. The `fix/damaged-building-flames`
branch has partial work on this.

### sparklies.gaf (build sparkles)

- `data/anims/sparklies.gaf` (3 entries)
  - `Sparkly01` (12 frames)
  - `Sparkly02` (12 frames)
  - `Sparkly03` (10 frames)

Additional sparkle effects. The engine uses per-side build sparkle GAFs
(`aramonbuild`, `tarosbuild`, `verunabuild`, `zhonbuild`) referenced from
`sidedata.tdf`. These sparklies may be fallback or additional decoration.

## Special effects

### CrystalBallEffect

- `data/anims/crystalballeffect.gaf` (1 entry)
  - `CrystalBallEffect` (24 frames)

The crystal ball scrying effect, used when a player activates the crystal
ball ability to reveal an area of the map temporarily.

Engine location: `src/ui/hud.c` already draws the CrystalBall HUD widget
(~line 1181) as a mana pool gauge. The scrying ability would reveal fog
in a radius, playing this effect at the reveal point. `src/game/fog.c`
handles fog state.

### transport.gaf (alternate transport effect)

- `data/anims/transport.gaf` (1 entry)
  - `Transport Swirl2` (18 frames)

A second transport swirl variant, longer than the 11-frame `TransSwirl`
in `transportfx.gaf`.

## Weapon visual features not yet implemented

These weapon TDF flags are parsed by the original but not yet drawn:

- `smoketrail` + `smokedelay`: periodic smoke puffs along a projectile path.
  Engine: add fields to UnitWeaponDef (`include/tak_unit.h`), parse in the
  weapon loader (`src/render/units.c` ~line 5950), draw in the projectile
  step loop (~line 2100-2260).
- `startsmoke`: a smoke puff at the weapon's muzzle on firing. Engine:
  spawn in the projectile launch function (~line 1060-1180).
- `endsmoke`: a smoke puff at the impact point. Engine: spawn in
  `shot_burst` (~line 1954).
- `nimbus`: a glowing aura on the god unit while a spell weapon fires.
  Engine: draw in the per-unit render pass, keyed by the side-data nimbus
  field parsed from `gamedata/sidedata.tdf` (`src/game/sides.c`).
- Projectile shadows: `shadowgaf` + `shadowart` are already parsed and
  drawn by `projectile_casts_shadow` (~line 15694). In use.

## Side-specific effects not yet implemented

Side data (`gamedata/sidedata.tdf`) names these per-faction effects:

- `nimbus`: the god-power glow effect name, per side (e.g. `nimbus_aramon`)
- `frozengaf`: frozen unit overlay when hit by ice weapons
- `stonegaf`: petrified unit overlay when hit by stone weapons

## Summary

| Category | Status | Art available |
|---|---|---|
| Build sparkles | Done | Per-side GAFs |
| Resurrection flash | Done | `PurpleDeath` |
| Cleanup flash | Done | `Death01` |
| Transport swirl | Done | `TransSwirl` |
| Mind-control spin | Done | `mindspin` |
| Flame particles | Done | `flame` |
| Projectile smoke trails | Not implemented | `Smoke01`, `Smoke02` |
| Invalid placement indicator | Not implemented | `NobuildX` |
| Building damage fires | Partially done | `flames.gaf` (11 variants) |
| Weapon explosion classes | Loaded, not all drawn | `fx.gaf` (30 entries) |
| Magic rings / blue fire | Not implemented | `rings.gaf` (7 entries) |
| God nimbus glow | Not implemented | Per-side nimbus |
| Frozen/stone overlays | Not implemented | Per-side GAFs |
| Projectile shadows | Done | Per-weapon `shadowgaf` |
| Crystal ball scrying | Not implemented | `CrystalBallEffect` |
| Radiance auras | Not implemented | `radiated.gaf` (5 entries) |
