/* Browser smoke test for the bring-your-own-files flow.
 *
 * Drives Edge (or Chrome) with Playwright against a served engine build,
 * feeds it the archives, the Maps/ packs and Music/ from a real game
 * install, and checks:
 *   1. first visit: picker, files accepted, Start button, engine boots to
 *      the main menu with no init failures
 *   2. reload: boots from the OPFS cache with no picker interaction
 *   3. settings: a file written under the preference directory comes
 *      back after a reload, which is what makes options.cfg persist
 *   4. ?args=--skirmish: a skirmish loads, the window title reports
 *      "In Game", and the frame is not black (needs python + Pillow for
 *      the pixel check, otherwise it only screenshots)
 *   5. forget: game cache cleared, picker returns, settings kept
 *   6. whole folder: the Music/ tracks and the Maps/ packs come along
 *   7. clips: the folder's Movies/ clips are mounted in place, the logo
 *      reel plays at startup, a hovered door's pixels change over time
 *      and the Credits door plays the credits reel (pixel checks need
 *      python + Pillow, otherwise they only screenshot)
 * Screenshots and the console log land in the output directory.
 *
 *   node scripts/web-smoke.js [url] [gameDir]
 *     url      default http://localhost:8081/tak-re.html
 *     gameDir  default C:/GOG Games/Total Annihilation Kingdoms
 *
 * Needs `playwright` resolvable (npm i playwright; no browser download is
 * needed when Edge or Chrome is installed). Not part of CI: it needs game
 * data.
 */
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { chromium } = require('playwright');

const url = process.argv[2] || 'http://localhost:8081/tak-re.html';
const gameDir = process.argv[3] || 'C:/GOG Games/Total Annihilation Kingdoms';
const outDir = process.env.WEB_SMOKE_OUT || path.join(process.cwd(), 'web-smoke-out');
const BOOT_TIMEOUT = 240000;
const FATAL = /Failed to initialize|VFS_Init: cannot|abort\(|Aborted\(|RuntimeError|PAGEERROR|autostart could not/;

const log = [];
let page = null;

function saveLog() {
  fs.writeFileSync(path.join(outDir, 'console.log'), log.join('\n'));
}
async function fail(msg, tag) {
  try { if (page) await page.screenshot({ path: path.join(outDir, 'fail-' + (tag || 'error') + '.png') }); } catch (e) {}
  saveLog();
  console.log('--- last console lines ---');
  log.slice(-15).forEach(l => console.log('  ' + l));
  console.error('FAIL: ' + msg);
  process.exit(1);
}

/* main() has run once the picker is gone and SDL has sized the canvas. */
function booted() {
  return page.waitForFunction(() =>
    document.getElementById('picker').hidden &&
    window.Module && window.Module.canvas && window.Module.canvas.width > 0,
    null, { timeout: BOOT_TIMEOUT });
}
function fatalLines(since) {
  return log.slice(since).filter(l => FATAL.test(l));
}
async function pressStart() {
  await page.waitForSelector('#btn-start:visible', { timeout: BOOT_TIMEOUT });
  await page.click('#btn-start');
}

/* Fraction of pixels brighter than a dark threshold, via Pillow. Returns
 * -1 when python or Pillow is unavailable. */
function litFraction(png) {
  const script = [
    'import sys',
    'from PIL import Image',
    'im = Image.open(sys.argv[1]).convert("L")',
    'h = im.histogram()',
    'total = sum(h)',
    'lit = sum(h[40:])',
    'print(lit / total if total else 0)',
  ].join('\n');
  for (const py of ['python', 'python3', 'py']) {
    try {
      const out = execFileSync(py, ['-c', script, png], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
      return parseFloat(out.trim());
    } catch (e) { /* try the next interpreter */ }
  }
  return -1;
}

/* Fraction of pixels that differ between two screenshots by more than
 * a little, via Pillow. -1 when python or Pillow is unavailable. */
function diffFraction(a, b) {
  const script = [
    'import sys',
    'from PIL import Image, ImageChops',
    'a = Image.open(sys.argv[1]).convert("RGB")',
    'b = Image.open(sys.argv[2]).convert("RGB")',
    'd = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 16 else 0)',
    'h = d.histogram()',
    'print(h[255] / float(sum(h)))',
  ].join('\n');
  for (const py of ['python', 'python3', 'py']) {
    try {
      const out = execFileSync(py, ['-c', script, a, b], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
      return parseFloat(out.trim());
    } catch (e) { /* try the next interpreter */ }
  }
  return -1;
}

/* A point on the 640x480 menu canvas in page coordinates. The canvas is
 * stretched over the whole window, so each axis has its own scale. */
function menuToPage(mx, my) {
  return page.evaluate(([mx, my]) => {
    const r = window.Module.canvas.getBoundingClientRect();
    return { x: r.left + mx * r.width / 640, y: r.top + my * r.height / 480, kx: r.width / 640, ky: r.height / 480 };
  }, [mx, my]);
}

/* The menu reads the mouse between ticks, so a move lands with a nudge
 * behind it and a press outlasts a tick. */
async function hoverMenu(mx, my) {
  const p = await menuToPage(mx, my);
  await page.mouse.move(p.x, p.y);
  await page.waitForTimeout(100);
  await page.mouse.move(p.x + 1, p.y + 1);
}
async function pressMouse() {
  await page.mouse.down();
  await page.waitForTimeout(150);
  await page.mouse.up();
}
async function pressKey(key) {
  await page.keyboard.down(key);
  await page.waitForTimeout(150);
  await page.keyboard.up(key);
}
async function waitLog(since, re, ms) {
  const t0 = Date.now();
  for (;;) {
    const hit = log.slice(since).find(l => re.test(l));
    if (hit || Date.now() - t0 > ms) return hit || null;
    await page.waitForTimeout(200);
  }
}

(async () => {
  const hpis = fs.readdirSync(gameDir).filter(n => /\.hpi$/i.test(n)).map(n => path.join(gameDir, n));
  if (!hpis.length) return fail('no .hpi files in ' + gameDir);
  /* The map packs live in Maps/ and the picker takes them too. */
  const mapsDir = path.join(gameDir, 'Maps');
  const kmps = fs.existsSync(mapsDir)
    ? fs.readdirSync(mapsDir).filter(n => /\.kmp$/i.test(n)).map(n => path.join(mapsDir, n))
    : [];
  fs.mkdirSync(outDir, { recursive: true });

  const profile = path.join(outDir, 'profile');
  fs.rmSync(profile, { recursive: true, force: true });
  const ctx = await chromium.launchPersistentContext(profile, {
    channel: process.env.WEB_SMOKE_CHANNEL || 'msedge',
    headless: true,
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
  });
  page = await ctx.newPage();
  page.on('console', m => {
    const t = m.text();
    log.push(t);
    if (/openkingdoms|Failed|abort|VFS_Init|BattleSetup: found|autostart|Music: (found|playing)/i.test(t)) console.log('  >', t);
  });
  page.on('pageerror', e => { log.push('PAGEERROR ' + e.message); console.log('  > PAGEERROR', e.message); });

  /* 1. first visit: picker shows, feed the archives, press Start, boot */
  console.log('1. first visit');
  await page.goto(url, { waitUntil: 'load' });
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  console.log('   picker visible, feeding ' + hpis.length + ' archives and ' + kmps.length + ' map packs');
  let mark = log.length;
  await page.setInputFiles('#hpi-input', hpis.concat(kmps));
  await pressStart();
  await booted();
  await page.waitForTimeout(5000);
  let bad = fatalLines(mark);
  if (bad.length) return fail('engine reported a failure after boot: ' + bad[0], 'first-boot');
  const size = await page.evaluate(() => [window.Module.canvas.width, window.Module.canvas.height]);
  console.log('   engine running, canvas ' + size.join('x') + ', no init failures');
  await page.screenshot({ path: path.join(outDir, '1-first-boot.png') });

  /* 2. reload: must boot from the OPFS cache with no picker interaction */
  console.log('2. reload from cache');
  mark = log.length;
  await page.reload({ waitUntil: 'load' });
  await pressStart();
  await booted();
  await page.waitForTimeout(3000);
  if (!log.slice(mark).some(t => /loaded \d+ file\(s\) from browser storage/.test(t)))
    return fail('reload did not boot from browser storage', 'cached-boot');
  bad = fatalLines(mark);
  if (bad.length) return fail('engine reported a failure after cached boot: ' + bad[0], 'cached-boot');
  console.log('   booted from browser storage, no init failures');
  await page.screenshot({ path: path.join(outDir, '2-cached-boot.png') });

  /* 3. settings: the preference directory is an in-memory one that dies
     with the tab, so the page mirrors it to origin private storage on
     write and back on load. Written through the same directory the
     engine's Settings_Save writes, so this exercises the mirror rather
     than a fixture. */
  console.log('3. settings survive a reload');
  const PREFDIR = '/libsdl/OpenKingdoms/OpenKingdoms';
  const marker = 'DisplayDamageBars=1\nWebSmokeMarker=4242\n';
  await page.evaluate(async ([dir, text]) => {
    window.Module.FS.writeFile(dir + '/options.cfg', text);
    window.Module.syncPrefs();
    /* Wait for the write to land rather than guessing at a delay. */
    for (let i = 0; i < 100; i++) {
      try {
        const root = await navigator.storage.getDirectory();
        const d = await root.getDirectoryHandle('prefs');
        const f = await d.getFileHandle('options.cfg');
        if ((await (await f.getFile()).text()) === text) return;
      } catch (e) { /* not there yet */ }
      await new Promise(r => setTimeout(r, 100));
    }
    throw new Error('settings were never written to browser storage');
  }, [PREFDIR, marker]);

  mark = log.length;
  await page.reload({ waitUntil: 'load' });
  await pressStart();
  await booted();
  const restored = await page.evaluate((dir) => {
    try { return new TextDecoder().decode(window.Module.FS.readFile(dir + '/options.cfg')); }
    catch (e) { return null; }
  }, PREFDIR);
  if (restored !== marker) return fail('settings did not come back after a reload: ' + JSON.stringify(restored), 'settings');
  console.log('   options.cfg came back from browser storage');

  /* 4. skirmish: load a map and check the frame is not black */
  console.log('4. skirmish (--skirmish)');
  mark = log.length;
  const sep = url.includes('?') ? '&' : '?';
  await page.goto(url + sep + 'args=--skirmish', { waitUntil: 'load' });
  await pressStart();
  await booted();
  await page.waitForFunction(() => /In Game/.test(document.title), null, { timeout: BOOT_TIMEOUT });
  console.log('   in game: ' + await page.title());
  await page.waitForTimeout(6000);
  /* One css pixel is one world pixel only while the canvas element is
   * the size of its css box. SDL scales a click by element over box,
   * so a box that drifted from the element would put every click off
   * by that ratio, growing away from the corner. */
  const box = await page.evaluate(() => {
    const c = window.Module.canvas, r = c.getBoundingClientRect();
    return { w: c.width, h: c.height, cssW: Math.round(r.width), cssH: Math.round(r.height) };
  });
  if (box.w !== box.cssW || box.h !== box.cssH)
    return fail('canvas element ' + box.w + 'x' + box.h + ' is not its css box ' + box.cssW + 'x' + box.cssH + ': clicks would land off by that ratio', 'canvas-box');
  console.log('   canvas element ' + box.w + 'x' + box.h + ' is its css box');
  /* Every map the player handed over has to be in the chooser, map
   * packs included. */
  const counts = log.slice(mark)
    .map(t => (t.match(/BattleSetup: found (\d+) maps/) || [])[1])
    .filter(Boolean).map(Number);
  const nmaps = counts.length ? Math.max.apply(null, counts) : 0;
  if (kmps.length && nmaps <= kmps.length)
    return fail('the chooser lists ' + nmaps + ' maps and the install has ' + kmps.length + ' map packs alone', 'maps');
  console.log('   chooser lists ' + nmaps + ' maps');
  bad = fatalLines(mark);
  if (bad.length) return fail('engine reported a failure in the skirmish: ' + bad[0], 'skirmish');
  const shot = path.join(outDir, '3-skirmish.png');
  await page.screenshot({ path: shot });
  const lit = litFraction(shot);
  if (lit < 0) console.log('   (no python+Pillow: skipped the black-frame check, see 3-skirmish.png)');
  else if (lit < 0.15) return fail('skirmish frame is ' + Math.round(lit * 100) + '% lit: looks black', 'skirmish');
  else console.log('   frame is ' + Math.round(lit * 100) + '% lit, terrain is drawing');

  /* 5. forget: the game cache goes, the settings stay. They live in a
     sibling directory at the storage root, so the forget link cannot
     reach them. */
  console.log('5. forget my files');
  await page.click('#forget-link');
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  const cached = await page.evaluate(async () => {
    try { const r = await navigator.storage.getDirectory(); await r.getDirectoryHandle('game'); return true; } catch (e) { return false; }
  });
  if (cached) return fail('cache still present after forget', 'forget');
  const keptSettings = await page.evaluate(async () => {
    try {
      const r = await navigator.storage.getDirectory();
      const d = await r.getDirectoryHandle('prefs');
      const f = await d.getFileHandle('options.cfg');
      return (await (await f.getFile()).text()).includes('WebSmokeMarker=4242');
    } catch (e) { return false; }
  });
  if (!keptSettings) return fail('forget my game files also deleted the settings', 'forget');
  console.log('   picker is back, the cache is gone and the settings are kept');

  /* 6. whole folder through the directory input: Music/ must come along */
  console.log('6. game folder (with Music/)');
  mark = log.length;
  await page.setInputFiles('#dir-input', gameDir);
  await pressStart();
  await booted();
  await page.waitForTimeout(3000);
  const music = log.slice(mark).find(t => /Music: found (\d+) tracks/.test(t));
  const ntracks = music ? parseInt(music.match(/found (\d+)/)[1], 10) : 0;
  const expected = fs.existsSync(path.join(gameDir, 'Music')) ? fs.readdirSync(path.join(gameDir, 'Music')).filter(n => /\.wav$/i.test(n)).length : 0;
  if (expected > 0 && ntracks === 0) return fail('folder pick brought no music tracks (install has ' + expected + ')', 'music');
  console.log('   music tracks found by the engine: ' + ntracks + ' (install has ' + expected + ')');
  const packLine = log.slice(mark).find(t => /map pack\(s\)/.test(t));
  const npacks = packLine ? parseInt(packLine.match(/(\d+) map pack/)[1], 10) : 0;
  if (kmps.length && npacks !== kmps.length)
    return fail('the folder pick brought ' + npacks + ' map packs, the install has ' + kmps.length, 'maps');
  console.log('   map packs taken from the folder: ' + npacks);
  if (expected > 0) {
    /* A found track must also start, and the page's audio context must
     * be running after the Start click, or nothing is heard. */
    let playing = null;
    for (let i = 0; i < 20 && !playing; i++) {
      playing = log.slice(mark).find(t => /Music: playing /.test(t));
      if (!playing) await page.waitForTimeout(500);
    }
    if (!playing) return fail('music tracks were found but no track started', 'music');
    const audio = await page.evaluate(() => {
      const ma = window.miniaudio;
      const devs = ma && ma.devices ? ma.devices.filter(Boolean) : [];
      return devs.map(d => d.webaudio ? d.webaudio.state : 'none');
    });
    if (!audio.includes('running')) return fail('a music track started but the audio context is ' + (audio.join(',') || 'missing'), 'music');
    console.log('   ' + playing.trim() + ', audio context running');
  }

  /* 7. clips: back from browser storage read in place, the logo reel,
     a hovered door's pixels moving, the Credits reel. Step 6 booted
     with --skirmish, which skips the logo, so this boots the menu. */
  console.log('7. clips (logo, doors, credits)');
  /* The line goes on to count models after the clips. */
  const picked = log.slice(mark).find(t => /(\d+) clip\(s\).* ready/.test(t));
  const npicked = picked ? parseInt(picked.match(/(\d+) clip\(s\)/)[1], 10) : 0;
  const moviesDir = path.join(gameDir, 'Movies');
  if (fs.existsSync(moviesDir)) {
    if (!npicked) return fail('the folder pick brought no clips, the install has a Movies folder', 'clips');
    console.log('   clips mounted from the folder: ' + npicked);
    /* The campaign's own, one before a mission and one after some. */
    const forMissions = fs.readdirSync(moviesDir).filter(n => /^(post)?tak(mission|x).*\.bik$/i.test(n)).length;
    if (forMissions && npicked < forMissions) {
      return fail('the campaign clips did not come with the folder: ' + npicked + ' picked, the install has ' + forMissions + ' for missions', 'clips');
    }
    if (forMissions) console.log('   of which for missions: ' + forMissions);
    mark = log.length;
    await page.goto(url, { waitUntil: 'load' });
    await pressStart();
    await booted();
    const cached = log.slice(mark).find(t => /(\d+) clip\(s\) read in place from browser storage/.test(t));
    const nclips = cached ? parseInt(cached.match(/(\d+) clip\(s\)/)[1], 10) : 0;
    if (nclips !== npicked) return fail('browser storage gave back ' + nclips + ' clips of the ' + npicked + ' picked', 'clips');
    console.log('   clips read in place from browser storage: ' + nclips);
    /* One of the campaign's, as the engine will see it: there, and the
       size it is on disk, without having been read into memory. */
    const first = fs.readdirSync(moviesDir).find(n => /^takmission01_mt[.]bik$/i.test(n));
    if (first) {
      const onDisk = fs.statSync(path.join(moviesDir, first)).size;
      const seen = await page.evaluate(() => {
        try { return FS.stat('/game/Movies/takmission01_mt.bik').size; } catch (e) { return -1; }
      });
      if (seen !== onDisk) return fail('the first mission clip is ' + seen + ' bytes to the engine and ' + onDisk + ' on disk', 'clips');
      console.log('   the first mission clip is there for the engine: ' + seen + ' bytes');
    }
    const logo = await waitLog(mark, /Credits: playing Movies\/logo\.bik/, 30000);
    if (!logo) return fail('the logo reel did not play at startup', 'clips');
    const opened = log.slice(mark).find(t => /BinkPlayer: opened .*logo\.bik/.test(t)) || '';
    console.log('   ' + logo.trim() + (opened ? ' (' + opened.replace(/.* in /, 'opened in ') + ')' : ''));
    const menuUp = await waitLog(mark, /MainMenu: Bink videos available for machine/, 90000);
    if (!menuUp) return fail('the menu did not open its door clips after the logo', 'clips');
    await page.waitForTimeout(2000);

    /* The machine door's clip is drawn at the .gui rect 40,192 at its
       own 160x195. At rest the door is a still, hovered it moves. */
    const tl = await menuToPage(40, 192);
    const clip = { x: tl.x, y: tl.y, width: 160 * tl.kx, height: 195 * tl.ky };
    const shot = async (name) => {
      const p = path.join(outDir, name + '.png');
      await page.screenshot({ path: p, clip });
      return p;
    };
    const rest = [await shot('7-door-rest-0')];
    await page.waitForTimeout(300);
    rest.push(await shot('7-door-rest-1'));
    await hoverMenu(120, 300);
    const hov = [];
    for (let i = 0; i < 6; i++) {
      hov.push(await shot('7-door-hover-' + i));
      await page.waitForTimeout(250);
    }
    const restDiff = diffFraction(rest[0], rest[1]);
    if (restDiff < 0) {
      console.log('   (no python+Pillow: skipped the door pixel check, see 7-door-*.png)');
    } else {
      const diffs = [];
      for (let i = 1; i < hov.length; i++) diffs.push(diffFraction(hov[i - 1], hov[i]));
      const moving = diffs.filter(d => d > 0.02).length;
      const vsRest = diffFraction(rest[0], hov[hov.length - 1]);
      console.log('   door pixels changed: at rest ' + restDiff.toFixed(3) + ', hovered ' +
                  diffs.map(d => d.toFixed(3)).join(' ') + ', last hovered frame vs rest ' + vsRest.toFixed(3));
      if (restDiff > 0.01) return fail('the door changes at rest, so the check cannot tell a clip from noise', 'clips');
      if (moving < 3 || vsRest < 0.05) return fail('hovering the door did not play its clip', 'clips');
    }

    /* The Credits door, then a key ends the reel. */
    await hoverMenu(160, 100);
    await page.waitForTimeout(1500);
    const before = log.length;
    await pressMouse();
    const credits = await waitLog(before, /Credits: playing Movies\/Credits\.bik/i, 20000);
    if (!credits) return fail('clicking the Credits door did not play the credits reel', 'clips');
    console.log('   ' + credits.trim());
    /* The reel is pages that hold for seconds, so it is sampled until a
       page turns. */
    await page.waitForTimeout(2000);
    let prev = path.join(outDir, '7-credits-0.png');
    await page.screenshot({ path: prev });
    let reel = -1;
    for (let i = 1; i <= 20; i++) {
      await page.waitForTimeout(500);
      const cur = path.join(outDir, '7-credits-' + i + '.png');
      await page.screenshot({ path: cur });
      reel = diffFraction(prev, cur);
      if (reel < 0) break;
      if (reel > 0.02) { console.log('   credits page turned after ' + (2 + i * 0.5) + ' s, ' + reel.toFixed(3) + ' of the pixels changed'); break; }
      fs.unlinkSync(prev);
      prev = cur;
    }
    if (reel >= 0 && reel <= 0.02) return fail('the credits reel did not turn a page in 12 s', 'clips');
    const beforeKey = log.length;
    await pressKey('Escape');
    if (!await waitLog(beforeKey, /MainMenu: Bink videos available for machine/, 30000))
      return fail('a key did not end the credits reel', 'clips');
    console.log('   a key ended the reel and the menu is back');
    bad = fatalLines(mark);
    if (bad.length) return fail('engine reported a failure around the clips: ' + bad[0], 'clips');
  } else {
    console.log('   (no Movies folder in the install: skipped)');
  }

  await page.click('#forget-link');
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });

  saveLog();
  await ctx.close();
  console.log('PASS');
})().catch(e => fail(e.stack || String(e)));
