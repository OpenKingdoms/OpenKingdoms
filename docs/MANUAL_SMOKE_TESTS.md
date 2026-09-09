# TAK-RE Manual Smoke Tests

Run these after any engine change. Native: `build\src\Debug\tak-re.exe`.
Browser: `python -m http.server 8080 --directory build-wasm\src` →
http://localhost:8080/tak-re.html (hard-reload with Ctrl+Shift+R after rebuilds).

## 1. Boot & menus (2 min)
- [ ] Main menu renders: background art, character sprites (mage/girl/knight), buttons respond
- [ ] Native only: menu character Bink videos play (browser shows static sprites — expected)
- [ ] Options screen opens and returns without visual garbage
- [ ] Audio: menu click sounds play (browser: click once first — autoplay policy)

## 2. Skirmish setup (2 min)
- [ ] Map list populates; picking maps updates the preview
- [ ] Change your faction; set opponent slot to AI; hit Play
- [ ] Loading bar advances through phases and lands in-game without a stall

## 3. Rendering parity (5 min) — start a skirmish on "Two Castles"
- [ ] Your monarch renders fully (body, head, cape, weapon)
- [ ] **Cape test**: order the monarch to walk N, S, E, W and diagonally.
      Watch the cape each direction — it should swing but never slice through
      the torso. (East/west walks were the worst case before the depth-key fix.)
- [ ] Zoom around your castle: walls, keep, gate all render complete — no
      missing walls or floating roofs
- [ ] Select a unit standing in the right third of the screen — confirm it's
      just *behind the HUD sidebar*, not invisible (drag the camera to check)
- [ ] Fog: unexplored = dark, explored = dim, visible = bright; no grid-line
      artifacts crawling across explored terrain

## 4. Economy & construction (5 min)
- [ ] Select monarch → build menu shows buildables with icons
- [ ] Place a lodestone on a mana spot: green ghost = valid, red = blocked
- [ ] Monarch walks over, construction sparkles, building fades in as HP fills
- [ ] Mana counter: watch +N regen tick up after the lodestone completes
- [ ] Try to place a building on a wall/cliff — red ghost, refuses to place

## 5. Factory production (5 min)
- [ ] Build a keep/castle (production structure). When done, select it
- [ ] Click a unit icon 3× → three queued; units emerge ONE at a time
- [ ] **Rally**: with the factory selected, click Move on open ground —
      subsequently produced units walk to that spot on completion
- [ ] Cancel: while a unit is mid-production, cancel it — nanoframe vanishes,
      next queued unit starts automatically

## 6. Movement & combat (5 min)
- [ ] Order a group across the map: they accelerate (not instant top speed),
      arc through turns, brake at the destination instead of stopping dead
- [ ] Units path AROUND walls/rocks/water — never through them; a land unit
      ordered across a lake walks the shoreline
- [ ] Attack an enemy: melee units close to touch range before swinging;
      archers stop at range and loose arrows (projectiles arc, not hitscan)
- [ ] Units in fog: enemy units vanish when your LOS retreats; no floating
      health bars or projectiles from fogged areas

## 7. AI opponent (10 min — just let it run)
- [ ] AI builds a lodestone early, then a production structure, then troops
- [ ] AI troops eventually march to your base and engage
- [ ] Kill the AI monarch (if not expendable) → Victory screen

## 8. Browser-specific (after any wasm rebuild)
- [ ] Loads without console errors (F12) beyond the known audio-autoplay warning
- [ ] Full skirmish (setup → play ≥10 min → victory/defeat) without a tab crash
- [ ] Task Manager: browser tab memory stays roughly flat during play
      (~1.5–1.8 GB total is normal: 644 MB assets + 1 GB heap)

## Known gaps (don't file these)
- Menu videos absent in browser (Bink needs FFmpeg — desktop only)
- Music absent in browser (Music/ folder not bundled)
- HUD queue-count badges / Shift+5 / Ctrl-continuous clicks not wired yet
- maxwaterslope (underwater slope limit) not yet enforced
