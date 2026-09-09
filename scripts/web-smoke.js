/* Browser smoke test for the bring-your-own-files flow.
 *
 * Drives Edge (or Chrome) with Playwright against a served engine build,
 * feeds it the archives from a real game install through the page's file
 * input, and checks that the engine boots to the main menu, that a reload
 * boots from the OPFS cache without the picker, and that "forget" brings
 * the picker back. Screenshots land in the output directory.
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
const { chromium } = require('playwright');

const url = process.argv[2] || 'http://localhost:8081/tak-re.html';
const gameDir = process.argv[3] || 'C:/GOG Games/Total Annihilation Kingdoms';
const outDir = process.env.WEB_SMOKE_OUT || path.join(process.cwd(), 'web-smoke-out');
const BOOT_TIMEOUT = 240000;
const FATAL = /Failed to initialize|VFS_Init: cannot|abort\(|Aborted\(|RuntimeError|PAGEERROR/;

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

(async () => {
  const hpis = fs.readdirSync(gameDir).filter(n => /\.hpi$/i.test(n)).map(n => path.join(gameDir, n));
  if (!hpis.length) return fail('no .hpi files in ' + gameDir);
  fs.mkdirSync(outDir, { recursive: true });

  const profile = path.join(outDir, 'profile');
  fs.rmSync(profile, { recursive: true, force: true });
  const ctx = await chromium.launchPersistentContext(profile, {
    channel: process.env.WEB_SMOKE_CHANNEL || 'msedge',
    headless: true,
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  page = await ctx.newPage();
  page.on('console', m => {
    const t = m.text();
    log.push(t);
    if (/openkingdoms|Failed|abort|VFS_Init|BattleSetup: found/i.test(t)) console.log('  >', t);
  });
  page.on('pageerror', e => { log.push('PAGEERROR ' + e.message); console.log('  > PAGEERROR', e.message); });

  /* 1. first visit: picker shows, feed the archives, engine boots */
  console.log('1. first visit');
  await page.goto(url, { waitUntil: 'load' });
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  console.log('   picker visible, feeding ' + hpis.length + ' archives');
  let mark = log.length;
  await page.setInputFiles('#hpi-input', hpis);
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
  await booted();
  await page.waitForTimeout(3000);
  if (!log.slice(mark).some(t => /loaded \d+ archive\(s\) from browser storage/.test(t)))
    return fail('reload did not boot from browser storage', 'cached-boot');
  bad = fatalLines(mark);
  if (bad.length) return fail('engine reported a failure after cached boot: ' + bad[0], 'cached-boot');
  console.log('   booted from browser storage, no init failures');
  await page.screenshot({ path: path.join(outDir, '2-cached-boot.png') });

  /* 3. forget: cache cleared, picker returns */
  console.log('3. forget my files');
  await page.click('#forget-link');
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  const cached = await page.evaluate(async () => {
    try { const r = await navigator.storage.getDirectory(); await r.getDirectoryHandle('game'); return true; } catch (e) { return false; }
  });
  if (cached) return fail('cache still present after forget', 'forget');
  console.log('   picker is back and the cache is gone');
  await page.screenshot({ path: path.join(outDir, '3-after-forget.png') });

  saveLog();
  await ctx.close();
  console.log('PASS');
})().catch(e => fail(e.stack || String(e)));
