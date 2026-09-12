# Total Annihilation: Kingdoms - Complete Data Format Reference

This document provides a comprehensive reference for every data file format found
in the extracted HPI archives of Total Annihilation: Kingdoms (TAK). It is intended
for developers writing C parsers or tools for these formats.

All offsets are from file start. All multi-byte integers are **little-endian** unless
otherwise noted.

---

## Table of Contents

1. [TDF - Text Definition Files](#1-tdf---text-definition-files)
2. [FBI - Unit Definition Files](#2-fbi---unit-definition-files)
3. [GUI - GUI Layout Definitions](#3-gui---gui-layout-definitions)
4. [OTA - Mission/Map Metadata](#4-ota---missionmap-metadata)
5. [TXT - AI Scripts](#5-txt---ai-scripts)
6. [SCC - Source Control Metadata](#6-scc---source-control-metadata)
7. [GAF - Graphic Archive Format](#7-gaf---graphic-archive-format)
8. [TAF - Truecolor Animation Format](#8-taf---truecolor-animation-format)
9. [TNT - Terrain Data](#9-tnt---terrain-data)
10. [3DO - 3D Object Models](#10-3do---3d-object-models)
11. [COB - Compiled BOS Script](#11-cob---compiled-bos-script-bytecode)
12. [PCX - PCX Image Format](#12-pcx---pcx-image-format)
13. [PAL - Palette Files](#13-pal---palette-files)
14. [WAV - Audio Files](#14-wav---audio-files)
15. [ALP - Alpha/Transparency Lookup Tables](#15-alp---alphatransparency-lookup-tables)
16. [SHD - Shadow Lookup Tables](#16-shd---shadow-lookup-tables)
17. [LHT - Light Remap Tables](#17-lht---light-remap-tables)
18. [GRY - Grayscale Remap Tables](#18-gry---grayscale-remap-tables)
19. [BLU - Blue Channel Remap Tables](#19-blu---blue-channel-remap-tables)
20. [CRT - Creature/Script Data](#20-crt---creaturescript-data)
21. [TSF - Truecolor Sprite Frame Definition](#21-tsf---truecolor-sprite-frame-definition)
22. [BAD - Section Terrain Blocks (TNT variant)](#22-bad---section-terrain-blocks)
23. [GAO/GAO2 - Graphic Archive Object (Truecolor)](#23-gaogao2---graphic-archive-object-truecolor)
24. [PCO - Palette Color Object](#24-pco---palette-color-object)
25. [FNT - Font Files](#25-fnt---font-files)

---

## Common Text Format: TDF-style Key-Value Blocks

Formats 1-5 (TDF, FBI, OTA, and some TXT) all share a common text-based structure
inherited from the original Total Annihilation engine. This is a hierarchical
key-value format with the following grammar:

```
file        := block*
block       := '[' section_name ']' '{' entry* '}'
entry       := key '=' value ';'? | block
comment     := '//' rest_of_line
```

### Parsing Rules

- Section names are enclosed in square brackets: `[SectionName]`
- Bodies are enclosed in curly braces: `{ ... }`
- Key-value pairs use `=` as separator
- Values may or may not end with `;` (inconsistent across files)
- `//` begins a line comment
- Whitespace (tabs, spaces, newlines) is insignificant outside values
- Keys are case-insensitive in the engine
- Nested sections are allowed to arbitrary depth
- Duplicate section names within the same parent are valid (used for arrays)
- Empty values (e.g. `key=;`) are valid

### C Parsing Strategy

```c
typedef struct TDFEntry {
    char *key;
    char *value;              // NULL if this is a section
    struct TDFEntry *children; // linked list of children (if section)
    struct TDFEntry *next;     // next sibling
} TDFEntry;
```

A recursive descent parser that:
1. Skips whitespace and comments
2. If `[` found, read section name until `]`, then expect `{`, parse children recursively until `}`
3. Otherwise read key until `=`, read value until `;` or newline or `}`
4. Values containing spaces are NOT quoted - they are raw text until the line terminator

---

## 1. TDF - Text Definition Files

**Location:** `data/gamedata/`, `data/canbuild/`, `data/camps/`, `data/features/`, `data/gamedata/soundclasses/`
**File count:** 1,159 files
**Size range:** 28 bytes to ~7 KB (largest are soundclasses.tdf at ~6 KB)
**Purpose:** General-purpose configuration files for game systems

TDF files use the common text block format described above. They serve many
different roles depending on their location:

### 1.1 Sidedata TDF (`data/gamedata/sidedata.tdf`)

Defines the playable factions/sides in the game.

**Complete example:**
```
[SIDE0]
{
    name=ARAMON;
    nameprefix=ARA;
    commander=ARAKING;
    palette=ara_textures.pal;
    buildpalette=arabipal.pcx;
    buildsparklygaf=aramonbuild;
    buildsparklyanim=aramonbuild;
    resurrectsparklygaf=aramonbuild;
    resurrectsparklyanim=aramonbuild;
    nimbus=nimbus_aramon;
    waterheight=40;
    musictracks = 1 2 3 4 5;
    logogaf=colorlogos2;
    logoart=arateam;
    FogColor = 255 255 255;
    stonegaf = ara_stone;
    stoneanim = basiliskstone;
    God = ARAGOD;
    underattack_sound = AlarmAra;
    underattack_delay = 30;
}
```

**Known sidedata fields:**

| Field | Type | Description |
|-------|------|-------------|
| name | string | Faction name (ARAMON, TAROS, VERUNA, ZHON, etc.) |
| nameprefix | string | 3-letter unit name prefix (ARA, TAR, VER, ZON) |
| commander | string | Unit name of the faction's monarch/commander |
| palette | string | Texture palette filename (.pal) |
| buildpalette | string | Build icon palette filename (.pcx) |
| buildsparklygaf | string | GAF file for build sparkle effect |
| buildsparklyanim | string | Animation name within the GAF |
| resurrectsparklygaf | string | GAF for resurrection effect |
| resurrectsparklyanim | string | Animation name for resurrection |
| nimbus | string | God nimbus effect name |
| waterheight | int | Default water level for this side |
| musictracks | int list | Space-separated list of music track numbers |
| logogaf | string | GAF containing team logos |
| logoart | string | Specific logo art name within GAF |
| FogColor | 3x int | RGB fog color values (space-separated) |
| stonegaf | string | GAF for basilisk stone effect |
| stoneanim | string | Animation name for stone effect |
| God | string | Unit name of the faction's god unit |
| underattack_sound | string | Sound played when under attack |
| underattack_delay | int | Seconds between "under attack" alerts |

The game defines 7 sides (0-6): ARAMON, TAROS, VERUNA, ZHON, LIFEFORMS,
NONPLAYERCHARACTERS, WANDERING_MONSTERS. The last three are non-playable.

### 1.2 CanBuild TDF (`data/canbuild/<builder>/<unit>.tdf`)

Defines the build menu for buildings/units. Extremely simple format.

**Complete example** (`data/canbuild/arabuild/araat.tdf`):
```
[Menu]
{
    Priority = 4;
}
```

**Fields:**

| Field | Type | Description |
|-------|------|-------------|
| Priority | int | Position/ordering in the build menu (1-based) |

The directory structure encodes the relationship: the parent directory name is
the builder unit, the filename is what it can build.

### 1.3 Campaign TDF (`data/camps/book of darien.tdf`)

Defines campaign mission sequences.

**Structure:**
```
[HEADER]
{
    campaignside=Core;
}
[MISSION0]
{
    missionfile=takmission01_mt.ota;
    missionname=takmission01_mt;
}
[MISSION1]
{
    missionfile=takmission02_mt.ota;
    missionname=takmission02_mt;
}
// ... continues for all missions
```

**Fields:**

| Field | Type | Description |
|-------|------|-------------|
| campaignside | string | Which side/faction the campaign is for |
| missionfile | string | .ota filename for the mission |
| missionname | string | Internal mission identifier |

### 1.4 Explosions TDF (`data/gamedata/explosions/explosions.tdf`)

Defines explosion visual classes referenced by FBI weapon definitions.

```
[small explosion]
{
    [0]
    {
        gaf = small1;
        anim = small1;
    }
    [1]
    {
        gaf = small2;
        anim = small2;
    }
    // ...
}
```

Each explosion class contains numbered sub-entries. The engine picks randomly
from these when displaying the effect.

### 1.5 MoveInfo TDF (`data/gamedata/moveinfo.tdf`)

Defines movement classes used by units (referenced via `movementclass` in FBI).

```
[CLASS0]
{
    Name=GROUND2;
    FootprintX=2;
    FootprintZ=2;
    MaxWaterDepth=20;
    MaxSlope=30;
    MaxWaterSlope=30;
}
```

**Fields:**

| Field | Type | Description |
|-------|------|-------------|
| Name | string | Movement class name (referenced in FBI files) |
| FootprintX | int | Width of pathfinding footprint in map units |
| FootprintZ | int | Depth of pathfinding footprint in map units |
| MaxWaterDepth | int | Max traversable water depth |
| MinWaterDepth | int | Min water depth (for naval units) |
| BadMinWaterDepth | int | Depth below which pathing is penalized |
| MaxSlope | int | Max traversable terrain slope (0-255) |
| BadSlope | int | Slope above which pathing is penalized |
| MaxWaterSlope | int | Max traversable underwater slope |
| BadWaterSlope | int | Water slope penalization threshold |

### 1.6 Feature TDF (`data/features/all worlds/*.tdf`)

Defines map features (corpses, stones, reclaimable objects).

**Complete example** (`araarch_stone.tdf`):
```
[araarch_stone]
{
    world=All Worlds;
    damage=4000;
    description=Stone Archer;
    object=araarch;
    footprintx=2;
    footprintz=2;
    height=0;
    noshadow=0;
    blocking=1;
    hitdensity=100;
    resurrectable=1;
    reclaimable=1;
    isstone=1;
}
```

### 1.7 Soundclass TDF (`data/gamedata/soundclasses/*.tdf`)

Two types exist:

**Unit-specific soundclass** (e.g., `araarch.tdf`):
```
[ARAARCH]
{
    prioritized = 1;
    [attack]  { TONEARA = 1.0; }
    [default] { TONEARA = 1.0; }
    [guard]   { TONEARA = 1.0; }
    [move]    { TONEARA = 3.0; ARAARCHMOV1 = 1.0; }
    [patrol]  { TONEARA = 1.0; }
    [select]  { TONEARA = 3.0; ARAARCHSEL1 = 1.0; }
}
```

Sound entries map action types to WAV file names with probability weights (floats).
The engine randomly selects based on relative weights.

**Hit sound soundclass** (`soundclasses.tdf`):
```
[sword]
{
    [default] { sound0=swrdhit1.wav; }
    [flesh]   { sound0=SWRDFL01.wav; sound1=SWRDFL02.wav; ... }
    [armor]   { sound0=SWRDAR01.wav; ... }
    [wood]    { sound0=SWRDWOD1.wav; ... }
    [stone]   { sound0=SWRDROK1.wav; ... }
}
```

Indexed by weapon type (outer section) and body type (inner section).

### 1.8 Other TDF Files

- **keys.tdf** - Keyboard binding configuration: `[CUSTOMKEYS] { LOWER_A = UnitCommand Attack; ... }`
- **gods.tdf** - God unit timing: `[TIMING] { GameChance = 0.1; AppearTimeMin = 30.0; ... }`
- **interface.tdf** - Interface settings: `[InterfaceMusic] { musictracks = 15; }`
- **render.tdf** - Render settings: `[RENDERINFO] { transparentcolor=5; }`
- **damageflames.tdf** - Damage flame animations: `[smallflame] { gaf = flames; anim = flame small; }`
- **effects.tdf** - Particle effects (lightning, fire, etc.) with palette ramps and emitter definitions
- **ainames.tdf** - AI opponent name pool

---

## 2. FBI - Unit Definition Files

**Location:** `data/units/`
**File count:** 502 files
**Size range:** 645 bytes (aranull.fbi) to 3,523 bytes (aragren.fbi)
**Purpose:** Defines all properties of a game unit (stats, capabilities, weapons)

FBI files use the standard TDF key-value format. Every FBI file has at minimum
a `[UNITINFO]` section, and may have `[WEAPON1]`, `[WEAPON2]`, `[WEAPON3]` sections.

### Complete Example 1: Mobile Combat Unit (`araarch.fbi`)

```
[UNITINFO]
{
    acceleration = 10;
    bloodcolor1 = 160 35 0;
    bloodcolor2 = 170 40 0;
    bloodcolor3 = 180 30 5;
    bmcode = 1;
    bodytype = flesh;
    brakerate = 10;
    buildcost = 325;
    buildtime = 125;
    canattack = 1;
    canguard = 1;
    canmove = 1;
    canpatrol = 1;
    canstop = 1;
    category = ARA BALLISTIC ATTACK;
    copyright = Copyright 1999 Humongous Entertainment. All rights reserved.;
    corpse = araarch_dead;
    damagecategory = Human;
    defaultmissiontype = Standby;
    description = Aramon;
    experiencepoints = 7;
    fireatwillrandom = 1;
    healtime = 0.520833333;
    maneuverleashlength = 500;
    maxdamage = 1100;
    maxvelocity = 1.25;
    mogriumincome = 0;
    mogriumstorage = 0;
    movementclass = GROUND2;
    name = Archer;
    objectname = ARAARCH;
    roadmultiplier = 1.21;
    shadowart = shadow03;
    shadowgaf = shadows;
    shootme = 1;
    side = ARA;
    sightdistance = 180;
    soundcategory = ARAARCH;
    soundclass = ARAARCH;
    standingunitorder = 1;
    stone = araarch_stone;
    tedclass = Aramon;
    turninplacerate = 2400;
    turnrate = 2400;
    unitname = ARAARCH;
    unitnumber = 1;
    upright = 1;
    version = 1;
    watermultiplier = 0.81;
}

[WEAPON1]
{
    aimtolerance = 1024;
    model = Araarrow;
    name = Bow and Arrows;
    range = 450;
    reloadtime = 3;
    soundhitclass = arrow;
    type = Ballistic;
    waterexplosionclass = small water explosion;
    weaponvelocity = 750;

    [DAMAGE]
    {
        default = 213;
    }
}
```

### Complete Example 2: Building/Factory Unit (`aracastl.fbi`)

```
[UNITINFO]
{
    bodytype = stone;
    buildangle = 8192;
    buildcost = 9204;
    builddistance = 100;
    builder = 1;
    buildtime = 1522;
    canmove = 1;
    canpatrol = 1;
    canstop = 1;
    cantbestoned = 1;
    category = ARA FACTORY;
    copyright = Copyright 1999 Humongous Entertainment. All rights reserved.;
    corpse = aracastl_dead;
    corpseadjustz = 3;
    damagecategory = factory;
    description = Aramon;
    experiencepoints = 100;
    footprintx = 7;
    footprintz = 14;
    healtime = 3.17083;
    maxdamage = 25116;
    maxslope = 15;
    maxwaterdepth = 0;
    mogriumincome = 1;
    mogriumstorage = 200;
    name = Keep;
    objectname = ARACASTL;
    radardistance = 1000;
    shadowgaf = shadows;
    shootme = 1;
    side = ARA;
    sightdistance = 300;
    soundcategory = ARACASTL;
    soundclass = ARACASTL;
    tedclass = Aramon;
    unitname = ARACASTL;
    unitnumber = 7;
    unitstandorders = 0;
    version = 1;
    wind = 1;
    workertime = 10;
    yardmap = ....... ....... ....... ooooooo ooooooo ooooooo ooooooo
              ooooooo ooooooo ooooooo ccccccc ccccccc ccccccc ccccccc;
}
```

### Complete Example 3: Monarch/Commander (`araking.fbi`)

(See full contents in the data - includes 3 weapon sections with lightning,
meteor, and earthen wave spells, plus mana costs, nimbus effects, etc.)

### Comprehensive FBI Field Reference

#### Core Identity Fields
| Field | Type | Description |
|-------|------|-------------|
| unitname | string | Internal unit identifier (uppercase, e.g., ARAARCH) |
| name | string | Display name (e.g., "Archer") |
| description | string | Faction name for display |
| side | string | 3-letter faction code (ARA, TAR, VER, ZON, LIF, NPC, MON) |
| objectname | string | 3D model name in objects3d/ (without .3do) |
| unitnumber | int | Unique numeric ID |
| version | int | File format version (always 1) |
| category | string | Space-separated category tags for selection/targeting |
| tedclass | string | Editor classification |

#### Combat Stats
| Field | Type | Description |
|-------|------|-------------|
| maxdamage | int | Maximum hit points |
| buildcost | int | Mogrium cost to build |
| buildtime | int | Time in game ticks to build |
| experiencepoints | int | XP value when killed |
| healtime | float | Seconds per HP of natural healing (lower = faster) |
| maxmana | int | Maximum mana pool |
| manarechargerate | int | Mana regeneration per tick |
| damagecategory | string | Damage type (Human, Monarch, factory, etc.) |
| bodytype | string | Material type for hit sounds (flesh, armor, stone, scale, wood) |
| shootme | bool | Whether this unit can be targeted |
| cantbestoned | bool | Immune to basilisk stone |

#### Movement Fields
| Field | Type | Description |
|-------|------|-------------|
| bmcode | int | 1=mobile, 0=immobile |
| canmove | bool | Can receive move orders |
| maxvelocity | float | Max movement speed |
| acceleration | int | Acceleration rate |
| brakerate | int | Deceleration rate |
| turnrate | int | Turn speed (angular units, 65536 = full circle) |
| turninplacerate | int | Turn speed when stationary |
| movementclass | string | Reference to moveinfo.tdf class (GROUND2, WATER3, etc.) |
| roadmultiplier | float | Speed multiplier on roads (>1.0 = faster) |
| watermultiplier | float | Speed multiplier in water (<1.0 = slower) |
| upright | bool | Unit stands upright (biped) vs. prone |
| maneuverleashlength | int | Max distance unit will chase before returning |

#### Capability Flags
| Field | Type | Description |
|-------|------|-------------|
| canattack | bool | Can attack |
| canguard | bool | Can guard other units |
| canpatrol | bool | Can patrol |
| canstop | bool | Can be ordered to stop |
| canreclaim | bool | Can reclaim corpses/features |
| canresurrect | bool | Can resurrect dead units |
| builder | bool | Can construct buildings |
| commander | bool | Is a monarch/commander unit |
| cancloak | bool | Can become invisible |
| weaponswitching | bool | Can switch between weapons |

#### Building-specific Fields
| Field | Type | Description |
|-------|------|-------------|
| footprintx | int | Building width in map cells |
| footprintz | int | Building depth in map cells |
| maxslope | int | Max terrain slope for placement |
| maxwaterdepth | int | Max water depth for placement |
| buildangle | int | Angular orientation when built (in angular units) |
| builddistance | int | Range at which builder can construct |
| workertime | int | Build speed multiplier |
| yardmap | string | Grid of cell types: `.`=open, `o`=blocked, `c`=exit path |
| wind | bool | Has wind animation (flags, etc.) |

#### Economy Fields
| Field | Type | Description |
|-------|------|-------------|
| mogriumincome | int | Mogrium (resource) generated per tick |
| mogriumstorage | int | Mogrium storage capacity |
| economybonus | int | Percentage bonus to nearby resource generation |

#### Visual/Audio Fields
| Field | Type | Description |
|-------|------|-------------|
| shadowgaf | string | GAF file containing shadow sprites |
| shadowart | string | Specific shadow sprite name |
| soundcategory | string | Sound category for voice/effects |
| soundclass | string | Sound class for voice responses |
| corpse | string | Feature name for dead unit (reference to features TDF) |
| stone | string | Feature name when turned to stone |
| bloodcolor1/2/3 | 3x int | RGB blood particle colors |
| radardistance | int | Radar/minimap reveal radius |
| sightdistance | int | Line of sight radius |

### Weapon Section Fields (`[WEAPON1]`, `[WEAPON2]`, `[WEAPON3]`)

| Field | Type | Description |
|-------|------|-------------|
| name | string | Display name of the weapon |
| type | string | Weapon type: `Ballistic`, `Line of Sight`, `Remote Effect` |
| subtype | string | Sub-type (e.g., `lightning`) |
| range | int | Maximum firing range |
| reloadtime | float | Seconds between shots |
| weaponvelocity | int | Projectile speed |
| aimtolerance | int | Aim cone width (angular units) |
| areaofeffect | int | Splash damage radius |
| edgeeffectiveness | float | Damage at edge of AoE (0.0-1.0) |
| model | string | 3DO model for projectile |
| manapershot | int | Mana cost per firing |
| nimbus | bool | Requires god nimbus to use |
| soundhitclass | string | Reference to soundclasses.tdf for impact |
| soundhit | string | Direct sound file for impact |
| explosionclass | string | Reference to explosions.tdf |
| waterexplosionclass | string | Explosion when hitting water |
| hweffect | string | Hardware-accelerated effect name |
| innercolor | 3x int | RGB inner color for line effects |
| middlecolor | 3x int | RGB middle color for line effects |
| outercolor | 3x int | RGB outer color for line effects |
| emittime | int | Duration of visual effect in frames |
| unitsonly | bool | Only damages units, not terrain |
| spinheading | int | Projectile spin rate (heading axis) |
| spinpitch | int | Projectile spin rate (pitch axis) |
| builduptime | float | Charge-up time before firing |
| decaytime | float | Visual decay time after impact |
| radiusart0/1/2 | string | Ring effect art names |
| ringcount | int | Number of expanding rings |
| ringdelay | float | Delay between ring spawns |
| ringduration | float | How long each ring is visible |
| spritecount | int | Number of particles in effect |
| showeffect | bool | Whether to show the firing effect |

#### Weapon Damage Sub-section (`[DAMAGE]`)
```
[DAMAGE]
{
    default = 213;
}
```
The `default` key gives base damage. Other keys can specify per-damagecategory damage.

### Weapon Types

- **Ballistic** - Arcing projectile affected by gravity (arrows, catapult stones)
- **Line of Sight** - Direct-fire projectile or instant hit (lightning, fireballs)
- **Remote Effect** - Area effect at target location (earthquakes, spells)

### Button Image Fields (per weapon)
| Field | Type | Description |
|-------|------|-------------|
| buttonimageup | string | GAF frame for normal button state |
| buttonimagedown | string | GAF frame for pressed state |
| buttonimageselected | string | GAF frame for selected state |
| buttonimagedisabled | string | GAF frame for disabled/unavailable state |

---

## 3. GUI - GUI Layout Definitions

**Location:** `data/guis/`, `boneyards/guis/`, `boneyards2/guis/`
**File count:** 127 files
**Size range:** 188 bytes (defeattext.gui) to 30,811 bytes (battlemenumulti.gui)
**Purpose:** Define the layout, widgets, and behavior of all game UI screens

### Format Description

GUI files use a **numeric token-based** format (NOT the TDF key-value format).
This is a custom serialization format where widget types are identified by
leading integer codes, and properties are encoded as sequences of numbers and strings.

### Widget Structure

Each widget entry follows this pattern:
```
<widget_type> <flags>
<count>
<rect: x y width height> <visible> <enabled> <param1> <param2>
<color_count> <alpha> <r> <g> <b>
<status_count> <val1> <val2>
<font_flag> [<font_name_len> <font_name>]
<name_len> <name> <tag1> <tag2>
<image_count>
<image entries...>
<state_count>
<state entries...>
<sound entries...>
<action: action_flag action_name_len action_name>
<child_count>
```

### Widget Type Codes

| Code | Widget Type | Description |
|------|-------------|-------------|
| 2 | Panel/Container | Background panel, can contain children |
| 4 | Button | Clickable button with up/down/disabled states |
| 9 | Group | Logical grouping container |
| 13 | Desktop | Root/top-level container |
| 17 | Toggle Button | Button that stays pressed (radio/checkbox) |
| 18 | Color Picker | Color selection widget (e.g., player color) |
| 19 | Static/Label | Non-interactive display element (text, image, bar) |

### Image Entry Format
```
1 <gaf_name_len> <gaf_name> <anim_name_len> <anim_name> <frame_index> <render_mode>
```
or for empty images:
```
1 0 0 0 0
```

Render modes: 9 = normal, 18 = transparent/alpha

### Text Entry Format
```
1 <font_name_len> <font_gaf_name>
```
Font names reference GAF files containing bitmap font glyphs (e.g.,
`times new roman (100).gaf`).

### Action Format
```
2 <flag> <action_name_len> <action_name>
```
or
```
2 0 0
```

Action names map to engine commands: `Start Game`, `Stop`, `Move`, `Attack`,
`Guard`, `Patrol`, `Heal`, `Load`, `Unload`, `Clear`, `Offensive`, `Defensive`,
`Passive`, `Cloak`, `UnCloak`, `Activate`, `Deactivate`, `Previous Screen`.

### Sound Entry Format
```
1 <sound_name_len> <sound_filename>
```
or `1 0` for no sound. References .wav files.

### Hotkey Binding
The root widget can include a hotkey string:
```
2 0 24 #Enter#Play#Esc#Previous
```
Format: `#<key>#<widget_name>` pairs, mapping keyboard keys to button widgets.

### Example Fragment (battlemenusingle.gui, start):
```
2 1                              // Panel widget, 1 flag
2 0 0 640 480 1 1 0 0           // rect(0,0,640,480), visible, enabled
3 255 48 48 48                   // color: alpha=255, r=48, g=48, b=48
1 0 0                            // no status
1 0                              // no font
10 BattleMenu 1 2                // name="BattleMenu", tags
2                                // 2 images
1 21 BattleSkirmScreen.gaf 11 BattleSkirm 0 9  // image 0
1 21 BattleSkirmScreen.gaf 11 BattleSkirm 1 9  // image 1
2                                // 2 states
2 0 0 2 0 0 2                   // state data
1 0                              // sound 0
1 0                              // sound 1
2 0 24 #Enter#Play#Esc#Previous  // action with hotkeys
30                               // 30 children follow
```

### Parsing Notes for C

- Read tokens separated by whitespace (space, tab, newline)
- String tokens are preceded by their length (e.g., `12 aramonig.gaf`)
- Integer tokens can be parsed with `strtol`
- The child count at the end of each widget tells you how many more widgets to
  parse recursively as children
- The format is fully sequential, with no random-access pointers

---

## 4. OTA - Mission/Map Metadata

**Location:** `maps/Maps/`, `missions/missions/`
**File count:** 77 files
**Size range:** 504 bytes (simple skirmish maps) to 58,469 bytes (complex missions)
**Purpose:** Define map properties, starting positions, pre-placed units, and scripting

OTA files use the standard TDF key-value format.

### Complete Example 1: Skirmish Map (`Ground War.ota`)

```
[GlobalHeader]
{
    Copyright=Copyright 1998 Cavedog Entertainment. All rights reserved.;
    missionname=;
    missiondescription=Scripted Multiplayer Only;
    kingdom=veruna;
    numplayers=4;
    size=5 x 5;
    memory=32 MB;
    useonlyunits=Ground War.tdf;
    hasscenario=1;
    [Map Data]
    {
        Type=Network 1;
        aiprofile=DEFAULT;
        [specials]
        {
            [special0]
            {
                specialwhat=StartPos1;
                XPos=26;
                ZPos=135;
            }
            [special1]
            {
                specialwhat=StartPos2;
                XPos=123;
                ZPos=16;
            }
            // ...
        }
    }
}
```

### Complete Example 2: Campaign Mission (`takmission01_mt.ota`)

```
[GlobalHeader]
{
    Copyright=Copyright 1998 Humongous Entertainment. All rights reserved.;
    missionname=takmission01_MT;
    missiondescription=;
    kingdom=Aramon;
    ismission=0;
    lineofsight=1;
    mapping=0;
    tidalstrength=20;
    solarstrength=20;
    lavaworld=0;
    killmul=50;
    timemul=0;
    minwindspeed=25;
    maxwindspeed=5000;
    gravity=112;
    maxunits=200;
    nosealeveltrigger=0;
    waterdoesdamage=0;
    waterdamage=100;
    Player1=logo 3 aramon;
    Player2=strategic opponent logo 1 taros;
    Player10=passive neutral logo 8;
    numplayers=;
    size=6 x 6;
    memory=16 mb;
    MoveUnitToRadius=NPCEMEN, 130, 76, 15;
    [Map Data]
    {
        Type=Medium;
        aiprofile=DEFAULT;
        [units]
        {
            [unit0]
            {
                Unitname=ARASWORD;
                Ident=;
                XPos=69;
                YPos=80;
                ZPos=175;
                Player=1;
                HealthPercentage=100;
                ManaPercentage=0;
                Angle=305;
                Kills=0;
            }
            // ... more units
        }
    }
}
```

### OTA Field Reference

#### GlobalHeader Fields
| Field | Type | Description |
|-------|------|-------------|
| missionname | string | Internal mission name |
| missiondescription | string | Display description |
| kingdom | string | Default faction for this map |
| numplayers | int | Max number of players |
| size | string | Map size description (e.g., "5 x 5") |
| memory | string | Recommended RAM |
| ismission | bool | 1=campaign mission, 0=skirmish |
| lineofsight | bool | Line of sight option. A campaign mission ignores it and keeps line of sight on (legacy:168885) |
| mapping | bool | 1 starts a campaign mission with the map black, anything else starts it explored (legacy:168883) |
| tidalstrength | int | Tidal energy generation |
| solarstrength | int | Solar energy generation |
| lavaworld | bool | Lava world flag |
| killmul | int | Kill score multiplier |
| timemul | int | Time score multiplier |
| minwindspeed | int | Minimum wind speed |
| maxwindspeed | int | Maximum wind speed |
| gravity | int | Projectile gravity (default ~112) |
| maxunits | int | Max unit count per player |
| waterdoesdamage | bool | Water damages units |
| waterdamage | int | Damage per tick in water |
| useonlyunits | string | TDF file restricting available units |
| hasscenario | bool | Has scripted scenario |
| PlayerN | string | Player slot config: `[ai_type] [opponent] logo <N> [faction]` |
| MoveUnitToRadius | string | Comma-separated: unit, x, z, radius |

#### Map Data Fields
| Field | Type | Description |
|-------|------|-------------|
| Type | string | Map type (Network 1, Medium, etc.) |
| aiprofile | string | AI profile name from ai/ directory |

#### Special Entries (Start Positions)
| Field | Type | Description |
|-------|------|-------------|
| specialwhat | string | StartPosN (1-based player start) |
| XPos | int | X coordinate (map units) |
| ZPos | int | Z coordinate (map units) |

#### Unit Entries (Pre-placed Units)
| Field | Type | Description |
|-------|------|-------------|
| Unitname | string | Unit type (from FBI) |
| Ident | string | Script identifier for this instance |
| XPos | int | X position |
| YPos | int | Y/height position |
| ZPos | int | Z position |
| Player | int | Owning player number |
| HealthPercentage | int | Starting HP percentage (0-100) |
| ManaPercentage | int | Starting mana percentage (0-100) |
| Angle | int | Facing angle in degrees |
| Kills | int | Pre-set kill count |

---

## 5. TXT - AI Scripts

**Location:** `data/ai/`
**File count:** ~20 files
**Size range:** ~2 KB to ~8 KB
**Purpose:** Define AI behavior profiles (unit build priorities and population limits)

### Format Description

AI text files use a simple line-based format (NOT TDF-style blocks):

```
// Comment
weight <unitname> <priority_weight>
limit <unitname> <max_count>
```

### Example (default.txt, partial):
```
// Kingdoms Default AI Profile 3-28-99

weight araarch 5
weight araat 1
weight arabow 8
weight arabroad 5
weight arabuild 10
weight aracan 4
weight aracastl 10
weight aradrag 25
weight arafast 1

limit araarch 16
limit araat 8
limit arabow 16
limit arabuild 10
limit aracan 3
limit aracastl 2
limit aradrag 1
```

### Fields

- **weight** - Build priority. Higher values mean the AI prefers to build this unit.
  A weight of 0 means the AI will never build it. Values typically range 1-25.
- **limit** - Maximum number of this unit the AI will maintain at once. 0 = never build.

The file contains entries for ALL units across ALL factions. The AI uses the entries
matching its current faction. The default profile is loaded unless overridden by the
OTA's `aiprofile` field.

### Parsing in C

```c
while (fgets(line, sizeof(line), fp)) {
    if (line[0] == '/' && line[1] == '/') continue; // comment
    if (sscanf(line, "weight %s %d", name, &value) == 2) { /* store weight */ }
    if (sscanf(line, "limit %s %d", name, &value) == 2) { /* store limit */ }
}
```

---

## 6. SCC - Source Control Metadata

**Location:** `data/gamedata/soundclasses/vssver.scc`, `data/guis/vssver.scc`
**File count:** 2 files
**Size:** 48 bytes each
**Purpose:** Visual SourceSafe version control metadata (development artifact)

These are **not game data files**. They are Microsoft Visual SourceSafe tracking
files accidentally included in the archives. They contain binary GUIDs and version
numbers. Safe to ignore for any parser/reimplementation.

### Header (48 bytes):
```
Offset  Size  Description
0x00    4     Magic/version (0x00011234)
0x04    16    GUID (COM class ID)
0x14    4     Unknown
0x18    4     Unknown
0x1C    4     Timestamp or counter
0x20    16    Another GUID or hash
```

---

## 7. GAF - Graphic Archive Format

**Location:** `data/anims/`
**File count:** 465 files
**Size range:** 0 bytes (empty) to 3,164,200 bytes (arabuild.gaf)
**Purpose:** Contains 2D sprite animations (8-bit paletted) for units, buildings, effects, UI

### Header Structure

```
Offset  Size  Type      Description
0x00    2     uint16    Version (always 0x0001)
0x02    2     uint16    Subversion (always 0x0001)
0x04    4     uint32    Number of entries (animations) in the archive
0x08    4     uint32    Unknown/reserved (always 0x00000000)
```

**Magic bytes:** `00 01 01 00` (version 1.1 in little-endian as two uint16s)

Note: TAF files share identical header magic. They are distinguished by context
(filename suffix) and by the pixel format of their frame data.

### Entry Pointer Table

Immediately after the header at offset 0x0C:

```
Offset        Size  Type      Description
0x0C + i*4    4     uint32    Absolute file offset to Entry[i] header
```

### Entry Header (at pointed-to offset)

```
Offset  Size  Type      Description
0x00    2     uint16    Number of frames in this animation
0x02    2     uint16    Unknown (often 0x0000)
0x04    4     uint32    Unknown
0x08    32    char[32]  Null-terminated animation name (padded with zeros)
```

Total: 40 bytes per entry header.

### Frame Pointer Table

Immediately after the entry header, for each frame:

```
Offset        Size  Type      Description
0x00 + i*8    4     uint32    Absolute file offset to frame data
0x04 + i*8    4     uint32    Unknown (often 0x00000000)
```

### Frame Header (at pointed-to offset)

```
Offset  Size  Type      Description
0x00    2     uint16    Width in pixels
0x02    2     uint16    Height in pixels
0x04    2     int16     X offset (anchor/hotspot X)
0x06    2     int16     Y offset (anchor/hotspot Y)
0x08    1     uint8     Unknown/compression type
0x09    1     uint8     Unknown flags
0x0A    2     uint16    Unknown
0x0C    4     uint32    Absolute offset to pixel data
0x10    4     uint32    Unknown (possibly uncompressed size)
```

### Pixel Data Encoding (8-bit paletted, RLE compressed)

GAF frame data uses a line-based RLE compression:

For each scanline (height lines total):
- Read a uint16 line data size (in bytes)
- Process line data bytes:
  - If byte >= 0x01 and byte <= 0x7F: literal run of N pixels (read N bytes)
  - If byte >= 0x81: transparent run of (256 - byte) pixels
  - If byte == 0x00: end of line
  - Special codes may vary, and the exact RLE scheme needs careful testing

Colors are indices into the associated .pal palette file.
Transparency index is typically index 9 (configurable via render.tdf `transparentcolor`).

### Engine Usage

GAF files are referenced by name (without .gaf extension) from:
- FBI files (`shadowgaf`)
- TDF files (`gaf = ...` in explosions, effects, damageflames)
- GUI files (image references like `aramonig.gaf`)
- Sidedata TDF (`buildsparklygaf`, `logogaf`, `stonegaf`)

---

## 8. TAF - Truecolor Animation Format

**Location:** `data/anims/`
**File count:** 256 files (always in `_1555`/`_4444` pairs)
**Size range:** 472 bytes to 621,494 bytes
**Purpose:** 16-bit truecolor sprite animations (TAK extension over TA's 8-bit GAF)

### Naming Convention

TAF files come in pairs with suffixes indicating pixel format:
- `*_1555.taf` - ARGB 1555 format (1-bit alpha, 5-bit R, 5-bit G, 5-bit B)
- `*_4444.taf` - ARGB 4444 format (4-bit each ARGB channel)

The engine selects based on video card capabilities.

### Header Structure

**Identical to GAF header:**
```
Offset  Size  Type      Description
0x00    2     uint16    Version (0x0001)
0x02    2     uint16    Subversion (0x0001)
0x04    4     uint32    Number of entries
0x08    4     uint32    Reserved (0x00000000)
```

### Entry/Frame Structure

Same pointer table and entry header structure as GAF (see Section 7).

### Entry Header

```
Offset  Size  Type      Description
0x00    2     uint16    Number of frames
0x02    2     uint16    Unknown
0x04    4     uint32    Unknown
0x08    32    char[32]  Null-terminated animation name
```

### Frame Header

```
Offset  Size  Type      Description
0x00    2     uint16    Width
0x02    2     uint16    Height
0x04    2     int16     X anchor offset
0x06    2     int16     Y anchor offset
0x08    1     uint8     Compression flags (0x00 = standard, 0x05 = ?)
0x09    3     bytes     Unknown
0x0C    4     uint32    Absolute offset to pixel data
0x10    4     uint32    Unknown
```

### Pixel Data

TAF pixel data is also RLE-compressed but uses 16-bit pixels instead of 8-bit
palette indices. The RLE control codes operate on uint16 pixel values.

**ARGB 1555 pixel layout:**
```
Bit:  15  14-10  9-5  4-0
      A   RRRRR  GGGGG BBBBB
```

**ARGB 4444 pixel layout:**
```
Bit:  15-12  11-8  7-4  3-0
      AAAA   RRRR  GGGG BBBB
```

---

## 9. TNT - Terrain Data

**Location:** `maps/Maps/`, `sections/Sections/`
**File count:** 2,624 files
**Size range:** 17,596 bytes (section tiles) to ~8 MB (full maps)
**Purpose:** Terrain heightmap, tile data, minimap, and feature placement

### Header Structure (TAK variant)

```
Offset  Size  Type      Description
0x00    4     uint32    Version/magic (0x00004000 = TAK terrain)
0x04    4     uint32    Width in tiles
0x08    4     uint32    Height in tiles
0x0C    4     uint32    Unknown (possibly tile count or flags, often 0)
0x10    4     uint32    Unknown (offset or size value)
0x14    4     uint32    Offset to tile index data
0x18    4     uint32    Unknown offset
0x1C    4     uint32    Unknown (tile data offset or count)
0x20    4     uint32    Offset to heightmap data
0x24    4     uint32    Offset to minimap data
0x28    4     uint32    Offset to unknown data block 1
0x2C    4     uint32    Offset to unknown data block 2
0x30    4     uint32    Offset to feature data
```

**Magic:** `00 40 00 00` (0x00004000 little-endian)

Note: The original TA used magic `00 20 00 00` (0x00002000). TAK uses 0x00004000,
indicating the TAK-specific extended format.

### Tile Data

After the header, tile index data begins. Each map cell references a tile by index.
The tile graphics are stored as 8-bit paletted pixel blocks (typically 32x32 pixels).

For section files (`.bad` extension), the structure is identical (same magic `0x00004000`),
these are used for randomly generated terrain sections.

### Heightmap

The heightmap stores one byte per map vertex (width+1 by height+1 grid), giving
elevation values 0-255.

### Feature Placement

Feature entries describe pre-placed objects on the terrain (trees, rocks, etc.).

### Example hex (Ground War.tnt):
```
00000000: 00 40 00 00  a0 00 00 00  a0 00 00 00  3a 00 00 00
00000010: 34 00 00 00  34 64 00 00  34 2c 01 00  15 00 00 00
00000020: 08 37 01 00  08 9b 01 00  08 b4 01 00  08 cd 01 00
00000030: 14 0b 02 00  [tile data follows: 3e 3e 3e ...]
```

Width=0xA0 (160), Height=0xA0 (160) tiles.

---

## 10. 3DO - 3D Object Models

**Location:** `data/objects3d/`
**File count:** 389 files
**Size range:** 167 bytes (araarrow2.3do) to 19,019 bytes (npcwagon.3do)
**Purpose:** 3D polygon models for all units, projectiles, and map objects

### Header Structure

```
Offset  Size  Type      Description
0x00    4     uint32    Version (0x00000001)
0x04    4     uint32    Number of vertices
0x08    4     uint32    Number of primitives (faces)
0x0C    4     int32     X offset (fixed-point position)
0x10    4     int32     Y offset
0x14    4     int32     Z offset
0x18    4     uint32    Offset to name string data (absolute)
0x1C    4     uint32    Unknown/always 0
0x20    4     uint32    Offset to vertex array
0x24    4     uint32    Offset to primitive array
0x28    4     uint32    Offset to sibling object (0 = none)
0x2C    4     uint32    Offset to child object (0 = none)
```

**Magic:** First 4 bytes = `01 00 00 00` (version 1)

### Vertex Data

Array of vertices at the vertex offset. Each vertex:
```
Offset  Size  Type    Description
0x00    4     int32   X coordinate (fixed-point, >>16 for float)
0x04    4     int32   Y coordinate
0x08    4     int32   Z coordinate
```

### Primitive (Face) Data

Array of polygon face descriptors:
```
Offset  Size  Type      Description
0x00    4     uint32    Color/palette index or texture reference
0x04    4     uint32    Number of vertices in this face
0x08    4     uint32    Offset to vertex index array (always from file start)
0x0C    4     uint32    Offset to texture name (null-terminated string)
0x10    4     uint32    Unknown/texture coordinates offset
0x14    4     uint32    Unknown
```

### Name Strings

Texture and piece names are stored as null-terminated strings in a string table
area. The hex dump shows names like: `tuniclogo4`, `torsologo3`, `swordleg8`,
`hiboot1`, `footleather`, etc. These reference texture names from the texture
palette system.

### Object Hierarchy

3DO files form a tree structure via sibling/child offsets. This allows articulated
models (e.g., body -> arm -> hand). Each sub-object has its own vertices and
primitives with a local transform offset. The COB script system animates these
pieces by name.

---

## 11. COB - Compiled BOS Script (Bytecode)

**Location:** `data/scripts/`
**File count:** 205 files
**Size range:** 75 bytes (arawall.cob) to 192,881 bytes (zonbasil.cob)
**Purpose:** Compiled animation/behavior scripts controlling unit piece movement

### Header Structure

```
Offset  Size  Type      Description
0x00    4     uint32    Version marker (0x00000006 for TAK)
0x04    4     uint32    Number of script functions (routines)
0x08    4     uint32    Number of 3DO pieces referenced
0x0C    4     uint32    Code length (in 32-bit instructions)
0x10    4     uint32    Number of static variables
0x14    4     uint32    Unknown/always 0
0x18    4     uint32    Offset to code section start
0x1C    4     uint32    Offset to function name table
0x20    4     uint32    Offset to function entry point table
0x24    4     uint32    Total file length in 32-bit words(?)
0x28    4     uint32    Offset to piece name table
0x2C    4     uint32    Offset to script name/string table
```

**Magic/version:** `06 00 00 00` (version 6)

Note: Original TA used version 4. TAK uses version 6 with additional opcodes.

### Code Section

The code section contains 32-bit instructions (opcodes). Each instruction is
a uint32 value. The COB VM is a stack-based virtual machine.

### Common Opcodes (partial list from TA/TAK community research)

| Opcode     | Mnemonic       | Description |
|------------|----------------|-------------|
| 0x10000000 | PUSH_CONSTANT  | Push immediate value |
| 0x10001000 | PUSH_VAR       | Push local variable |
| 0x10002000 | PUSH_STATIC    | Push static variable |
| 0x10010000 | POP_VAR        | Pop into local variable |
| 0x10020000 | POP_STATIC     | Pop into static variable |
| 0x10011000 | ADD            | Add top two stack values |
| 0x10012000 | SUB            | Subtract |
| 0x10013000 | MUL            | Multiply |
| 0x10014000 | DIV            | Divide |
| 0x10021000 | RAND           | Random number |
| 0x10041000 | MOVE_PIECE     | Move 3DO piece |
| 0x10042000 | TURN_PIECE     | Rotate 3DO piece |
| 0x10043000 | SPIN_PIECE     | Continuous rotation |
| 0x10051000 | WAIT_FOR_MOVE  | Block until move completes |
| 0x10052000 | WAIT_FOR_TURN  | Block until turn completes |
| 0x10061000 | CALL_SCRIPT    | Call another function |
| 0x10062000 | RETURN         | Return from function |
| 0x10063000 | JUMP           | Unconditional jump |
| 0x10064000 | JUMP_NOT_EQUAL | Conditional branch |
| 0x10065000 | SLEEP          | Sleep N ticks |

### Function Name Table

Null-terminated strings referenced by index. Common function names:
`Create`, `SweetSpot`, `AimPrimary`, `FirePrimary`, `AimSecondary`,
`FireSecondary`, `Killed`, `StartMoving`, `StopMoving`, `StartBuilding`,
`StopBuilding`, `Activate`, `Deactivate`, `HitByWeapon`.

### Example (arawall.cob - 75 bytes, minimal script):
```
Header: version=6, 1 function, 0 pieces
Code: single "Create" function (likely empty or minimal setup)
String table: "Create"
```

---

## 12. PCX - PCX Image Format

**Location:** `data/anims/`, `data/bitmaps/`, `data/palettes/`, `boneyards/anims/`
**File count:** 220 files
**Size range:** 899 bytes (build palettes) to 118,650 bytes (loading screens)
**Purpose:** Standard PCX image format for textures, UI backgrounds, and build palette icons

### Header Structure (Standard ZSoft PCX)

```
Offset  Size  Type      Description
0x00    1     uint8     Manufacturer (0x0A = ZSoft)
0x01    1     uint8     Version (5 = 3.0+)
0x02    1     uint8     Encoding (1 = RLE)
0x03    1     uint8     Bits per pixel per plane (8)
0x04    2     uint16    X min (0)
0x06    2     uint16    Y min (0)
0x08    2     uint16    X max
0x0A    2     uint16    Y max
0x0C    2     uint16    Horizontal DPI
0x0E    2     uint16    Vertical DPI
0x10    48    byte[48]  EGA palette (16 colors x 3 bytes RGB)
0x40    1     uint8     Reserved (0)
0x41    1     uint8     Number of color planes (1 for 256-color)
0x42    2     uint16    Bytes per scanline per plane
0x44    2     uint16    Palette type (1=color, 2=grayscale)
0x46    2     uint16    Horizontal screen size
0x48    2     uint16    Vertical screen size
0x4A    54    byte[54]  Padding (zeros)
```

**Magic byte:** `0x0A` at offset 0 (ZSoft PCX identifier)

### Image Data

RLE-encoded scanlines starting at offset 0x80:
- If byte has top 2 bits set (>= 0xC0): run of (byte & 0x3F) copies of next byte
- Otherwise: literal single pixel value

### VGA Palette (256-color PCX)

For 8-bit images, the 256-color palette is at the end of the file:
- Look for byte `0x0C` (palette marker) at `file_size - 769`
- Followed by 256 x 3 bytes (RGB triplets)

### Usage in TAK

- Build palette PCX files (e.g., `arabipal.pcx`) are small 256-color palette reference images
- UI background images and loading screens
- Used alongside GAF for composite graphics

---

## 13. PAL - Palette Files

**Location:** `data/palettes/`, `data/anims/`
**File count:** 9 files
**Size range:** 1,024 bytes (standard) to 11,559 bytes (byclient.pal)

### Standard PAL Format (1024 bytes)

Most PAL files are exactly 1024 bytes: 256 entries x 4 bytes each (RGBX format).

```
Offset          Size  Type    Description
0x00 + i*4      1     uint8   Red (0-255)
0x01 + i*4      1     uint8   Green (0-255)
0x02 + i*4      1     uint8   Blue (0-255)
0x03 + i*4      1     uint8   Padding/flags (usually 0x00)
```

### Example (palette.pal, first entries):
```
Index 0:  R=0x00 G=0x00 B=0x00 (black)
Index 1:  R=0x80 G=0x00 B=0x00 (dark red)
Index 2:  R=0x00 G=0x80 B=0x00 (dark green)
...
Index 5:  R=0x54 G=0x54 B=0xFC (blue - "transparent" color marker?)
```

### Extended PAL Format (byclient.pal - 11,559 bytes)

The Boneyards client palette files have an extended format:

```
Offset  Size  Type      Description
0x00    1     uint8     Version? (0x0A)
0x01    1     uint8     Type? (0x05)
0x02    1     uint8     Encoding? (0x01)
0x03    1     uint8     Bits per pixel? (0x08)
0x04    4     uint32    Unknown
0x08    2     uint16    Width? (0x027F = 639)
0x0A    2     uint16    Height? (0x01DF = 479)
0x0C    2     uint16    Alternate width? (0x0280 = 640)
0x0E    2     uint16    Alternate height? (0x01E0 = 480)
```

This appears to be a PCX-like header followed by palette data, possibly a
full PCX image with embedded palette. The first 4 bytes match PCX magic format.

### Engine Usage

Each faction has its own texture palette:
- `ara_textures.pal` - Aramon
- `tar_textures.pal` - Taros
- `ver_textures.pal` - Veruna
- `zon_textures.pal` - Zhon
- `gameart.pal` - General game artwork
- `guipal.pal` - GUI elements

These palettes are used with the `.alp`, `.shd`, `.lht`, `.gry`, `.blu` lookup
tables to perform real-time color effects (transparency, shadows, lighting).

---

## 14. WAV - Audio Files

**Location:** `english/Sounds/`
**File count:** 656 files
**Purpose:** All game sound effects and voice lines

### Format

Standard Microsoft RIFF WAVE format.

```
Offset  Size  Type      Description
0x00    4     char[4]   "RIFF" magic
0x04    4     uint32    File size - 8
0x08    4     char[4]   "WAVE"
0x0C    4     char[4]   "fmt " chunk ID
0x10    4     uint32    fmt chunk size (16 for PCM)
0x14    2     uint16    Audio format (1 = PCM)
0x16    2     uint16    Number of channels (1 = mono)
0x18    4     uint32    Sample rate (typically 11025 Hz)
0x1C    4     uint32    Byte rate
0x20    2     uint16    Block align
0x22    2     uint16    Bits per sample (8)
0x24    4     char[4]   "data" chunk ID
0x28    4     uint32    Data size
0x2C    ...   bytes     PCM audio samples
```

**Typical format:** 8-bit mono PCM at 11025 Hz (unsigned, 0x80 = silence).

### Naming Convention

Voice files follow the pattern: `<3-digit-actor-id><category><number>.WAV`
- Actor IDs: 133, 134, 168, 258, 235, etc.
- Categories: `AAV` (ambient voice), `SPT` (spot/selection), `DTZ` (death), `SPA` (special)

---

## 15. ALP - Alpha/Transparency Lookup Tables

**Location:** `data/palettes/`
**File count:** 35 files
**Size:** Always exactly 65,536 bytes (256 x 256)
**Purpose:** Pre-computed alpha blending lookup table for 8-bit paletted rendering

### Format

A 256x256 byte array with no header. To blend palette color A over palette color B:

```c
uint8_t result_color = alp_table[A * 256 + B];
// or equivalently:
uint8_t result_color = alp_table[A][B];
```

The result is the palette index that best approximates the semi-transparent
blending of color A over color B.

### Engine Usage

When drawing a translucent sprite pixel (palette index A) over a background pixel
(palette index B), the engine looks up `alp[A][B]` to get the blended palette
index, avoiding expensive per-pixel RGB blending on 1990s hardware.

Each faction and texture set has its own ALP table because the palettes differ.

---

## 16. SHD - Shadow Lookup Tables

**Location:** `data/palettes/`
**File count:** 35 files
**Size:** Always exactly 8,192 bytes (256 x 32)
**Purpose:** Pre-computed shadow darkening lookup table

### Format

A 256x32 byte array with no header:

```c
uint8_t shadowed_color = shd_table[color_index * 32 + shadow_level];
```

Where `shadow_level` ranges from 0 (no shadow) to 31 (darkest shadow).
The result is the palette index of the darkened color.

### Note on Initial Data

The `aiden.shd` file starts with all zeros for the first ~256+ bytes, which
is correct: palette index 0 (typically black/transparent) darkened at any level
remains index 0.

---

## 17. LHT - Light Remap Tables

**Location:** `data/palettes/`
**File count:** 35 files
**Size:** Always exactly 8,192 bytes (256 x 32)
**Purpose:** Pre-computed lighting/brightening lookup table

### Format

A 256x32 byte array with no header. Similar structure to SHD:

```c
uint8_t lit_color = lht_table[color_index * 32 + light_level];
```

Where `light_level` ranges from 0 (no extra light) to 31 (maximum brightness).

### Example Data (aiden.lht)

The first few bytes show a mostly sequential pattern (identity-like for low
light levels), with specific remappings:
```
00: 00 01 02 03 04 05 06 F8 08 09 0A 0B 0C 0D 0E 0F
10: 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F
```

At light level 0, most indices map to themselves (no change). At higher levels,
dark colors remap to brighter equivalents within the same hue family.

---

## 18. GRY - Grayscale Remap Tables

**Location:** `data/palettes/`
**File count:** 35 files
**Size:** Always exactly 256 bytes
**Purpose:** Maps each palette index to its grayscale equivalent

### Format

A simple 256-byte lookup table with no header:

```c
uint8_t gray_index = gry_table[color_index];
```

### Engine Usage

Used for desaturation effects (e.g., fog of war, UI dimming, disabled states).
Each entry maps a color palette index to the nearest gray palette index.

---

## 19. BLU - Blue Channel Remap Tables

**Location:** `data/palettes/`
**File count:** 35 files
**Size:** Always exactly 256 bytes
**Purpose:** Maps each palette index to a blue-tinted equivalent

### Format

A 256-byte lookup table with no header:

```c
uint8_t blue_index = blu_table[color_index];
```

### Engine Usage

Used for underwater tinting effects. When a unit or terrain is underwater,
each pixel's palette index is remapped through this table to produce a
blue-shifted appearance.

---

## 20. CRT - Creature/Script Data

**Location:** `maps/Maps/`
**File count:** 29 files
**Size range:** 56 bytes (empty maps) to 120,248 bytes (complex maps)
**Purpose:** Scripted creatures/events associated with maps

### Header Structure

Files that are 56 bytes appear to be "empty" CRT files (no creatures):

```
Offset  Size  Type      Description
0x00    4     float32   Unknown (1.0 = 0x3F800000)
0x04    4     uint32    Unknown (0)
0x08    4     uint32    Possible entry count or flags
0x0C    varies          Entry data (if any)
```

### Non-empty CRT Structure

Larger CRT files contain positioned object references:

```
Offset  Size    Type      Description
0x00    4       float32   Version/scale? (1.0)
0x04    4       uint32    Unknown
0x08    4       uint32    Data offset or count
0x0C    8+      char[]    Null-terminated unit name string (e.g., "VERPULT")
...     varies            Additional placement data
```

The data appears to contain:
- Unit/creature name strings (null-terminated)
- Position coordinates (pairs of uint16 or int32)
- Count/index values
- Possibly patrol paths or trigger definitions

The string "VERPULT" visible in the Ground War.crt hex dump confirms these
reference unit types from the FBI database.

### Engine Usage

CRT files define scripted unit placements and behaviors specific to a map,
supplementing the pre-placed units in the OTA file. They may define neutral
creatures, wandering monsters, or triggered reinforcements.

---

## 21. TSF - Truecolor Sprite Frame Definition

**Location:** `boneyards/anims/`, `meta/Anims/`
**File count:** 7 files (Boneyards expansion only)
**Purpose:** Text-based sprite definition referencing external PNG files

### Format

TSF files are **plain text** using the standard TDF key-value format.
They define animation frames that reference external PNG image files instead
of embedded pixel data.

### Complete Example (BYAdBanners.tsf, partial):

```
/* Rank Icons. */

[BYAdBanners]
{
    Looping = 0;
    [Frame0]
    {
        Delay = 0;
        [Layer0]
        {
            AnchorX = 0;
            AnchorY = 0;
            Filename = BYBeta.png;
        }
    }
    [Frame1]
    {
        Delay = 0;
        [Layer0]
        {
            AnchorX = 0;
            AnchorY = 0;
            Filename = BYInGameAd.png;
        }
    }
}
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| Looping | bool | Whether the animation loops |
| Delay | int | Frame delay (in ticks) |
| AnchorX | int | Horizontal anchor/offset |
| AnchorY | int | Vertical anchor/offset |
| Filename | string | External PNG image filename |

### Engine Usage

This is a Boneyards (online play expansion) format. It provides a text-based
alternative to GAF for defining sprite animations, using standard PNG files
instead of the proprietary compressed pixel data. Each frame can have multiple
layers.

---

## 22. BAD - Section Terrain Blocks

**Location:** `sections/Sections/`
**File count:** 7 files (in the extracted data examined)
**Size range:** 17,200 to 19,612 bytes
**Purpose:** Pre-built terrain sections for random map generation

### Format

BAD files share the **exact same format as TNT files** (Section 9). They use
the same `0x00004000` magic header.

```
Offset  Size  Type      Description
0x00    4     uint32    Magic (0x00004000)
0x04    4     uint32    Width in tiles (typically 0x14 = 20)
0x08    4     uint32    Height in tiles (typically 0x12 = 18)
0x0C    ...             Same structure as TNT
```

### Engine Usage

The random map generator assembles playable maps from these section blocks.
Sections are categorized by faction/terrain type (e.g., `Taros/High Specials/`,
`Taros/Low Paths/`) and difficulty/type. The generator stitches compatible
sections together based on edge matching.

---

## 23. GAO/GAO2 - Graphic Archive Object (Truecolor)

**Location:** `data/anims/`
**File count:** 1 GAO file (41,444 bytes), 1 GAO2 file (16,168 bytes)
**Purpose:** Truecolor sprite data (companion to GAF)

### GAO Header

```
Offset  Size  Type      Description
0x00    2     uint16    Version (0x0001)
0x02    2     uint16    Subversion (0x0001)
0x04    4     uint32    Number of entries (0x00000004)
0x08    4     uint32    Reserved (0x00000000)
```

The header is **identical** to GAF/TAF. The GAO format appears to be
functionally equivalent to GAF but may contain uncompressed or differently
encoded pixel data.

### GAO2 Header

Same structure as GAO. The `gao2` extension likely indicates a format
variant (perhaps matching the `_4444` vs `_1555` TAF distinction).

### Engine Usage

Only one file pair exists: `teamlogos.gao` and `teamlogos.gao2`. These contain
the team logo sprites in truecolor format. The associated `teamlogos.pco` file
likely provides palette/color override data.

---

## 24. PCO - Palette Color Object

**Location:** `data/anims/`
**File count:** 1 file (1,700 bytes)
**Purpose:** Palette color data associated with GAO files

### Header Structure

```
Offset  Size  Type      Description
0x00    1     uint8     Version? (0x0A)
0x01    1     uint8     Type? (0x05)
0x02    1     uint8     Encoding? (0x01)
0x03    1     uint8     Bits per pixel? (0x08)
0x04    4     uint32    Unknown (zeros)
0x08    2     uint16    Width? (0x001F = 31)
0x0A    2     uint16    Height? (0x001F = 31)
0x0C    2     uint16    Alternate width? (0x0048 = 72)
0x0E    2     uint16    Alternate height? (0x0048 = 72)
```

The first 4 bytes match PCX format (`0x0A, 0x05, 0x01, 0x08`), suggesting
this is actually a standard PCX file with a `.pco` extension. The PCO likely
provides the base palette data or color key information for the associated
GAO truecolor sprites.

---

## 25. FNT - Font Files

**Location:** `data/fonts/`
**File count:** 2 files (both `roman10.fnt`, 1,675 bytes each)
**Purpose:** Bitmap font glyph metrics and data

### Header Structure

```
Offset  Size  Type      Description
0x00    2     uint16    Font height in pixels (0x000D = 13)
0x02    2     uint16    Unknown/flags (0x0002)
0x04    4     uint32    Unknown (0x00000000)
0x08    4     uint32    Unknown (0x00000000)
0x0C    4     uint32    Unknown (0x00000000)
```

### Glyph Offset Table

Starting at offset 0x10, there are padding/reserved bytes until offset 0x40,
then a table of uint16 offsets for each ASCII character (starting from space/0x20):

```
Offset  Size    Type      Description
0x40    2       uint16    Character 0 (space) - absent, offset = 0
0x42    2       uint16    Offset for '!' (0x0211 = 529)
0x44    2       uint16    Offset for '"' (0x021C = 540)
...
```

Each offset points to the glyph's pixel data within the file. Offsets are
relative to file start (or to a base offset).

### Glyph Data

Between the offset table and the character data, there appears to be additional
zero-padding. The actual glyph bitmaps follow, with each glyph stored as a
packed monochrome or 2-bit bitmap.

### Engine Usage

The FNT file provides the base font metrics. However, most text rendering in
TAK uses **GAF-based bitmap fonts** (e.g., `times new roman (100).gaf`,
`ig_times new roman (100).gaf`) referenced in GUI files. The FNT format may
be used for the in-game console or fallback rendering.

---

## Appendix A: File Count Summary

| Format | Count | Total Size | Location(s) |
|--------|-------|------------|-------------|
| TDF | 1,159 | ~500 KB | gamedata/, canbuild/, camps/, features/, soundclasses/ |
| FBI | 502 | ~671 KB | units/ |
| GUI | 127 | ~762 KB | guis/ |
| OTA | 77 | ~1 MB | maps/, missions/ |
| TXT (AI) | ~20 | ~40 KB | ai/ |
| SCC | 2 | 96 B | gamedata/, guis/ (dev artifact) |
| GAF | 465 | ~73 MB | anims/ |
| TAF | 256 | ~28 MB | anims/ |
| TNT | 2,624 | ~multi GB | maps/, sections/ |
| 3DO | 389 | ~3.3 MB | objects3d/ |
| COB | 205 | ~8 MB | scripts/ |
| PCX | 220 | ~321 KB | anims/, bitmaps/, palettes/ |
| PAL | 9 | ~30 KB | palettes/, anims/ |
| WAV | 656 | varies | english/Sounds/ |
| ALP | 35 | ~2.3 MB | palettes/ |
| SHD | 35 | ~280 KB | palettes/ |
| LHT | 35 | ~280 KB | palettes/ |
| GRY | 35 | ~9 KB | palettes/ |
| BLU | 35 | ~9 KB | palettes/ |
| CRT | 29 | ~520 KB | maps/ |
| BAD | 7 | ~126 KB | sections/ |
| TSF | 7 | varies | boneyards/, meta/ |
| GAO | 1 | ~41 KB | anims/ |
| GAO2 | 1 | ~16 KB | anims/ |
| PCO | 1 | ~2 KB | anims/ |
| FNT | 2 | ~3 KB | fonts/ |

---

## Appendix B: Palette System Architecture

TAK uses an 8-bit paletted rendering system with pre-computed lookup tables for
real-time effects. The palette ecosystem for each faction/texture set consists of:

```
Palette (.pal)         - 256 RGB colors (1024 bytes, 4 bytes per entry)
    |
    +-- Alpha (.alp)   - 256x256 blending table (65,536 bytes)
    +-- Shadow (.shd)  - 256x32 darkening table (8,192 bytes)
    +-- Light (.lht)   - 256x32 brightening table (8,192 bytes)
    +-- Grayscale (.gry) - 256-byte desaturation map
    +-- Blue (.blu)    - 256-byte underwater tint map
```

To render a translucent shadow on an Aramon unit:
1. Look up the shadow color for the source pixel: `shd[aramon][src_color][shadow_depth]`
2. Blend with the background: `alp[aramon][shadow_result][bg_color]`

This avoids per-pixel floating-point math, which was critical for 1998-era hardware.

---

## Appendix C: Key Architectural Relationships

```
FBI (unit definition)
  |-- references --> 3DO (objectname)
  |-- references --> COB (same name as unit, in scripts/)
  |-- references --> GAF (shadowgaf, button images)
  |-- references --> PAL (via sidedata.tdf faction palette)
  |-- references --> TDF (movementclass -> moveinfo.tdf)
  |-- references --> TDF (soundclass -> soundclasses/)
  |-- references --> TDF (explosionclass -> explosions.tdf)
  |-- references --> TDF (corpse -> features/)
  |-- references --> TDF (stone -> features/)

OTA (mission/map)
  |-- references --> FBI (unit placements)
  |-- references --> TDF (useonlyunits, aiprofile)
  |-- references --> TNT (terrain, same base name)
  |-- references --> CRT (creatures, same base name)

GUI
  |-- references --> GAF (all widget images)
  |-- references --> WAV (button sounds)
  |-- references --> GAF (bitmap fonts)

Sidedata TDF
  |-- references --> PAL (palette)
  |-- references --> PCX (buildpalette)
  |-- references --> GAF (buildsparklygaf, logogaf, stonegaf)
  |-- references --> FBI (commander, God)
```

---

## Appendix D: Parsing Priority Guide

For a C reimplementation, parse formats in this order:

1. **TDF parser** (shared by TDF, FBI, OTA, TSF) - covers ~1,800 files
2. **PAL loader** (trivial 1024-byte read)
3. **PCX loader** (standard format, many libraries available)
4. **GAF/TAF loader** (most complex, needed for all sprites)
5. **3DO loader** (needed for unit rendering)
6. **TNT/BAD loader** (needed for terrain)
7. **COB VM** (needed for animation)
8. **ALP/SHD/LHT/GRY/BLU** (trivial table loads, needed for rendering)
9. **WAV loader** (standard RIFF format)
10. **GUI parser** (custom format, needed for UI)
11. **FNT loader** (low priority, GAF fonts are primary)
12. **CRT loader** (low priority, map scripting)
