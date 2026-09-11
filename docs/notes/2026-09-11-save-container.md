# The .oksave container

Line numbers of the form `legacy:NNNN` are anchors into the legacy
reference, as in every note here.

A saved game is a snapshot, not a replay. It writes the simulation's
current values and restores them exactly, which works today with the
engine's floats in place. The container described here knows nothing
about units or maps. It carries bytes, and the state serialisers hand
it finished payloads and ask for them back by four character code.

## The file

`<slug>.oksave`, written as `<slug>.oksave.tmp` and then moved over the
real name, so a crash mid write leaves the previous save intact. The
digest is left to catch media damage rather than interrupted writes.

Every integer is little endian and every field is written at an
explicit width. No struct is ever handed to a write call. The shipping
Windows build is 32 bit and the browser build is wasm32 while macOS and
Linux are 64 bit, so the same struct has two sizes and two padding
layouts. `include/tak_bytes.h` holds the shift and store helpers, and
the command serialiser in src/net/commands.c uses the same set.

Floats travel as their IEEE-754 binary32 bit pattern, moved with
memcpy. A pointer cast breaks strict aliasing and a union is not
guaranteed to move the representation rather than a value the compiler
kept in a wider register. No CMake file sets a fast math flag, so this
round trips bit for bit.

## Header, 240 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u8[8] | magic, `4F 4B 53 41 56 45 1A 0A` |
| 8 | u32 | container_version, 1. A reader refuses anything higher |
| 12 | u32 | header_bytes, 240 |
| 16 | u64 | file_bytes. The first truncation check |
| 24 | u32 | schema_version |
| 28 | u32 | flags. bit 0 some section is deflated, bit 1 campaign |
| 32 | u8[32] | SHA-256 over the whole file with these 32 bytes read as zero |
| 64 | u32 | section_count |
| 68 | u32 | sim_tick |
| 72 | u32 | sim_state_hash, `TAK_SimHash()` before the write |
| 76 | u32 | rng_ai |
| 80 | u8 | determinism_class. 0 float simulation, 1 fixed point |
| 81 | u8 | tick_hz, 60 |
| 82 | u8 | save_kind. 1 skirmish, 2 campaign in battle, 3 between missions |
| 83 | u8 | float_form. 0 binary32 bit patterns, 1 fixed |
| 84 | u32 | unit_stable_id_next |
| 88 | u32 | unit_slot_count, dead slots included |
| 92 | u64 | saved_at_utc |
| 100 | char[64] | engine_build, `<version>+<git describe>` |
| 164 | u8[32] | map_fingerprint, all zero when unavailable |
| 196 | u8[44] | reserved, zero |

The build stamp is recorded and never compared. A save from a different
build of the same schema loads, because refusing every save on every
release is worse for the player than the small risk the state hash
already catches. The original recorded a build date and a build time as
key names with zero values and never compared those either
(legacy:164927-164930), but it also carried no real schema version, so
a save from any build was accepted whatever had changed.

## Section header, 16 bytes

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | id, a four character code, in file order |
| 4 | u16 | version |
| 6 | u16 | flags. bit 0 required, bit 1 deflated, bit 2 record array |
| 8 | u32 | stored_bytes |
| 12 | u32 | plain_bytes, equal to stored_bytes when not deflated |

## The walk, in order

1. A section header or payload that would run past `file_bytes` means
   the file is truncated. Refuse, naming the code.
2. An unknown id with the required bit clear. Step over
   `stored_bytes` and carry on, so a newer writer's optional section is
   invisible to an older reader.
3. An unknown id with the required bit set. Refuse and name the code.
4. A known id at a higher version than this reader handles. Rules 2 and
   3 apply identically.
5. A known id at a lower version. Either an explicit upgrade path
   exists or the reader refuses, naming the id and both versions.
6. A record array carries `u32 count` then `u16 fixed_bytes`. Matching
   width reads normally. A smaller stored width reads the prefix and
   zeroes the tail. A larger one reads our prefix and steps over the
   rest.

Rule 6 is the original's own tolerance. It accepted a unit record of
0xeb or 0xe9 bytes and zeroed the trailing field on the short form
(legacy:227605-227609). That tolerance is the half that let old saves
keep loading, and a version key on its own would invalidate every save
on every field addition.

The obligation it puts on us: within one section version, fields are
append only and zero must be a safe default for every appended field. A
field whose zero is not safe forces a version bump.

## Compression

Per section deflate through the vendored miniz, at level 1. Measured
against a 359 KB state blob, level 1 gives 131694 bytes in 3.55 ms
where level 9 gives 119361 in 41.23 ms, so level 1 keeps 94 percent of
the benefit for 9 percent of the cost. Inflate is 0.80 ms and the
digest 1.18 ms over the same blob.

A section that does not shrink by a useful margin is stored plain, and
`SUMM` is never deflated whatever it costs, so the load list reads one
short header per file and inflates nothing.

The original's own bank compression is real but is not why we compress.
Nobody but us has to read these files, so the bank framing buys us
nothing. We also do not reuse the `.sav` extension: it belonged to a
debug console command (legacy:36857-36868) and would collide with the
original's own files in a shared folder.

## Refusals

Every refusal names what is wrong in the player's terms: not a saved
game, written by a newer version of the game, incomplete, damaged, or
carrying a section this build does not know. The original had a version
mismatch on its unit section return silently, leaving the player on the
right map with an empty world and no message (legacy:227596-227597).

## Portability, stated plainly

A save is portable across Windows, macOS, Linux and the browser today,
in both directions, because restoring exact bit patterns leaves no
second machine to disagree. What is not portable is a save across a
determinism class. When the fixed point conversion lands,
`determinism_class` becomes 1, `schema_version` bumps, and a float
class save is refused by name rather than converted. That is a one time
break enforced by a field rather than by hope.
