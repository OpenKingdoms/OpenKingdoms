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
/* The plate as Chromium renders it: 419x32 on one line, and wrapped to
   its room's width below that, 384x50 on a 640 wide page and 227x50 on a
   390 wide one. A room narrower still may take a third line. */
const PLATE_W = 419, PLATE_H = 32, PLATE_WRAPPED_H = 50, PLATE_THREE_H = 68;
function plates(roomOnPage) {
  if (roomOnPage.width >= PLATE_W) return [{ width: PLATE_W, height: PLATE_H }];
  const w = Math.floor(roomOnPage.width);
  const out = [{ width: w, height: PLATE_WRAPPED_H }];
  if (w < 227) out.push({ width: w, height: PLATE_THREE_H });
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
check(helpers.plateSpot(big, { width: PLATE_W, height: PLATE_H }) !== null, 'no plate on the 1280x720 menu');
check(helpers.plateSpot(big, { width: big.width + 1, height: 32 }) === null, 'a plate wider than its room placed');
check(helpers.plateSpot(big, { width: 100, height: big.height + 1 }) === null, 'a plate taller than its room placed');
check(helpers.plateRoomOnPage('menu', room, { left: 0, top: 0, width: 0, height: 0 }, gap) === null,
      'a room on a canvas with no box');

/* The saved games panel opens over a backdrop that covers the page, so
   no menu control can be reached, or covered by a panel that looks like
   part of the menu, until it is closed. */
function cssRule(sel) {
  const m = shell.match(new RegExp('\\n  ' + sel.replace(/[.#]/g, '\\$&') + ' \\{([^}]*)\\}'));
  return m ? m[1] : '';
}
function px(rule, prop) {
  const m = rule.match(new RegExp('(?:^|[;\\s])' + prop + ':\\s*([^;]+);'));
  return m ? m[1].trim() : '';
}
const zOf = rule => Number(px(rule, 'z-index'));
const backdrop = cssRule('#saves'), panelBox = cssRule('#saves .box');
check(backdrop !== '' && panelBox !== '', 'the page styles the saved games backdrop and its box');
check(px(backdrop, 'position') === 'fixed' && px(backdrop, 'inset') === '0',
      'the saved games backdrop does not cover the whole page');
check(/rgba\(0,\s*0,\s*0,\s*\.\d+\)/.test(px(backdrop, 'background')), 'the saved games backdrop does not dim the menu');
check(zOf(backdrop) > zOf(cssRule('#forget')), 'the saved games backdrop is under the plate');
check(zOf(cssRule('#toast')) > zOf(backdrop), 'a warning lands under the saved games backdrop');
check(px(panelBox, 'box-sizing') === 'border-box' && px(panelBox, 'overflow') === 'auto',
      'the saved games box does not scroll inside its own size');
/* The box's size, read from its CSS: min(Apx, calc(100vw - Bpx)). */
function capOf(value, unit) {
  const m = value.match(new RegExp('min\\((\\d+)px,\\s*calc\\(100' + unit + ' - (\\d+)px\\)\\)'));
  return m ? { max: +m[1], margin: +m[2] } : null;
}
const wCap = capOf(px(panelBox, 'width'), 'vw'), hCap = capOf(px(panelBox, 'max-height'), 'vh');
check(wCap && hCap, 'the saved games box is not sized to the page');
check(/savesPanel\.addEventListener\('click', function \(e\) \{\s*if \(e\.target === savesPanel\) closeSaves\(\);/.test(shell),
      'a click on the backdrop does not close the panel');
check(/savesPanel\.addEventListener\('keydown', function \(e\) \{\s*e\.stopPropagation\(\);/.test(shell),
      'a key pressed in the panel reaches the menu');
/* The box's height with n rows before its cap: title, note, buttons, and
   a row per file, two lines each on a narrow page. */
function contentHeight(n, width) {
  const row = width < 360 ? 46 : 27;
  return 28 + (width < 360 ? 90 : 60) + 44 + Math.max(1, n) * row + 20;
}
if (wCap && hCap) {
  for (const [w, h] of [[1280, 600], [1280, 720], [1920, 1080], [800, 600], [640, 360], [390, 844]]) {
    for (const box of boxes(w, h)) {
      const kx = box.width / 640, ky = box.height / 480;
      const onPage = c => ({ left: box.left + c.x * kx, top: box.top + c.y * ky,
                             right: box.left + (c.x + c.w) * kx, bottom: box.top + (c.y + c.h) * ky });
      for (const n of [0, 3, 12]) {
        const at = w + 'x' + h + ' ' + box.name + ' with ' + n + ' saves: ';
        const bw = Math.min(wCap.max, w - wCap.margin);
        const bh = Math.min(contentHeight(n, bw), hCap.max, h - hCap.margin);
        const b = { left: (w - bw) / 2, top: (h - bh) / 2, right: (w + bw) / 2, bottom: (h + bh) / 2 };
        check(b.left >= 0 && b.top >= 0 && b.right <= w && b.bottom <= h, at + 'the box is off the page');
        check(bw >= Math.min(300, w - 16), at + 'the box is too narrow to read');
        for (const c of controls) {
          const r = onPage(c);
          /* Every control is under the backdrop, which takes its clicks. */
          check(r.left >= 0 && r.top >= 0 && r.right <= w + 1e-9 && r.bottom <= h + 1e-9,
                at + 'a control past the backdrop');
        }
      }
    }
  }
}

/* One flag says the engine has the files: the plate waits for it. */
check(!/var playing\b/.test(shell), 'a second flag for a started game');
check((shell.match(/var started\b/g) || []).length === 1, 'the started flag is declared once');
function plateWith(started) {
  const forget = { hidden: true, style: {}, getBoundingClientRect: () => ({ width: PLATE_W, height: PLATE_H }) };
  const panel = { hidden: false }, ask = { hidden: false };
  const place = new Function('started', 'screenNow', 'screenRoom', 'canvas', 'forget', 'savesPanel',
    'forgetAsk', 'PLATE_GAP', 'plateRoomOnPage', 'plateSpot',
    functionText(shell, 'placePlate') + '\nreturn placePlate;')(
    started, 'menu', room, { getBoundingClientRect: () => ({ left: 0, top: 0, width: 1280, height: 720 }) },
    forget, panel, ask, gap, helpers.plateRoomOnPage, helpers.plateSpot);
  place();
  return { plate: !forget.hidden, panel: !panel.hidden };
}
const before = plateWith(false), after = plateWith(true);
check(!before.plate && !before.panel, 'the plate showed before the game started');
check(after.plate && after.panel, 'the plate did not show once started, or it closed an open panel');
const startText = functionText(shell, 'start');
check(/started = true;/.test(startText) && /placePlate\(\);/.test(startText) &&
      startText.indexOf('started = true;') < startText.indexOf('placePlate();'),
      'start sets the flag before it places the plate');

console.log('ran ' + checks + ' checks');
if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('the plate stays in the menu\'s free room and off every other screen, and its panel keeps the menu out of reach');
