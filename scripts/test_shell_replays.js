#!/usr/bin/env node
/* Which replays web/shell.html prunes from browser storage. Reads
   replaysToPrune out of the page, and the page's limits out of both the
   page and include/tak_replay.h, so the two cannot drift apart. No
   browser needed. */
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const root = path.join(__dirname, '..');
const text = fs.readFileSync(path.join(root, 'web', 'shell.html'), 'utf8');
const header = fs.readFileSync(path.join(root, 'include', 'tak_replay.h'), 'utf8');

function block(src, start, name) {
  if (start < 0) throw new Error('no ' + name + ' in the page');
  let depth = 0;
  for (let j = src.indexOf('{', start); j < src.length; j++) {
    if (src[j] === '{') depth++;
    else if (src[j] === '}' && --depth === 0) return src.slice(start, j + 1);
  }
  throw new Error('no end to ' + name);
}

let failed = 0;
function check(what, fn) {
  try { fn(); console.log('ok   ' + what); }
  catch (e) { failed++; console.log('FAIL ' + what + '\n     ' + e.message); }
}

/* The C limit as bytes: "(48u * 1024u * 1024u)" or "30". */
function cLimit(name) {
  const m = header.match(new RegExp('#define\\s+' + name + '\\s+(.+)'));
  if (!m) throw new Error('no ' + name + ' in tak_replay.h');
  return Function('return ' + m[1].replace(/\/\*.*$/, '').replace(/u/g, '') + ';')();
}
function pageVar(name) {
  const m = text.match(new RegExp('var\\s+' + name + '\\s*=\\s*([^;]+);'));
  if (!m) throw new Error('no ' + name + ' in the page');
  return Function('return ' + m[1] + ';')();
}

let prune = null;
check('the page has replaysToPrune', () => {
  const src = block(text, text.indexOf('function replaysToPrune('), 'replaysToPrune');
  prune = new Function(src + '\nreturn replaysToPrune;')();
});

check('the page keeps to the engine\'s own limits', () => {
  assert.strictEqual(pageVar('REPLAY_KEEP'), cLimit('TAK_REPLAY_KEEP'));
  assert.strictEqual(pageVar('REPLAY_BUDGET'), cLimit('TAK_REPLAY_BUDGET_BYTES'));
  assert.strictEqual(pageVar('REPLAY_MAX_BYTES'), cLimit('TAK_REPLAY_MAX_BYTES'));
});

function rep(name, at, size) { return { name: name + '.okreplay', at: at, size: size || 100 }; }

check('the newest replays stay and the oldest go', () => {
  const items = [];
  for (let i = 0; i < 35; i++) items.push(rep('r' + i, 1000 + i));
  const gone = prune(items, 30, 1e9, 16 * 1024 * 1024).sort();
  assert.deepStrictEqual(gone, ['r0', 'r1', 'r2', 'r3', 'r4'].map((n) => n + '.okreplay').sort());
});

check('a saved game is never pruned', () => {
  const items = [{ name: 'campaign.oksave', at: 1, size: 5e8 }];
  for (let i = 0; i < 40; i++) items.push(rep('r' + i, 1000 + i));
  const gone = prune(items, 30, 1e9, 16 * 1024 * 1024);
  assert.ok(gone.indexOf('campaign.oksave') < 0);
  assert.strictEqual(gone.length, 10);
});

check('the byte budget prunes from the oldest', () => {
  const items = [rep('a', 1, 400), rep('b', 2, 400), rep('c', 3, 400), rep('d', 4, 50)];
  assert.deepStrictEqual(prune(items, 30, 800, 16 * 1024 * 1024).sort(), ['a.okreplay', 'b.okreplay']);
});

check('a replay past the size guard is pruned whatever its age', () => {
  const items = [rep('huge', 9999, 17 * 1024 * 1024), rep('ok', 1, 100)];
  assert.deepStrictEqual(prune(items, 30, 64 * 1024 * 1024, pageVar('REPLAY_MAX_BYTES')), ['huge.okreplay']);
});

console.log(failed ? failed + ' failed' : 'the page prunes replays as the engine does');
process.exit(failed ? 1 : 0);
