#!/usr/bin/env node
/* The mod registry in the page, web/mods.js: the registry's rules, the
   check on a download, installs into a store and back out, the way a
   download is fetched, and what Join does for a room that plays a mod.
   No browser and no network: zips are built here and fetch is faked. */
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const assert = require('assert');

const M = require(path.join(__dirname, '..', 'web', 'mods.js'));
const WEB = path.join(__dirname, '..', 'web');

let failed = 0;
const cases = [];
function check(what, fn) { cases.push([what, fn]); }

/* ── fixtures ───────────────────────────────────────────────────────── */

function crc32(buf) {
  let crc = 0xffffffff;
  for (const b of buf) {
    let c = (crc ^ b) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/* A zip of [name, text] pairs, deflated unless stored is set. */
function zip(files, stored) {
  const locals = [], centrals = [];
  let at = 0;
  for (const [name, text] of files) {
    const data = Buffer.from(text);
    const body = stored ? data : zlib.deflateRawSync(data);
    const n = Buffer.from(name);
    const h = Buffer.alloc(30);
    h.writeUInt32LE(0x04034b50, 0); h.writeUInt16LE(20, 4); h.writeUInt16LE(0x800, 6);
    h.writeUInt16LE(stored ? 0 : 8, 8); h.writeUInt32LE(crc32(data), 14);
    h.writeUInt32LE(body.length, 18); h.writeUInt32LE(data.length, 22); h.writeUInt16LE(n.length, 26);
    const c = Buffer.alloc(46);
    c.writeUInt32LE(0x02014b50, 0); c.writeUInt16LE(20, 4); c.writeUInt16LE(20, 6); c.writeUInt16LE(0x800, 8);
    c.writeUInt16LE(stored ? 0 : 8, 10); c.writeUInt32LE(crc32(data), 16);
    c.writeUInt32LE(body.length, 20); c.writeUInt32LE(data.length, 24); c.writeUInt16LE(n.length, 28);
    c.writeUInt32LE(at, 42);
    locals.push(h, n, body);
    centrals.push(c, n);
    at += 30 + n.length + body.length;
  }
  const cd = Buffer.concat(centrals);
  const e = Buffer.alloc(22);
  e.writeUInt32LE(0x06054b50, 0); e.writeUInt16LE(files.length, 8); e.writeUInt16LE(files.length, 10);
  e.writeUInt32LE(cd.length, 12); e.writeUInt32LE(at, 16);
  return new Uint8Array(Buffer.concat(locals.concat([cd, e])));
}

const inflateRaw = (d) => Promise.resolve(new Uint8Array(zlib.inflateRawSync(Buffer.from(d))));
const sha = (b) => require('crypto').createHash('sha256').update(b).digest('hex');
const text = (b) => Buffer.from(b).toString('utf8');

/* A store in memory, the shape of the page's origin private one. */
function memStore() {
  const files = new Map();
  return {
    files,
    write: (r, b) => { files.set(r, Buffer.from(b)); return Promise.resolve(); },
    read: (r) => Promise.resolve(files.has(r) ? new Uint8Array(files.get(r)) : null),
    remove: (r) => { files.delete(r); return Promise.resolve(); },
    list: (d) => Promise.resolve([...files.keys()].filter((k) => k.startsWith(d + '/') &&
      !k.slice(d.length + 1).includes('/')).map((k) => k.slice(d.length + 1)))
  };
}

function entry(id, modset, bytes, extra) {
  return Object.assign({
    id, name: 'Tough Swords', version: '0.2', author: 'Someone', page: 'https://example.org/ts',
    modset, url: 'https://example.org/' + id + '.zip', size: bytes ? bytes.length : 1234,
    sha256: bytes ? sha(bytes) : '00'.repeat(32), fingerprint: '5d0e44a1abcdef12'
  }, extra || {});
}

const FOLDER_MOD = [
  ['Mods/Tough Swords/features/zz/okstone.tdf', '[OKSTONE]\n{\nheight=12;\n}\n'],
  ['Mods/Tough Swords/readme.txt', 'under Mods, so kept'],
  ['Kingdoms.exe', 'not ours to replace'],
  ['Keys.tdf', "the player's keys stay"],
  ['TAKEnhanced/TAKEnhanced.dll', 'not read here'],
  ['README.txt', 'left out'],
];

/* ── the registry ───────────────────────────────────────────────────── */

check('the shipped registry is valid and its test mod is the zip beside it', async () => {
  const reg = M.parse(fs.readFileSync(path.join(WEB, 'mods', 'registry.json'), 'utf8'));
  assert.ok(reg, 'not a registry');
  assert.deepStrictEqual(reg.errors, []);
  const t = M.find(reg, 'ok-registry-test');
  assert.ok(M.oneClick(t));
  const bytes = new Uint8Array(fs.readFileSync(path.join(WEB, 'mods', 'ok-registry-test-1.0.zip')));
  assert.strictEqual(await M.check(t, bytes, t.fingerprint), '');
  /* A mod by someone else is credited and links to its author's page. */
  for (const m of reg.mods) assert.ok(m.author && /^https:\/\//.test(m.page), m.id);
  const nera = M.find(reg, 'the-new-era');
  assert.ok(nera && !M.oneClick(nera) && nera.manual);
});

check('an entry with a bad field is left out and the reason given', () => {
  const good = { id: 'ts', name: 'Tough Swords', version: '0.2', author: 'Someone', page: 'https://x.org',
                 modset: 'tough-swords', url: 'https://x.org/ts.zip', size: 1234, sha256: '00'.repeat(32),
                 fingerprint: '0123456789abcdef' };
  const man = { id: 'm', name: 'n', version: '1', author: 'a', page: 'https://x.org', manual: 'm' };
  const bad = [
    [Object.assign({}, man, { id: 'Caps' }), 'id must'],
    [Object.assign({}, man, { page: 'http://x.org' }), 'page must'],
    [Object.assign({}, man, { manual: undefined }), 'url or manual'],
    [Object.assign({}, man, { url: 'https://x.org/a.zip' }), 'not both'],
    [Object.assign({}, man, { author: '' }), 'author'],
    [Object.assign({}, man, { name: 'a name longer than a room can carry' }), 'too long'],
    [Object.assign({}, good, { id: 'u', url: 'ftp://x.org/a.zip' }), 'url must'],
    [Object.assign({}, good, { id: 'u', size: 64 * 1024 * 1024 + 1 }), 'over the limit'],
    [Object.assign({}, good, { id: 'u', size: 1.5 }), 'whole number'],
    [Object.assign({}, good, { id: 'u', sha256: '0011' }), 'sha256'],
    [Object.assign({}, good, { id: 'u', fingerprint: '0000000000000000' }), 'zero'],
    [good, 'twice'],
  ];
  for (const [e, why] of bad) {
    const reg = M.parse({ registry: 1, about: { x: [1, 2] }, mods: [good, e] });
    assert.strictEqual(reg.mods.length, 1, why);
    assert.strictEqual(reg.errors.length, 1, why);
    assert.ok(reg.errors[0].includes(why), reg.errors[0] + ' / ' + why);
  }
});

check('text that is not a registry is refused', () => {
  for (const t of ['', '[]', '{"registry":2,"mods":[]}', '{"mods":[]}', '{"registry":1}',
                   '{"registry":1,"mods":[]} trailing', '{"registry":1,"mods":[5]}',
                   '{"registry":1,"mods":[{"id":5}]}'])
    assert.strictEqual(M.parse(t), null, t);
  assert.deepStrictEqual(M.parse('{ "registry" : 1 , "mods" : [ ] }'), { mods: [], errors: [] });
});

check('a room finds its entry by fingerprint, then by name and version', () => {
  const reg = M.parse({ registry: 1, mods: [entry('ts', 'tough-swords')] });
  assert.ok(M.forRoom(reg, { mod: 'Else', mod_version: '9', fingerprint: '5D0E44A1ABCDEF12' }));
  assert.ok(M.forRoom(reg, { mod: 'tough swords', mod_version: '0.2', fingerprint: '1111111111111111' }));
  assert.strictEqual(M.forRoom(reg, { mod: 'Tough Swords', mod_version: '0.3' }), null);
  assert.strictEqual(M.forRoom(reg, { mod: 'Vanilla' }), null);
});

/* ── the check on a download ────────────────────────────────────────── */

check('a good download passes and a tampered one is refused', async () => {
  const bytes = zip([['Mods/Tough Swords/units/arasword.fbi', '[UNITINFO]\n{\nMaxDamage=1200;\n}\n']]);
  const e = entry('tough-swords', 'tough-swords', bytes);
  assert.strictEqual(await M.check(e, bytes, '5d0e44a1abcdef12'), '');
  assert.strictEqual(await M.check(e, bytes, ''), '');
  const bad = bytes.slice();
  bad[bad.length >> 1] ^= 1;
  assert.ok((await M.check(e, bad, '5d0e44a1abcdef12')).includes("does not match the registry\u2019s fingerprint"));
  assert.ok((await M.check(e, bytes.subarray(1), '')).includes('bytes and the registry says'));
  assert.ok((await M.check(e, bytes, '0000000000001234')).includes('plays other data than that game'));
});

/* ── install and remove ─────────────────────────────────────────────── */

check('a zip entry name lands under the mod root or nowhere', () => {
  for (const n of ['Mods/../../evil.txt', '/Mods/x.hpi', 'C:/Mods/x.hpi', 'Mods\\..\\x.hpi',
                   'Mods//x.hpi', 'Mods/./x.hpi', 'Mods/a\x01.hpi', ''])
    assert.strictEqual(M.rel(n).k, -1, JSON.stringify(n));
  assert.deepStrictEqual(M.rel('mods\\A b\\c.hpi'), { k: 1, rel: 'Mods/A b/c.hpi' });
  assert.deepStrictEqual(M.rel('takenhanced/presets/p.json'), { k: 1, rel: 'TAKEnhanced/Presets/p.json' });
  assert.strictEqual(M.rel('TAKEnhanced/TAKEnhanced.dll').k, 0);
  assert.strictEqual(M.rel('Mods/').k, 0);
  assert.strictEqual(M.folderId('Tough Swords'), 'tough-swords');
  assert.strictEqual(M.folderId('Ca\u00f1on'), 'ca--on');
});

check('an install takes only Mods and presets and writes the manifest and receipt', async () => {
  const s = memStore();
  const bytes = zip(FOLDER_MOD);
  const e = entry('tough-swords', 'tough-swords', bytes);
  assert.strictEqual(await M.install(e, bytes, s, inflateRaw), '');
  assert.deepStrictEqual([...s.files.keys()].sort(), [
    'Mods/Tough Swords/features/zz/okstone.tdf', 'Mods/Tough Swords/mod.tdf',
    'Mods/Tough Swords/readme.txt', 'Mods/tough-swords.registry.txt']);
  assert.strictEqual(text(s.files.get('Mods/Tough Swords/mod.tdf')),
                     '[MOD]\n{\nname=Tough Swords;\nversion=0.2;\nfingerprint=5d0e44a1abcdef12;\n}\n');
  /* The same receipt the desktop writes (src/core/mod_install.c). */
  assert.strictEqual(text(s.files.get('Mods/tough-swords.registry.txt')),
    '# Installed from the OpenKingdoms mod registry. Removing it deletes these files.\n' +
    'id=tough-swords\nversion=0.2\n' +
    'file=Mods/Tough Swords/features/zz/okstone.tdf\nfile=Mods/Tough Swords/readme.txt\n' +
    'file=Mods/Tough Swords/mod.tdf\n');
  assert.deepStrictEqual(Object.assign({}, await M.installed(s)), { 'tough-swords': '0.2' });
});

check('a preset mod gets its manifest beside the preset, stored or deflated', async () => {
  for (const stored of [false, true]) {
    const s = memStore();
    const bytes = zip([
      ['Mods/', ''],
      ['Mods/Fix One.hpi', 'an archive'],
      ['TAKEnhanced/Presets/vanilla.preset.json', '{"id":"vanilla","mods":{"enabled":false}}'],
      ['TAKEnhanced/Presets/te.preset.json', '{"id":"TE-Set","name":"TE","mods":{"enabled":true,"selectedMods":["Fix One.hpi"]}}'],
    ], stored);
    const e = entry('te', 'te-set', bytes, { fingerprint: '0000000000000077' });
    assert.strictEqual(await M.install(e, bytes, s, inflateRaw), '');
    assert.ok(s.files.has('Mods/Fix One.hpi'));
    assert.ok(s.files.get('TAKEnhanced/Presets/te.mod.tdf').toString().includes('fingerprint=0000000000000077;'));
    assert.strictEqual(await M.remove('te', s), '');
    assert.deepStrictEqual([...s.files.keys()], []);
  }
});

check('a zip that climbs out, lacks its mod set or is no zip is refused and leaves nothing', async () => {
  const s = memStore();
  let bytes = zip([['Mods/Tough Swords/a.tdf', 'fine'], ['Mods/../../evil.txt', 'climbs']]);
  let why = await M.install(entry('ts', 'tough-swords', bytes), bytes, s, inflateRaw);
  assert.ok(why.includes('outside the game folder'), why);
  bytes = zip(FOLDER_MOD);
  why = await M.install(entry('ts', 'blunt-swords', bytes), bytes, s, inflateRaw);
  assert.ok(why.includes('has no mod set blunt-swords'), why);
  why = await M.install(entry('ts', 'ts', bytes), new Uint8Array(9), s, inflateRaw);
  assert.ok(why.includes('not a zip'), why);
  /* A body that does not inflate to its listed size is damaged. */
  const name = 'Mods/Tough Swords/a.tdf';
  const lied = Buffer.from(zip([[name, 'fine']], true));
  lied.writeUInt32LE(99, 30 + name.length + 'fine'.length + 24);   /* the directory's unpacked size */
  why = await M.install(entry('ts', 'tough-swords', lied), new Uint8Array(lied), s, inflateRaw);
  assert.ok(why.includes('damaged zip'), why);
  assert.deepStrictEqual([...s.files.keys()], []);
});

check('remove deletes what the install wrote and keeps files another install lists', async () => {
  const s = memStore();
  const a = zip([['Mods/Shared.hpi', 'both ship this'], ['Mods/A Mod/a.tdf', 'a']]);
  const b = zip([['Mods/Shared.hpi', 'both ship this'], ['Mods/B Mod/b.tdf', 'b']]);
  assert.strictEqual(await M.install(entry('a', 'a-mod', a), a, s, inflateRaw), '');
  assert.strictEqual(await M.install(entry('b', 'b-mod', b), b, s, inflateRaw), '');
  assert.strictEqual(await M.remove('a', s), '');
  assert.ok(s.files.has('Mods/Shared.hpi'));
  assert.ok(!s.files.has('Mods/A Mod/a.tdf') && !s.files.has('Mods/a.registry.txt'));
  assert.ok((await M.remove('a', s)).includes('was not installed'));
  assert.strictEqual(await M.remove('b', s), '');
  assert.deepStrictEqual([...s.files.keys()], []);
});

check('the shipped test mod installs where the engine looks for a folder mod', async () => {
  const reg = M.parse(fs.readFileSync(path.join(WEB, 'mods', 'registry.json'), 'utf8'));
  const t = M.find(reg, 'ok-registry-test');
  const bytes = new Uint8Array(fs.readFileSync(path.join(WEB, 'mods', 'ok-registry-test-1.0.zip')));
  const s = memStore(), game = memStore();
  assert.strictEqual(await M.install(t, bytes, M.both(s, game), inflateRaw), '');
  assert.ok(s.files.has('Mods/OK Registry Test/features/zz-ok-registry-test/okmarker.tdf'));
  assert.ok(s.files.has('Mods/OK Registry Test/mod.tdf'));
  assert.ok(!s.files.has('README.txt'));
  assert.strictEqual(M.folderId('OK Registry Test'), t.modset);
  /* A pick that kept no files still gets the installed mods. */
  const fresh = memStore();
  assert.strictEqual(await M.copyInstalled(s, fresh), 2);
  assert.ok(fresh.files.has('Mods/OK Registry Test/mod.tdf'));
});

/* ── fetching ───────────────────────────────────────────────────────── */

function fakeFetch(answers, calls) {
  return (url) => {
    calls.push(url);
    const a = answers[url];
    if (!a) return Promise.resolve({ ok: false, status: 404, text: () => Promise.resolve('{"error":"not found"}') });
    if (a.status) return Promise.resolve({ ok: false, status: a.status, text: () => Promise.resolve(a.body) });
    const chunks = [a.slice(0, a.length >> 1), a.slice(a.length >> 1)];
    return Promise.resolve({
      ok: true, status: 200,
      body: { getReader: () => ({ read: () => Promise.resolve(chunks.length ? { done: false, value: chunks.shift() } : { done: true }),
                                  cancel: () => {} }) }
    });
  };
}

check('a download comes from this site directly and from elsewhere through the game server', () => {
  const e = entry('ts', 'tough-swords');
  assert.strictEqual(M.downloadUrl(e, 'https://relay.example/api', 'https://openkingdoms.net'),
                     'https://relay.example/api/mods/ts/download');
  const own = entry('ts', 'tough-swords', null, { url: 'https://openkingdoms.net/mods/ts.zip' });
  assert.strictEqual(M.downloadUrl(own, 'https://relay.example/api', 'https://openkingdoms.net'), own.url);
  assert.strictEqual(M.downloadUrl(e, '', 'https://openkingdoms.net'), e.url);
});

check('a download reports progress, stops past its size and reads the server\u2019s reason', async () => {
  const bytes = zip(FOLDER_MOD);
  const e = entry('ts', 'tough-swords', bytes);
  const seen = [];
  const got = await M.download(e, 'u', (g, n) => seen.push([g, n]), fakeFetch({ u: bytes }, []));
  assert.deepStrictEqual(Buffer.from(got), Buffer.from(bytes));
  assert.strictEqual(seen[seen.length - 1][0], bytes.length);
  const small = Object.assign({}, e, { size: 10 });
  await assert.rejects(M.download(small, 'u', null, fakeFetch({ u: bytes }, [])), /larger than the registry says/);
  await assert.rejects(M.download(e, 'u', null, fakeFetch({ u: { status: 404, body: '{"error":"that mod is not in the registry"}' } }, [])),
                       /The server said: that mod is not in the registry\./);
});

/* ── Join on a room that plays a mod ────────────────────────────────── */

function env(reg, store, answers, calls, yes) {
  return {
    registry: () => Promise.resolve(reg), store, sets: [], api: 'https://relay.example/api',
    origin: 'https://openkingdoms.net', confirm: () => yes, progress: () => {},
    inflateRaw, fetch: fakeFetch(answers, calls)
  };
}

check('join fetches a one click mod it lacks, checks it, installs it and chooses it', async () => {
  const bytes = zip(FOLDER_MOD);
  const e = entry('tough-swords', 'tough-swords', bytes);
  const reg = M.parse({ registry: 1, mods: [e] });
  const room = { code: 'ABCD', mod: 'Tough Swords', mod_version: '0.2', fingerprint: e.fingerprint };
  const calls = [], s = memStore();
  const url = 'https://relay.example/api/mods/tough-swords/download';
  let r = await M.forJoin(room, env(reg, s, { [url]: bytes }, calls, true));
  assert.deepStrictEqual([r.go, r.modset], [true, 'tough-swords']);
  assert.deepStrictEqual(calls, [url]);
  assert.ok(s.files.has('Mods/Tough Swords/mod.tdf'));
  /* Installed now, so the next Join only chooses it. */
  r = await M.forJoin(room, env(reg, s, {}, calls, true));
  assert.deepStrictEqual([r.go, r.modset], [true, 'tough-swords']);
  assert.strictEqual(calls.length, 1);
});

check('join refuses a tampered download, a room on other data, and a mod that installs by hand', async () => {
  const bytes = zip(FOLDER_MOD);
  const e = entry('tough-swords', 'tough-swords', bytes);
  const man = { id: 'tne', name: 'The New Era', version: '6.0', author: 'Sage', page: 'https://x.org/t', manual: 'by hand' };
  const reg = M.parse({ registry: 1, mods: [e, man] });
  const url = 'https://relay.example/api/mods/tough-swords/download';
  const bad = bytes.slice();
  bad[bad.length >> 1] ^= 1;
  let calls = [], s = memStore();
  let r = await M.forJoin({ mod: 'Tough Swords', mod_version: '0.2', fingerprint: e.fingerprint },
                          env(reg, s, { [url]: bad }, calls, true));
  assert.strictEqual(r.go, false);
  assert.ok(r.why.includes('does not match the registry'), r.why);
  assert.deepStrictEqual([...s.files.keys()], []);
  /* Same name and version, other data: refused before any fetch. */
  calls = [];
  r = await M.forJoin({ mod: 'Tough Swords', mod_version: '0.2', fingerprint: '0000000000000abc' },
                      env(reg, s, { [url]: bytes }, calls, true));
  assert.strictEqual(r.go, false);
  assert.ok(r.why.includes('plays other data'), r.why);
  assert.deepStrictEqual(calls, []);
  r = await M.forJoin({ mod: 'The New Era', mod_version: '6.0' }, env(reg, s, {}, calls, true));
  assert.strictEqual(r.go, false);
  assert.ok(r.why.includes('installs by hand') && r.why.includes('https://x.org/t'), r.why);
  /* Said no: nothing fetched, nothing joined. */
  r = await M.forJoin({ mod: 'Tough Swords', mod_version: '0.2', fingerprint: e.fingerprint },
                      env(reg, s, { [url]: bytes }, calls, false));
  assert.deepStrictEqual([r.go, r.why], [false, '']);
  assert.deepStrictEqual(calls, []);
});

check('join goes on for vanilla, a mod set already there, and a mod the registry lacks', async () => {
  const reg = M.parse({ registry: 1, mods: [] });
  const calls = [];
  const s = memStore();
  assert.deepStrictEqual(await M.forJoin({ mod: 'Vanilla' }, env(reg, s, {}, calls, true)), { go: true });
  assert.deepStrictEqual(await M.forJoin({}, env(reg, s, {}, calls, true)), { go: true });
  const e2 = env(reg, s, {}, calls, true);
  e2.sets = [{ id: 'tak-enhanced', name: 'TA:K Enhanced' }];
  assert.deepStrictEqual(await M.forJoin({ mod: 'TA:K Enhanced', mod_version: '' }, e2), { go: true, modset: 'tak-enhanced' });
  assert.deepStrictEqual(await M.forJoin({ mod: 'Homebrew', mod_version: '1' }, env(reg, s, {}, calls, true)), { go: true });
  assert.deepStrictEqual(calls, []);
});

/* ── the game page's hooks, read out of web/shell.html ──────────────── */

const SHELL = fs.readFileSync(path.join(WEB, 'shell.html'), 'utf8');
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

check('the page loads mods.js before its own script', () => {
  const at = SHELL.indexOf('<script src="mods.js"></script>');
  assert.ok(at > 0 && at < SHELL.indexOf('Bring-your-own-files loader'));
  assert.ok(SHELL.includes('href="mods.html"'));
});

check('Join waits for the mod check and Watch does not', () => {
  const args = [], held = [];
  const join = new Function('args', 'ready', 'start', 'liveNote', 'pendingJoin', 'modsFor',
    functionSource('joinGame') + 'return joinGame;')(
    args, { hidden: true }, () => {}, { textContent: '' }, null, (r, then) => held.push(then));
  join({ code: 'MOD', mod: 'Tough Swords' }, false);
  assert.deepStrictEqual(args, []);
  held[0]({ go: true });
  assert.deepStrictEqual(args, ['--join', 'MOD']);
  join({ code: 'WAT', mod: 'Tough Swords' }, true);
  assert.deepStrictEqual(args, ['--watch', 'WAT']);
  assert.strictEqual(held.length, 1);
});

function runModsFor(result, loaded) {
  const got = { told: [], chosen: null, offered: 0, then: null };
  const window = { OKMods: Object.assign({}, M, { forJoin: () => Promise.resolve(result), opfsStore: () => memStore() }),
                   confirm: () => true };
  const localStorage = { setItem: (k, v) => { if (k === 'ok.modset') got.chosen = v; } };
  const modsFor = new Function('window', 'Module', 'ready', 'STORE', 'liveApi', 'location', 'tell',
    'localStorage', 'offerMods', 'modSets', 'modReg', 'fetch', 'console',
    functionSource('modsFor') + 'return modsFor;')(
    window, { FS: {} }, { hidden: !loaded }, 'game', 'https://relay.example/api', { origin: 'https://site' },
    (t, err) => got.told.push([t, !!err]), localStorage, () => { got.offered++; }, () => [], null, null, console);
  return new Promise((resolve) => {
    modsFor({ code: 'MOD', mod: 'Tough Swords' }, (r) => { got.then = r; });
    setTimeout(() => resolve(got), 10);
  });
}

check('the page chooses the mod set a room needs, and says why it stays', async () => {
  let got = await runModsFor({ go: true, modset: 'tough-swords', installed: { name: 'Tough Swords', version: '0.2' } }, true);
  assert.strictEqual(got.chosen, 'tough-swords');
  assert.strictEqual(got.offered, 1);
  assert.deepStrictEqual(got.then, { go: true, modset: 'tough-swords', installed: { name: 'Tough Swords', version: '0.2' } });
  assert.ok(got.told.some(([t]) => t.includes('is installed and checked')));
  got = await runModsFor({ go: false, why: 'The download of Tough Swords 0.2 does not match.' }, false);
  assert.strictEqual(got.then, null);
  assert.strictEqual(got.chosen, null);
  assert.deepStrictEqual(got.told, [['The download of Tough Swords 0.2 does not match.', true]]);
  got = await runModsFor({ go: false, why: '' }, false);
  assert.deepStrictEqual([got.then, got.told], [null, []]);
});

check('mods kept in storage without the game files do not pass for them', async () => {
  async function kept(entries) {
    const isArchive = new Function(functionSource('isArchive') + 'return isArchive;')();
    const tryCache = new Function('opfsDir', 'cacheEntries', 'picker', 'say', 'lazyFile', 'writeGame',
      'MODELS', 'DATA', 'console', 'isArchive', functionSource('tryCache') + 'return tryCache;')(
      () => Promise.resolve({}), () => Promise.resolve(entries), {}, () => {}, () => {}, () => {},
      'models3d', '/data', { log() {}, warn() {} }, isArchive);
    return tryCache({});
  }
  const file = { getFile: () => Promise.resolve({ arrayBuffer: () => Promise.resolve(new ArrayBuffer(1)) }) };
  assert.strictEqual(await kept([{ rel: 'Mods/TAK Enhanced.hpi', handle: file },
                                 { rel: 'TAKEnhanced/Presets/te.preset.json', handle: file }]), 0);
  assert.strictEqual(await kept([{ rel: 'data.hpi', handle: file }, { rel: 'Mods/x.hpi', handle: file }]), 2);
  const isPreset = new Function(functionSource('isPreset') + 'return isPreset;')();
  assert.ok(isPreset('tak-enhanced.preset.json') && isPreset('tak-enhanced.mod.tdf') && !isPreset('readme.txt'));
});

(async () => {
  for (const [what, fn] of cases) {
    try { await fn(); console.log('ok   ' + what); }
    catch (e) { failed++; console.log('FAIL ' + what + '\n     ' + (e && e.message)); }
  }
  console.log(failed ? failed + ' failed' : 'all passed');
  process.exit(failed ? 1 : 0);
})();
