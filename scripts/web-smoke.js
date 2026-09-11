/* Browser smoke test for the bring-your-own-files flow.
 *
 * Drives Edge (or Chrome) with Playwright against a served engine build,
 * feeds it the archives (and Music/) from a real game install, and checks:
 *   1. first visit: picker, files accepted, Start button, engine boots to
 *      the main menu with no init failures
 *   2. reload: boots from the OPFS cache with no picker interaction
 *   3. ?args=--skirmish: a skirmish loads, the window title reports
 *      "In Game", and the frame is not black (needs python + Pillow for
 *      the pixel check, otherwise it only screenshots)
 *   4. forget: cache cleared, picker returns
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

(async () => {
  const hpis = fs.readdirSync(gameDir).filter(n => /\.hpi$/i.test(n)).map(n => path.join(gameDir, n));
  if (!hpis.length) return fail('no .hpi files in ' + gameDir);
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
  console.log('   picker visible, feeding ' + hpis.length + ' archives');
  let mark = log.length;
  await page.setInputFiles('#hpi-input', hpis);
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

  /* 3. skirmish: load a map and check the frame is not black */
  console.log('3. skirmish (--skirmish)');
  mark = log.length;
  const sep = url.includes('?') ? '&' : '?';
  await page.goto(url + sep + 'args=--skirmish', { waitUntil: 'load' });
  await pressStart();
  await booted();
  await page.waitForFunction(() => /In Game/.test(document.title), null, { timeout: BOOT_TIMEOUT });
  console.log('   in game: ' + await page.title());
  await page.waitForTimeout(6000);
  bad = fatalLines(mark);
  if (bad.length) return fail('engine reported a failure in the skirmish: ' + bad[0], 'skirmish');
  const shot = path.join(outDir, '3-skirmish.png');
  await page.screenshot({ path: shot });
  const lit = litFraction(shot);
  if (lit < 0) console.log('   (no python+Pillow: skipped the black-frame check, see 3-skirmish.png)');
  else if (lit < 0.15) return fail('skirmish frame is ' + Math.round(lit * 100) + '% lit: looks black', 'skirmish');
  else console.log('   frame is ' + Math.round(lit * 100) + '% lit, terrain is drawing');

  /* 4. forget: cache cleared, picker returns */
  console.log('4. forget my files');
  await page.click('#forget-link');
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  const cached = await page.evaluate(async () => {
    try { const r = await navigator.storage.getDirectory(); await r.getDirectoryHandle('game'); return true; } catch (e) { return false; }
  });
  if (cached) return fail('cache still present after forget', 'forget');
  console.log('   picker is back and the cache is gone');

  /* 5. whole folder through the directory input: Music/ must come along */
  console.log('5. game folder (with Music/)');
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
  await page.click('#forget-link');
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });

  saveLog();
  await ctx.close();
  console.log('PASS');
})().catch(e => fail(e.stack || String(e)));
