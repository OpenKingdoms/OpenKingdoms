#!/usr/bin/env node
/* The site's visit counts as the page keeps them: what is sent, what is
   never sent, and that nothing it does can trouble the page. Runs
   web/counts.js in node with a fake browser. */
'use strict';
const assert = require('assert');
const C = require('./counts.js');

let failed = 0;
function test(name, fn) {
  return Promise.resolve().then(fn).then(
    () => console.log('  ' + name + ' PASS'),
    (e) => { failed++; console.log('  ' + name + ' FAIL\n    ' + (e && e.stack || e)); });
}

function storage() {
  const m = new Map();
  return {
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
    map: m
  };
}

/* A browser with a beacon that keeps what it was handed, and timers a
   test runs by hand. */
function browser(opts) {
  opts = opts || {};
  const sent = [];
  const timers = [];
  const env = {
    nav: Object.assign({
      userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0 Safari/537.36',
      maxTouchPoints: 0,
      sendBeacon: (url, body) => { sent.push({ url, body: JSON.parse(body) }); return true; }
    }, opts.nav || {}),
    win: opts.win || {},
    local: opts.local || storage(),
    session: opts.session || storage(),
    screen: { width: 1920, height: 1080 },
    relayText: 'relayText' in opts ? opts.relayText : Promise.resolve('wss://relay.example/relay\n'),
    setTimeout: (fn) => { timers.push(fn); return timers.length; },
    clearTimeout: () => {}
  };
  return { env, sent, timers, run: () => { while (timers.length) timers.shift()(); } };
}

const tick = () => new Promise((r) => setTimeout(r, 0));

(async function () {
  await test('the browser is told apart coarsely', () => {
    assert.strictEqual(C.browserFamily('Mozilla/5.0 (Windows NT 10.0) AppleWebKit/537.36 Chrome/141.0 Safari/537.36 Edg/141.0'), 'edge');
    assert.strictEqual(C.browserFamily('Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0'), 'firefox');
    assert.strictEqual(C.browserFamily('Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6) AppleWebKit/605.1.15 Version/18.0 Safari/605.1.15'), 'safari');
    assert.strictEqual(C.browserFamily('Mozilla/5.0 (iPhone; CPU iPhone OS 18_0) AppleWebKit/605.1.15 CriOS/141.0 Mobile/15E148 Safari/604.1'), 'chrome');
    assert.strictEqual(C.browserFamily('Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 SamsungBrowser/26.0 Chrome/122.0 Mobile Safari/537.36'), 'samsung');
    assert.strictEqual(C.browserFamily('Opera/9.80 (Windows NT 6.1) Presto/2.12 Version/12.16'), 'opera');
    assert.strictEqual(C.browserFamily('curl/8.0'), 'other');
    assert.strictEqual(C.browserFamily('', [{ brand: 'Microsoft Edge' }]), 'edge');
  });

  await test('the system and the device are told apart coarsely', () => {
    assert.strictEqual(C.osFamily('Mozilla/5.0 (Windows NT 10.0; Win64; x64)', 0), 'windows');
    assert.strictEqual(C.osFamily('Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6)', 0), 'macos');
    assert.strictEqual(C.osFamily('Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6)', 5), 'ios');
    assert.strictEqual(C.osFamily('Mozilla/5.0 (Linux; Android 14; Pixel 8)', 5), 'android');
    assert.strictEqual(C.osFamily('Mozilla/5.0 (X11; CrOS x86_64 15000.0.0)', 0), 'chromeos');
    assert.strictEqual(C.osFamily('Mozilla/5.0 (X11; Linux x86_64)', 0), 'linux');
    assert.strictEqual(C.deviceKind('Mozilla/5.0 (Linux; Android 14; Pixel 8) Mobile Safari', undefined, 5), 'mobile');
    assert.strictEqual(C.deviceKind('Mozilla/5.0 (Linux; Android 14; SM-X710) Safari', undefined, 5), 'tablet');
    assert.strictEqual(C.deviceKind('Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6)', false, 5), 'tablet');
    assert.strictEqual(C.deviceKind('Mozilla/5.0 (Windows NT 10.0)', true, 0), 'mobile');
    assert.strictEqual(C.deviceKind('Mozilla/5.0 (Windows NT 10.0)', false, 0), 'desktop');
    assert.strictEqual(C.screenBucket(1920, 1080), '1920-2559');
    assert.strictEqual(C.screenBucket(390, 844), '<1024');
    assert.strictEqual(C.screenBucket(1366, 768), '1366-1599');
    assert.strictEqual(C.screenBucket(3840, 2160), '2560+');
    assert.strictEqual(C.screenBucket(0, 0), 'other');
  });

  await test('the counting address comes from relay.txt and nowhere else', () => {
    assert.strictEqual(C.apiFrom('wss://relay.example/relay\n'), 'https://relay.example/api/ev');
    assert.strictEqual(C.apiFrom('https://relay.example/relay/'), 'https://relay.example/api/ev');
    assert.strictEqual(C.apiFrom('ws://127.0.0.1:8811/relay'), 'http://127.0.0.1:8811/api/ev');
    assert.strictEqual(C.apiFrom(''), '');
    assert.strictEqual(C.apiFrom('<html>not found</html>'), '');
    assert.strictEqual(C.apiFrom('javascript:alert(1)'), '');
  });

  await test('do not track, privacy control and the opt out each send nothing', async () => {
    for (const opts of [{ nav: { doNotTrack: '1' } }, { win: { doNotTrack: '1' } },
                        { nav: { globalPrivacyControl: true } }]) {
      const b = browser(opts);
      const c = C.create(b.env);
      c.view('landing'); c.step('visit'); c.error('boom', true);
      c.game('start', { mode: 'skirmish' });
      await tick(); b.run(); c.flush();
      assert.deepStrictEqual(b.sent, []);
      assert.strictEqual(b.env.local.getItem('ok.counts.id'), null);
      assert.strictEqual(c.state().counting, false);
    }
    const local = storage();
    local.setItem('ok.counts.off', '1');
    const b = browser({ local });
    const c = C.create(b.env);
    c.view('landing');
    await tick(); b.run(); c.flush();
    assert.deepStrictEqual(b.sent, []);
    assert.deepStrictEqual(c.state(), { counting: false, why: 'off' });
  });

  await test('a visit is one batch with the id, the device and its events', async () => {
    const b = browser();
    const c = C.create(b.env);
    c.view('landing');
    c.step('visit');
    c.step('files');
    c.game('start', { mode: 'skirmish', map: 'Two Rivers', ai: 2, lvl: 'hard', rb: 1, side: 'zhon',
                      mod: 'vanilla', secret: 'not sent' });
    c.game('end', { mode: 'skirmish', out: 'win', secs: 754.6 });
    await tick();
    assert.strictEqual(b.sent.length, 0);
    b.run();
    assert.strictEqual(b.sent.length, 1);
    const s = b.sent[0];
    assert.strictEqual(s.url, 'https://relay.example/api/ev');
    assert.strictEqual(s.body.v, 1);
    assert.match(s.body.id, /^[A-Za-z0-9_-]{22}$/);
    assert.strictEqual(s.body.nv, 1);
    assert.deepStrictEqual(s.body.env, { br: 'chrome', os: 'windows', dev: 'desktop', scr: '1920-2559' });
    assert.deepStrictEqual(s.body.ev, [
      { k: 'view', page: 'landing', s: 1 },
      { k: 'step', step: 'visit' },
      { k: 'step', step: 'files' },
      { k: 'start', mode: 'skirmish', map: 'Two Rivers', ai: 2, lvl: 'hard', rb: 1, mod: 'vanilla', side: 'zhon' },
      { k: 'step', step: 'battle' },
      { k: 'end', mode: 'skirmish', out: 'win', secs: 754 }
    ]);
    /* The id stays in this browser and the next load is not new. */
    const id = b.env.local.getItem('ok.counts.id');
    assert.strictEqual(id, s.body.id);
    const again = browser({ local: b.env.local, session: b.env.session });
    const c2 = C.create(again.env);
    c2.view('play');
    c2.step('files');
    await tick(); again.run();
    assert.strictEqual(again.sent[0].body.id, id);
    assert.strictEqual(again.sent[0].body.nv, undefined);
    /* Same tab: no new session, and a step it reached is not sent again. */
    assert.deepStrictEqual(again.sent[0].body.ev, [{ k: 'view', page: 'play', s: 0 }]);
  });

  await test('a long visit is cut into batches under both of the server caps', () => {
    const head = { v: 1, id: 'x'.repeat(22), env: { br: 'chrome', os: 'windows', dev: 'desktop', scr: '<1024' } };
    const many = [];
    for (let i = 0; i < 60; i++) many.push({ k: 'error', msg: 'm'.repeat(110) + i, fatal: 0 });
    const out = C.bodies(head, many);
    assert.ok(out.length >= 3);
    let total = 0;
    for (const b of out) {
      assert.ok(b.length <= C.MAX_BODY, 'a body of ' + b.length);
      const ev = JSON.parse(b).ev;
      assert.ok(ev.length >= 1 && ev.length <= C.MAX_EVENTS);
      total += ev.length;
    }
    assert.strictEqual(total, 60);
    /* The cap is the server's, in bytes, and text outside ASCII takes
       more of them than it has characters. */
    const wide = [];
    for (let i = 0; i < 40; i++) wide.push({ k: 'error', msg: 'é中'.repeat(50) + i, fatal: 0 });
    for (const b of C.bodies(head, wide)) {
      assert.ok(Buffer.byteLength(b, 'utf8') <= C.MAX_BODY, 'a body of ' + Buffer.byteLength(b, 'utf8') + ' bytes');
    }
    assert.strictEqual(C.bodies(head, wide).reduce((n, b) => n + JSON.parse(b).ev.length, 0), 40);
  });

  await test('with no server named nothing is sent and nothing throws', async () => {
    for (const make of [() => Promise.resolve(''), () => Promise.reject(new Error('offline')), () => undefined]) {
      const b = browser({ relayText: make() });
      const c = C.create(b.env);
      c.view('landing');
      c.error('boom', true);
      await tick(); b.run(); c.flush();
      assert.deepStrictEqual(b.sent, []);
    }
  });

  await test('a beacon that fails falls back to fetch, and a fetch that fails is ignored', async () => {
    const posts = [];
    const b = browser({
      nav: { sendBeacon: () => { throw new Error('blocked'); } },
      win: { fetch: (url, o) => { posts.push({ url, o }); return Promise.reject(new Error('down')); } }
    });
    const c = C.create(b.env);
    c.view('mods');
    await tick(); b.run();
    assert.strictEqual(posts.length, 1);
    assert.strictEqual(posts[0].o.keepalive, true);
    assert.strictEqual(posts[0].o.mode, 'no-cors');
    assert.strictEqual(posts[0].o.credentials, 'omit');
    await tick();
  });

  await test('errors are cleaned, sent once each and few to a page', async () => {
    assert.strictEqual(C.cleanMessage('RuntimeError: memory access out of bounds\n    at https://openkingdoms.net/tak-re.wasm:1:2'),
                       'RuntimeError: memory access out of bounds');
    assert.strictEqual(C.cleanMessage('failed to fetch https://example.org/x?join=ABC123 now'), 'failed to fetch url now');
    assert.strictEqual(C.cleanMessage(new Error('boom')), 'boom');
    assert.strictEqual(C.cleanMessage(''), 'unknown');
    assert.strictEqual(C.cleanMessage('x'.repeat(500)).length, 120);
    const b = browser();
    const c = C.create(b.env);
    for (let i = 0; i < 3; i++) c.error('the same fault', false);
    for (let i = 0; i < 10; i++) c.error('fault ' + i, false);
    c.game('error', { msg: 'from the engine' });
    await tick(); b.run();
    const ev = b.sent.flatMap((s) => s.body.ev);
    assert.strictEqual(ev.length, 5);
    assert.deepStrictEqual(ev[0], { k: 'error', msg: 'the same fault', fatal: 0 });
    /* A fatal one goes at once, the page may be about to go. */
    const b2 = browser();
    const c2 = C.create(b2.env);
    await tick();
    c2.error('Aborted(OOM)', true);
    assert.strictEqual(b2.sent.length, 1);
    assert.deepStrictEqual(b2.sent[0].body.ev, [{ k: 'error', msg: 'Aborted(OOM)', fatal: 1 }]);
  });

  await test('a page closed in the middle of a battle ends it as left', async () => {
    let clock = 1000;
    const b = browser();
    b.env.now = () => clock;
    const c = C.create(b.env);
    await tick();
    c.game('start', { mode: 'skirmish', map: 'Two Rivers' });
    clock += 61500;
    c.leave();
    const ev = b.sent.flatMap((s) => s.body.ev);
    assert.deepStrictEqual(ev[ev.length - 1], { k: 'end', mode: 'skirmish', out: 'left', secs: 61 });
    /* One the engine saw end is not ended twice. */
    const b2 = browser();
    const c2 = C.create(b2.env);
    await tick();
    c2.game('start', { mode: 'campaign', camp: 'Aramon', mission: 'aram01' });
    c2.game('end', { mode: 'campaign', out: 'win', secs: 900 });
    c2.leave();
    const ev2 = b2.sent.flatMap((s) => s.body.ev);
    assert.strictEqual(ev2.filter((e) => e.k === 'end').length, 1);
  });

  await test('opting out stops the counting and forgets the id', async () => {
    const b = browser();
    const c = C.create(b.env);
    c.view('privacy');
    await tick(); b.run();
    assert.strictEqual(b.sent.length, 1);
    c.optOut();
    assert.strictEqual(b.env.local.getItem('ok.counts.id'), null);
    assert.strictEqual(b.env.local.getItem('ok.counts.off'), '1');
    c.view('privacy');
    c.error('after', true);
    b.run(); c.flush();
    assert.strictEqual(b.sent.length, 1);
    assert.deepStrictEqual(c.state(), { counting: false, why: 'off' });
    c.optIn();
    assert.strictEqual(b.env.local.getItem('ok.counts.off'), null);
    assert.strictEqual(C.create(browser({ local: b.env.local }).env).state().counting, true);
  });

  await test('storage that throws costs the counts, never the page', async () => {
    const angry = { getItem: () => { throw new Error('denied'); }, setItem: () => { throw new Error('denied'); },
                    removeItem: () => { throw new Error('denied'); } };
    const b = browser({ local: angry, session: angry });
    const c = C.create(b.env);
    c.view('landing');
    c.view('play');
    c.step('menu');
    c.step('menu');
    c.optOut();
    c.optIn();
    await tick(); b.run();
    assert.strictEqual(b.sent.length, 0);
    const b2 = browser({ local: angry, session: angry });
    const c2 = C.create(b2.env);
    c2.view('landing');
    c2.view('play');
    await tick(); b2.run();
    assert.strictEqual(b2.sent.length, 1);
    assert.deepStrictEqual(b2.sent[0].body.ev, [{ k: 'view', page: 'landing', s: 1 }, { k: 'view', page: 'play', s: 0 }]);
    assert.strictEqual(b2.sent[0].body.nv, 1);
  });

  console.log(failed ? failed + ' failed' : 'all passed');
  process.exit(failed ? 1 : 0);
})();
