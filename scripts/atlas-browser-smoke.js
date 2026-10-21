/* Browser smoke test for the Asset Atlas, web/atlas.html, against a real
 * install. Serve a site that holds the web/ files and the build's
 * atlas-dec.js and atlas-dec.wasm, then:
 *
 *   node scripts/atlas-browser-smoke.js [url] [gameDir]
 *     url      default http://localhost:8097/atlas.html
 *     gameDir  default ~/Games/Total Annihilation Kingdoms
 *
 * It picks the game folder, waits for the index, opens a feature picture
 * and the remaster model beside it, a unit's 3DO, writes a note, finds it
 * by search, exports a GLB and reads it back, and finds the note again
 * after a reload. Counting is switched off for the visit. Screenshots
 * land in WEB_SMOKE_OUT. ATLAS_SMOKE_BROWSER=firefox runs it in Firefox,
 * WEB_SMOKE_CHANNEL picks the Chromium (msedge by default). Needs
 * playwright resolvable. Not part of CI: it needs game data.
 */
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const pw = require('playwright');

const url = process.argv[2] || 'http://localhost:8097/atlas.html';
const gameDir = process.argv[3] || path.join(os.homedir(), 'Games', 'Total Annihilation Kingdoms');
const outDir = process.env.WEB_SMOKE_OUT || path.join(process.cwd(), 'atlas-smoke-out');
const firefox = process.env.ATLAS_SMOKE_BROWSER === 'firefox';

let page = null;
const errors = [];
async function fail(msg) {
  try { if (page) await page.screenshot({ path: path.join(outDir, 'fail.png') }); } catch (e) {}
  console.error('FAIL: ' + msg);
  process.exit(1);
}
function step(s) { console.log(s); }

async function pick(query, titleRe) {
  await page.fill('#q', query);
  await page.waitForTimeout(400);
  const title = await page.evaluate((src) => {
    const re = new RegExp(src, 'i');
    const c = Array.from(document.querySelectorAll('.cell')).find(x => re.test(x.title));
    if (c) c.click();
    return c ? c.title : null;
  }, titleRe);
  if (!title) await fail('no cell matching ' + titleRe + ' for "' + query + '"');
  return title;
}

/* Distinct colours in the viewer's canvas, read in the frame it was drawn
   in, since a WebGL canvas keeps no copy. A drawn picture has many. */
function viewerColours() {
  return page.evaluate(() => new Promise((done) => requestAnimationFrame(() => {
    const d3 = document.getElementById('viewer').classList.contains('d3');
    const c = document.getElementById(d3 ? 'v3d' : 'v2d');
    const t = document.createElement('canvas');
    t.width = c.width; t.height = c.height;
    const x = t.getContext('2d');
    x.drawImage(c, 0, 0);
    const px = x.getImageData(0, 0, t.width, t.height).data, seen = new Set();
    for (let i = 0; i < px.length; i += 4 * 31) seen.add(px[i] << 16 | px[i + 1] << 8 | px[i + 2]);
    done(seen.size);
  })));
}

async function waitStatus(re, ms) {
  await page.waitForFunction((src) => new RegExp(src).test(document.getElementById('st-main').textContent), re, { timeout: ms });
}

(async () => {
  if (!fs.existsSync(path.join(gameDir, 'data.hpi'))) return fail('no data.hpi in ' + gameDir);
  fs.mkdirSync(outDir, { recursive: true });
  const profile = path.join(outDir, 'profile');
  fs.rmSync(profile, { recursive: true, force: true });
  const opts = { headless: true, viewport: { width: 1440, height: 900 }, acceptDownloads: true };
  const ctx = firefox
    ? await pw.firefox.launchPersistentContext(profile, Object.assign(opts, { firefoxUserPrefs: { 'webgl.force-enabled': true } }))
    : await pw.chromium.launchPersistentContext(profile, Object.assign(opts, {
        channel: process.env.WEB_SMOKE_CHANNEL || 'msedge',
        args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'] }));
  await ctx.addInitScript(() => { try { localStorage.setItem('ok.counts.off', '1'); } catch (e) {} });
  page = await ctx.newPage();
  const watch = (p) => {
    p.on('pageerror', e => errors.push('PAGEERROR ' + e.message));
    p.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  };
  watch(page);

  step('1. pick the game folder and index it');
  await page.goto(url);
  await page.waitForSelector('#welcome:not([hidden])');
  const t0 = Date.now();
  await page.setInputFiles('#dir-input', gameDir);
  await waitStatus('in \\d+ package', 300000);
  const assets = await page.evaluate(() => parseInt(document.getElementById('st-main').textContent.replace(/,/g, ''), 10));
  step('   ' + assets + ' assets in ' + (Date.now() - t0) + ' ms');
  if (!(assets > 1000)) await fail('only ' + assets + ' assets indexed');
  await page.waitForTimeout(2000);
  await page.screenshot({ path: path.join(outDir, '1-grid.png') });

  step('2. a feature picture, and the remaster beside it painted from it');
  await pick('arabuild01', 'anims/arabuild\\.gaf # arabuild01\\n');
  await page.waitForTimeout(1200);
  let n = await viewerColours();
  step('   the picture shows ' + n + ' colours');
  if (n < 40) await fail('the feature picture did not draw');
  await page.click('#mode button[data-v="pair"]');
  await page.waitForFunction(() => document.getElementById('v-msg').hidden, null, { timeout: 60000 });
  await page.waitForTimeout(1500);
  n = await viewerColours();
  step('   the pair shows ' + n + ' colours');
  if (n < 40) await fail('the side by side view did not draw');
  await page.screenshot({ path: path.join(outDir, '2-pair.png') });

  step('3. a unit\'s 3DO with its textures');
  await pick('arasword', 'objects3d/arasword\\.3do');
  await page.click('#mode button[data-v="3d"]').catch(() => {});
  await page.waitForFunction(() => document.getElementById('v-msg').hidden, null, { timeout: 30000 });
  await page.waitForTimeout(1500);
  n = await viewerColours();
  step('   the model shows ' + n + ' colours');
  if (n < 40) await fail('the model did not draw');
  await page.screenshot({ path: path.join(outDir, '3-model.png') });

  step('4. a note and a rating, found by search');
  await page.click('.stars button:nth-child(4)');
  await page.fill('textarea.note', 'atlas smoke note');
  await page.waitForTimeout(800);
  await page.fill('#q', 'smoke note');
  await page.waitForTimeout(500);
  if (!/^1 asset/.test(await page.textContent('#scope .n'))) await fail('the note is not found by search');

  step('5. the model as a GLB, read back');
  await pick('arasword', 'objects3d/arasword\\.3do');
  const [dl] = await Promise.all([page.waitForEvent('download'), page.click('text=GLB for the engine')]);
  const glb = path.join(outDir, dl.suggestedFilename());
  await dl.saveAs(glb);
  const b = fs.readFileSync(glb);
  if (b.readUInt32LE(0) !== 0x46546C67) await fail('the GLB has no glTF header');
  const json = JSON.parse(b.subarray(20, 20 + b.readUInt32LE(12)).toString());
  step('   ' + json.nodes.length + ' pieces, ' + (json.images || []).length + ' pictures');
  if (json.nodes.length < 2 || !(json.images || []).length) await fail('the GLB lost its pieces or pictures');

  step('6. after a reload the index and the note are still there');
  await page.close();
  page = await ctx.newPage();
  watch(page);
  await page.goto(url);
  await waitStatus('from the index in this browser|in \\d+ package', 60000);
  await page.fill('#q', 'smoke note');
  await page.waitForTimeout(600);
  if (!/^1 asset/.test(await page.textContent('#scope .n'))) await fail('the note did not survive a reload');

  await ctx.close();
  if (errors.length) await fail('the page logged errors:\n  ' + errors.slice(0, 8).join('\n  '));
  console.log('PASS');
})().catch(e => fail(e && e.stack ? e.stack : String(e)));
