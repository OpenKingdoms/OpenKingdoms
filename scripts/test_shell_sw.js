#!/usr/bin/env node
/* The page, its script and its wasm come from one build, the one the
   site serves now. Runs web/sw.js against a fake network, cache and set
   of open pages, and the page's reload guards from web/shell.html. No
   browser needed. scripts/sw-update-smoke.js is the same in a browser. */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const ROOT = path.join(__dirname, '..');
const SW = fs.readFileSync(path.join(ROOT, 'web', 'sw.js'), 'utf8');
const SHELL = fs.readFileSync(path.join(ROOT, 'web', 'shell.html'), 'utf8');
const ORIGIN = 'https://openkingdoms.net';

/* './tak-re.js' for any spelling of a file, './index.html' for the site. */
function rel(u) {
  const p = new URL(typeof u === 'string' ? u : u.url, ORIGIN + '/').pathname.replace(/^.*\//, './');
  return p === './' ? './index.html' : p;
}

function response(file, build) {
  const body = file + '@' + build;
  return { ok: true, status: 200, body, text: () => Promise.resolve(body),
           headers: { get: (h) => (h === 'etag' ? '"' + build + '"' : null) } };
}
const NOT_FOUND = { ok: false, status: 404, text: () => Promise.resolve(''), headers: { get: () => null } };
const build = (r) => r.body.replace(/^.*@/, '');

/* The site. Holding it keeps every fetch waiting until release. A
   missing file is a 404 and a stalled one never answers. */
class Net {
  constructor() { this.build = null; this.online = true; this.gate = null; this.missing = new Set(); this.stalled = new Set(); }
  hold() { let open; this.gate = new Promise((r) => { open = r; }); this.release = () => { this.gate = null; open(); }; }
  fetch(u) {
    const f = rel(u), b = this.build;
    if (this.stalled.has(f)) return new Promise(() => {});
    return (this.gate || Promise.resolve()).then(() => {
      if (!this.online) throw new TypeError('Failed to fetch');
      return this.missing.has(f) ? NOT_FOUND : response(f, b);
    });
  }
}

/* CacheStorage. Holding it keeps every put waiting until release. */
class Caches {
  constructor() { this.all = new Map(); this.gate = null; }
  hold() { let open; this.gate = new Promise((r) => { open = r; }); this.release = () => { this.gate = null; open(); }; }
  keys() { return Promise.resolve([...this.all.keys()]); }
  open(name) {
    if (!this.all.has(name)) this.all.set(name, new Map());
    const m = this.all.get(name);
    return Promise.resolve({
      put: (k, r) => (this.gate || Promise.resolve()).then(() => { m.set(rel(k), r); }),
      match: (k) => Promise.resolve(m.get(rel(k)))
    });
  }
  match(k, opts) {
    const names = opts && opts.cacheName ? [opts.cacheName] : [...this.all.keys()];
    for (const n of names) {
      const m = this.all.get(n);
      if (m && m.has(rel(k))) return Promise.resolve(m.get(rel(k)));
    }
    return Promise.resolve(undefined);
  }
  has(name) { return Promise.resolve(this.all.has(name)); }
  delete(name) { return Promise.resolve(this.all.delete(name)); }
  builds() { return [...this.all.values()].map((m) => (m.has('./index.html') ? build(m.get('./index.html')) : '-')).sort(); }
}

/* The open pages, by client id. */
class Clients {
  constructor() { this.live = new Set(); }
  matchAll() { return Promise.resolve([...this.live].map((id) => ({ id }))); }
  claim() { return Promise.resolve(); }
}

const settle = async () => { for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r)); };

function world() {
  const w = { net: new Net(), caches: new Caches(), clients: new Clients(), clock: 1700000000000, timers: [] };
  /* Every timer the worker set runs now. */
  w.fire = () => { const t = w.timers.splice(0); t.forEach((fn) => fn()); };
  return w;
}

/* One start of the worker. A restart is another call on the same world. */
function worker(w) {
  const on = {};
  const self = {
    location: { origin: ORIGIN, href: ORIGIN + '/sw.js' },
    clients: w.clients,
    skipWaiting: () => Promise.resolve(),
    addEventListener: (type, fn) => { on[type] = fn; }
  };
  vm.runInNewContext(SW, {
    self, caches: w.caches, URL, Promise, console,
    fetch: (u) => w.net.fetch(u),
    setTimeout: (fn) => { w.timers.push(fn); return w.timers.length; },
    clearTimeout: () => {},
    Date: { now: () => ++w.clock }
  });
  return {
    async install() {
      const waits = [];
      on.install({ waitUntil: (p) => waits.push(p) });
      await Promise.all(waits);
    },
    /* The response, or undefined when the worker let it go by. */
    get(file, opts) {
      opts = opts || {};
      let res;
      const e = {
        request: { method: 'GET', url: ORIGIN + '/' + file, mode: opts.navigate ? 'navigate' : 'no-cors' },
        clientId: opts.navigate ? '' : (opts.client || ''),
        resultingClientId: opts.navigate ? (opts.client || '') : '',
        respondWith: (p) => { res = p; },
        waitUntil: () => {}
      };
      on.fetch(e);
      return res ? Promise.resolve(res) : Promise.resolve(undefined);
    }
  };
}

/* A page load: the page, then the script and the wasm it asks for. */
async function load(sw, w, client) {
  w.clients.live.add(client);
  const page = await sw.get('', { navigate: true, client });
  const js = await sw.get('tak-re.js', { client });
  const wasm = await sw.get('tak-re.wasm', { client });
  return [build(page), build(js), build(wasm)].join(' ');
}

async function installed(b, missing) {
  const w = world();
  w.net.build = b;
  (missing || []).forEach((f) => w.net.missing.add(f));
  const sw = worker(w);
  await sw.install();
  return [w, sw];
}

let failed = 0;
async function check(what, fn) {
  try { await fn(); console.log('ok   ' + what); }
  catch (e) { failed++; console.log('FAIL ' + what + '\n     ' + e.message); }
}

const asserts = [

['the visit after a deploy runs the new build', async () => {
  const [w, sw] = await installed('A');
  w.net.build = 'B';
  assert.strictEqual(await load(sw, w, 'c1'), 'B B B');
  await settle();
  assert.deepStrictEqual(w.caches.builds(), ['A', 'B']);
}],

['a cache from before the site named its build takes the deploy', async () => {
  const [w, sw] = await installed('A', ['./version.txt']);
  w.net.missing.clear();
  w.net.build = 'B';
  assert.strictEqual(await load(sw, w, 'c1'), 'B B B');
}],

['a site that does not name its build is served from the cache as before', async () => {
  const [w, sw] = await installed('A', ['./version.txt']);
  w.net.build = 'B';
  assert.strictEqual(await load(sw, w, 'c1'), 'A A A');
  await settle();
  assert.strictEqual(await load(sw, w, 'c2'), 'B B B');
}],

['when the site does not answer in time the cache starts the game', async () => {
  const [w, sw] = await installed('A');
  w.net.build = 'B';
  w.net.stalled.add('./version.txt');
  w.clients.live.add('c1');
  const pending = sw.get('', { navigate: true, client: 'c1' });
  await settle();
  w.fire();
  const page = await pending;
  const js = await sw.get('tak-re.js', { client: 'c1' });
  const wasm = await sw.get('tak-re.wasm', { client: 'c1' });
  assert.strictEqual([build(page), build(js), build(wasm)].join(' '), 'A A A');
}],

['when the new build does not come whole the cached one starts', async () => {
  const [w, sw] = await installed('A');
  w.net.build = 'B';
  w.net.missing.add('./tak-re.wasm');
  assert.strictEqual(await load(sw, w, 'c1'), 'A A A');
  await settle();
  assert.deepStrictEqual(w.caches.builds(), ['A']);
}],

['a page keeps its build when a newer one lands while it loads', async () => {
  const [w, sw] = await installed('A');
  w.clients.live.add('c1');
  const page = await sw.get('', { navigate: true, client: 'c1' });
  const js = await sw.get('tak-re.js', { client: 'c1' });
  w.net.build = 'B';
  assert.strictEqual(await load(sw, w, 'c2'), 'B B B', 'a page opened after the deploy');
  const wasm = await sw.get('tak-re.wasm', { client: 'c1' });
  assert.strictEqual([build(page), build(js), build(wasm)].join(' '), 'A A A');
  assert.deepStrictEqual(w.caches.builds(), ['A', 'B'], 'A stays for the open page');
}],

['a generation still being written is never read', async () => {
  const [w, sw] = await installed('A');
  w.net.build = 'B';
  w.caches.hold();
  for (const c of ['c1', 'c2']) {
    w.clients.live.add(c);
    const pending = sw.get('', { navigate: true, client: c });
    await settle();
    w.fire();
    const page = await pending;
    const js = await sw.get('tak-re.js', { client: c });
    const wasm = await sw.get('tak-re.wasm', { client: c });
    assert.strictEqual([build(page), build(js), build(wasm)].join(' '), 'A A A', 'a page that waited its time on ' + c);
  }
  w.caches.release();
  await settle();
  assert.strictEqual(await load(sw, w, 'c3'), 'B B B');
}],

['a visit while an older refresh is under way still gets the new build', async () => {
  const [w, sw] = await installed('A');
  w.net.hold();
  w.clients.live.add('c0');
  w.clients.live.add('c1');
  const before = sw.get('', { navigate: true, client: 'c0' });
  await settle();
  w.net.build = 'B';
  const after = sw.get('', { navigate: true, client: 'c1' });
  await settle();
  w.net.release();
  assert.strictEqual(build(await before), 'A');
  assert.strictEqual(build(await after), 'B', 'asked after the deploy, while the refresh from before it ran');
  assert.strictEqual(await load(sw, w, 'c2'), 'B B B');
}],

['a restarted worker gives a page both engine files from one build', async () => {
  const [w, sw] = await installed('A');
  w.net.build = 'B';
  w.clients.live.add('c1');
  await sw.get('', { navigate: true, client: 'c1' });
  await settle();
  const again = worker(w);
  const js = await again.get('tak-re.js', { client: 'c1' });
  w.net.build = 'C';
  const wasm = await again.get('tak-re.wasm', { client: 'c1' });
  assert.strictEqual(build(js), build(wasm));
}],

['an old generation goes once no page uses it', async () => {
  const [w, sw] = await installed('A');
  assert.strictEqual(await load(sw, w, 'c1'), 'A A A');
  w.net.build = 'B';
  assert.strictEqual(await load(sw, w, 'c2'), 'B B B');
  w.net.build = 'C';
  assert.strictEqual(await load(sw, w, 'c3'), 'C C C');
  await settle();
  assert.deepStrictEqual(w.caches.builds(), ['A', 'B', 'C'], 'A stays while its page is open');
  w.clients.live.delete('c1');
  w.net.build = 'D';
  assert.strictEqual(await load(sw, w, 'c4'), 'D D D');
  await settle();
  assert.deepStrictEqual(w.caches.builds(), ['B', 'C', 'D'], 'the page on B is open, the one on A closed');
}],

['an unchanged build is not stored again', async () => {
  const [w, sw] = await installed('A');
  const before = [...w.caches.all.keys()];
  assert.strictEqual(await load(sw, w, 'c1'), 'A A A');
  await settle();
  assert.deepStrictEqual([...w.caches.all.keys()], before);
}],

['offline, the page and the engine come from the cache', async () => {
  const [w, sw] = await installed('A');
  w.net.online = false;
  assert.strictEqual(await load(sw, w, 'c1'), 'A A A');
  await settle();
  assert.deepStrictEqual(w.caches.builds(), ['A']);
}],

['the relay and the leaderboard pass by', async () => {
  const [w, sw] = await installed('A');
  w.clients.live.add('c1');
  assert.strictEqual(await sw.get('relay.txt', { client: 'c1' }), undefined);
  assert.strictEqual(await sw.get('leaderboard.html', { navigate: true, client: 'c2' }), undefined);
  assert.strictEqual(await sw.get('version.txt?1', { client: 'c1' }), undefined, 'a page asks the site, not the cache');
}]

];

/* The page's guard, read out of the page. */
function functionSource(name) {
  const start = SHELL.indexOf('function ' + name + '(');
  if (start < 0) throw new Error('no ' + name);
  let depth = 0;
  for (let j = SHELL.indexOf('{', start); j < SHELL.length; j++) {
    if (SHELL[j] === '{') depth++;
    else if (SHELL[j] === '}' && --depth === 0) return SHELL.slice(start, j + 1);
  }
  throw new Error('no end to ' + name);
}

function storage() {
  const m = new Map();
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => { m.set(k, String(v)); } };
}

const guard = [

['a script and wasm from two builds reload the page once', () => {
  const reloadOnce = new Function(functionSource('reloadOnce') + '\nreturn reloadOnce;')();
  const s = storage();
  assert.strictEqual(reloadOnce('ASM_CONSTS[code] is not a function', true, s), true);
  assert.strictEqual(reloadOnce('ASM_CONSTS[code] is not a function', true, s), false, 'never twice');
  assert.strictEqual(reloadOnce('LinkError: WebAssembly.instantiate(): Import #3 "a" "b": function import requires a callable', true, storage()), true);
  assert.strictEqual(reloadOnce('RuntimeError: null function or function signature mismatch', true, storage()), true);
}],

['any other stop, or one after the start, is told as it is', () => {
  const reloadOnce = new Function(functionSource('reloadOnce') + '\nreturn reloadOnce;')();
  assert.strictEqual(reloadOnce('Aborted(OOM)', true, storage()), false);
  assert.strictEqual(reloadOnce('ASM_CONSTS[code] is not a function', false, storage()), false);
  assert.strictEqual(reloadOnce(undefined, true, storage()), false);
  const blocked = { getItem: () => { throw new Error('denied'); }, setItem: () => { throw new Error('denied'); } };
  assert.strictEqual(reloadOnce('ASM_CONSTS[code] is not a function', true, blocked), false, 'no storage, no reload');
  const forgetful = { getItem: () => null, setItem: () => {} };
  assert.strictEqual(reloadOnce('ASM_CONSTS[code] is not a function', true, forgetful), false, 'a guard that does not hold, no reload');
}],

['an open page takes a newer deploy only on a screen where nothing is under way', () => {
  const reloadForNewer = new Function(functionSource('reloadForNewer') + '\nreturn reloadForNewer;')();
  for (const screen of ['menu', 'Select Game', 'Skirmish Lobby', 'Campaign', 'Credits'])
    assert.strictEqual(reloadForNewer('A', 'B', screen, storage()), true, screen);
  for (const screen of ['In Game', 'Loading', 'Multiplayer', 'Options', 'Exiting', ''])
    assert.strictEqual(reloadForNewer('A', 'B', screen, storage()), false, screen || 'before the engine is up');
}],

['an open page reloads once for each deploy and only for a newer one', () => {
  const reloadForNewer = new Function(functionSource('reloadForNewer') + '\nreturn reloadForNewer;')();
  assert.strictEqual(reloadForNewer('A', 'A', 'menu', storage()), false, 'the same build');
  assert.strictEqual(reloadForNewer('A', '', 'menu', storage()), false, 'the site did not say');
  assert.strictEqual(reloadForNewer('@OK_SITE_BUILD@', 'B', 'menu', storage()), false, 'a page no deploy stamped');
  const s = storage();
  assert.strictEqual(reloadForNewer('A', 'B', 'menu', s), true);
  assert.strictEqual(reloadForNewer('A', 'B', 'menu', s), false, 'a reload that did not take is not tried again');
  assert.strictEqual(reloadForNewer('A', 'C', 'menu', s), true, 'a later deploy is');
  const blocked = { getItem: () => { throw new Error('denied'); }, setItem: () => { throw new Error('denied'); } };
  assert.strictEqual(reloadForNewer('A', 'B', 'menu', blocked), false, 'no storage, no reload');
}]

];

(async () => {
  for (const [what, fn] of asserts) await check(what, fn);
  for (const [what, fn] of guard) await check(what, fn);
  if (failed) { console.log(failed + ' failed'); process.exit(1); }
})();
