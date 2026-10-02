#!/usr/bin/env node
/* Which games web/shell.html tells the player about. Reads freshRooms
   out of the page and holds it to a few listings. No browser needed. */
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const text = fs.readFileSync(path.join(__dirname, '..', 'web', 'shell.html'), 'utf8');

function block(src, start, name) {
  if (start < 0) throw new Error('no ' + name);
  let depth = 0;
  for (let j = src.indexOf('{', start); j < src.length; j++) {
    if (src[j] === '{') depth++;
    else if (src[j] === '}' && --depth === 0) return src.slice(start, j + 1);
  }
  throw new Error('no end to ' + name);
}
function functionSource(name) {
  return block(text, text.indexOf('function ' + name + '('), name);
}

const freshRooms = new Function(functionSource('freshRooms') + '\nreturn freshRooms;')();

function room(code, extra) {
  return Object.assign({ code, host: 'h' + code, map: 'm', status: 'open', players: 1, max: 4 }, extra);
}
const codes = (d) => d.fresh.map((r) => r.code);
let failed = 0;
function check(what, fn) {
  try { fn(); console.log('ok   ' + what); }
  catch (e) { failed++; console.log('FAIL ' + what + '\n     ' + e.message); }
}

check('the first listing only marks what is there', () => {
  const d = freshRooms([room('AAA'), room('BBB')], null, []);
  assert.deepStrictEqual(codes(d), []);
  assert.deepStrictEqual(Object.keys(d.seen).sort(), ['AAA', 'BBB']);
});

check('a game that opens after it is told', () => {
  const d = freshRooms([room('AAA'), room('CCC')], { AAA: 1 }, []);
  assert.deepStrictEqual(codes(d), ['CCC']);
  assert.ok(d.seen.CCC);
});

check('the player\'s own game is never told', () => {
  const d = freshRooms([room('OWN'), room('DDD')], {}, ['OWN']);
  assert.deepStrictEqual(codes(d), ['DDD']);
});

check('a game seen once is not told again after it leaves the list', () => {
  let seen = freshRooms([room('AAA')], null, []).seen;
  seen = freshRooms([], seen, []).seen;
  assert.deepStrictEqual(codes(freshRooms([room('AAA')], seen, [])), []);
});

check('a game seen full is not told when a seat frees', () => {
  const full = freshRooms([room('EEE', { players: 4 })], {}, []);
  assert.deepStrictEqual(codes(full), []);
  assert.deepStrictEqual(codes(freshRooms([room('EEE', { players: 3 })], full.seen, [])), []);
});

check('games under way, starting or full are not told', () => {
  const d = freshRooms([
    room('PPP', { status: 'playing' }), room('SSS', { status: 'starting' }),
    room('FFF', { players: 2, max: 2 }), { status: 'open' }, null,
  ], {}, []);
  assert.deepStrictEqual(codes(d), []);
});

check('a listing with no rooms is harmless', () => {
  assert.deepStrictEqual(codes(freshRooms(undefined, {}, [])), []);
});

/* The poll with the page around it stubbed, counting what it asks. */
function poll(page) {
  const asked = [];
  const body = 'var liveTimer = 0, liveApi = "", liveIdle = 0, livePing = 0;' +
    functionSource('liveDelay') + functionSource('pollLive') + 'return pollLive;';
  const run = new Function('picker', 'started', 'telling', 'document', 'fetch', 'performance',
                           'setTimeout', 'drawLive', 'tellAbout', 'liveBox', body)(
    { hidden: page.pickerHidden }, page.started, () => page.telling, { visibilityState: 'visible' },
    (url) => { asked.push(url); return new Promise(() => {}); }, { now: () => 0 },
    () => 1, () => {}, () => {}, {});
  run();
  return asked.length;
}

check('the list is asked for while cached files load and the picker is still hidden', () => {
  assert.strictEqual(poll({ pickerHidden: true, started: false, telling: false }), 1);
});

check('a running game asks only for a player who wants to be told', () => {
  assert.strictEqual(poll({ pickerHidden: true, started: true, telling: false }), 0);
  assert.strictEqual(poll({ pickerHidden: true, started: true, telling: true }), 1);
});

check('the room the engine opens is kept for the player\'s other tabs at once', () => {
  const c = fs.readFileSync(path.join(__dirname, '..', 'src', 'ui', 'multiplayer.c'), 'utf8');
  const hook = block(c, c.indexOf('EM_JS(void, mp_note_room'), 'mp_note_room');
  const js = hook.slice(hook.indexOf('{') + 1, -1);
  const Module = {}, kept = {};
  new Function('Module', 'UTF8ToString', 'localStorage', 'code', js)(
    Module, (p) => p, { setItem: (k, v) => { kept[k] = v; } }, 'OWN123');
  assert.strictEqual(Module.okOwnRoom, 'OWN123');
  assert.strictEqual(kept['ok.ownroom'], 'OWN123');
});

/* A notification clicked: the front page joins in place, a running game
   asks first, because the join link reloads the page. */
function click(started, answer) {
  const got = { asked: null, joined: null };
  const location = { pathname: '/', href: '' };
  const openRoom = new Function('started', 'confirm', 'location', 'joinGame',
                                functionSource('openRoom') + 'return openRoom;')(
    started, (q) => { got.asked = q; return answer; }, location, (r) => { got.joined = r.code; });
  openRoom(room('JJJ', { host: 'Zach' }));
  got.href = location.href;
  return got;
}

check('a click on the front page joins without asking', () => {
  const got = click(false, false);
  assert.strictEqual(got.asked, null);
  assert.strictEqual(got.joined, 'JJJ');
  assert.strictEqual(got.href, '');
});

check('a click behind a running game asks before it leaves', () => {
  const no = click(true, false);
  assert.strictEqual(no.asked, 'Leave this game to join Zach’s room?');
  assert.strictEqual(no.href, '');
  assert.strictEqual(no.joined, null);
  assert.strictEqual(click(true, true).href, '/?join=JJJ');
});

/* Join while a seat is free, Watch once a game is under way and its host
   lets people watch (#294). */
const roomAction = new Function(functionSource('roomAction') + '\nreturn roomAction;')();

check('an open game with a seat offers Join and a full one nothing', () => {
  assert.strictEqual(roomAction(room('AAA')), 'join');
  assert.strictEqual(roomAction(room('BBB', { players: 4 })), '');
  assert.strictEqual(roomAction(room('CCC', { status: 'starting' })), '');
  assert.strictEqual(roomAction(null), '');
});

check('a game under way with a computer seat to take offers Join in', () => {
  assert.strictEqual(roomAction(room('PPP', { status: 'playing', drop_in: true, players: 4 })), 'dropin');
  assert.strictEqual(roomAction(room('PPQ', { status: 'playing', drop_in: true, watchable: true })), 'dropin');
  assert.strictEqual(roomAction(room('QQQ', { status: 'playing', drop_in: false })), '');
  const draw = functionSource('drawLive');
  assert.ok(draw.indexOf('roomAction(r)') >= 0);
  assert.ok(draw.indexOf("'Join in'") >= 0);
});

check('a game under way offers Watch while it takes watchers', () => {
  assert.strictEqual(roomAction(room('PPP', { status: 'playing', watchable: true })), 'watch');
  assert.strictEqual(roomAction(room('QQQ', { status: 'playing', watchable: false })), '');
  assert.strictEqual(roomAction(room('RRR', { status: 'playing', watchable: true, watchers: 8 })), '');
  assert.strictEqual(roomAction(room('SSS', { status: 'playing', watchable: true, watchers: 7 })), 'watch');
});

/* Join and Watch hand the engine the one flag, however often they are
   pressed, and a watch link at load becomes --watch. */
function pressed(watch, before) {
  const args = before.slice();
  const liveNote = { textContent: '' };
  let started = 0;
  const joinGame = new Function('args', 'ready', 'start', 'liveNote', 'pendingJoin', 'modsFor',
    functionSource('joinGame') + 'return joinGame;')(
    args, { hidden: true }, () => { started++; }, liveNote, null, (r, then) => then({ go: true }));
  joinGame(room('WWW', { host: 'Zach' }), watch);
  return { args, note: liveNote.textContent };
}

check('Watch hands the engine --watch in place of any earlier choice', () => {
  const got = pressed(true, ['--join', 'OLD', '--watch', 'OLDER']);
  assert.deepStrictEqual(got.args, ['--watch', 'WWW']);
  assert.strictEqual(got.note, 'Choose your game files and you will watch Zach\u2019s game as it is played.');
  assert.deepStrictEqual(pressed(false, ['--watch', 'X']).args, ['--join', 'WWW']);
});

check('a watch link at load watches that game', () => {
  const i = text.indexOf("var watchCode = params.get('watch')");
  assert.ok(i > 0);
  assert.ok(text.indexOf("args.push('--watch', watchCode)", i) > i);
});

/* Android Chrome has Notification but its constructor throws. */
function supported(N) {
  return new Function('window', functionSource('notifySupported') + 'return notifySupported;')(
    { Notification: N })();
}

check('a browser whose Notification cannot be made is not offered the switch', () => {
  assert.strictEqual(supported(undefined), false);
  function Throws() { throw new TypeError('Illegal constructor'); }
  Throws.permission = 'default';
  assert.strictEqual(supported(Throws), false);
  function Works() { this.close = () => {}; }
  Works.permission = 'default';
  assert.strictEqual(supported(Works), true);
});

check('the probe never runs once permission is granted, where it would show', () => {
  let made = 0;
  function Granted() { made++; }
  Granted.permission = 'granted';
  assert.strictEqual(supported(Granted), true);
  assert.strictEqual(made, 0);
});

if (failed) { console.log(failed + ' failed'); process.exit(1); }
