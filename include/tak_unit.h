#ifndef TAK_UNIT_H
#define TAK_UNIT_H

#include "tak_types.h"
#include "tak_platform.h"
#include "tak_cob.h"      /* for CobScript */
#include "tak_cob_vm.h"   /* for CobEngine, CobPiece */

/* Forward-declared; Units_Render uses it in its signature but the
 * header doesn't dereference it. Files that actually consume a
 * GameWorld include tak_world.h. */
struct GameWorld;

/* ── UnitMesh: GPU-friendly flattened 3DO ────────────────────────────
 *
 * Phase C M4-M5 lives here. Obj3DFile is a *tree* — convenient for
 * inspection, terrible to render (recursion + per-node vertex arrays
 * + per-prim variable-length index lists). Mesh_Bake walks an Obj3DFile
 * once and produces this flat layout:
 *
 *   positions[3*v]    model-space float vertex coords (TA units)
 *   uvs[2*v]          atlas-space UV pairs in [0..1]
 *   colors[v]         per-vertex RGBA8888 (white if textured, palette
 *                     lookup if FLAT_COLOR)
 *   indices[3*t]      triangle list, fan-triangulated from 3DO prims
 *   batches[]         draw groups by atlas — every triangle in
 *                     [first_index, first_index+index_count) samples
 *                     from the same atlas_tex. NULL atlas_tex means
 *                     "no texture, modulate per-vertex color directly"
 *                     (the FLAT_COLOR fallback for primitives whose
 *                     texture name doesn't resolve via TexAtlas).
 *
 * Per-node parent offsets are *baked into* positions during the walk,
 * so each vertex is already in the unit's model frame (origin = feet)
 * — render-time math just rotates/scales/projects, no tree traversal.
 *
 * One UnitMesh per UnitDef. Lazy-baked on first spawn; M6 moves the
 * bake call into LS_LOAD_UNITS for all defs. */

/* Forward — full struct lives in tak_gpu.h (opaque outside that file). */
typedef struct GPU_Texture GPU_Texture;

typedef struct UnitMeshBatch {
    GPU_Texture *atlas_tex;     /* NULL = flat-color fallback batch */
    int          first_index;   /* offset into UnitMesh.indices */
    int          index_count;   /* triangle-count * 3 */
} UnitMeshBatch;

#define UNIT_MESH_MAX_BATCHES 32
#define UNIT_MESH_MAX_NODES   128   /* shipped TAK monarchs have ≤50 */

/* Per-3DO-node info preserved at bake time so the submit path can
 * apply per-piece runtime transforms (Phase D animation).
 *
 * The node tree is depth-first in `nodes[]`: parents always precede
 * children. node[0] is the root and has parent=-1. */
typedef struct UnitMeshNode {
    char    name[32];
    int16_t parent;             /* index in nodes[]; -1 for root */
    int16_t _pad;
    float   offset[3];          /* static offset from parent (TA units) */
} UnitMeshNode;

typedef struct UnitMesh {
    /* Vertex data is in NODE-LOCAL space (Phase D M3 change). To get
     * a vertex's unit-space position, transform by the owning node's
     * cumulative world transform (parent chain × per-piece COB rot/pos).
     * For all-identity piece state, the cumulative transform is pure
     * translation by the parent chain offsets, recovering Phase C's
     * flat-fold positions. */
    float          *positions;   /* 3 * vert_count, node-local (TA units) */
    float          *uvs;         /* 2 * vert_count, atlas-space [0..1] */
    uint32_t       *colors;      /* 1 * vert_count, RGBA8 — per-prim flat shade */
    uint16_t       *indices;     /* 3 * tri_count, batch-relative */
    uint16_t       *vert_node_idx; /* vert_count entries; index into nodes[] */
    /* Authored draw order per triangle (global tri index → sequence in
     * the 3DO tree walk). The legacy renderer draws prims in model
     * order with backface culling only — NO depth sort
     * (legacy:197793-197807) — and artists authored prim order
     * for correct layering (capes after torsos). Single-unit draws
     * replay this order. */
    uint32_t       *tri_seq;     /* tri_count entries */
    int             vert_count;
    int             tri_count;

    UnitMeshBatch   batches[UNIT_MESH_MAX_BATCHES];
    int             batch_count;

    /* Per-node tree info — used by the renderer to compose per-piece
     * world transforms each frame. */
    UnitMeshNode    nodes[UNIT_MESH_MAX_NODES];
    int             node_count;

    /* Model-space AABB. Used for frustum culling. */
    float           aabb_min[3];
    float           aabb_max[3];
} UnitMesh;

/* ── Unit type registry (UnitDef) ───────────────────────────────────
 *
 * One UnitDef per entry in the units/ FBI tree. Loaded once per map at
 * LS_LOAD_UNITS time from every FBI the VFS exposes. Instances (Unit
 * struct, below) reference their definition by index into the global
 * UnitDef array.
 *
 * Phase C first pass captures only the fields needed to identify a
 * unit and find its sprite — weapons, damage tables, buttons, sounds
 * all live in the FBI but go unread for now. Add fields to UnitDef
 * and extend fbi_parser.c as Phase C features need them.
 */

#define TAK_UNITDEF_NAME_MAX   32     /* UnitName is usually ≤16 chars */
#define TAK_UNITDEF_SIDE_MAX   16     /* "ARA"/"VER"/"TAR"/"ZON" + null */
#define TAK_UNITDEF_OBJ_MAX    32
#define TAK_UNITDEF_DESC_MAX   64
#define TAK_UNITDEF_CAT_MAX    64     /* "ARA Monarch", "VER Infantry Attack", etc. */
#define TAK_DAMAGE_CATEGORY_MAX 16

typedef struct UnitDamageScale {
    char  category[32];
    float scale;
} UnitDamageScale;

/* Per-weapon stats parsed from FBI [WEAPONn] subsections. Up to 3
 * per unit. Sprint 1 captures the minimum needed for combat;
 * Phase G extends this with damage-category multipliers, projectile
 * physics (weaponvelocity, areaofeffect, edgeeffectiveness, etc.). */
typedef struct UnitWeapon {
    char    name[32];
    char    type[32];          /* type: Ballistic, Line of Sight, Remote Effect */
    char    damage_type[32];   /* damagetype */
    char    explosion_class[32]; /* explosionclass */
    char    weapon_art[32];    /* weaponart */
    char    model[32];         /* model projectile 3DO basename */
    char    subtype[32];       /* subtype for spell/special rules */
    char    hit_sound_class[24]; /* soundhitclass → soundclasses hit table */
    char    hit_sound[24];       /* soundhit — direct wav fallback */
    char    start_sound[24];     /* soundstart — played at each emission */
    /* Line-of-Sight weapons (lightning/fire/mindcontrol/...): instant
     * ray + held beam effect rather than a travelling bolt. */
    /* Legacy dispatches Line-of-Sight weapons on SUBTYPE (:249761-249836):
     * 0 = plain flying LOS bolt (draws its model/weaponart like a
     * ballistic shot), 1 = lightning beam, 2 = flame cone (emittime),
     * 3 = instant effect with no projectile of its own. */
    uint8_t los_kind;
    uint8_t is_los;             /* los_kind != 0 */
    int32_t emit_ticks;          /* emittime (30Hz frames) → 60Hz ticks */
    uint8_t beam_inner[3];       /* innercolor RGB */
    uint8_t beam_middle[3];      /* middlecolor RGB */
    uint8_t beam_outer[3];       /* outercolor RGB */
    int32_t water_weapon;     /* waterweapon targeting flag */
    int32_t to_air_weapon;    /* toairweapon targeting flag */
    int32_t no_air_weapon;    /* noairweapon targeting flag */
    int32_t no_radar;         /* noradar projectile/minimap flag */
    int32_t range;            /* world pixels */
    int32_t min_range;        /* minrange; 0 means no minimum */
    int32_t area_of_effect;   /* areaofeffect radius in world pixels */
    float   edge_effectiveness; /* edgeeffectiveness; AoE damage at radius */
    int32_t burst;            /* burst projectile count; 0/1 = single shot */
    int32_t burst_rate_ticks; /* burstrate converted to 60Hz ticks */
    int32_t spray_angle;      /* sprayangle authored angular spread */
    int32_t reload_ticks;     /* ticks (60 Hz); converted from FBI's seconds */
    int32_t damage;           /* HP per hit (default damage; categories ignored for MVP) */
    int32_t damage_scale_count;
    UnitDamageScale damage_scales[TAK_DAMAGE_CATEGORY_MAX];
    int32_t mana_per_shot;    /* manapershot — drained from owner's pool on each fire */
    int32_t velocity_pps;     /* weaponvelocity → projectile pixels/sec (0 = hitscan) */
    /* Projectile art, resolved once at parse. Legacy reads `model` into
     * the weapon's 3DO slot and `weaponart` into its GAF slot
     * (legacy:250074, legacy:250088) and draws whichever is set. */
    uint8_t art_kind;         /* UNIT_WEAPON_ART_* */
    char    art_name[32];     /* model basename or weaponart sequence */
    int16_t explosion_idx;    /* explosionclass slot, -1 = none */
    /* Spin: when all three are zero the projectile's pitch tracks its
     * velocity vector instead (legacy:250016-250020, legacy:246661). */
    int32_t spin_pitch;       /* spinpitch, 65536/turn per legacy tick */
    int32_t spin_heading;     /* spinheading */
    int32_t spin_roll;        /* spinroll */
    /* Ballistic flight. gravityadjustment scales the engine constant
     * and lobpreferred picks the high arc (legacy:246477). */
    float   gravity_adjust;
    uint8_t lob_preferred;
    uint8_t is_gravity;       /* type = Ballistic → arced flight */
    uint8_t dropped;          /* subtype = Dropped → no launch impulse */
    /* Button icon JPEG names (no extension, no path) — resolve to
     * `data/anims/weaponpic/<lowercased>.jpg`. The legacy engine reads
     * these `buttonimage*` fields from the inline [WEAPONn] section
     * to render per-weapon icons on the Primary/Secondary/Special
     * sidebar buttons (legacy:250088+). */
    char    icon_up[32];        /* buttonimageup     — normal state    */
    char    icon_down[32];      /* buttonimagedown   — pressed state   */
    char    icon_selected[32];  /* buttonimageselected — active weapon */
    char    icon_disabled[32];  /* buttonimagedisabled — out of mana   */
} UnitWeapon;

/* In-flight projectile spawned when a weapon fires. Travels toward
 * the target unit (or its last-known position) at weapon velocity;
 * applies damage + despawns on arrival. Rendered as a small bright
 * dot until per-weapon GAF sprites are wired (legacy: explosionclass /
 * model from the [WEAPONn] FBI block). Mirrors legacy data flow at
 * legacy:249423 (`Weapon_SpawnProjectile`). */
typedef struct Projectile {
    int32_t  world_x, world_y;
    float    dir_x, dir_y;        /* unit vector in world space   */
    float    speed_ppt;           /* pixels per tick (60Hz)        */
    int32_t  damage;
    int32_t  area_of_effect;
    float    edge_effectiveness;
    int32_t  damage_scale_count;
    UnitDamageScale damage_scales[TAK_DAMAGE_CATEGORY_MAX];
    int16_t  target;              /* unit slot, or -1              */
    int16_t  shooter;             /* firing unit slot — for XP credit */
    int16_t  ttl_ticks;           /* despawn safety net            */
    uint8_t  alive;
    uint8_t  player_id;
    uint8_t  visual_kind;         /* UNIT_PROJECTILE_VIS_* */
    uint8_t  friendly_fire;       /* ground shot: damage ALL teams */
    int32_t  dest_x, dest_y;      /* detonation point when target < 0 */
    char     hit_sound_class[24]; /* copied from the firing weapon */
    char     hit_sound[24];
    /* LOS beam: damage already applied at fire; this entry only holds
     * the visual for ttl_ticks. src→dest is the drawn ray. */
    uint8_t  is_beam;
    uint8_t  beam_rgb[3][3];      /* [inner|middle|outer][r,g,b] */
    int32_t  src_x, src_y;
    /* Arc + orientation. Legacy integrates gravity into the vertical
     * velocity each substep and derives pitch from the velocity vector
     * unless the weapon spins (legacy:246648-246676). */
    float    height;              /* world px above the launch ground   */
    float    vel_up_ppt;          /* vertical velocity, px per tick     */
    float    gravity_ppt2;        /* per-tick gravity; 0 = flat flight  */
    float    heading, pitch, roll;/* render orientation, radians        */
    float    spin_pitch, spin_heading, spin_roll;  /* radians per tick  */
    int32_t  src_height;          /* terrain height under the muzzle    */
    uint16_t age_ticks;           /* drives the weaponart frame cycle   */
    uint8_t  art_kind;            /* UNIT_WEAPON_ART_*                  */
    uint8_t  color_idx;           /* owner team colour (legacy:249446)  */
    int16_t  art_idx;             /* art cache slot, -1 = unresolved    */
    int16_t  explosion_idx;       /* explosionclass slot, -1 = none     */
} Projectile;

#define UNIT_PROJECTILE_VIS_GENERIC 0
#define UNIT_PROJECTILE_VIS_ARROW   1
#define UNIT_PROJECTILE_VIS_CANNON  2
#define UNIT_PROJECTILE_VIS_MAGIC   3
#define UNIT_PROJECTILE_VIS_REMOTE  4

/* Resolved projectile art (legacy:250074 model / legacy:250088
 * weaponart / the beam subtypes at legacy:249761). */
#define UNIT_WEAPON_ART_NONE   0
#define UNIT_WEAPON_ART_MODEL  1
#define UNIT_WEAPON_ART_SPRITE 2
#define UNIT_WEAPON_ART_BEAM   3

/* One-shot impact effect: the weapon's explosionclass sprite played at
 * the point of impact (legacy:245025). */
typedef struct ProjectileEffect {
    int32_t  world_x, world_y;
    int32_t  height;
    int16_t  sprite_idx;
    uint16_t age_ticks;
    uint8_t  alive;
} ProjectileEffect;

typedef struct UnitDef {
    char     unitname[TAK_UNITDEF_NAME_MAX];   /* canonical id, e.g. "ARAKING" */
    char     side    [TAK_UNITDEF_SIDE_MAX];   /* "ARA" / "VER" / "TAR" / "ZON" */
    char     objectname[TAK_UNITDEF_OBJ_MAX];  /* 3DO/GAF asset name           */
    /* FBI's `name` field — display name shown in the HUD's status box.
     * For monarchs this is the character's name ("Elsin", "Lokken",
     * "Kirenna", "Thirsha"); for other units a class label like "Bowman". */
    char     display_name[TAK_UNITDEF_DESC_MAX];
    /* FBI's `description` field — faction or unit-class tag, e.g.
     * "Aramon", "Taros", "Veruna", "Zhon" for monarchs. Shown as
     * subtitle/tooltip in legacy. */
    char     description[TAK_UNITDEF_DESC_MAX];
    char     category[TAK_UNITDEF_CAT_MAX];    /* free-form, e.g. "ARA Monarch" */
    char     damage_category[32];              /* damagecategory lookup key */
    char     soundcategory[32];                /* soundclass TDF section (legacy def+0xa2) */
    float    mogrium_bounty;                   /* mana granted to the killer (def+0x222) */
    int      unitnumber;
    float    buildtime;
    int      build_cost;        /* mana spent to construct this unit  */
    float    worker_time;       /* builder work rate from FBI workertime */
    int32_t  build_distance;    /* builddistance — reach to the build site */
    float    heal_time;         /* healtime; seconds to restore this unit */
    /* `experiencepoints` from FBI — when this unit dies, the killer
     * gains this many XP (legacy legacy:162918, default
     * 0x29a = 666). Drives the veteran-rank system. */
    int      kill_xp_value;

    /* Combat / movement stats from FBI UNITINFO. */
    int32_t  max_health;       /* maxdamage; 0 if not specified */
    int32_t  sight_distance;   /* world pixels; 0 if not specified */
    int32_t  radar_distance;   /* radardistance; radar-only detection radius */
    int32_t  can_fly;          /* canfly; needed for air target filters */
    int32_t  cruise_alt;       /* cruisealt: flight height above ground (legacy:162970) */
    /* activatewhenbuilt — legacy activates the unit the moment it
     * finishes (sets ACTIVATION and runs the COB Activate script;
     * lodestones raise their crystal through this). */
    int32_t  activate_when_built;
    int32_t  floater;          /* floater; unit floats on water surface */
    int32_t  waterline;        /* waterline; model waterline offset */
    int32_t  transport_size;   /* transportsize; carried slot footprint */
    int32_t  transport_capacity; /* transportcapacity; carried unit count */
    int32_t  transport_size_capacity; /* transportsizecapacity; size budget */
    int32_t  cant_be_transported; /* cantbetransported flag */
    int32_t  transported_size; /* transportedsize override when carried */
    int32_t  transport_distance; /* transportdistance; load/unload radius */
    char     movement_class[32];/* movementclass key into moveinfo.tdf */
    int32_t  min_water_depth;  /* minwaterdepth */
    int32_t  max_water_depth;  /* maxwaterdepth */
    int32_t  bad_min_water_depth; /* badminwaterdepth */
    int32_t  bad_max_water_depth; /* badmaxwaterdepth */
    int32_t  bad_slope;        /* badslope */
    int32_t  max_water_slope;  /* maxwaterslope */
    int32_t  bad_water_slope;  /* badwaterslope */
    float    max_velocity;     /* TAK units/sec from FBI; convert to pixels/tick at runtime */
    /* Movement law (FBI acceleration/brakerate/turnrate). Authored at
     * the original 30 Hz frame rate: acceleration and brakerate are
     * velocity-units gained/lost per frame, turnrate is TA angle units
     * (65536 = full circle) per frame. Converted at runtime so the
     * authored times-to-speed and turn periods are preserved at 60 Hz. */
    float    acceleration;
    float    brake_rate;
    float    turn_rate;

    /* Capability bitmask — drives which action buttons appear when
     * this unit is selected (matches decomp at the legacy reference
     * line 150627+ where flag 0x264 maps to the dialog's per-button
     * visibility bytes). FBI fields parsed: canmove, canstop,
     * canattack, canguard, canpatrol, cancloak, cantransport,
     * builder. Each bit set ↔ button should appear. */
    uint32_t cap_flags;
#define UNIT_CAP_MOVE      (1u << 0)
#define UNIT_CAP_STOP      (1u << 1)
#define UNIT_CAP_ATTACK    (1u << 2)
#define UNIT_CAP_GUARD     (1u << 3)
#define UNIT_CAP_PATROL    (1u << 4)
#define UNIT_CAP_CLOAK     (1u << 5)
#define UNIT_CAP_TRANSPORT (1u << 6)
#define UNIT_CAP_BUILDER   (1u << 7)
#define UNIT_CAP_RECLAIM   (1u << 8)   /* canreclaim — CLEAR button */
#define UNIT_CAP_RESURRECT (1u << 9)   /* canresurrect              */
#define UNIT_CAP_REPAIR    (1u << 10)  /* canrepair  — HEAL button  */
#define UNIT_CAP_LOAD      (1u << 11)  /* canload    — LOAD button  */
#define UNIT_CAP_W_SWITCH  (1u << 12)  /* weaponswitching — Pri/Sec/Special */

    /* Mana economy fields. The legacy engine treats two FBI pairs
     * as additive contributions to the owning player's pool:
     *   `MaxMana`            (monarch built-in capacity)
     *   `ManaRechargeRate`   (monarch built-in regen, per-second)
     *   `mogriumstorage`     (lodestone capacity bonus)
     *   `mogriumincome`      (lodestone regen bonus, per-second)
     *
     * The unit contributes (max_mana + mogrium_storage) to its
     * player's pool capacity and (mana_recharge_per_sec +
     * mogrium_income_per_sec) to regen on spawn, and removes the
     * same on death. Spending side (manapershot) is per-weapon and
     * lives on UnitWeapon. */
    int32_t  max_mana;
    float    mana_recharge_per_sec;
    int32_t  mogrium_storage;
    float    mogrium_income_per_sec;

    /* Building footprint in tiles (16 world-pixels per tile). 0 for
     * non-buildings. The placement preview uses this to draw the
     * outline at the cursor and clip-test against terrain features.
     * `max_slope` bounds the steepest tile the unit can be built on
     * (legacy maxslope/badslope at FBI offsets 0x10/0x11). */
    int      footprint_x;
    int      footprint_z;
    int      max_slope;

    /* Occupancy inputs. `bmcode` is the FBI build-menu code
     * (legacy:162925): 0 means a building, and only buildings get a
     * parsed yardmap (legacy:163208). `yardmap` is footprint_z rows of
     * footprint_x bytes, one per map cell. See tak_occupancy.h.
     * `is_gate` (FBI gate=, legacy:163110-163112) tags the cells that
     * block only while closed as gate cells; `onoffable`
     * (legacy:163023-163024) is what puts the Active/Inactive order
     * buttons on the HUD. */
    uint8_t *yardmap;
    int32_t  bmcode;
    int32_t  is_gate;
    int32_t  onoffable;
    /* Set when any yardmap cell carries the sacred bit ('S',
     * legacy:163237): the building only stands on a sacred site. */
    int      yardmap_sacred;

    /* Inline [WEAPON1..3] sections from FBI. num_weapons is 0..3. */
    int          num_weapons;
    UnitWeapon   weapons[3];

    /* Per-player-color baked meshes. NULL until first spawn with
     * that color; Mesh_Bake fills it lazily. */
    UnitMesh *mesh_per_color[12];

    /* COB script bundle (Phase D). Loaded eagerly per-def. */
    CobScript *cob_script;
} UnitDef;

/* ── Unit instance ──────────────────────────────────────────────────
 *
 * One Unit per thing on the map. Stored in a flat array on the
 * GameWorld side (Units_GetActive etc.) so the render loop is a
 * simple linear walk. 2000 cap matches the original engine's "Units"
 * slider in Battle Setup.
 *
 * Coordinates are **world pixels** — same space as cam_x/cam_y. The
 * render layer converts to window pixels with (world_x - cam_x).
 */
/* Command kinds for unit orders (Sprint 1 minimal RTS).
 * Eventually Phase H formalises this with a queue and richer kinds. */
#define UNIT_CMD_NONE    0
#define UNIT_CMD_MOVE    1   /* walk to (cmd_x, cmd_y) */
#define UNIT_CMD_ATTACK  2   /* approach + attack target_handle */
#define UNIT_CMD_BUILD   3   /* approach (cmd_x,cmd_y) and add HP to target */
#define UNIT_CMD_PATROL  4   /* walk between (cmd_x,cmd_y) and patrol_x/y */
#define UNIT_CMD_GUARD   5   /* follow/protect friendly target_handle */
#define UNIT_CMD_REPAIR  6   /* target friendly damaged unit/building */
#define UNIT_CMD_RECLAIM 7   /* reclaim/clear target feature or unit */
#define UNIT_CMD_LOAD    8   /* transport pickup target */
#define UNIT_CMD_UNLOAD  9   /* transport drop point */
#define UNIT_CMD_ATTACK_GROUND 10 /* fire at (cmd_x,cmd_y) until new order */

#define UNIT_PATH_MAX_WAYPOINTS 96

/* ── Animation state machine ───────────────────────────────────────
 *
 * A unit is always in exactly one anim state. The per-tick
 * update_animation function reconciles the *desired* state (derived
 * from cmd, target, health, etc.) with the *active* state, firing
 * COB scripts on entry and killing them on exit. Extending the
 * machine to spell-cast, build, takeoff, etc. is a matter of
 * adding new states + their script names.
 *
 * Per-weapon state is independent of the unit-level state — a unit
 * can be MOVING while weapon[0] is mid-aim. The combat system
 * loops over weapons each tick; each has its own cooldown +
 * AimWeapon thread reference. Multi-weapon units (some monarchs
 * have 3 weapons) get all of them firing on schedule. */
typedef enum {
    UNIT_ANIM_IDLE     = 0,   /* Create finished, no orders */
    UNIT_ANIM_MOVING   = 1,   /* walk script keeps animating legs */
    UNIT_ANIM_ATTACKING= 2,   /* in weapon range, AimWeapon/FireWeapon */
    UNIT_ANIM_BUILDING = 3,   /* StartBuilding script (Phase F) */
    UNIT_ANIM_DYING    = 4,   /* Killed script running */
    UNIT_ANIM_DEAD     = 5    /* Killed done; cleanup pending */
} UnitAnimState;

/* Entry points legacy invokes exactly once per state edge. The engine
 * counts its own invocations so tests can assert the contract without
 * caring whether a given script defines the entry point.
 * See docs/notes/2026-09-09-cob-entry-points.md. */
typedef enum {
    UNIT_SCRIPT_EV_START_BUILDING = 0,
    UNIT_SCRIPT_EV_STOP_BUILDING  = 1,
    UNIT_SCRIPT_EV_START_MOVING   = 2,
    UNIT_SCRIPT_EV_STOP_MOVING    = 3,
    UNIT_SCRIPT_EV_ACTIVATE       = 4,
    UNIT_SCRIPT_EV_BEGIN_FLIGHT   = 5,
    UNIT_SCRIPT_EV_BEGIN_LANDING  = 6,
    UNIT_SCRIPT_EV_COUNT          = 7
} UnitScriptEvent;

/* Per-weapon runtime state, one per slot up to UnitDef.num_weapons.
 * cooldown_ticks counts down each tick after a shot; aim_thread_slot
 * tracks an active AimWeapon thread so we don't spawn duplicates. */
typedef struct UnitWeaponState {
    int32_t cooldown_ticks;
    int32_t burst_ticks;
    int16_t burst_remaining;
    int16_t burst_target;
    int8_t  aim_thread_slot;     /* -1 if no aim thread active */
    int16_t aim_ticks;           /* bounded wait for AimWeapon completion */
    int16_t aim_target;          /* target handle for the active aim */
} UnitWeaponState;

/* Aggression posture per the legacy engine
 * (the legacy reference ~9063 + 151409). Stored at unit+0x264 bits
 * 21..23 in legacy; we keep a clean enum here. Drives the auto-target
 * loop: passive units never auto-attack, defensive only fires at
 * close threats, offensive actively pursues enemies in sight range. */
#define UNIT_AGGRO_PASSIVE    0
#define UNIT_AGGRO_DEFENSIVE  1
#define UNIT_AGGRO_OFFENSIVE  2

#define UNIT_ALIVE_DEAD        0
#define UNIT_ALIVE_ACTIVE      1
#define UNIT_ALIVE_DYING       2
#define UNIT_ALIVE_TRANSPORTED 3

typedef struct Unit {
    uint32_t   stable_id;   /* deterministic replay/network identity */
    int32_t    world_x;     /* pixel position, top-left of footprint */
    int32_t    world_y;
    float      heading;     /* radians, 0 = facing south             */
    int32_t    velocity;    /* current speed in COB units/sec; 0 = stationary */
    int32_t    health;      /* current HP (Sprint 1 placeholder — Phase G refines) */
    int32_t    max_health;
    int32_t    cmd_x, cmd_y;/* command target world coords (when MOVE/ATTACK) */
    int32_t    patrol_x, patrol_y; /* patrol return point */
    int16_t    target;      /* target unit slot index, or -1 if none */
    int16_t    cmd_kind;    /* UNIT_CMD_* */
    int16_t    attack_cooldown; /* ticks until next attack swing      */
    uint16_t   def_idx;     /* index into the UnitDef registry       */
    uint8_t    player_id;   /* 1..TAK_MAX_PLAYERS                    */
    uint8_t    team_color_idx;/* 0..11, indexes Units_GetTeamColorRGBA */
    uint8_t    alive;       /* UNIT_ALIVE_* lifecycle state          */
    uint8_t    aggro_mode;  /* UNIT_AGGRO_* (default: OFFENSIVE)     */
    uint8_t    weapon_slot; /* 0..2 — which UnitWeapon fires (legacy
                              * Minimap_SetMode mode) */
    int32_t    experience_pts; /* accumulated XP for veteran ranks    */
    /* When cmd_kind == UNIT_CMD_BUILD: handle of the building this
     * unit is constructing. The builder walks to (cmd_x, cmd_y) and
     * adds delta-HP to the target each tick until it reaches max.
     * -1 when not building. */
    int16_t    build_target;
    /* UNIT_CMD_RECLAIM with target < 0: the map feature being cleared,
     * held as its tile so the world feature array stays free to
     * compact when something else is removed. -1 when not reclaiming a
     * feature. */
    int16_t    reclaim_tile_x, reclaim_tile_y;
    float      reclaim_accum;   /* fractional feature HP removed */
    int16_t    carried_by;  /* transport handle when TRANSPORTED, else -1 */
    int16_t    cargo_count; /* number of units carried by this transport */
    int16_t    cargo_size_used; /* sum of transported_size/transportsize */
    /* Building under construction: 1 while health < max_health and
     * a builder is feeding it; lets render code show construction
     * scaffolding/dust without confusing it with battle damage. */
    uint8_t    under_construction;
    /* COB SET-VALUE port state written by unit scripts (0x10082000)
     * and read back by GET ports 1 / 5 / 18. */
    uint8_t    cob_activation;    /* port 1  ACTIVATION    */
    uint8_t    cob_build_stance;  /* port 5  INBUILDSTANCE */
    uint8_t    cob_yard_open;     /* port 18 YARD_OPEN     */
    uint8_t    cob_bugger_off;    /* port 19 BUGGER_OFF    */
    /* Flight. A flyer takes off when it gets something to do and lands
     * when it goes idle (legacy:24117, legacy:24302). flight_alt is the
     * height above the ground it is drawn and hit at; sfx_occupy is the
     * last setSFXoccupy state sent (legacy:185079-185112). */
    float      flight_alt;
    uint8_t    flying;
    uint8_t    sfx_occupy;
    /* A caster's own mana: a value and its cap (legacy unit+0xd8),
     * filled by manarechargerate per frame and spent per shot. */
    float      mana;
    float      mana_max;
    /* Occupancy bookkeeping: occ_on once the footprint is stamped,
     * occ_pending while an imprint yielded a cell and must retry
     * (legacy:217994-218006). gate_scan_cd staggers the gate
     * proximity scan; gate_hold keeps a manual open/close order from
     * being undone by the next scan. */
    uint8_t    occ_on;
    uint8_t    occ_pending;
    int16_t    gate_scan_cd;
    int16_t    gate_hold;
    /* Top-left occupancy tile of the footprint currently stamped for a
     * mobile unit; lets the per-tick update early-out when it has not
     * changed cell (legacy:217933-217971 re-imprints on every move). */
    int16_t    occ_tx;
    int16_t    occ_ty;
    /* Move-class footprint in tiles, resolved once at spawn: the step
     * test runs per probe and must not re-search the move-class table. */
    uint8_t    occ_fx;
    uint8_t    occ_fz;
    float      build_hp_accum;  /* fractional construction HP fed by builders */
    /* Ticks since a builder last fed this nanoframe. Past a 10s grace
     * an abandoned frame decays at half build rate, refunding mana
     * proportionally (legacy :9629-9657 + :39510-39524). */
    int16_t    nano_idle_ticks;
    /* Sub-pixel movement accumulator: per-tick movement is often
     * less than one pixel, so we keep a float remainder and only
     * advance integer world_x/y when it crosses 1.0. */
    float      subpixel_x, subpixel_y;
    /* Current speed in pixels/tick for the bang-bang movement law
     * (accelerates toward max, brakes inside stopping distance). */
    float      cur_speed_ppt;
    int32_t    path_goal_x, path_goal_y;
    uint8_t    path_len;
    uint8_t    path_index;
    uint8_t    path_failed;
    uint8_t    path_pending;    /* A* deferred by the per-tick budget */
    uint8_t    path_wait;       /* ticks waiting for that plan */
    uint8_t    blocked_ticks;   /* consecutive terrain-blocked steps */
    int8_t     avoid_side;      /* remembered obstacle-hug direction */
    int16_t    wp_stall;        /* ticks without closing on the waypoint */
    int32_t    wp_best_d2;      /* closest approach to it so far */
    /* Cooldown between exhausted-path replans — a crowd parked on a
     * shared goal must not re-run A* per unit per tick. */
    int16_t    path_replan_cd;
    uint8_t    _path_pad[1];
    int32_t    path_x[UNIT_PATH_MAX_WAYPOINTS];
    int32_t    path_y[UNIT_PATH_MAX_WAYPOINTS];
    /* Animation state machine — see UnitAnimState above. */
    uint8_t    anim_state;
    int8_t     walk_thread_slot;     /* -1 if no walk thread active */
    int8_t     killed_thread_slot;   /* -1 if no Killed thread active */
    int8_t     build_thread_slot;    /* -1 if no StartBuilding thread active */
    /* Cached values legacy compares against before invoking MoveRate
     * and TurnDirection, so each fires only on a change
     * (legacy:184448, legacy:183171). */
    int8_t     move_rate_tier;
    int8_t     turn_dir_sign;
    /* Engine-driven entry-point invocations, counted so the guard
     * tests can assert legacy's once-per-edge contract. */
    uint16_t   script_ev[UNIT_SCRIPT_EV_COUNT];
    /* Per-weapon runtime state. weapons.num_weapons in UnitDef tells
     * how many slots are populated; trailing slots are unused. */
    UnitWeaponState weapon_state[3];
    /* Factory production queue (manual: multiple clicks queue units;
     * the structure builds each in turn). def indices, FIFO. */
#define UNIT_PROD_QUEUE_MAX 32
    int16_t    prod_queue[UNIT_PROD_QUEUE_MAX];
    uint8_t    prod_queue_len;
    /* Rally point (manual: select the structure and click Move —
     * units emerging rally to that point). */
    uint8_t    rally_set;
    int32_t    rally_x, rally_y;

    /* COB engine — heap-allocated per unit. Owns the per-piece state
     * the renderer reads each frame. NULL if the unit's def has no
     * cob_script. Allocated on Spawn, freed on death/clear. */
    CobEngine *cob;
} Unit;

/* ── UnitDef registry (static, per-world) ─────────────────────────── */

/* Walk every .fbi under units, parse each into a UnitDef, store in the global
 * registry. Replaces any previous registry (safe to call per-world).
 * Returns number of defs loaded (>= 0), or -1 on allocation failure. */
int               Units_LoadDefs(void);

/* Tear down the registry and free all defs. Called from World_End.
 * Safe to call when no registry is loaded. */
void              Units_FreeDefs(void);

/* Lookup by unitname (case-insensitive). Returns -1 if not found.
 * The index is stable for the lifetime of the registry. */
int               Units_FindDefByName(const char *unitname);

/* Find the faction's monarch UnitDef. side_prefix is the ALL-CAPS
 * faction tag as it appears in the FBI's `category` field —
 * "ARA" / "VER" / "TAR" / "ZON". Returns -1 if no monarch def is
 * registered for that faction. */
int               Units_FindMonarchDef(const char *side_prefix);

/* Read-only accessor; returns NULL on out-of-range index. */
const UnitDef    *Units_GetDef(int idx);

int               Units_GetDefCount(void);

/* ── Active unit array (per-world) ──────────────────────────────── */

/* Reset the active unit array to empty. Call once per map load. */
void              Units_ClearInstances(void);

/* Spawn one unit at the given world coords, owned by player_id.
 * team_color_idx (0..11) picks which entry of the team-color palette
 * tints flagged vertices at render time. Returns a unit handle
 * (index >= 0) on success, -1 if the array is full or def_idx is
 * invalid. */
int               Units_Spawn(int def_idx, int player_id, int team_color_idx,
                               int32_t world_x, int32_t world_y);

/* Render a one-shot translucent "ghost" of a building at the given
 * world coords — used by the placement cursor to preview what the
 * player is about to build. Tinted green when valid, red when the
 * site is blocked. alpha255 = peak opacity (e.g. 128 for ~50%). */
struct GameWorld;
struct TAK_Platform;
void              Units_RenderBuildGhost(struct TAK_Platform *plat,
                                          const struct GameWorld *world,
                                          int def_idx, int color_idx,
                                          int32_t world_x, int32_t world_y,
                                          uint8_t alpha255, int valid);

/* Backface culling toggle. On by default; off draws both sides of
 * every triangle (useful for diagnosing whether TAK's models use
 * the opposite winding convention). */
int               Units_GetBackfaceCullOn(void);
void              Units_SetBackfaceCullOn(int on);
int               Units_GetBackfaceCullInvert(void);
void              Units_SetBackfaceCullInvert(int v);

/* Rotate every alive unit's heading by `delta_rad` radians. Used by
 * the debug panel's heading +/- buttons so you can see the front and
 * sides of the model. */
void              Units_DebugRotateAll(float delta_rad);

/* Set one unit's heading (radians, 0 = facing south). No-op if `handle`
 * is out of range or the slot is not alive. */
void              Units_SetHeading(int handle, float heading);

/* Apply a mission-authored starting health percentage after spawn.
 * pct is clamped to 0..100; units with a positive max health stay at
 * least 1 HP so scenario placement cannot create an immediately empty
 * live slot by accident. */
void              Units_SetHealthPercent(int handle, int pct);

/* Phase D M3 debug: rotate the head piece of every alive unit by
 * delta_units (in COB fixed-point: 65536 = 2π). Used by ingame
 * hotkey 'H' to verify per-piece transforms work. Falls back to no-op
 * if the unit's mesh has no node named "Head". */
void              Units_DebugRotateHead(int32_t delta_units);

/* Phase D M5 debug: invoke a named script (e.g. "walk") on every
 * alive unit. Each call spawns a fresh thread on the unit's COB
 * engine — multiple invocations stack until thread slots fill (16).
 * Returns the number of threads successfully started across all
 * units. */
int               Units_DebugInvokeScript(const char *script_name);

/* Phase D M5b debug: nudge every alive unit's velocity by delta
 * (units/sec). MoveWatcher and walk scripts poll this via
 * GET-UNIT-VALUE, so a non-zero velocity can drive walk animation. */
void              Units_DebugBumpVelocity(int32_t delta);

/* Phase D M6 debug: invoke Killed on the first alive unit. The
 * script's "Killed" thread runs (typically explodes pieces, plays
 * death sfx); when its threads finish the unit is despawned by
 * Units_TickEngines. */
int               Units_DebugKillFirst(void);
int               Units_DebugKillHandle(int handle);
/* Test hook: set posture on any unit (PASSIVE also clears its target). */
void              Units_DebugSetAggro(int handle, int aggro_mode);

/* Test hook: how many times the engine has invoked one of the
 * once-per-edge entry points on this unit. -1 for a bad handle. */
int               Units_DebugScriptEventCount(int handle, UnitScriptEvent ev);

/* Test hook: world heading (radians, 0 = north, +x = east at pi/2) that
 * a named mesh piece faces, its authored forward composed through the
 * unit's own heading. Lets a test assert a turret points at its target
 * instead of away from it. Returns 0 if the piece is not found. */
int               Units_DebugPieceWorldHeading(int handle,
                                               const char *piece_name,
                                               float *out_heading);

/* Sprint 1 debug: spawn an enemy monarch (player_id = 2, opposite
 * faction from player 1) near the camera center for fight testing. */
int               Units_DebugSpawnEnemy(int32_t world_x, int32_t world_y);

/* Read-only slice of in-flight projectiles. Render walks this list
 * and draws each; iterate [0, *out_count) and skip entries where
 * alive == 0. */
const Projectile *Units_GetProjectiles(int *out_count);

/* Read-only slice of live impact effects (explosionclass sprites). */
const ProjectileEffect *Units_GetProjectileEffects(int *out_count);

/* Resolved projectile art for one weapon slot. Returns UNIT_WEAPON_ART_*
 * or -1 for a bad slot; when out_name is given it receives the model
 * basename or weaponart sequence (empty for beams and melee). */
int               Units_GetWeaponArtKind(int def_idx, int weapon_slot,
                                         char *out_name, int out_cap);

/* Drop all per-color cached UnitDef meshes. Use after TexAtlas_Reload
 * so the next spawn re-bakes meshes against the new atlases. Active
 * unit instances remain in the array but won't render until next bake.
 * (Caller should typically Units_ClearInstances after this.) */
void              Units_DropAllMeshCaches(void);

/* Eager-bake the four canonical monarch meshes (ARAKING, TARNECRO,
 * VERMAGE, ZONHUNT). Called once during LS_LOAD_UNITS after
 * Units_LoadDefs. Lazy-bake still works for any def the eager pass
 * skipped. Returns the number of meshes successfully baked. */
int               Units_BakeMonarchMeshes(void);

/* Look up the RGBA value of one of the 12 player team-color slots.
 * Matches battle_setup.c's bs_player_colors mapping (Blue / Red /
 * Green / Yellow / Cyan / Magenta / Orange / White / Dark Blue /
 * Dark Red / Dark Green / Grey). idx outside 0..11 returns white. */
uint32_t          Units_GetTeamColorRGBA(int idx);

/* Read-only slice of the active array. *out_count is set to the
 * number of valid entries; iterate [0, *out_count) and skip entries
 * where alive == 0. Pointer is invalidated by any Spawn/Kill call. */
const Unit       *Units_GetActive(int *out_count);
uint32_t          Units_GetStableId(int handle);
int               Units_FindByStableId(uint32_t stable_id);

/* ── 3D render path (Phase C M4) ──────────────────────────────────── */

/* Phase D: tick every alive unit's COB engine — animator pass + run
 * threads. Currently called per render frame from ingame.c (will move
 * to the 60Hz fixed sim tick proper once that wiring lands). */
void              Units_TickEngines(void);

/* Toggle the per-unit health bars rendered above each unit. Default
 * on. Bound to '~' per manual §IV.2 ("Health bars can be turned on
 * and off with the '~' key"). */
void              Units_ToggleHealthBars(void);
void              Units_SetHealthBarsOn(int on);
/* Where the unit's damage bar would draw this frame, in viewport
 * pixels, or 0 when the original's rule draws none (setting off, not
 * the local player's unit without cheat codes, or under 1 HP). */
int               Units_DebugHealthBarRect(int handle, SDL_Rect *out);
int               Units_GetHealthBarsOn(void);
/* The unit's own mana reserve. Returns 0 for a unit without one. */
int               Units_GetMana(int handle, float *out_cur, float *out_max);
void              Units_DebugSetMana(int handle, float value);

/* ── Selection + manual commands ──────────────────────────────────
 *
 * Left-click selects (replaces selection); shift-click toggles a unit
 * in/out of the selection; drag-rectangle marquee-selects every own
 * unit whose centre falls inside (shift-drag adds). Ctrl+0..9 stores
 * the selection in a control group, plain 0..9 recalls it. Commands
 * issued through Units_Command*Selected apply to every selected unit. */

#define UNITS_SELECTION_MAX 100

/* Add one unit to the selection (no-op if full/dead/duplicate). */
void              Units_SelectAdd(int handle);
/* Shift-click semantics: remove if selected, else add. */
void              Units_SelectToggle(int handle);
/* Marquee: select own (player 1) alive units whose centre lies inside
 * the world-space rect (corners in any order). additive keeps the
 * existing selection (shift-drag). Returns the new selection count. */
int               Units_SelectInRect(int32_t x0, int32_t y0,
                                     int32_t x1, int32_t y1, int additive);
/* Control groups 0..9: assign copies the current selection; recall
 * replaces the selection with the group's still-living members and
 * returns the resulting count. */
void              Units_AssignControlGroup(int group);
int               Units_RecallControlGroup(int group);

/* Find the alive unit closest to (world_x, world_y) within radius pixels.
 * Returns the unit's slot handle, or -1 if no unit is in range. */
int               Units_PickAt(int32_t world_x, int32_t world_y, int radius);

/* Look up player_id of a unit by handle. Returns 0 if handle is invalid
 * or the slot is not alive — used by the click handler to distinguish
 * friendly vs enemy clicks. */
int               g_units_get_player(int handle);
int               Units_IsUnderConstruction(int handle);
int               Units_CanStandAt(int handle, int32_t x, int32_t y);
/* Veteran rank 0..10 (0 = not a veteran). */
int               Units_GetVeteranLevel(int handle);
int               Units_GetSelectedVeteranLevel(void);
void              Units_CommandAttackGroundSelected(int32_t world_x,
                                                    int32_t world_y);
int               Units_SelectionHasBuilder(void);

/* HUD-side accessors for the first selected unit (NULL/zero when no
 * selection). Display strings are derived from the unit's anim_state
 * and def fields. The strings are static — caller must not free or
 * cache them past the next call. */
const char       *Units_GetSelectedName(void);
const char       *Units_GetSelectedStatus(void);
void              Units_GetSelectedHealth(int *out_hp, int *out_max);
const UnitDef    *Units_GetSelectedDef(void);   /* NULL when nothing selected */

/* Spawn a building at (world_x, world_y) for `player_id` at low health
 * and immediately order the selected friendly builder(s) to walk to
 * the site and add HP to it (legacy 1.0/workertime per tick formula).
 * Returns the new building's unit handle, or -1 on failure (player
 * couldn't afford it, no buildable def, no friendly builder selected,
 * etc). Mirrors the legacy reference's `Build_BeginConstruction`. */
int               Units_BeginBuilding(int building_def_idx,
                                       int32_t world_x, int32_t world_y);
int               Units_BeginBuildingForUnit(int builder_handle,
                                             int building_def_idx,
                                             int32_t world_x,
                                             int32_t world_y);

/* ── Factory production queue + rally (manual §Summoning Units) ────
 *
 * Enqueue a unit for factory production. Starts immediately when the
 * factory is idle, otherwise appends to the FIFO (cap
 * UNIT_PROD_QUEUE_MAX). Returns 0 on success, -1 on bad args/full
 * queue. Queue advances automatically as each unit completes. */
int               Units_FactoryEnqueue(int factory_handle, int product_def_idx);

/* Cancel the in-progress production (removes the nanoframe; mana
 * already fed is forfeit — TAK pays per tick during construction) and
 * starts the next queued item, if any. Returns 0 if something was
 * cancelled. */
int               Units_FactoryCancelCurrent(int factory_handle);

/* Number of queued (not yet started) products. */
int               Units_FactoryQueueCount(int factory_handle);
/* In-progress + queued count of one product def (HUD badge). */
int               Units_FactoryQueuedCountForDef(int factory_handle,
                                                 int def_idx);
/* Right-click on a build card: remove last queued of def, else cancel
 * the in-progress one. Returns 0 on removal, -1 if none. */
int               Units_FactoryDequeueDef(int factory_handle, int def_idx);

/* Set/clear the rally point: completed products walk here instead of
 * the default yard-exit spot. Mirrors the manual's "click Move on the
 * structure" flow — Units_CommandMoveSelected routes here for
 * immobile builders. */
void              Units_FactorySetRally(int factory_handle,
                                        int32_t world_x, int32_t world_y);

/* World position of a factory's build pad, the ring at the front of
 * the yard where products materialise. Resolved by running the unit's
 * COB QueryBuildInfo script and mapping the piece it names into world
 * space (legacy:9347-9362). Returns 1 and writes the spot, or 0 when
 * the script names no piece (production then falls back to the yard
 * centre). NOTE: this RUNS the script, so a stateful one advances its
 * own state one step per call, exactly as in legacy. */
int               Units_FactoryBuildSpot(int factory_handle,
                                         int32_t *out_x, int32_t *out_y);

/* Check whether a building of `def_idx` can legally be placed centred
 * at (world_x, world_y) — no other units / buildings inside its
 * footprint. Returns 1 if clear, 0 if blocked. Mirrors legacy
 * `Terrain_FindBuildPlacement` (legacy:219106): walks every
 * footprint tile and checks the occupancy grid. Our occupancy proxy
 * is the active-unit array (no separate tile grid yet); slope + water
 * checks are stubbed until heightmap-aware terrain lands. */
int               Units_IsBuildSiteClear(int def_idx,
                                          int32_t world_x, int32_t world_y);

/* Snap a build centre onto the cell grid the way legacy turns a
 * cursor into a build cell and reads its centre back
 * (legacy:184168, :184216). Units_IsBuildSiteClear applies it for
 * you; callers that place or draw at the site apply it too so the
 * ghost, the click and the finished building agree. */
void              Units_SnapBuildSite(int def_idx,
                                       int32_t *world_x, int32_t *world_y);

/* Yardmap cell-code bits, as the FBI parse assigns them
 * (legacy:163224-163256) and placement tests them (legacy:218804). */
#define TAK_YARD_LEVEL     0x08   /* cell feeds the ground height span */
#define TAK_YARD_WATER     0x10   /* cell floats: track its height only */
#define TAK_YARD_BLOCK     0x20   /* cell rejects blocking features   */
#define TAK_YARD_SACRED    0x80   /* cell must sit on a sacred site   */
#define TAK_YARD_MAX_CELLS 256    /* largest shipped footprint is 160 */

/* Expand a def's yardmap into one code byte per footprint cell, row
 * major. Returns the cell count written, or 0 when the def has no
 * yardmap or its footprint exceeds `max`. */
int               Units_ExpandYardmap(const UnitDef *d,
                                       uint8_t *out, int max);

/* ── Build menu ──────────────────────────────────────────────────────
 *
 * For a given builder def, enumerate the unit defs it can build.
 * Returns the count written to out_def_idxs (capped at max_out).
 *
 * The legacy data lives at data/canbuild/<builder>/<buildable>.tdf —
 * each TDF carries a [Menu] section with a Priority field. We sort by
 * priority ascending so the menu order matches legacy. */
int               Units_GetBuildables(int builder_def_idx,
                                       int *out_def_idxs,
                                       int max_out);

/* Footprint dimensions in world tiles (16-pixel units). Matches the
 * `footprintx` and `footprintz` FBI fields. Also exposes maxslope so
 * the building-placement preview can color-code valid vs invalid sites
 * without re-parsing the FBI. */
int               Units_GetFootprintX(int def_idx);
int               Units_GetFootprintZ(int def_idx);

/* Replace the current selection with [handle] (or clear if handle < 0). */
void              Units_SelectSingle(int handle);

/* Read-only access to current selection. */
const int        *Units_GetSelection(int *out_count);

/* Issue a MOVE command to all selected friendly units. */
void              Units_CommandMoveSelected(int32_t world_x, int32_t world_y);

/* Direct per-unit command helper for scenario loading and script systems. */
void              Units_CommandMoveUnit(int handle,
                                        int32_t world_x,
                                        int32_t world_y);
/* Standing patrol for one unit (anchor = its current position). */
void              Units_CommandPatrolUnit(int handle,
                                          int32_t world_x,
                                          int32_t world_y);

void              Units_CommandPatrolSelected(int32_t world_x, int32_t world_y);
void              Units_CommandGuardSelected(int target_handle);
void              Units_CommandAttackUnit(int handle, int target_handle);
void              Units_CommandAttackUnitScript(int handle, int target_handle);
void              Units_CommandRepairSelected(int target_handle);
void              Units_CommandReclaimSelected(int target_handle);
/* CLEAR / sweep cursor on the terrain: send every selected reclaimer to
 * the map feature under (world_x, world_y). Returns the number of units
 * given the order, 0 when the click hit no reclaimable feature. Legacy
 * resolves the clicked cell the same way (legacy:187186-187198). */
int               Units_CommandReclaimFeatureSelected(int32_t world_x,
                                                      int32_t world_y);
void              Units_CommandLoadSelected(int target_handle);
void              Units_CommandUnloadSelected(int32_t world_x, int32_t world_y);
void              Units_SetOwner(int handle, int player_id, int team_color_idx);
void              Units_SetVelocity(int handle, int32_t velocity);

/* Issue an ATTACK command to all selected friendly units. */
void              Units_CommandAttackSelected(int target_handle);

/* Issue a STOP command to all selected friendly units (clears any
 * pending move/attack/etc; unit halts in place). Mirrors legacy
 * STOP_UNITORDER mission script. */
void              Units_CommandStopSelected(void);

/* Set aggression posture (UNIT_AGGRO_*) on every selected friendly
 * unit. Mirrors legacy NetPacket_Method03 Selection_IssueAttackOrder
 * dispatch (legacy:151409+). */
void              Units_CommandSetAggroSelected(int aggro_mode);

/* Set active weapon slot (0..2 — Primary/Secondary/Special) on every
 * selected friendly unit. Combat code reads this to pick which
 * UnitWeapon to fire in TickCombat. Mirrors legacy Minimap_SetMode
 * (legacy:233957). */
void              Units_CommandSetWeaponSlotSelected(int slot);

/* Read aggro mode of the first selected unit (or -1 if no
 * selection). Used by the HUD to highlight the active aggro button. */
int               Units_GetSelectedAggroMode(void);

/* ── Gate open/close order ────────────────────────────────────────
 *
 * The legacy order panel shows an Active / Inactive button pair for
 * any `onoffable` def and issues the ACTIVATE / DEACTIVATE order
 * (legacy:151449-151470), which flips the unit's activation bit and
 * runs the COB Activate / Deactivate thread (legacy:236399-236406).
 * A gate's Activate opens its yard, Deactivate closes it.
 *
 * Units_GateState: 1 open (active), 0 closed, -1 if the handle is not
 * an onoffable gate. Units_SetGateOpen issues the order and holds it
 * briefly against the auto-open scan. */
int               Units_GateState(int handle);
void              Units_SetGateOpen(int handle, int open);

/* Same, for the first selected unit. What the HUD button row calls. */
int               Units_SelectedGateState(void);
void              Units_ToggleSelectedGate(void);

/* Yard state setter behind COB port 18. Refusable: returns 0 without
 * writing when another live unit stands on a cell that would block
 * under the new state (legacy:218984-219042). */
int               Units_TrySetYardOpen(int handle, int open);

/* Read active weapon slot of first selected unit (-1 if none). */
int               Units_GetSelectedWeaponSlot(void);
int               Units_GetWeaponVisualKind(int def_idx, int weapon_slot);
int               Units_ComputeSplashDamage(int base_damage,
                                             int area_of_effect,
                                             float edge_effectiveness,
                                             int64_t dist_sq);
int               Units_ComputeWeaponDamageForCategory(const UnitWeapon *wp,
                                                        const char *category);

/* Build draw cmds for every alive unit, transform their meshes from
 * model→world→screen, and emit triangles via SDL_RenderGeometryRaw.
 * Lazy-bakes UnitDef.mesh on first sight of each def. Called from
 * ingame.c between Terrain_Render and Minimap_Draw.
 *
 * M4 ignores textures (everything renders flat white). M5 wires UVs,
 * M7 adds heading/Y-sort/team color. */
void              Units_Render(const struct GameWorld *world, TAK_Platform *plat);

/* Debug spawn for M4: drop a monarch of the given side ("ARA" / "VER"
 * / "TAR" / "ZON") at the given world coords. Returns 0 on success,
 * -1 if no monarch def is registered for that side or the active
 * array is full. Triggered by hotkey '3' in ingame.c. */
int               Units_DebugSpawnMonarch(const char *side, int32_t world_x, int32_t world_y);

/* Debug stress test (M8): clear the active array, then spawn up to N
 * monarchs of `side` in a ceil(√N) × ceil(√N) grid centered at
 * (cx, cy) with `pitch` world-pixels between rows/cols. Each spawned
 * unit is owned by player_id with team color color_idx. Stops early
 * if Units_Spawn returns -1 (array full). Returns the count actually
 * spawned. Triggered by hotkeys '4' (500) and '5' (2000) in ingame.c. */
int               Units_DebugSpawnGrid(int n, const char *side,
                                        int player_id, int color_idx,
                                        int32_t cx, int32_t cy, int pitch);

/* ── Projection tuning (debug keys, R3) ─────────────────────────── */

/* TA_SCALE: model-space TA units → world-space pixels.
 * TAN_TILT: tan(camera tilt). screen_y = world_z - cam_y - world_y * TAN_TILT.
 *
 * These are file-globals in units.c. Get/Set so ingame.c's '-/=' and
 * '[/]' hotkeys can tune them in real time. R3 (PHASE_C_3DO.md §5.R3)
 * gets answered by iterating these until the monarch looks right. */
float             Units_GetTAScale(void);
void              Units_SetTAScale(float s);
float             Units_GetTanTilt(void);
void              Units_SetTanTilt(float t);

/* Screen-space AABB of one live unit through the submit-path
 * transform. Test hook for model proportions. 0 on success. */
int               Units_DebugProjectedBounds(int handle,
                                             const struct GameWorld *world,
                                             float out_min[2], float out_max[2]);

/* Owning node and height key of each triangle in submit order for one
 * live unit (out_keys may be NULL). Test hook for piece layering.
 * Returns the triangle count. */
int               Units_DebugSubmitOrder(int handle,
                                         const struct GameWorld *world,
                                         uint16_t *out_nodes, float *out_keys,
                                         int max_tris);

#endif /* TAK_UNIT_H */
