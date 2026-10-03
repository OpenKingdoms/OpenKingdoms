#!/usr/bin/env node
/*
 * A cached page takes a new deploy on its next visit, offline it starts
 * from the cache, and a page left open takes the deploy at a calm
 * screen and not in a room. Real browser, web/sw.js and the built page,
 * no game data. Each deploy is the same engine marked with its name.
 *
 *   NODE_PATH=<dir>/node_modules node scripts/sw-update-smoke.js <build>/src
 *
 * SW_SMOKE_WEB points at another web/ folder, SW_SMOKE_OUT at the
 * profile's parent (default: a fresh one under the temp folder).
 */
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { chromium } = require('playwright');

const ROOT = path.join(__dirname, '..');
const BUILD = process.argv[2];
const WEB = process.env.SW_SMOKE_WEB || path.join(ROOT, 'web');
if (!BUILD || !fs.existsSync(path.join(BUILD, 'tak-re.wasm'))) {
  console.error('usage: node scripts/sw-update-smoke.js <folder with tak-re.html, tak-re.js, tak-re.wasm>');
  process.exit(2);
}

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.txt': 'text/plain',
                '.jpg': 'image/jpeg', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };

/* The site as web.yml assembles it, marked as deploy tag. */
function deploy(tag) {
  const read = (dir, f) => fs.readFileSync(path.join(dir, f));
  const page = read(BUILD, 'tak-re.html').toString('utf8').split('@OK_SITE_BUILD@').join(tag);
  const name = Buffer.from('ok');
  const mark = Buffer.from(tag);
  const custom = Buffer.concat([Buffer.from([0, 1 + name.length + mark.length, name.length]), name, mark]);
  const files = {
    'index.html': Buffer.from(page),
    'tak-re.js': Buffer.concat([read(BUILD, 'tak-re.js'), Buffer.from('\n/* deploy ' + tag + ' */\n')]),
    'tak-re.wasm': Buffer.concat([read(BUILD, 'tak-re.wasm'), custom]),
    'version.txt': Buffer.from(tag + '\n'),
    'banner.jpg': read(path.join(ROOT, 'docs', 'img'), 'banner.jpg')
  };
  for (const f of ['sw.js', 'mods.js', 'scroll-pick.jpg', 'scroll-ready.jpg', 'favicon.png',
                   'manifest.webmanifest', 'icon-192.png', 'icon-512.png'])
    files[f] = read(WEB, f);
  return { tag, files };
}

/* Served the way GitHub Pages serves: ten minutes of max-age, an ETag. */
let site = null;
let offline = false;
const server = http.createServer((req, res) => {
  if (offline) { req.socket.destroy(); return; }
  let f = decodeURIComponent(new URL(req.url, 'http://x').pathname.replace(/^\/+/, '')) || 'index.html';
  const body = site.files[f];
  if (!body) { res.writeHead(404); res.end(); return; }
  const etag = '"' + site.tag + '-' + body.length.toString(16) + '"';
  const head = { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream',
                 'Cache-Control': 'max-age=600', ETag: etag };
  if (req.headers['if-none-match'] === etag) { res.writeHead(304, head); res.end(); return; }
  res.writeHead(200, head);
  res.end(body);
});

/* The deploy a page runs: its stamp, and the script and wasm it gets. */
async function running(page) {
  return page.evaluate(async () => {
    const meta = document.querySelector('meta[name="ok-build"]');
    const js = await (await fetch('tak-re.js')).text();
    const wasm = new Uint8Array(await (await fetch('tak-re.wasm')).arrayBuffer());
    const jsTag = (js.match(/\/\* deploy (\S+) \*\/\s*$/) || [])[1] || '?';
    const wasmTag = String.fromCharCode(wasm[wasm.length - 1]);
    return [meta ? meta.content : '?', jsTag, wasmTag].join(' ');
  });
}

let failed = 0;
function check(what, got, want) {
  if (got === want) console.log('ok   ' + what + ': ' + got);
  else { failed++; console.log('FAIL ' + what + ': ' + got + ', wanted ' + want); }
}

(async () => {
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const url = 'http://127.0.0.1:' + server.address().port + '/?sw=1';
  const out = process.env.SW_SMOKE_OUT || fs.mkdtempSync(path.join(os.tmpdir(), 'ok-sw-smoke-'));
  const profile = path.join(out, 'profile');
  fs.rmSync(profile, { recursive: true, force: true });
  const ctx = await chromium.launchPersistentContext(profile, {
    channel: process.env.WEB_SMOKE_CHANNEL || 'msedge', headless: true
  });
  try {
    site = deploy('A');
    let page = await ctx.newPage();
    await page.goto(url);
    await page.waitForFunction(() => !!navigator.serviceWorker.controller &&
      caches.keys().then((ks) => Promise.all(ks.map((k) => caches.open(k)
        .then((c) => c.match('./version.txt')).then((r) => (r ? r.text() : '')))))
        .then((ts) => ts.some((t) => t.trim() === 'A')), null, { timeout: 60000, polling: 500 });
    check('the first visit', await running(page), 'A A A');
    await page.close();

    site = deploy('B');
    page = await ctx.newPage();
    await page.goto(url);
    check('the visit after a deploy', await running(page), 'B B B');
    await page.close();

    offline = true;
    page = await ctx.newPage();
    await page.goto(url);
    offline = false;
    check('a visit with no network', await running(page), 'B B B');

    site = deploy('C');
    await page.evaluate(() => Module.onScreen('Multiplayer', 0, 0, 0, 0));
    await page.waitForTimeout(3000);
    check('an open page in a room', await running(page), 'B B B');
    const reloaded = page.waitForEvent('load', { timeout: 30000 });
    await page.evaluate(() => Module.onScreen('menu', 0, 0, 0, 0));
    await reloaded.catch(() => {});
    check('an open page back on the menu', await running(page), 'C C C');
  } catch (e) {
    failed++;
    console.log('FAIL ' + e.message);
  } finally {
    await ctx.close();
    server.close();
  }
  console.log(failed ? failed + ' failed' : 'PASS');
  process.exit(failed ? 1 : 0);
})();
