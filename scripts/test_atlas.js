#!/usr/bin/env node
/* The Asset Atlas without a browser, web/atlas.js: which archives are the
   game's own, what a file is, the names the game gives a model and the
   remaster model for it, search, the notes and their merge rule, and the
   zip and GLB files it writes. Also that the site carries every file the
   page loads. No game data: every fixture is made here. */
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const ROOT = path.join(__dirname, '..');
const A = require(path.join(ROOT, 'web', 'atlas.js'));
const Mods = require(path.join(ROOT, 'web', 'mods.js'));

let failed = 0;
const cases = [];
function check(what, fn) { cases.push([what, fn]); }

/* ── packages ───────────────────────────────────────────────────────── */

check('the retail archives are official and the rest are custom', () => {
  const data = A.packageInfo('data.hpi');
  assert.deepStrictEqual([data.group, data.official, data.label], ['game', true, 'Kingdoms']);
  assert.strictEqual(A.packageInfo('IPData.hpi').label, 'The Iron Plague');
  assert.strictEqual(A.packageInfo('V3Rocket.hpi').label, 'Patch 3.0');
  assert.strictEqual(A.packageInfo('IPGerman.hpi').official, true);
  const mine = A.packageInfo('rev31.ufo');
  assert.deepStrictEqual([mine.group, mine.official], ['game', false]);
  const pack = A.packageInfo('Maps/Adamantine Gate.kmp');
  assert.deepStrictEqual([pack.group, pack.official, pack.label, pack.key], ['maps', true, 'Darien Crusades', 'maps/adamantine gate.kmp']);
  assert.strictEqual(A.packageInfo('Maps/My Island.kmp').official, false);
  assert.strictEqual(A.packageInfo('Mods/Tough Swords/units.hpi').group, 'mods');
  assert.strictEqual(A.packageInfo('Mods/Tough Swords/units.hpi').official, false);
});

check('archives mount as the engine mounts them and show in the game\'s order', () => {
  assert.deepStrictEqual(A.mountOrder(['Maps/b.kmp', 'z.ufo', 'terrain.hpi', 'Data.hpi', 'Maps/a.kmp']),
                         ['Data.hpi', 'terrain.hpi', 'z.ufo', 'Maps/a.kmp', 'Maps/b.kmp']);
  const shown = A.displayOrder(['boneyards.hpi', 'zz.hpi', 'IPData.hpi', 'data.hpi', 'Maps/x.kmp', 'terrain.hpi']
    .map(A.packageInfo)).map(p => p.rel);
  assert.deepStrictEqual(shown, ['data.hpi', 'terrain.hpi', 'IPData.hpi', 'boneyards.hpi', 'zz.hpi', 'Maps/x.kmp']);
});

check('files are sorted into textures, models and the data that names them', () => {
  assert.deepStrictEqual(A.classify('anims/arabuild.gaf'), { k: 'gaf' });
  assert.deepStrictEqual(A.classify('anims/x_4444.TAF'), { k: 'taf' });
  assert.deepStrictEqual(A.classify('terrain/a.JPG'), { k: 'img', fmt: 'jpg' });
  assert.deepStrictEqual(A.classify('bitmaps/x.pcx'), { k: 'img', fmt: 'pcx' });
  assert.deepStrictEqual(A.classify('objects3d/aralode.3do'), { k: '3do' });
  assert.deepStrictEqual(A.classify('units/araarch.fbi'), { k: 'fbi' });
  assert.deepStrictEqual(A.classify('features/aramon/arabuild.tdf'), { k: 'tdf' });
  assert.strictEqual(A.classify('gamedata/sidedata.tdf'), null);
  assert.strictEqual(A.classify('sounds/a.wav'), null);
});

check('notes are keyed by archive name and path, entries by the whole package path', () => {
  assert.strictEqual(A.noteKey('Data.hpi', 'anims/Arabuild.gaf', 'Arabuild01'), 'data.hpi|anims/arabuild.gaf#arabuild01');
  assert.strictEqual(A.noteKey('data.hpi', ''), 'data.hpi|');
  assert.deepStrictEqual(A.splitKey('data.hpi|anims/a.gaf#s'), { pkg: 'data.hpi', path: 'anims/a.gaf', seq: 's' });
  assert.strictEqual(A.splitKey('no bar here'), null);
  assert.strictEqual(A.uid('mods/a/x.hpi', 'p.gaf', 'S'), 'mods/a/x.hpi|p.gaf#s');
});

/* ── the game's data ────────────────────────────────────────────────── */

const FBI = `[UNITINFO]
{
	// the unit
	UnitName = TESTKNIGHT;
	Objectname = TESTKNT;
	Name = Test Knight;
	side = ARA;
	description = Aramon;
	[WEAPON1]
	{
	name = Lance;
	}
}`;
const TDF = `/* two features */
[TestTree01]
	{
	world=Aramon;
	description=Tree;
	filename=TestTrees;
	seqname=TestTree01;
	}
[TestStatue]
	{
	world=allworlds;
	description=Statue;
	object=teststatue;
	}
[NoArt]
	{
	world=Zhon;
	}`;

check('a unit file gives the unit\'s names and model, and not its weapon\'s', () => {
  assert.deepStrictEqual(A.unitFromFbi(FBI), { unit: 'TESTKNIGHT', object: 'TESTKNT', name: 'Test Knight', side: 'ARA', desc: 'Aramon' });
  assert.strictEqual(A.unitFromFbi('[OTHER]{a=1;}'), null);
});

check('a feature file gives each feature\'s picture or model', () => {
  const f = A.featuresFromTdf(TDF);
  assert.strictEqual(f.length, 2);
  assert.deepStrictEqual(f[0], { name: 'TestTree01', file: 'TestTrees', seq: 'TestTree01', object: '', world: 'Aramon', desc: 'Tree' });
  assert.strictEqual(f[1].object, 'teststatue');
});

check('picture sizes come from their headers', () => {
  const png = Buffer.alloc(32); png.set([0x89, 0x50, 0x4E, 0x47], 0); png.writeUInt32BE(300, 16); png.writeUInt32BE(200, 20);
  assert.deepStrictEqual(A.imageSize(png, 'png'), { w: 300, h: 200 });
  const jpg = Buffer.from([0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 0, 0, 0xFF, 0xC0, 0, 11, 8, 0, 48, 0, 64, 3, 0, 0, 0]);
  assert.deepStrictEqual(A.imageSize(jpg, 'jpg'), { w: 64, h: 48 });
  const pcx = Buffer.alloc(128); pcx[0] = 0x0A; pcx.writeUInt16LE(639, 8); pcx.writeUInt16LE(479, 10);
  assert.deepStrictEqual(A.imageSize(pcx, 'pcx'), { w: 640, h: 480 });
  const bmp = Buffer.alloc(54); bmp.write('BM', 0); bmp.writeUInt32LE(40, 14); bmp.writeInt32LE(32, 18); bmp.writeInt32LE(-16, 22);
  assert.deepStrictEqual(A.imageSize(bmp, 'bmp'), { w: 32, h: 16 });
  const tga = Buffer.alloc(18); tga.writeUInt16LE(128, 12); tga.writeUInt16LE(64, 14);
  assert.deepStrictEqual(A.imageSize(tga, 'tga'), { w: 128, h: 64 });
  assert.strictEqual(A.imageSize(Buffer.from('nope'), 'png'), null);
});

check('the decoder\'s GAF listing becomes one entry a sequence', () => {
  const info = A.parseGafInfo('0\tTestTree01\t2\t50,60,25,55,1;52,61,26,56,1\n1\tGlow\t1\t16,16,0,0,5\n2\t\t1\t1,1,0,0,1\n');
  assert.strictEqual(info.length, 3);
  const es = A.gafEntries({ key: 'data.hpi', name: 'data.hpi' }, 'anims/testtrees.gaf', 'gaf', 999, info);
  assert.strictEqual(es.length, 2);
  assert.deepStrictEqual([es[0].id, es[0].nk, es[0].w, es[0].h, es[0].f, es[0].fw], ['data.hpi|anims/testtrees.gaf#testtree01', 'data.hpi|anims/testtrees.gaf#testtree01', 52, 61, 2, 50]);
  assert.deepStrictEqual([es[1].k, es[1].fmt], ['taf', 'argb1555']);
});

/* ── the remaster ───────────────────────────────────────────────────── */

const REMASTER = {
  okAtlasRemaster: 1, repo: 'OpenKingdoms/Stars-of-Darien', commit: 'abc123',
  base: 'https://raw.githubusercontent.com/OpenKingdoms/Stars-of-Darien/abc123/',
  models: [
    { n: 'TestTree01', p: 'unity/Assets/Overrides/Features/TestTree01.glb', k: 'feature', b: 10, t: 100, i: 0, pt: 1 },
    { n: 'TESTKNIGHT', p: 'unity/Assets/Overrides/Units/TESTKNIGHT.glb', k: 'unit', b: 10, t: 900, i: 2 },
    { n: 'Bad', p: 'unity/Assets/../x.glb', k: 'feature' },
    { n: 'Space Name', p: 'unity/Assets/Overrides/Features/Space Name.glb', k: 'feature' }
  ]
};

check('the remaster list is read, and a path that climbs or needs escaping is handled', () => {
  const r = A.parseRemaster(JSON.stringify(REMASTER));
  assert.strictEqual(r.entries.length, 3);
  assert.strictEqual(r.entries[0].id, 'stars-of-darien|features/testtree01.glb');
  assert.strictEqual(r.entries[2].url, REMASTER.base + 'unity/Assets/Overrides/Features/Space%20Name.glb');
  assert.strictEqual(A.parseRemaster('{"okAtlasRemaster":2,"models":[]}'), null);
  assert.strictEqual(A.parseRemaster({ okAtlasRemaster: 1, base: 'http://x/', models: [] }), null);
});

function world() {
  const pkg = { key: 'data.hpi', name: 'data.hpi' };
  const entries = [
    A.fileEntry(pkg, 'objects3d/testknt.3do', { k: '3do' }, 10, { tris: 12 }),
    A.fileEntry(pkg, 'objects3d/teststatue.3do', { k: '3do' }, 10, { tris: 12 }),
    A.fileEntry(pkg, 'objects3d/arrow.3do', { k: '3do' }, 10, { tris: 2 })
  ].concat(A.gafEntries(pkg, 'anims/testtrees.gaf', 'gaf', 10, A.parseGafInfo('0\tTestTree01\t1\t50,60,0,0,1\n1\tTestTree02\t1\t5,6,0,0,1\n')),
           A.gafEntries(pkg, 'anims/buttons.gaf', 'gaf', 10, A.parseGafInfo('0\tTestTree01\t1\t5,6,0,0,1\n')));
  const book = A.nameBook([{ units: [A.unitFromFbi(FBI)], features: A.featuresFromTdf(TDF) }]);
  const sod = A.parseRemaster(REMASTER).entries;
  return { entries, book, sod };
}

check('originals link to the remaster model for them, as the remaster looks one up', () => {
  const w = world();
  assert.strictEqual(A.linkRemasters(w.entries, w.book, w.sod), 2);
  const by = Object.fromEntries(w.entries.map(e => [e.id, e]));
  assert.strictEqual(by['data.hpi|objects3d/testknt.3do'].rm, 'stars-of-darien|units/testknight.glb');
  assert.strictEqual(by['data.hpi|objects3d/testknt.3do'].title, 'Test Knight');
  assert.strictEqual(by['data.hpi|anims/testtrees.gaf#testtree01'].rm, 'stars-of-darien|features/testtree01.glb');
  /* The statue could have one and has none. A sequence of the same name in
     another file is not the feature's picture. */
  assert.strictEqual(by['data.hpi|objects3d/teststatue.3do'].rmable, true);
  assert.strictEqual(by['data.hpi|objects3d/teststatue.3do'].rm, undefined);
  assert.strictEqual(by['data.hpi|anims/buttons.gaf#testtree01'].rmable, undefined);
  assert.deepStrictEqual(w.sod[0].orig, ['data.hpi|anims/testtrees.gaf#testtree01']);
});

/* ── search ─────────────────────────────────────────────────────────── */

check('search needs every word, and filters narrow it', () => {
  const w = world();
  A.linkRemasters(w.entries, w.book, w.sod);
  const all = w.entries.concat(w.sod);
  const notes = { 'data.hpi|objects3d/arrow.3do': { note: 'Shiny arrowhead', rating: 4, tags: ['ammo'] } };
  const hay = e => A.haystack(e, 'data.hpi', notes[e.nk], null);
  const rate = e => (notes[e.nk] || {}).rating || 0;
  const pkgOf = k => (k === 'data.hpi' ? { official: true } : null);
  const ids = (q, f) => A.search(all, q, f || {}, hay, rate, pkgOf).map(e => e.id);
  assert.deepStrictEqual(ids('shiny'), ['data.hpi|objects3d/arrow.3do']);
  assert.deepStrictEqual(ids('ammo 3do'), ['data.hpi|objects3d/arrow.3do']);
  assert.deepStrictEqual(ids('"test knight"'), ['data.hpi|objects3d/testknt.3do']);
  assert.strictEqual(ids('', { kind: 'model' }).length, 6);
  assert.deepStrictEqual(ids('', { rating: 3 }), ['data.hpi|objects3d/arrow.3do']);
  assert.deepStrictEqual(ids('', { remaster: 'needs' }), ['data.hpi|objects3d/teststatue.3do', 'data.hpi|objects3d/arrow.3do']);
  assert.strictEqual(ids('', { remaster: 'has' }).length, 4);
  assert.strictEqual(ids('', { source: 'remaster' }).length, 3);
  assert.strictEqual(ids('', { source: 'official' }).length, 6);
});

check('the grid draws only the rows in view', () => {
  assert.deepStrictEqual(A.gridWindow(0, 300, 100, 4, 50, 0), { first: 0, last: 15, rows: 13, total: 1300 });
  assert.deepStrictEqual(A.gridWindow(1000, 300, 100, 4, 50, 1), { first: 36, last: 49, rows: 13, total: 1300 });
  assert.strictEqual(A.gridWindow(0, 300, 100, 4, 0, 0).last, -1);
});

/* ── notes ──────────────────────────────────────────────────────────── */

check('a note is cleaned before it is kept', () => {
  assert.deepStrictEqual(A.cleanNote({ note: ' hi\u0007 ', rating: 9, tags: 'Roof, roof,  Big  hall ', at: '2026-01-02T03:04:05Z' }),
                         { note: 'hi', rating: 0, tags: ['big hall', 'roof'], at: '2026-01-02T03:04:05.000Z' });
  assert.strictEqual(A.cleanNote({ note: '', rating: 0, tags: [] }), null);
});

check('notes merge: new ones are added, the later edit wins and tags join', () => {
  const mine = { 'a.hpi|x': { note: 'mine', rating: 2, tags: ['one'], at: '2026-01-01T00:00:00.000Z' },
                 'a.hpi|y': { note: 'keep me', rating: 5, tags: [], at: '2026-03-01T00:00:00.000Z' } };
  const theirs = { 'a.hpi|x': { note: 'theirs', rating: 4, tags: ['two'], at: '2026-02-01T00:00:00Z' },
                   'a.hpi|y': { note: 'older', rating: 1, tags: ['t'], at: '2026-01-01T00:00:00Z' },
                   'a.hpi|z': { note: 'new', tags: [] },
                   'BAD KEY': { note: 'x' } };
  const m = A.mergeNotes(mine, theirs);
  assert.deepStrictEqual(m.notes['a.hpi|x'], { note: 'theirs', rating: 4, tags: ['one', 'two'], at: '2026-02-01T00:00:00.000Z' });
  assert.deepStrictEqual(m.notes['a.hpi|y'], { note: 'keep me', rating: 5, tags: ['t'], at: '2026-03-01T00:00:00.000Z' });
  assert.strictEqual(m.notes['a.hpi|z'].note, 'new');
  assert.strictEqual(m.notes['BAD KEY'], undefined);
  assert.deepStrictEqual([m.added, m.changed], [1, 2]);
  assert.strictEqual(mine['a.hpi|x'].note, 'mine');
});

check('notes go out and come back as the same notes, and other files are refused', () => {
  const notes = { 'data.hpi|anims/a.gaf#s': { note: 'n', rating: 3, tags: ['t'], at: '2026-01-01T00:00:00.000Z' } };
  const text = A.exportNotes(notes, new Date('2026-10-16T00:00:00Z'));
  assert.ok(text.indexOf('"okAtlasNotes": 1') > 0);
  const back = A.parseNotes(text);
  assert.deepStrictEqual(Object.assign({}, back.notes), notes);
  assert.ok(A.parseNotes('{"registry":1}').error);
  assert.ok(A.parseNotes('not json').error);
});

check('the community notes file in the site reads', () => {
  const c = A.parseNotes(fs.readFileSync(path.join(ROOT, 'web', 'atlas', 'community.json'), 'utf8'));
  assert.ok(c.notes, c.error);
  assert.strictEqual(c.skipped, 0);
});

/* ── palettes and painting ──────────────────────────────────────────── */

check('a picture is first shown in the palette the game draws it with', () => {
  const w = world();
  const tex = { k: 'gaf', path: 'textures/arabldg.gaf' };
  assert.deepStrictEqual(A.paletteChoice(tex, {}).spec, { rule: 1 });
  const tree = w.entries.find(e => e.seq === 'TestTree01' && /testtrees/.test(e.path));
  assert.deepStrictEqual(A.paletteChoice(tree, A.namesOf(tree, w.book)).spec, { file: 'data/palettes/aramon_features.pcx', transparent: 0 });
  assert.deepStrictEqual(A.paletteChoice({ k: 'gaf', path: 'anims/gui.gaf' }, { features: [] }).spec, { rule: 0 });
  assert.strictEqual(A.paletteChoice({ k: 'taf', path: 'anims/x.taf' }, {}), null);
  assert.strictEqual(A.flatPalette({ name: 'zonmage' }), 'data/palettes/zhon.pcx');
  assert.strictEqual(A.flatPalette({ name: 'arrow' }), 'data/palettes/gameart.pcx');
});

check('a painted remaster material finds its picture as the remaster does', () => {
  const w = world();
  assert.deepStrictEqual(A.paintSource({ kind: 'feature', name: 'TestTree01', world: 'veruna' }, w.book),
                         { seq: 'testtree01', file: 'testtrees', pcx: 'data/palettes/veruna_features.pcx', corner: false });
  assert.strictEqual(A.paintSource({ kind: 'feature', name: 'TestTree01' }, w.book).pcx, 'data/palettes/aramon_features.pcx');
  assert.deepStrictEqual(A.paintSource({ kind: 'texture', name: 'Cobbles1', world: 'zhon' }, w.book),
                         { texture: 'cobbles1', pcx: 'data/palettes/zon_textures.pcx', corner: true });
  assert.strictEqual(A.paintSource({ kind: 'feature', name: 'Nothing' }, w.book), null);
});

/* ── contributing ───────────────────────────────────────────────────── */

check('a replacement is named for the engine and for Stars of Darien', () => {
  const w = world();
  const knt = w.entries.find(e => e.name === 'testknt');
  const c = A.contribute(knt, A.namesOf(knt, w.book));
  assert.deepStrictEqual(c.engine.map(s => s.path), ['models3d/testknt.glb', 'objects3d/testknt.3do']);
  assert.deepStrictEqual(c.sod.map(s => s.path), ['unity/Assets/Overrides/Units/TESTKNIGHT.glb']);
  assert.ok(c.issue.indexOf('https://github.com/OpenKingdoms/Stars-of-Darien/issues/new?template=model.yml&title=%5Bmodel%5D%20TESTKNIGHT&replaces=TESTKNIGHT&kind=Unit') === 0);
  const tree = w.entries.find(e => e.seq === 'TestTree01' && /testtrees/.test(e.path));
  const t = A.contribute(tree, A.namesOf(tree, w.book));
  assert.deepStrictEqual(t.engine.map(s => s.path), ['models3d/testtree01.glb', 'Mods/<your mod>/anims/testtrees.gaf']);
  assert.deepStrictEqual(t.sod.map(s => s.path), ['unity/Assets/Overrides/Features/TestTree01.glb']);
  const statue = w.entries.find(e => e.name === 'teststatue');
  assert.deepStrictEqual(A.contribute(statue, A.namesOf(statue, w.book)).sod.map(s => s.path), ['unity/Assets/Overrides/Features/TestStatue.glb']);
  const button = w.entries.find(e => /buttons/.test(e.path));
  const b = A.contribute(button, A.namesOf(button, w.book));
  assert.deepStrictEqual([b.engine.length, b.sod.length, b.issue], [1, 0, '']);
});

/* ── files it writes ────────────────────────────────────────────────── */

check('crc32 is the zip one', () => {
  assert.strictEqual(A.crc32(Buffer.from('123456789')), 0xCBF43926);
});

check('a frames zip reads back with the mod installer\'s own reader', async () => {
  const files = [{ name: 'a_001.png', bytes: new Uint8Array([1, 2, 3]) }, { name: 'a.json', bytes: Buffer.from('{"x":1}') }];
  const z = A.zipStore(files, new Date(2026, 9, 16, 12, 0, 0));
  const list = Mods.listZip(z);
  assert.deepStrictEqual(list.map(e => [e.name, e.size, e.method]), [['a_001.png', 3, 0], ['a.json', 7, 0]]);
  const back = await Mods.readEntry(z, list[1]);
  assert.strictEqual(Buffer.from(back).toString(), '{"x":1}');
});

check('a GLB keeps the piece tree, its offsets and the scale', () => {
  const tri = new Float32Array([0, 1, 0, 1, 1, 0, 0, 2, 0]);
  const glb = A.writeGlb([{ name: 'cloth', node: 1, pos: tri, uv: new Float32Array(6), png: new Uint8Array([137, 80, 78, 71]), alpha: true },
                          { name: 'colour7', node: 0, pos: tri, color: [1, 0, 0, 1] }],
                         { nodes: [{ name: 'base', parent: -1, t: [0, 0, 0] }, { name: 'torso', parent: 0, t: [0, 1, 0] }], scale: 0.5 });
  const g = A.readGlb(glb);
  assert.ok(g && g.bin);
  assert.strictEqual(glb.length % 4, 0);
  assert.deepStrictEqual(g.json.nodes.map(n => n.name), ['base', 'torso']);
  assert.deepStrictEqual(g.json.nodes[1].translation, [0, 0.5, 0]);
  assert.deepStrictEqual(g.json.nodes[0].children, [1]);
  assert.deepStrictEqual(g.json.accessors[0].min, [0, 0, 0]);
  assert.deepStrictEqual(g.json.accessors[0].max, [0.5, 0.5, 0]);
  assert.strictEqual(g.json.materials[0].alphaMode, 'MASK');
  assert.deepStrictEqual(g.json.materials[1].pbrMetallicRoughness.baseColorFactor, [1, 0, 0, 1]);
  assert.strictEqual(A.glbTriangles(g.json), 2);
  assert.strictEqual(A.readGlb(new Uint8Array(8)), null);
});

/* ── the site ───────────────────────────────────────────────────────── */

check('the site carries every file the atlas loads', () => {
  const page = fs.readFileSync(path.join(ROOT, 'web', 'atlas.html'), 'utf8');
  const worker = fs.readFileSync(path.join(ROOT, 'web', 'atlas-worker.js'), 'utf8');
  const app = fs.readFileSync(path.join(ROOT, 'web', 'atlas-app.js'), 'utf8');
  const web = fs.readFileSync(path.join(ROOT, '.github', 'workflows', 'web.yml'), 'utf8');
  const need = [...page.matchAll(/<script src="([^"]+)"/g)].map(m => m[1])
    .concat([...worker.matchAll(/importScripts\(([^)]*)\)/g)][0][1].split(',').map(s => s.trim().replace(/'/g, '')))
    .concat([...app.matchAll(/fetch\('(atlas\/[^']+)'\)/g)].map(m => m[1]))
    .concat(['atlas-worker.js', 'atlas-dec.wasm']);
  assert.ok(need.length >= 8, need.join(' '));
  need.forEach(f => {
    const name = f.replace(/^atlas\//, '');
    assert.ok(new RegExp('cp [^\\n]*\\b' + name.replace(/[.-]/g, '\\$&') + '\\b').test(web), f + ' is not copied into the site by web.yml');
    if (!/^atlas-dec\./.test(f)) assert.ok(fs.existsSync(path.join(ROOT, 'web', f)), f + ' is missing from web/');
  });
});

check('the remaster list in the site is one the page reads', () => {
  const r = A.parseRemaster(fs.readFileSync(path.join(ROOT, 'web', 'atlas', 'remaster.json'), 'utf8'));
  assert.ok(r && r.entries.length > 0);
  assert.ok(/^[0-9a-f]{40}$/.test(r.commit));
  r.entries.forEach(e => assert.ok(e.url.indexOf('https://raw.githubusercontent.com/' + A.SOD_REPO + '/' + r.commit + '/') === 0, e.url));
});

(async () => {
  for (const [what, fn] of cases) {
    try { await fn(); console.log('ok   ' + what); }
    catch (e) { failed++; console.log('FAIL ' + what + '\n     ' + (e && e.message)); }
  }
  console.log(failed ? failed + ' failed' : 'all passed');
  process.exit(failed ? 1 : 0);
})();
