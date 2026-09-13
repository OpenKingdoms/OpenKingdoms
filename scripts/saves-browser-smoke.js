/* Does the page keep a player's saved games, and does it stop reading
 * every one of them before the main menu draws?
 *
 * Three things, none of which a test without a browser can see.
 *
 *  - Booting no longer copies the saved games in. The engine used to
 *    wait for all of them before main() ran.
 *  - Asking for them brings them in, which is what the load and save
 *    dialogs do when they open.
 *  - The panel writes one out as a file and reads one back, which is
 *    the only way a save in this browser becomes a save the player
 *    owns.
 *
 * Not part of CI: it needs a browser, a served build and the player's
 * own game files.
 *
 *   python -m http.server 8082 -d <wasm build>/src
 *   node scripts/saves-browser-smoke.js <repo root>
 */
const fs = require('fs');
const path = require('path');
const os = require('os');
const root = process.argv[2] || '.';
const { chromium } = require(path.join(root, 'node_modules', 'playwright'));

const URL_BASE = process.argv[3] || 'http://localhost:8082/tak-re.html';
const GAME_DIR = process.argv[4] || 'C:/GOG Games/Total Annihilation Kingdoms';
const OUT = path.join(root, 'saves-shots');

function archives(dir) {
  return fs.readdirSync(dir)
    .filter(f => /\.(hpi|ufo|ccx|gpf|gp3)$/i.test(f))
    .map(f => path.join(dir, f));
}

let failures = 0;
function check(what, ok) {
  console.log((ok ? '  ok   ' : '  FAIL ') + what);
  if (!ok) failures++;
}

async function boot(page, log) {
  await page.goto(URL_BASE, { waitUntil: 'load' });
  await page.waitForSelector('#picker:not([hidden])', { timeout: 60000 });
  await page.setInputFiles('#hpi-input', archives(GAME_DIR));
  await page.waitForSelector('#btn-start:visible', { timeout: 60000 });
  await page.click('#btn-start');
  await page.waitForFunction(() =>
    document.getElementById('picker').hidden &&
    window.Module && window.Module.canvas && window.Module.canvas.width > 0,
    null, { timeout: 180000 });
  await page.waitForTimeout(3000);
}

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const ctx = await browser.newContext({ acceptDownloads: true });
  const page = await ctx.newPage();
  let log = [];
  page.on('console', m => log.push(m.text()));
  page.on('pageerror', e => log.push('PAGEERROR ' + e.message));

  console.log('first boot, to fill the caches');
  await boot(page, log);

  /* Two saved games, put where a real save would land. Real bytes are
   * not needed: nothing here opens one, and what is being measured is
   * whether the page reads them and when. */
  console.log('planting two saved games in storage');
  await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const prefs = await root.getDirectoryHandle('prefs', { create: true });
    const saves = await prefs.getDirectoryHandle('saves', { create: true });
    for (const name of ['Alpha.oksave', 'Bravo.oksave']) {
      const h = await saves.getFileHandle(name, { create: true });
      const w = await h.createWritable();
      await w.write(new Uint8Array(400 * 1024));
      await w.close();
    }
  });

  console.log('second boot, with the saved games in storage');
  log = [];
  await boot(page, log);

  const bootLines = log.filter(t => /openkingdoms/.test(t));
  for (const l of bootLines) console.log('    > ' + l);
  check('the boot did not read the saved games',
        !bootLines.some(t => /saved game\(s\) from browser storage/.test(t)));

  const before = await page.evaluate(() => {
    const FS = Module.FS || window.FS;
    try { return FS.readdir('/libsdl/OpenKingdoms/OpenKingdoms/saves')
      .filter(n => n !== '.' && n !== '..'); } catch (e) { return ['ERR ' + e]; }
  });
  check('the filesystem holds no saved games yet: ' + JSON.stringify(before),
        before.length === 0);

  console.log('asking for them, the way a load dialog does');
  const after = await page.evaluate(async () => {
    Module.restoreSaves();
    const pending = Module.savesPending;
    for (let i = 0; i < 200 && Module.savesPending; i++) {
      await new Promise(r => setTimeout(r, 50));
    }
    const FS = Module.FS || window.FS;
    let names = [];
    try { names = FS.readdir('/libsdl/OpenKingdoms/OpenKingdoms/saves')
      .filter(n => n !== '.' && n !== '..'); } catch (e) { names = ['ERR ' + e]; }
    return { pending: pending, names: names.sort(), stillPending: Module.savesPending };
  });
  check('it said it was working', after.pending === 1);
  check('it finished', after.stillPending === 0);
  check('both arrived: ' + JSON.stringify(after.names),
        after.names.length === 2 &&
        after.names[0] === 'Alpha.oksave' && after.names[1] === 'Bravo.oksave');

  console.log('the panel');
  await page.click('#saves-link');
  await page.waitForTimeout(1500);
  await page.screenshot({ path: path.join(OUT, '1-panel.png') });
  const rows = await page.$$eval('#saves-list li .name', els => els.map(e => e.textContent));
  check('it lists both: ' + JSON.stringify(rows),
        rows.length === 2 && rows.indexOf('Alpha') >= 0 && rows.indexOf('Bravo') >= 0);

  const dl = page.waitForEvent('download', { timeout: 20000 });
  await page.click('#saves-list li:first-child a.get');
  const got = await dl;
  const saved = path.join(OUT, got.suggestedFilename());
  await got.saveAs(saved);
  const size = fs.existsSync(saved) ? fs.statSync(saved).size : -1;
  check('a save came out as a file, ' + size + ' bytes', size === 400 * 1024);

  console.log('putting one back');
  const incoming = path.join(os.tmpdir(), 'Charlie.oksave');
  fs.writeFileSync(incoming, Buffer.alloc(1234, 7));
  await page.setInputFiles('#saves-input', incoming);
  await page.waitForTimeout(2500);
  await page.screenshot({ path: path.join(OUT, '2-imported.png') });
  const after2 = await page.$$eval('#saves-list li .name', els => els.map(e => e.textContent));
  check('the panel lists it: ' + JSON.stringify(after2), after2.indexOf('Charlie') >= 0);
  const inFs = await page.evaluate(() => {
    const FS = Module.FS || window.FS;
    try { return FS.readdir('/libsdl/OpenKingdoms/OpenKingdoms/saves')
      .filter(n => n !== '.' && n !== '..').sort(); } catch (e) { return ['ERR ' + e]; }
  });
  check('the game can see it too: ' + JSON.stringify(inFs),
        inFs.indexOf('Charlie.oksave') >= 0);

  /* And a second import of the same name keeps both rather than
     writing over someone's game. */
  await page.setInputFiles('#saves-input', incoming);
  await page.waitForTimeout(2500);
  const after3 = await page.$$eval('#saves-list li .name', els => els.map(e => e.textContent));
  check('a repeat import kept both: ' + JSON.stringify(after3),
        after3.indexOf('Charlie') >= 0 && after3.indexOf('Charlie (2)') >= 0);

  console.log('--- every page line ---');
  for (const l of log.filter(t => /openkingdoms|PAGEERROR|Error|error/i.test(t))) {
    console.log('    > ' + l);
  }
  const errs = log.filter(t => t.indexOf('PAGEERROR') === 0);
  check('the page threw nothing: ' + JSON.stringify(errs.slice(0, 3)), errs.length === 0);

  await browser.close();
  console.log(failures ? failures + ' failure(s)' : 'all good');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.log('FAILED ' + (e && e.stack || e)); process.exit(2); });
