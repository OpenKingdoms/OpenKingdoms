/* Where web/shell.html puts its "Saved games . Forget my game files"
 * plate. It reads plateRoomOnPage and plateSpot out of the page and the
 * menu's control rects out of src/render/main_menu.c, then holds the
 * plate to the menu's free room at many page sizes and to no place at
 * all on any other screen. No browser and no game data needed.
 *
 *   node scripts/test_shell_plate.js
 */
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.dirname(__dirname);
const shell = fs.readFileSync(path.join(ROOT, 'web', 'shell.html'), 'utf8');
const menu = fs.readFileSync(path.join(ROOT, 'src', 'render', 'main_menu.c'), 'utf8');

function functionText(text, name) {
  const start = text.indexOf('function ' + name + '(');
  if (start < 0) throw new Error('no function ' + name + ' in the page');
  let depth = 0;
  for (let j = text.indexOf('{', start); j < text.length; j++) {
    if (text[j] === '{') depth++;
    else if (text[j] === '}' && --depth === 0) return text.slice(start, j + 1);
  }
  throw new Error('no end to ' + name);
}
const helpers = new Function(
  functionText(shell, 'plateRoomOnPage') + '\n' + functionText(shell, 'plateSpot') +
  '\nreturn { plateRoomOnPage: plateRoomOnPage, plateSpot: plateSpot };')();
const gap = Number((shell.match(/var PLATE_GAP = (\d+);/) || [])[1]);

function rectsIn(src) {
  return [...src.matchAll(/\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}/g)]
    .map(m => ({ x: +m[1], y: +m[2], w: +m[3], h: +m[4] }));
}
function block(name) {
  const m = menu.match(new RegExp(name + '\\[[^\\]]*\\] = \\{([\\s\\S]*?)\\n\\};'));
  if (!m) throw new Error('no ' + name + ' in main_menu.c');
  return rectsIn(m[1]);
}
function one(name) {
  const m = menu.match(new RegExp('SDL_Rect ' + name + ' = (\\{[^}]*\\});'));
  if (!m) throw new Error('no ' + name + ' in main_menu.c');
  return rectsIn(m[1])[0];
}
const room = one('plate_room');
const help = one('helptext_rect');
const controls = block('button_rects').concat(block('character_hit_rects'), [
  help, { x: 0, y: help.y - 28, w: 640, h: 28 }]);

let checks = 0, failed = 0;
function check(ok, what) {
  checks++;
  if (!ok) { failed++; console.log('FAIL  ' + what); }
}
function meets(a, b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* The canvas box on the page: Original fills it, Fit is 16:9 inside it. */
function boxes(w, h) {
  const fw = Math.min(w, h * 16 / 9), fh = fw * 9 / 16;
  return [
    { name: 'original', left: 0, top: 0, width: w, height: h },
    { name: 'fit', left: (w - fw) / 2, top: (h - fh) / 2, width: fw, height: fh },
  ];
}
/* A plate on one line, and the same plate wrapped into its room. */
function plates(roomOnPage) {
  const out = [{ width: 392, height: 32 }];
  if (roomOnPage.width < 392) out.push({ width: roomOnPage.width, height: 32 * Math.ceil(392 / roomOnPage.width) });
  return out;
}

check(gap > 0, 'the page names a gap for the plate');
check(!controls.some(c => meets(room, c)), 'the room in main_menu.c holds no control');

const sizes = [[1280, 720], [1280, 600], [1920, 1080], [1366, 768], [1024, 768],
  [800, 600], [640, 360], [480, 270], [390, 844], [2560, 1440], [3840, 2160], [1280, 1024]];
const others = ['Skirmish Lobby', 'In Game', 'Loading', 'Options', 'Credits',
  'Campaign', 'Multiplayer', 'Select Game', 'Exiting', '', undefined];
let shown = 0;
for (const [w, h] of sizes) {
  for (const box of boxes(w, h)) {
    const at = w + 'x' + h + ' ' + box.name;
    for (const s of others) {
      check(helpers.plateRoomOnPage(s, room, box, gap) === null, at + ': a room on the ' + s + ' screen');
    }
    check(helpers.plateRoomOnPage('menu', null, box, gap) === null, at + ': a room when the engine gave none');
    const r = helpers.plateRoomOnPage('menu', room, box, gap);
    if (!r) continue;
    for (const p of plates(r)) {
      const spot = helpers.plateSpot(r, p);
      if (!spot) continue;
      shown++;
      /* Back into menu units, where the controls are. */
      const kx = box.width / 640, ky = box.height / 480;
      const m = { x: (spot.left - box.left) / kx, y: (spot.top - box.top) / ky, w: p.width / kx, h: p.height / ky };
      const tag = at + ' plate ' + p.width + 'x' + p.height;
      check(m.x >= room.x && m.y >= room.y && m.x + m.w <= room.x + room.w + 1e-9 &&
            m.y + m.h <= room.y + room.h + 1e-9, tag + ': outside its room');
      check(!controls.some(c => meets(m, c)), tag + ': over a menu control');
      check(spot.left >= 0 && spot.top >= 0 && spot.left + p.width <= w && spot.top + p.height <= h,
            tag + ': off the page');
    }
  }
}
check(shown > 0, 'the plate showed at some size');
const big = helpers.plateRoomOnPage('menu', room, { left: 0, top: 0, width: 1280, height: 720 }, gap);
check(helpers.plateSpot(big, { width: 392, height: 32 }) !== null, 'no plate on the 1280x720 menu');
check(helpers.plateSpot(big, { width: big.width + 1, height: 32 }) === null, 'a plate wider than its room placed');
check(helpers.plateSpot(big, { width: 100, height: big.height + 1 }) === null, 'a plate taller than its room placed');
check(helpers.plateRoomOnPage('menu', room, { left: 0, top: 0, width: 0, height: 0 }, gap) === null,
      'a room on a canvas with no box');

/* The saved games panel hangs under the plate at every size it shows. */
const savesSpot = new Function(
  (shell.match(/var SAVES_MARGIN[^\n]*\n/) || [''])[0] + functionText(shell, 'savesSpot') +
  '\nreturn savesSpot;')();
const panelWidth = Number((shell.match(/savesSpot\(forget\.getBoundingClientRect\(\), (\d+), page\)/) || [])[1]);
check(panelWidth > 0, 'the page names the saved games panel\'s width');
const savesRule = (shell.match(/\n  #saves \{([^}]*)\}/) || [])[1] || '';
check(savesRule && !/\b(bottom|right)\s*:/.test(savesRule), 'the saved games panel is still pinned to a corner');
check(/box-sizing:\s*border-box/.test(savesRule), 'the saved games panel\'s width leaves out its padding');
let hung = 0;
for (const [w, h] of sizes.concat([[360, 640], [320, 568], [600, 800]])) {
  for (const box of boxes(w, h)) {
    const r = helpers.plateRoomOnPage('menu', room, box, gap);
    if (!r) continue;
    for (const p of plates(r)) {
      const spot = helpers.plateSpot(r, p);
      if (!spot) continue;
      const plate = { left: spot.left, top: spot.top, right: spot.left + p.width, bottom: spot.top + p.height };
      const s = savesSpot(plate, panelWidth, { width: w, height: h });
      const tag = w + 'x' + h + ' ' + box.name + ' plate ' + p.width + 'x' + p.height + ' panel';
      hung++;
      check(s.left >= 0 && s.top >= 0 && s.left + s.width <= w && s.top + s.maxHeight <= h, tag + ': off the page');
      check(s.width >= Math.min(panelWidth, w - 16) - 1e-9, tag + ': narrower than the page allows');
      check(s.maxHeight >= Math.min(160, h - 16), tag + ': too short to show a row');
      check(s.left < plate.right && s.left + s.width > plate.left, tag + ': not beside its link');
      if (h - plate.bottom >= 160 + 16) check(s.top >= plate.bottom, tag + ': over its link');
      if (w - 8 >= plate.right) check(Math.abs(s.left + s.width - plate.right) < 1e-9 || s.left === 8,
                                       tag + ': not flush with its link');
    }
  }
}
check(hung > 0, 'the panel hung at some size');
for (const [w, h] of [[1280, 600], [1280, 720], [1920, 1080]]) {
  const box = { left: 0, top: 0, width: w, height: h };
  const r = helpers.plateRoomOnPage('menu', room, box, gap);
  const spot = helpers.plateSpot(r, { width: 392, height: 32 });
  const plate = { left: spot.left, top: spot.top, right: spot.left + 392, bottom: spot.top + 32 };
  const s = savesSpot(plate, panelWidth, { width: w, height: h });
  const at = w + 'x' + h + ': ';
  check(s.top - plate.bottom >= 0 && s.top - plate.bottom <= 8, at + 'the panel is not right under its link');
  check(s.left + s.width === plate.right, at + 'the panel is not flush with its link');
  check(s.top + s.maxHeight <= h - 8, at + 'the panel runs off the bottom');
  check(s.width === panelWidth, at + 'the panel is cut narrower than it needs');
}

/* One flag says the engine has the files: the plate waits for it. */
check(!/var playing\b/.test(shell), 'a second flag for a started game');
check((shell.match(/var started\b/g) || []).length === 1, 'the started flag is declared once');
function plateWith(started) {
  const forget = { hidden: true, style: {}, getBoundingClientRect: () => ({ width: 392, height: 32 }) };
  const panel = { hidden: false }, ask = { hidden: false };
  const place = new Function('started', 'screenNow', 'screenRoom', 'canvas', 'forget', 'savesPanel',
    'forgetAsk', 'PLATE_GAP', 'plateRoomOnPage', 'plateSpot', 'placeSaves',
    functionText(shell, 'placePlate') + '\nreturn placePlate;')(
    started, 'menu', room, { getBoundingClientRect: () => ({ left: 0, top: 0, width: 1280, height: 720 }) },
    forget, panel, ask, gap, helpers.plateRoomOnPage, helpers.plateSpot, () => { panel.placed = true; });
  place();
  return { plate: !forget.hidden, panel: !panel.hidden, placed: !!panel.placed };
}
const before = plateWith(false), after = plateWith(true);
check(!before.plate && !before.panel, 'the plate showed before the game started');
check(after.plate && after.placed, 'the plate did not show, or its open panel did not follow it, once started');
const startText = functionText(shell, 'start');
check(/started = true;/.test(startText) && /placePlate\(\);/.test(startText) &&
      startText.indexOf('started = true;') < startText.indexOf('placePlate();'),
      'start sets the flag before it places the plate');

console.log('ran ' + checks + ' checks');
if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('the plate stays in the menu\'s free room and off every other screen, and its panel hangs under it');
