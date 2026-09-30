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

console.log('ran ' + checks + ' checks');
if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('the plate stays in the menu\'s free room and off every other screen');
