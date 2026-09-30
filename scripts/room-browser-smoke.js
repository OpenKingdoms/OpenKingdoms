/* The whole room, two browsers, one relay: names, chat, a rule, a
 * colour, a team, the map, ready, and Play into the battle.
 *
 *   okrelay --port 8811 &
 *   python -m http.server 8082 -d <wasm build>/src
 *   node scripts/room-browser-smoke.js <repo root> [url] [relay]
 *
 * The joiner finds the host's room by this run's name in the room list
 * frames its own socket receives, so a slow remote relay or a room left
 * over from an earlier run cannot send it to the wrong row.
 */
const fs = require('fs');
const path = require('path');
const root = process.argv[2] || '.';
const { chromium } = require(path.join(root, 'node_modules', 'playwright'));
const URL_BASE = process.argv[3] || 'http://localhost:8082/tak-re.html';
const RELAY = process.argv[4] || 'ws://127.0.0.1:8811/relay';
const GAME_DIR = 'C:/GOG Games/Total Annihilation Kingdoms';
const OUT = path.join(root, 'room-full-shots');
/* A name no earlier run used, so its room is told apart by name. */
const HOST_NAME = 'Zach' + Date.now().toString(36).slice(-4);
const ROOM_NAME = HOST_NAME + "'s game";
const LIST_TIMEOUT = 90000;
/* Rows the list shows before it scrolls, at the least. */
const ROWS_SHOWN = 8;
const MSG_WELCOME = 2, MSG_ROOM_LIST = 17, MSG_ROOM_STATE = 22;   /* include/tak_net_protocol.h */

/* Virtual 640x480 rects from the shipped .gui files. */
const R = {
  Name:      { x: 69, y: 57, w: 194, h: 21 },
  HostGame:  { x: 467, y: 406, w: 57, h: 57 },
  Update:    { x: 193, y: 369, w: 52, h: 40 },
  Join:      { x: 541, y: 407, w: 39, h: 54 },
  Row0:      { x: 64, y: 126, w: 311, h: 22 },
  Play:      { x: 544, y: 407, w: 39, h: 51 },
  Ready0:    { x: 70, y: 64, w: 13, h: 13 },
  Ready1:    { x: 70, y: 86, w: 13, h: 13 },
  Color0:    { x: 296, y: 62, w: 17, h: 17 },
  Team0:     { x: 322, y: 61, w: 50, h: 20 },
  Team1:     { x: 322, y: 83, w: 50, h: 20 },
  LoS:       { x: 558, y: 87, w: 13, h: 13 },
  Map:       { x: 113, y: 406, w: 58, h: 56 },
  MapRow1:   { x: 130, y: 94 + 19, w: 204, h: 19 },
  MapOK:     { x: 482, y: 385, w: 37, h: 52 },
  UnitsInc:  { x: 532, y: 219, w: 12, h: 12 },
};

let failures = 0;
function check(what, ok) { console.log((ok ? '  ok   ' : '  FAIL ') + what); if (!ok) failures++; }
function archives(dir) {
  return fs.readdirSync(dir).filter(f => /\.(hpi|ufo|ccx|gpf|gp3)$/i.test(f)).map(f => path.join(dir, f));
}

/* Relay frames: type u8, payload length u16 LE, payload. */
function eachFrame(buf, fn) {
  for (let o = 0; o + 3 <= buf.length;) {
    const len = buf.readUInt16LE(o + 1);
    if (o + 3 + len > buf.length) break;
    fn(buf[o], buf.subarray(o + 3, o + 3 + len));
    o += 3 + len;
  }
}
function cstr(b, at, n) {
  const f = b.subarray(at, at + n);
  const z = f.indexOf(0);
  return f.subarray(0, z < 0 ? n : z).toString('utf8');
}
/* A room list is flags, count, then fixed-width summaries that open
   with room id u32, code[7], name[32], host name[16]. */
function decodeRoomList(p) {
  const count = p[1];
  if (!count) return [];
  const stride = (p.length - 2) / count;
  if (!Number.isInteger(stride) || stride < 59) return null;
  const rooms = [];
  for (let i = 0; i < count; i++) {
    const s = p.subarray(2 + i * stride, 2 + (i + 1) * stride);
    rooms.push({ id: s.readUInt32LE(0), code: cstr(s, 4, 7), name: cstr(s, 11, 32), host: cstr(s, 43, 16) });
  }
  return rooms;
}

async function clickVirtual(page, key, hold) {
  const r = typeof key === 'string' ? R[key] : key;
  const pt = await page.evaluate((rect) => {
    const c = document.getElementById('canvas');
    const box = c.getBoundingClientRect();
    return { x: box.left + (rect.x + rect.w / 2) * (c.width / 640) * (box.width / c.width),
             y: box.top + (rect.y + rect.h / 2) * (c.height / 480) * (box.height / c.height) };
  }, r);
  await page.bringToFront();
  await page.mouse.move(pt.x, pt.y);
  await page.waitForTimeout(150);
  await page.mouse.down();
  await page.waitForTimeout(hold || 60);   /* a person's click, not a held one */
  await page.mouse.up();
  await page.waitForTimeout(900);
}

async function boot(browser, label, state) {
  const page = await (await browser.newContext()).newPage();
  state.log[label] = [];
  page.on('console', m => state.log[label].push(m.text()));
  page.on('pageerror', e => state.log[label].push('PAGEERROR ' + e.message));
  page.on('websocket', ws => ws.on('framereceived', f => {
    if (typeof f.payload === 'string') return;
    eachFrame(Buffer.from(f.payload), (type, p) => {
      if (type === MSG_WELCOME) {
        state.welcomes[label] = (state.welcomes[label] || 0) + 1;
        delete state.rooms[label];
      } else if (type === MSG_ROOM_LIST) {
        const rooms = decodeRoomList(p);
        if (rooms) state.rooms[label] = rooms;
      } else if (type === MSG_ROOM_STATE && p.length >= 4) {
        state.inRoom[label] = p.readUInt32LE(0);
      }
    });
  }));
  const args = encodeURIComponent('--multiplayer --relay ' + RELAY);
  await page.goto(URL_BASE + '?args=' + args, { waitUntil: 'load' });
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  await page.setInputFiles('#hpi-input', archives(GAME_DIR));
  await page.waitForSelector('#btn-start:visible', { timeout: 60000 });
  await page.click('#btn-start');
  await page.waitForFunction(() => document.getElementById('picker').hidden &&
    window.Module && window.Module.canvas && window.Module.canvas.width > 0, null, { timeout: 180000 });
  await page.waitForTimeout(6000);
  /* A toast can sit over the lobby's lower buttons. The forget plate
     shows only on the main menu. */
  await page.addStyleTag({ content: '#toast { display: none !important; }' });
  return page;
}

/* A new name reconnects to the relay, and a click made before the new
   welcome is lost, so this waits for it. */
async function typeName(page, label, state, name) {
  const before = state.welcomes[label] || 0;
  await clickVirtual(page, 'Name');
  await page.keyboard.type(name, { delay: 40 });
  await page.keyboard.press('Enter');
  const by = Date.now() + 30000;
  while ((state.welcomes[label] || 0) <= before && Date.now() < by) await page.waitForTimeout(250);
  if ((state.welcomes[label] || 0) <= before) console.log(label + ': no new welcome after the name, going on');
  await page.waitForTimeout(500);
}

/* Waits for the relay to list this run's room to the joiner, and
   answers its row. Rooms from earlier runs have other names and are
   passed over wherever they sit. */
async function findRoom(page, label, state) {
  const until = Date.now() + LIST_TIMEOUT;
  let nextUpdate = Date.now() + 10000;
  for (;;) {
    const rooms = state.rooms[label];
    const row = rooms ? rooms.findIndex(r => r.name.startsWith(ROOM_NAME)) : -1;
    if (row >= 0) return { row, room: rooms[row], rooms };
    if (Date.now() > until) {
      const seen = rooms ? JSON.stringify(rooms.map(r => r.name)) : 'no room list at all';
      throw new Error(label + ': the relay did not list "' + ROOM_NAME + '" within ' +
                      LIST_TIMEOUT / 1000 + ' s. Listed: ' + seen);
    }
    /* The relay pushes the list as it changes. Update asks again in
       case a push was missed. */
    if (Date.now() > nextUpdate) {
      await clickVirtual(page, 'Update');
      nextUpdate = Date.now() + 10000;
    }
    await page.waitForTimeout(500);
  }
}

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const state = { log: {}, rooms: {}, inRoom: {}, welcomes: {} };
  const shot = (p, n) => p.screenshot({ path: path.join(OUT, n) });

  const host = await boot(browser, 'host', state);
  console.log('host ' + HOST_NAME + ' hosts "' + ROOM_NAME + '"');
  await typeName(host, 'host', state, HOST_NAME);
  await clickVirtual(host, 'HostGame');
  await host.waitForTimeout(2500);

  const joiner = await boot(browser, 'join', state);
  await typeName(joiner, 'join', state, 'Bennett');
  const found = await findRoom(joiner, 'join', state);
  const stale = found.rooms.length - 1;
  console.log('joiner found "' + found.room.name + '" at row ' + found.row +
              (stale ? ', past ' + stale + ' other room(s) still listed' : ''));
  if (found.row >= ROWS_SHOWN)
    throw new Error('"' + ROOM_NAME + '" is at row ' + found.row + ', below the rows this script clicks. ' +
                    'Wait for the relay to drop the ' + stale + ' other room(s) and run it again.');
  await clickVirtual(joiner, Object.assign({}, R.Row0, { y: R.Row0.y + found.row * R.Row0.h }));
  await clickVirtual(joiner, 'Join');
  const joinBy = Date.now() + 15000;
  while (state.inRoom.join === undefined && Date.now() < joinBy) await joiner.waitForTimeout(250);
  if (state.inRoom.join !== found.room.id)
    throw new Error('the joiner did not enter "' + ROOM_NAME + '" (room ' + found.room.id + '): ' +
                    (state.inRoom.join === undefined ? 'no room state arrived'
                                                     : 'it entered room ' + state.inRoom.join));
  check('the joiner entered this run\'s room, ' + found.room.code, true);
  await joiner.waitForTimeout(2500);
  await shot(host, '1-room.png');

  console.log('chat: the joiner says hello, the host answers');
  await joiner.bringToFront();
  await joiner.keyboard.type('hello from bennett', { delay: 30 });
  await joiner.keyboard.press('Enter');
  await host.bringToFront();
  await host.keyboard.type('hi bennett', { delay: 30 });
  await host.keyboard.press('Enter');
  await host.waitForTimeout(1500);
  await shot(host, '2-chat-host.png');
  await shot(joiner, '3-chat-join.png');

  console.log('host: colour, team, a rule, the unit limit');
  await clickVirtual(host, 'Color0');
  await clickVirtual(host, 'Team0');
  await clickVirtual(host, 'LoS');
  await clickVirtual(host, 'UnitsInc');
  await clickVirtual(joiner, 'Team1');
  await clickVirtual(joiner, 'Team1');   /* team 2, not the host's team 1 */
  await host.waitForTimeout(1500);
  await shot(host, '4-edits-host.png');
  await shot(joiner, '5-edits-join.png');

  console.log('joiner tries a rule: refused, and told why');
  await clickVirtual(joiner, 'LoS');
  await joiner.waitForTimeout(800);
  await shot(joiner, '6-join-rule-refused.png');

  console.log('host: choose the second map');
  await clickVirtual(host, 'Map');
  await host.waitForTimeout(1500);
  await shot(host, '7-map-chooser.png');
  await clickVirtual(host, 'MapRow1');
  await host.waitForTimeout(800);
  await shot(host, '8-map-picked.png');
  await clickVirtual(host, 'MapOK');
  await host.waitForTimeout(2500);
  await shot(host, '9-room-new-map.png');
  await shot(joiner, '10-join-new-map.png');

  console.log('host presses Play before anyone is ready: refused, and told why');
  await clickVirtual(host, 'Play');
  await host.waitForTimeout(1200);
  await shot(host, '11-play-refused.png');

  console.log('both ready, then Play');
  await clickVirtual(host, 'Ready0');
  await clickVirtual(joiner, 'Ready1');
  await host.waitForTimeout(1500);
  await shot(host, '12-both-ready.png');
  await clickVirtual(host, 'Play');
  await host.waitForTimeout(30000);
  await shot(host, '13-host-battle.png');
  await shot(joiner, '14-join-battle.png');

  for (const n of ['host', 'join']) {
    const l = state.log[n];
    const spawned = l.filter(t => /LS_FINALIZE: spawned/.test(t));
    check(n + ' reached the battle: ' + spawned.length + ' monarchs spawned', spawned.length === 2);
    const errs = l.filter(t => /PAGEERROR|unhandled click/.test(t));
    check(n + ' threw nothing and every click was handled: ' + JSON.stringify(errs.slice(0, 3)), errs.length === 0);
    const cam = l.find(t => /LS_POSITION_CAMERA/.test(t));
    if (cam) console.log('    ' + n + ' ' + cam);
    const map = l.find(t => /LS_LOAD_TNT: loaded/.test(t));
    if (map) console.log('    ' + n + ' ' + map);
  }
  await browser.close();
  console.log(failures ? failures + ' failure(s)' : 'all good');
  console.log('shots in ' + OUT);
  process.exit(failures ? 1 : 0);
})().catch(e => { console.log('FAILED ' + (e && e.stack || e)); process.exit(2); });
