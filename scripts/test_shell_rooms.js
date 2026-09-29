#!/usr/bin/env node
/* Which games web/shell.html tells the player about. Reads freshRooms
   out of the page and holds it to a few listings. No browser needed. */
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const text = fs.readFileSync(path.join(__dirname, '..', 'web', 'shell.html'), 'utf8');

function functionSource(name) {
  const start = text.indexOf('function ' + name + '(');
  if (start < 0) throw new Error('no ' + name + ' in the page');
  let depth = 0;
  for (let j = text.indexOf('{', start); j < text.length; j++) {
    if (text[j] === '{') depth++;
    else if (text[j] === '}' && --depth === 0) return text.slice(start, j + 1);
  }
  throw new Error('no end to ' + name);
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

if (failed) { console.log(failed + ' failed'); process.exit(1); }
