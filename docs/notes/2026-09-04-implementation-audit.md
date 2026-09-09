# TAK-RE implementation audit (2026-09-04)

Snapshot at commit 9308747 (which committed the previously-uncommitted gameplay
layer). Full module inventory in the agent transcript. This file records the
actionable findings.

This audit is about **our own** source tree. Citations of the form `file.c:NNN`
are TAK-RE line numbers at that commit and have moved since. A bare `:NNNNN` is
an evidence pointer into a private reference that is not distributed.

## Matrix over-claims (reality weaker)

1. Sound events are completely unwired. GameSound_UnitAction/WeaponHit/PlayUI have zero call
   sites. Only music plays, native only (browser music dead: music.c uses raw fopen on
   /game/Music which holds only HPIs). Smoke §1 "menu click sounds" cannot pass anywhere.
2. "node smoke" browser validation does not exist. There is no node script in the repo. The
   build artifact is real, dated Jul 13.
3. AI difficulty is cosmetic. TAK_AI_PursuitRadius only affects auto-target scan radius
   (units.c:3671). TAK_AI_TickSkirmish never reads ai_difficulty (hardcoded %60 cadence).
4. No group selection. Units_SelectSingle hard-sets g_selection_count=1, and there are no
   marquee/shift/control groups. Blocks real play and blocks smoke §3/§6.
5. The multiplayer menu is a dead-end screen. ui/multiplayer.c has empty mp_routes[].

## Matrix under-claims (reality stronger)

6. XP accumulation implemented (kill_xp_value → experience_pts, units.c:452, cite :162918).
   Only rank thresholds + port 32 missing (units.c:2723 returns 0).
7. COB EMIT_SFX/PLAY_SOUND/EXPLODE opcodes decoded in VM. Only host callbacks missing.
8. src/net has a real versioned wire format + byte-exact test (missing: transport, lockstep,
   sync hash, lobby).
9. max_water_depth IS enforced via move class (units.c:3034). Only maxwaterslope variant
   missing (parsed at units.c:1478-1480 in this snapshot, never read).

## Confirmed absent

Save/load (zero symbols) · corpses (zero matches) · water rendering · wall-drag · HUD queue
badges · cloaking behavior (UNIT_CAP_CLOAK only picks a HUD label, hud.c:528) · AI expansion/
scouting/defense · fog los_mode enum · TAK_SERVER CMake option is a no-op knob (declared
CMakeLists.txt:44, referenced nowhere).

## Architectural notes

- units.c is 5,592 lines, with sim + render not separated. It is the de facto simulation
  core.
- AI calls Units_Command* directly, which is NOT on the command path lockstep needs.
  Reroute AI through TAK_GameCommand before multiplayer.
- TODO hotspots: options.c:179 (controls unwired), battle_setup.c:809 (Load Game),
  units.c:1034 (build spot should come from COB QueryBuildInfo piece), units.c:1372
  (buildangle unhonored), minimap.c:191, hpi_decompress.c:61.
- src/CMakeLists.txt:179: battle_setup + features must move to VFS_ListFiles before browser
  bundle can drop the 356MB loose tree (bundle currently 644MB = HPIs + loose tree both).

## Browser build facts

Only 3 __EMSCRIPTEN__ ifdefs (all main.c). Everything else is gated by CMake/feature macros.
No Bink (no ffmpeg under emcc → stubs). No music (fopen path). Tests/tools excluded (early
return() src/CMakeLists.txt:209). 1GB fixed heap, GROWABLE_ARRAYBUFFERS=0 mandatory (Chrome
rejects texture uploads from resizable heap). EXIT_RUNTIME=0 → shutdown block never runs in
browser (leak checks native-only). wasm_pre.js has a stale comment (claims no HPIs shipped,
when the opposite is true) + redundant FS.mkdir('/game'). Browser build requires GOG install
at configure time (file(GLOB) over TAK_GAME_DIR).

## Test suite

34 ctest entries green (see agent transcript for full table). Not registered: test_bink +
7 probes. Coverage gaps matching matrix : no focused tests for weapon reload/burst/spray,
patrol, guard, damage-category multipliers, splash-edge falloff, cancellation refunds,
transport capacity.
